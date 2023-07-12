/*
 * Copyright (c) 2018, 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/cdsThread.hpp"
#include "cds/filemap.hpp"
#include "cds/streamingArchiveHeapLoader.hpp"
#include "cds/heapShared.hpp"
#include "cds/metaspaceShared.hpp"
#include "classfile/classLoaderDataShared.hpp"
#include "classfile/stringTable.hpp"
#include "gc/shared/collectedHeap.inline.hpp"
#include "gc/shared/oopStorage.inline.hpp"
#include "gc/shared/oopStorageSet.inline.hpp"
#include "logging/log.hpp"
#include "runtime/mutex.hpp"
#include "oops/access.inline.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/thread.hpp"
#include "memory/iterator.inline.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/stack.inline.hpp"

#include <type_traits>

#if INCLUDE_CDS_JAVA_HEAP

FileMapRegion* StreamingArchiveHeapLoader::_heap_region;
FileMapRegion* StreamingArchiveHeapLoader::_bitmap_region;
address StreamingArchiveHeapLoader::_roots_old_addr;
oop* StreamingArchiveHeapLoader::_roots;
BitMapView StreamingArchiveHeapLoader::_oopmap;
bool StreamingArchiveHeapLoader::_is_loaded;
int StreamingArchiveHeapLoader::_last_batch_last_object;
int StreamingArchiveHeapLoader::_current_batch_last_object;
int StreamingArchiveHeapLoader::_current_root;
size_t StreamingArchiveHeapLoader::_current_buffer_offset;
bool StreamingArchiveHeapLoader::_allow_gc;
bool StreamingArchiveHeapLoader::_stop_background_processing;
bool StreamingArchiveHeapLoader::_finished_processing;
size_t StreamingArchiveHeapLoader::_num_archived_objects;

size_t* StreamingArchiveHeapLoader::_dfs_to_archive_offset_table;
void** StreamingArchiveHeapLoader::_dfs_to_heap_object_table;
int* StreamingArchiveHeapLoader::_roots_highest_dfs;

bool StreamingArchiveHeapLoader::_waiting_for_iterator;

template <bool allow_gc>
oop StreamingArchiveHeapLoader::heap_object_for_index(int object_index) {
  if (allow_gc) {
    oop* handle = (oop*)_dfs_to_heap_object_table[object_index];
    if (handle == nullptr) {
      return nullptr;
    }
    return NativeAccess<>::oop_load(handle);
  } else {
    return cast_to_oop(_dfs_to_heap_object_table[object_index]);
  }
}

template <bool allow_gc, bool is_dumping_cached_code>
void StreamingArchiveHeapLoader::set_heap_object_for_index(int object_index, oop heap_object) {
  assert(heap_object_for_index<allow_gc>(object_index) == nullptr, "Should only set once with this API");
  if (allow_gc) {
    oop* handle = Universe::vm_global()->allocate();
    NativeAccess<>::oop_store(handle, heap_object);
    _dfs_to_heap_object_table[object_index] = (void*)handle;
  } else {
    _dfs_to_heap_object_table[object_index] = cast_from_oop<void*>(heap_object);
  }

  if (is_dumping_cached_code) {
    MutexLocker ml(ArchivedObjectTables_lock, Mutex::_no_safepoint_check_flag);
    HeapShared::add_to_permanent_index_table(heap_object, object_index);
  }
}

template <bool allow_gc>
void StreamingArchiveHeapLoader::replace_heap_object_for_index(int object_index, oop heap_object) {
  if (allow_gc) {
    oop* handle = (oop*)_dfs_to_heap_object_table[object_index];
    NativeAccess<>::oop_store(handle, heap_object);
  } else {
    _dfs_to_heap_object_table[object_index] = cast_from_oop<void*>(heap_object);
  }
}

static void patch_metadata(oop heap_object, int offset) {
  if (heap_object->metadata_field(offset) != nullptr) {
    heap_object->metadata_field_put(offset, (Metadata*)(address(heap_object->metadata_field(offset)) + MetaspaceShared::relocation_delta()));
  }
}

static void patch_metadata(oop heap_object) {
  if (java_lang_Class::is_instance(heap_object)) {
    patch_metadata(heap_object, java_lang_Class::klass_offset());
    patch_metadata(heap_object, java_lang_Class::array_klass_offset());
  } else if (java_lang_invoke_ResolvedMethodName::is_instance(heap_object)) {
    patch_metadata(heap_object, java_lang_invoke_ResolvedMethodName::vmtarget_offset());
  }
}

oop StreamingArchiveHeapLoader::allocate_object(oopDesc* archive_object, size_t size, JavaThread* thread) {
  assert(!archive_object->is_instanceRef(), "no such objects are archived");
  assert(!archive_object->is_stackChunk(), "no such objects are archived");

  oop heap_object;

  if (archive_object->is_instance()) {
    heap_object = Universe::heap()->obj_allocate(archive_object->klass(), size, thread);
  } else if (archive_object->is_typeArray()) {
    int len = static_cast<typeArrayOop>(archive_object)->length();
    heap_object = TypeArrayKlass::cast(archive_object->klass())->allocate(len, thread);
  } else {
    assert(archive_object->is_objArray(), "must be");
    int len = static_cast<objArrayOop>(archive_object)->length();
    heap_object = ObjArrayKlass::cast(archive_object->klass())->allocate(len, thread);
  }

  return heap_object;
}

void StreamingArchiveHeapLoader::install_root(int root_index, oop heap_object) {
  objArrayOop roots = objArrayOop((oop)NativeAccess<>::oop_load(_roots));
  OrderAccess::release(); // Once the store below publishes an object, it can be concurrently picked up by another thread without using the lock
  roots->obj_at_put(root_index, heap_object);
}

class PushReferenceOopClosure : public BasicOopIterateClosure {
private:
  Stack<CDSHeapTraversalEntry, mtClassShared>& _dfs_stack;
  oop _object;
  int _object_index;

public:
  PushReferenceOopClosure(Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, oop object, int object_index)
    : _dfs_stack(dfs_stack), _object(object), _object_index(object_index) {}

  virtual void do_oop(oop* p) { do_oop_work(p, (int)*(intptr_t*)p); }
  virtual void do_oop(narrowOop* p) { do_oop_work(p, *(int*)p); }

  template <typename T>
  void do_oop_work(T* p, int dfs_index) {
    if (dfs_index != 0) {
      uintptr_t field_offset = uintptr_t(p) - cast_from_oop<uintptr_t>(_object);
      _dfs_stack.push({dfs_index, _object_index, field_offset});
    }
  }
};

template <bool COOPS, bool allow_gc>
void StreamingArchiveHeapLoader::TracingObjectLoader::copy_object(oopDesc* archive_object, int archive_object_index, oop heap_object, size_t size, markWord mark, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  if (!allow_gc) {
    // TODO: Custom allocator
    Copy::disjoint_words((HeapWord*)archive_object, cast_from_oop<HeapWord*>(heap_object), size);
    PushReferenceOopClosure cl(dfs_stack, heap_object, archive_object_index);
    heap_object->oop_iterate(&cl);
    patch_metadata(heap_object);
    intptr_t archive_hash = mark.hash();
    if (archive_hash != 0) {
      heap_object->set_mark(heap_object->mark().copy_set_hash(archive_hash));
    }
    return;
  }
  size_t scale = COOPS ? 2 : 1;
  using RawElementT = std::conditional_t<COOPS, int32_t, int64_t>;
  using OopElementT = std::conditional_t<COOPS, narrowOop, oop>;

  address bottom = (address)_heap_region->mapped_base();

  size_t header_size = COOPS ? 3 : 2;

  const BitMap::idx_t start_bit = (BitMap::idx_t)(((HeapWord*)archive_object) - ((HeapWord*)bottom)) * scale;
  const BitMap::idx_t end_bit = start_bit + size * scale;

  BitMap::idx_t unfinished_bit = start_bit + header_size;
  BitMap::idx_t next_reference_bit = _oopmap.find_first_set_bit(unfinished_bit, end_bit);

  // Fill in heap object bytes
  while (unfinished_bit != end_bit) {
    // This is the adddress of the pointee inside the input stream
    RawElementT* archive_payload_addr = ((RawElementT*)archive_object) + unfinished_bit - start_bit;

    if (next_reference_bit != unfinished_bit) {
      // Primitive bytes available
      RawElementT* heap_payload_addr = cast_from_oop<RawElementT*>(heap_object) + unfinished_bit - start_bit;

      size_t primitive_elements = next_reference_bit - unfinished_bit;
      size_t primitive_bytes = primitive_elements * sizeof(RawElementT);
      memcpy(heap_payload_addr, archive_payload_addr, primitive_bytes);

      unfinished_bit = next_reference_bit;
    } else {
      // Encountered reference
      RawElementT* archive_p = (RawElementT*)archive_payload_addr;
      RawElementT pointee_dfs_index = *archive_p;

      dfs_stack.push({(int)pointee_dfs_index, archive_object_index, (unfinished_bit - start_bit) * sizeof(OopElementT)});

      unfinished_bit++;
      next_reference_bit = _oopmap.find_first_set_bit(unfinished_bit, end_bit);
    }
  }

  if (!COOPS && oopDesc::has_klass_gap()) {
    oopDesc::set_klass_gap(cast_from_oop<HeapWord*>(heap_object), *(int*)(address(archive_object) + oopDesc::klass_gap_offset_in_bytes()));
  }

  patch_metadata(heap_object);

  intptr_t archive_hash = mark.hash();
  if (archive_hash != 0) {
    heap_object->set_mark(heap_object->mark().copy_set_hash(archive_hash));
  }
}

template <bool COOPS, bool allow_gc, bool is_dumping_cached_code>
oop StreamingArchiveHeapLoader::TracingObjectLoader::materialize_object_inner(oopDesc* archive_object, int archive_object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  // Allocate object
  size_t size = archive_object->size();
  markWord mark = *archive_object->mark_addr();
  oop heap_object = allocate_object(archive_object, size, thread);

  // Install forwarding
  set_heap_object_for_index<allow_gc, is_dumping_cached_code>(archive_object_index, heap_object);

  // Fill in object contents, and recursively materialize
  copy_object<COOPS, allow_gc>(archive_object, archive_object_index, heap_object, size, mark, dfs_stack, thread);

  return heap_object;
}

template <bool COOPS, bool allow_gc, bool is_dumping_cached_code>
oop StreamingArchiveHeapLoader::TracingObjectLoader::materialize_object(oopDesc* archive_object, int archive_object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  oop heap_object = heap_object_for_index<allow_gc>(archive_object_index);

  if (archive_object_index <= _last_batch_last_object) {
    // The transitive closure of this object has been materialized; no need to do anything
    return heap_object;
  }

  if (archive_object_index <= _current_batch_last_object) {
    // The CDSThread is currently materializing this object and its transitive closure; only need to wait for it to complete
    _waiting_for_iterator = true;
    while (archive_object_index > _last_batch_last_object) {
      CDSHeapLoading_lock->wait();
    }
    _waiting_for_iterator = false;
    heap_object = heap_object_for_index<allow_gc>(archive_object_index);
    return heap_object;
  }

  if (heap_object != nullptr) {
    // Already materialized by mutator
    return heap_object;
  }

  heap_object = materialize_object_inner<COOPS, allow_gc, is_dumping_cached_code>(archive_object, archive_object_index, dfs_stack, thread);

  if (java_lang_String::is_instance(archive_object)) {
    address bottom = (address)_heap_region->mapped_base();
    size_t archive_object_offset = size_t(archive_object) - size_t(bottom);
    BitMap::idx_t obj_bit;
    if (COOPS) {
      obj_bit = BitMap::idx_t(archive_object_offset / sizeof(narrowOop));
    } else {
      obj_bit = BitMap::idx_t(archive_object_offset / sizeof(HeapWord));
    }
    if (_oopmap.at(obj_bit + 1)) {
      // Interned string... finish materializing and link it to the string table
      int value_dfs_index = archive_object_index + 1;
      oopDesc* value_archive_object = (oopDesc*)(bottom + _dfs_to_archive_offset_table[value_dfs_index]);
      oop value_heap_object = materialize_object<COOPS, allow_gc, is_dumping_cached_code>(value_archive_object, value_dfs_index, dfs_stack, thread);

      heap_object = heap_object_for_index<allow_gc>(archive_object_index);
      if (allow_gc) {
        heap_object->obj_field_put(java_lang_String::value_offset(), value_heap_object);
      } else {
        // Allocated objects are not properly initialized when GC isn't allowed
        heap_object->obj_field_put_access<IS_DEST_UNINITIALIZED>(java_lang_String::value_offset(), value_heap_object);
      }

      // Replace string with interned string
      heap_object = StringTable::intern(heap_object, thread);
      replace_heap_object_for_index<allow_gc>(archive_object_index, heap_object);
    }
  }

  return heap_object;
}

template <bool COOPS, bool allow_gc, bool is_dumping_cached_code>
void StreamingArchiveHeapLoader::TracingObjectLoader::drain_stack(Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  address bottom = (address)_heap_region->mapped_base();
  while (!dfs_stack.is_empty()) {
    CDSHeapTraversalEntry entry = dfs_stack.pop();
    int pointee_index = entry._pointee_index;
    oopDesc* pointee_archive_object = (oopDesc*)(bottom + _dfs_to_archive_offset_table[pointee_index]);
    oop pointee_heap_object = materialize_object<COOPS, allow_gc, is_dumping_cached_code>(pointee_archive_object, pointee_index, dfs_stack, thread);
    oop heap_object = heap_object_for_index<allow_gc>(entry._base_index);
    heap_object->obj_field_put_access<IS_DEST_UNINITIALIZED>((int)entry._heap_field_offset_bytes, pointee_heap_object);
  }
}

oop StreamingArchiveHeapLoader::TracingObjectLoader::materialize_object_transitive(oopDesc* archive_object, int object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  assert_locked_or_safepoint(CDSHeapLoading_lock);
  while (_waiting_for_iterator) {
    CDSHeapLoading_lock->wait();
  }

  objArrayOopDesc* roots_old = (objArrayOopDesc *)_roots_old_addr;

  Handle result;

  if (CDSConfig::is_dumping_cached_code() && StoreCachedCode) {
    if (UseCompressedOops) {
      if (_allow_gc) {
        result = Handle(thread, materialize_object<true, true, true>(archive_object, object_index, dfs_stack, thread));
        drain_stack<true, true, true>(dfs_stack, thread);
      } else {
        result = Handle(thread, materialize_object<true, false, true>(archive_object, object_index, dfs_stack, thread));
        drain_stack<true, false, true>(dfs_stack, thread);
      }
    } else {
      if (_allow_gc) {
        result = Handle(thread, materialize_object<false, true, true>(archive_object, object_index, dfs_stack, thread));
        drain_stack<false, true, true>(dfs_stack, thread);
      } else {
        result = Handle(thread, materialize_object<false, false, true>(archive_object, object_index, dfs_stack, thread));
        drain_stack<false, false, true>(dfs_stack, thread);
      }
    }
  } else {
    if (UseCompressedOops) {
      if (_allow_gc) {
        result = Handle(thread, materialize_object<true, true, false>(archive_object, object_index, dfs_stack, thread));
        drain_stack<true, true, false>(dfs_stack, thread);
      } else {
        result = Handle(thread, materialize_object<true, false, false>(archive_object, object_index, dfs_stack, thread));
        drain_stack<true, false, false>(dfs_stack, thread);
      }
    } else {
      if (_allow_gc) {
        result = Handle(thread, materialize_object<false, true, false>(archive_object, object_index, dfs_stack, thread));
        drain_stack<false, true, false>(dfs_stack, thread);
      } else {
        result = Handle(thread, materialize_object<false, false, false>(archive_object, object_index, dfs_stack, thread));
        drain_stack<false, false, false>(dfs_stack, thread);
      }
    }
  }

  return result();
}

oop StreamingArchiveHeapLoader::TracingObjectLoader::materialize_root(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  address bottom = (address)_heap_region->mapped_base();
  typeArrayOopDesc* roots_old = (typeArrayOopDesc *)_roots_old_addr;
  int root_dfs_index = roots_old->int_at(root_index);
  size_t root_buffer_offset = _dfs_to_archive_offset_table[root_dfs_index];
  oopDesc* root_obj = (oopDesc*)(bottom + root_buffer_offset);

  return materialize_object_transitive(root_obj, root_dfs_index, dfs_stack, thread);
}

oop StreamingArchiveHeapLoader::TracingObjectLoader::root(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  // Get the materialized roots array
  oop roots_obj = NativeAccess<>::oop_load(_roots);
  objArrayOop roots = (objArrayOop)roots_obj;

  // Check if we got the corresponding root
  oop root = roots->obj_at(root_index);

  if (root == nullptr) {
    // If not, materialize the root
    root = materialize_root(root_index, dfs_stack, thread);
    install_root(root_index, root);
  }

  return root;
}

int oop_handle_cmp(const void* left, const void* right) {
  oop* left_handle = *(oop**)left;
  oop* right_handle = *(oop**)right;
  return uintptr_t(right_handle) - uintptr_t(left_handle);
}

template <bool allow_gc>
class InflateReferenceOopClosure : public BasicOopIterateClosure {
private:
  void** _dfs_to_heap_object_table;

public:
  InflateReferenceOopClosure(void** dfs_to_heap_object_table)
    : _dfs_to_heap_object_table(dfs_to_heap_object_table) {}

  virtual void do_oop(oop* p) { do_oop_work(p, (int)*(intptr_t*)p); }
  virtual void do_oop(narrowOop* p) { do_oop_work(p, *(int*)p); }

  template <typename T>
  void do_oop_work(T* p, int dfs_index) {
    if (dfs_index != 0) {
      oop obj = StreamingArchiveHeapLoader::heap_object_for_index<allow_gc>(dfs_index);
      HeapAccess<IS_DEST_UNINITIALIZED>::oop_store(p, obj);
    }
  }
};

template <bool COOPS, bool allow_gc>
void StreamingArchiveHeapLoader::IterativeObjectLoader::copy_object(oopDesc* archive_object, oop heap_object, size_t size) {
  Copy::disjoint_words((HeapWord*)archive_object, cast_from_oop<HeapWord*>(heap_object), size);
  InflateReferenceOopClosure<allow_gc> cl(_dfs_to_heap_object_table);
  heap_object->oop_iterate(&cl);
  patch_metadata(heap_object);
  intptr_t archive_hash = archive_object->mark().hash();
  if (archive_hash != 0) {
    heap_object->set_mark(heap_object->mark().copy_set_hash(archive_hash));
  }
}

template <bool COOPS, bool allow_gc>
void StreamingArchiveHeapLoader::IterativeObjectLoader::initialize_object(oopDesc* archive_object, oop heap_object, int dfs_index, size_t size, JavaThread* thread) {
  // Fill in object contents, and recursively materialize
  copy_object<COOPS, allow_gc>(archive_object, heap_object, size);

  if (java_lang_String::is_instance(archive_object)) {
    address bottom = (address)_heap_region->mapped_base();
    size_t archive_object_offset = size_t(archive_object) - size_t(bottom);
    BitMap::idx_t obj_bit;
    if (COOPS) {
      obj_bit = BitMap::idx_t(archive_object_offset / sizeof(narrowOop));
    } else {
      obj_bit = BitMap::idx_t(archive_object_offset / sizeof(HeapWord));
    }
    if (_oopmap.at(obj_bit + 1)) {
      // Interned string... finish materializing and link it to the string table
      int value_dfs_index = dfs_index + 1;
      oop value_heap_object = heap_object_for_index<allow_gc>(value_dfs_index);
      oopDesc* value_archive_object = (oopDesc*)(((HeapWord*)archive_object) + size);
      // TODO: avoid double initialization of string values
      initialize_object<COOPS, allow_gc>(value_archive_object, value_heap_object, value_dfs_index, value_archive_object->size(), thread);
      replace_heap_object_for_index<allow_gc>(dfs_index, StringTable::intern(heap_object, thread));
    }
  }
}

template <bool COOPS, bool allow_gc>
size_t StreamingArchiveHeapLoader::IterativeObjectLoader::initialize_range(int first_dfs, size_t first_offset, int last_dfs, JavaThread* thread) {
  HeapWord* bottom = ((HeapWord*)_heap_region->mapped_base()) + first_offset;
  size_t initialized_words = 0;

  for (int i = first_dfs; i <= last_dfs; ++i) {
    oopDesc* archive_object = (oopDesc*)(((HeapWord*)bottom) + initialized_words);
    size_t size = archive_object->size();
    oop heap_object = heap_object_for_index<allow_gc>(i);
    initialize_object<COOPS, allow_gc>(archive_object, heap_object, i, size, thread);
    initialized_words += size;
  }

  return initialized_words;
}

template <bool COOPS, bool allow_gc, bool is_dumping_cached_code>
size_t StreamingArchiveHeapLoader::IterativeObjectLoader::materialize_range(int first_dfs, size_t first_offset, int last_dfs, JavaThread* thread) {
  HeapWord* bottom = ((HeapWord*)_heap_region->mapped_base()) + first_offset;

  size_t allocated_words = 0;

  GrowableArrayCHeap<int, mtClassShared>* lazy_object_indices = nullptr;

  for (int i = first_dfs; i <= last_dfs; ++i) {
    oopDesc* archive_object = (oopDesc*)(((HeapWord*)bottom) + allocated_words);
    size_t size = archive_object->size();
    oop heap_object = heap_object_for_index<allow_gc>(i); // TODO: Specialize is_empty check? Don't care what the actual oop value is here
    if (heap_object == nullptr) {
      // The normal case; no lazy loading have loaded the object yet
      heap_object = allocate_object(archive_object, size, thread);
      set_heap_object_for_index<allow_gc, is_dumping_cached_code>(i, heap_object);
    } else {
      // Lazy loading has already initialized the object; we must not mutate it
      if (lazy_object_indices == nullptr) {
        lazy_object_indices = new GrowableArrayCHeap<int, mtClassShared>();
      }
      lazy_object_indices->append(i);
    }
    allocated_words += size;
  }

  size_t initialized_words = 0;

  if (lazy_object_indices == nullptr) {
    // Normal case; no sprinkled lazy objects in the root subgraph
    initialized_words = initialize_range<COOPS, allow_gc>(first_dfs, first_offset, last_dfs, thread);
  } else {
    // The user lazy initialized some objects that are already initialized; we have to initialize around them
    // to make sure they are not mutated.
    int previous_index = first_dfs - 1; // Exclusive start of initialization slice
    for (int i = 0; i < lazy_object_indices->length(); ++i) {
      int lazy_object_index = lazy_object_indices->at(i);
      int slice_start = previous_index;
      int slice_end = lazy_object_index;

      if (slice_end - slice_start > 1) { // Both markers are exclusive
        initialized_words += initialize_range<COOPS, allow_gc>(slice_start + 1, first_offset + initialized_words, slice_end - 1, thread);
      }
      oop heap_object = heap_object_for_index<allow_gc>(lazy_object_index);
      initialized_words += heap_object->size();
      previous_index = lazy_object_index;
    }
    // Process tail range
    if (last_dfs - previous_index > 0) {
      initialized_words += initialize_range<COOPS, allow_gc>(previous_index + 1, first_offset + initialized_words, last_dfs, thread);
    }
    delete lazy_object_indices;
  }

  return initialized_words;
}

void StreamingArchiveHeapLoader::IterativeObjectLoader::materialize_next_root(JavaThread* thread) {
  int current_root = _current_root;
  int last_dfs_index = _roots_highest_dfs[current_root];

  typeArrayOopDesc* roots_old = (typeArrayOopDesc *)_roots_old_addr;
  int root_dfs_index = roots_old->int_at(current_root);
  oop root = nullptr;
  bool allow_gc = _allow_gc;

  // Materialize objects of necessary, representing the transitive closure of the root
  if (last_dfs_index > _last_batch_last_object) {
    int first_dfs_index = _last_batch_last_object + 1;
    _current_batch_last_object = last_dfs_index;
    {
      MutexUnlocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
      if (CDSConfig::is_dumping_cached_code() && StoreCachedCode) {
        if (UseCompressedOops) {
          if (allow_gc) {
            _current_buffer_offset += materialize_range<true, true, true>(first_dfs_index, _current_buffer_offset, last_dfs_index, thread);
          } else {
            _current_buffer_offset += materialize_range<true, false, true>(first_dfs_index, _current_buffer_offset, last_dfs_index, thread);
          }
        } else {
          if (allow_gc) {
            _current_buffer_offset += materialize_range<false, true, true>(first_dfs_index, _current_buffer_offset, last_dfs_index, thread);
          } else {
            _current_buffer_offset += materialize_range<false, false, true>(first_dfs_index, _current_buffer_offset, last_dfs_index, thread);
          }
        }
      } else {
        if (UseCompressedOops) {
          if (allow_gc) {
            _current_buffer_offset += materialize_range<true, true, false>(first_dfs_index, _current_buffer_offset, last_dfs_index, thread);
          } else {
            _current_buffer_offset += materialize_range<true, false, false>(first_dfs_index, _current_buffer_offset, last_dfs_index, thread);
          }
        } else {
          if (allow_gc) {
            _current_buffer_offset += materialize_range<false, true, false>(first_dfs_index, _current_buffer_offset, last_dfs_index, thread);
          } else {
            _current_buffer_offset += materialize_range<false, false, false>(first_dfs_index, _current_buffer_offset, last_dfs_index, thread);
          }
        }
      }
    }
    _last_batch_last_object = _current_batch_last_object;
    if (_waiting_for_iterator) {
      CDSHeapLoading_lock->notify_all();
    }
  }

  if (allow_gc) {
    root = heap_object_for_index<true>(root_dfs_index);
  } else {
    root = heap_object_for_index<false>(root_dfs_index);
  }

  // Install the root
  install_root(current_root, root);
}

void StreamingArchiveHeapLoader::materialize_objects() {
  // Get the materialized roots array
  JavaThread* thread = JavaThread::current();

  size_t bootstrap_max_memory = Universe::heap()->bootstrap_max_memory();
  size_t bootstrap_min_memory = 2 * M;

  size_t before_gc_materialize_budget_bytes = (bootstrap_max_memory > bootstrap_min_memory) ? bootstrap_max_memory - bootstrap_min_memory : 0;
  size_t before_gc_materialize_budget_words = before_gc_materialize_budget_bytes / HeapWordSize;

  log_info(cds, heap)("Max bootstrapping memory: " SIZE_FORMAT "M, min bootstrapping memory: " SIZE_FORMAT "M, selected budget: " SIZE_FORMAT "M",
                      bootstrap_max_memory / M, bootstrap_min_memory / M, before_gc_materialize_budget_bytes / M);

  int roots_length = compute_roots_length();

  // Objects are laid out in DFS order; DFS traverse the roots by linearly walking all objects
  HandleMark hm(thread);
  log_info(cds, heap)("Concurrent object materialization start");

  jlong start = os::javaTimeNanos();

  // Early materialization with a budget before GC is allowed
  MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
  for (_current_root = 0; _current_root < roots_length; ++_current_root) {
    if (_stop_background_processing || _allow_gc || _current_buffer_offset > before_gc_materialize_budget_words) {
      log_info(cds, heap)("Early object materialization interrupted at root %d", _current_root);
      break;
    }

    IterativeObjectLoader::materialize_next_root(thread);
  }

  jlong end = os::javaTimeNanos();

  bool finished_before_gc_allowed = !_allow_gc && _current_root == roots_length;

  if (_current_root == roots_length) {
    _finished_processing = true; // TODO: Move into mater_next_root
    CDSHeapLoading_lock->notify_all();
  }

  log_info(cds,heap)("Early object materialization time: " SIZE_FORMAT "ms",
                     (end - start) / 1000000);

  if (!await_gc_enabled()) {
    // Continue materializing with GC allowed
    log_info(cds, heap)("Concurrent object materialization start after gc enabling");

    for (; _current_root < roots_length; ++_current_root) {
      if (_stop_background_processing) {
        log_info(cds, heap)("Late object materialization interrupted at root %d", _current_root);
        break;
      }

      IterativeObjectLoader::materialize_next_root(thread);
    }

    if (_current_root == roots_length) {
      _finished_processing = true;
      CDSHeapLoading_lock->notify_all();
    }

    log_info(cds, heap)("Concurrent object materialization end");
  }

  await_finished_processing();

  log_info(cds, heap)("Concurrent object materialization cleanup start");

  // Remove OopStorage roots
  if (!finished_before_gc_allowed) {
    // All objects except the roots array
    size_t num_handles = _num_archived_objects - 1;
    // Skip the null entry
    oop** handles = ((oop**)_dfs_to_heap_object_table) + 1;
    qsort(handles, num_handles, sizeof(oop*), (int (*)(const void*, const void*))oop_handle_cmp);
    for (size_t i = 0; i < num_handles; ++i) {
      oop* handle = handles[i];
      NativeAccess<>::oop_store(handle, nullptr);
    }
    Universe::vm_global()->release(handles, num_handles);
  }

  FREE_C_HEAP_ARRAY(void*, _dfs_to_heap_object_table);

  // Unmap regions
  FileMapInfo::current_info()->unmap_region(MetaspaceShared::hp);
  FileMapInfo::current_info()->unmap_region(MetaspaceShared::bm);

  log_info(cds, heap)("Concurrent object materialization cleanup end");
}

void StreamingArchiveHeapLoader::switch_index_to_handle(int object_index) {
  oop heap_object = cast_to_oop(_dfs_to_heap_object_table[object_index]);
  if (heap_object == nullptr) {
    return;
  }

  oop* handle = Universe::vm_global()->allocate();
  NativeAccess<>::oop_store(handle, heap_object);
  _dfs_to_heap_object_table[object_index] = handle;
}

void StreamingArchiveHeapLoader::enable_gc() {
  log_info(cds, heap)("Enable GC");
  CDSThread::materialize_thread_object();
  MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
  // First wait until no tracing is active
  while (_waiting_for_iterator) {
    CDSHeapLoading_lock->wait();
  }

  _allow_gc = true;

  if (_current_root != compute_roots_length()) {
    // Inflate oop handles and continue materializing objects in a less efficient mode
    int num_handles = _num_archived_objects - 1;
    int current_end = _current_batch_last_object;
    int last_end = _last_batch_last_object;
    for (int i = current_end + 1; i <= num_handles; ++i) {
      // First upgrade handles in front of the iterative materialization
      switch_index_to_handle(i);
    }

    // The CDSThread is currently materializing this object and its transitive closure; only need to wait for it to complete
    _waiting_for_iterator = true;
    while (last_end == _last_batch_last_object) {
      CDSHeapLoading_lock->wait();
    }
    for (int i = 1; i <= current_end; ++i) {
      switch_index_to_handle(i);
    }
    _waiting_for_iterator = false;
  }
  CDSHeapLoading_lock->notify_all();
}

void StreamingArchiveHeapLoader::finish_materialize_objects() {
  // Get the materialized roots array
  JavaThread* thread = JavaThread::current();
  HandleMark hm(thread);
  int length = compute_roots_length();

  log_info(cds, heap)("Synchronous object materialization start");

  MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
  _stop_background_processing = true;
  if (_finished_processing) {
    return;
  }

  for (; _current_root < length; ++_current_root) {
    IterativeObjectLoader::materialize_next_root(thread);
  }

  _finished_processing = true;
  CDSHeapLoading_lock->notify_all();

  log_info(cds, heap)("Synchronous object materialization end");
}

static volatile jlong _accumulated_lazy_materialization_time_ns = 0;

void account_lazy_materialization_time_ns(jlong time, const char* description, int index) {
  Atomic::add(&_accumulated_lazy_materialization_time_ns, time);
  log_info(cds, heap)("Lazy materialization of %s: %d end (%ld us of %ld us)", description, index, time / 1000, _accumulated_lazy_materialization_time_ns / 1000);
}

oop StreamingArchiveHeapLoader::get_archived_object(int permanent_index) {
  JavaThread* thread = JavaThread::current();
  HandleMark hm(thread);
  Stack<CDSHeapTraversalEntry, mtClassShared> dfs_stack;
  address bottom = (address)_heap_region->mapped_base();
  oopDesc* archive_object = (oopDesc*)(bottom + _dfs_to_archive_offset_table[permanent_index]);

  oop result;
  jlong start = os::javaTimeNanos();

  {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    if (permanent_index <= _last_batch_last_object) {
      // Deep materialized by the CDS thread
      if (_allow_gc) {
        result = heap_object_for_index<true>(permanent_index);
      } else {
        result = heap_object_for_index<false>(permanent_index);
      }
    } else {
      result = TracingObjectLoader::materialize_object_transitive(archive_object, permanent_index, dfs_stack, thread);
    }
  }

  account_lazy_materialization_time_ns(os::javaTimeNanos() - start, "permanent object", permanent_index);

  return result;
}

// Initialize an empty array of CDS heap roots; materialize them lazily
void StreamingArchiveHeapLoader::initialize_roots() {
  JavaThread* thread = JavaThread::current();

  FileMapInfo::current_info()->map_bitmap_region();

  _heap_region = FileMapInfo::current_info()->region_at(MetaspaceShared::hp);
  _bitmap_region = FileMapInfo::current_info()->region_at(MetaspaceShared::bm);

  // HeapShared::roots() is at this offset in the stream.
  size_t heap_roots_offset = FileMapInfo::current_info()->heap_roots_offset();
  size_t forwarding_offset = FileMapInfo::current_info()->forwarding_offset();
  size_t roots_highest_dfs_offset = FileMapInfo::current_info()->roots_highest_dfs_offset();
  _num_archived_objects = FileMapInfo::current_info()->num_archived_objects();

  // The materialized address of the HeapShared::roots()
  _roots_old_addr = ((address)_heap_region->mapped_base()) + heap_roots_offset;
  typeArrayOopDesc* roots_old = (typeArrayOopDesc *)_roots_old_addr;
  int length = roots_old->length();

  objArrayOop roots = ObjArrayKlass::cast(Universe::objectArrayKlassObj())->allocate(length, thread);
  if (roots == nullptr) {
    fatal("Not enough memory available to initialize JVM");
  }
  _roots = Universe::vm_global()->allocate(); // TODO: Remove the root when we are done?
  NativeAccess<>::oop_store(_roots, roots);

  _dfs_to_archive_offset_table = (size_t*)(((address)_heap_region->mapped_base()) + forwarding_offset);
  _dfs_to_heap_object_table = NEW_C_HEAP_ARRAY(void*, _num_archived_objects + 1, mtClassShared);
  Copy::zero_to_bytes(_dfs_to_heap_object_table, (_num_archived_objects + 1) * sizeof(void*));

  _roots_highest_dfs = (int*)(((address)_heap_region->mapped_base()) + roots_highest_dfs_offset);

  address start = (address)(_bitmap_region->mapped_base()) + _heap_region->oopmap_offset();
  _oopmap = BitMapView((BitMap::bm_word_t*)start, _heap_region->oopmap_size_in_bits());

  _is_loaded = true;
  HeapShared::init_roots(roots);

  CDSThread::initialize();
}

oop StreamingArchiveHeapLoader::root(int root_index) {
  jlong start = os::javaTimeNanos();
  JavaThread* thread = JavaThread::current();
  Stack<CDSHeapTraversalEntry, mtClassShared> dfs_stack;
  HandleMark hm(thread);

  oop result;
  {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);

    if (_finished_processing) {
      objArrayOop roots = objArrayOop((oop)NativeAccess<>::oop_load(_roots));
      result = roots->obj_at(root_index);
    } else {
      typeArrayOopDesc* roots_old = (typeArrayOopDesc *)_roots_old_addr;
      int root_dfs_index = roots_old->int_at(root_index);
      result = TracingObjectLoader::root(root_index, dfs_stack, thread);
    }
  }

  account_lazy_materialization_time_ns(os::javaTimeNanos() - start, "root", root_index);

  return result;
}

int StreamingArchiveHeapLoader::compute_roots_length() {
  if (!_is_loaded) {
    return 0;
  }

  oop roots_obj = NativeAccess<>::oop_load(_roots);
  typeArrayOop roots = (typeArrayOop)roots_obj;
  int length = roots->length();

  return length;
}

bool StreamingArchiveHeapLoader::await_gc_enabled() {
  while (!_allow_gc || _waiting_for_iterator) {
    CDSHeapLoading_lock->wait();
  }

  return _stop_background_processing;
}

void StreamingArchiveHeapLoader::await_finished_processing() {
  while (!_finished_processing) {
    CDSHeapLoading_lock->wait();
  }
}

#endif // INCLUDE_CDS_JAVA_HEAP
