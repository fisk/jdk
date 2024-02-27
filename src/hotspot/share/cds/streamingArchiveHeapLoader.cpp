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
int* StreamingArchiveHeapLoader::_roots_archive;
oop* StreamingArchiveHeapLoader::_roots_heap;
BitMapView StreamingArchiveHeapLoader::_oopmap;
bool StreamingArchiveHeapLoader::_is_loaded;
int StreamingArchiveHeapLoader::_previous_batch_last_object_index;
int StreamingArchiveHeapLoader::_current_batch_last_object_index;
int StreamingArchiveHeapLoader::_current_root_index;
size_t StreamingArchiveHeapLoader::_allocated_words;
bool StreamingArchiveHeapLoader::_allow_gc;
bool StreamingArchiveHeapLoader::_stop_background_processing;
bool StreamingArchiveHeapLoader::_finished_processing;
size_t StreamingArchiveHeapLoader::_num_archived_objects;

size_t* StreamingArchiveHeapLoader::_object_index_to_buffer_offset_table;
void** StreamingArchiveHeapLoader::_object_index_to_heap_object_table;
int* StreamingArchiveHeapLoader::_root_highest_object_index_table;

bool StreamingArchiveHeapLoader::_waiting_for_iterator;

static jlong _early_materialization_time_ns = 0;
static jlong _late_materialization_time_ns = 0;
static jlong _final_materialization_time_ns = 0;
static jlong _cleanup_materialization_time_ns = 0;
static volatile jlong _accumulated_lazy_materialization_time_ns = 0;

int StreamingArchiveHeapLoader::object_index_for_root_index(int root_index) {
  return _roots_archive[root_index];
}

int StreamingArchiveHeapLoader::highest_object_index_for_root_index(int root_index) {
  return _root_highest_object_index_table[root_index];
}

size_t StreamingArchiveHeapLoader::buffer_offset_for_object_index(int object_index) {
  return _object_index_to_buffer_offset_table[object_index];
}

oopDesc* StreamingArchiveHeapLoader::archive_object_for_object_index(int object_index) {
  size_t buffer_offset = buffer_offset_for_object_index(object_index);
  address bottom = (address)_heap_region->mapped_base();
  return (oopDesc*)(bottom + buffer_offset);
}

size_t StreamingArchiveHeapLoader::buffer_offset_for_archive_object(oopDesc* archive_object) {
  address bottom = (address)_heap_region->mapped_base();
  return size_t(archive_object) - size_t(bottom);
}

BitMap::idx_t StreamingArchiveHeapLoader::obj_bit_idx_for_buffer_offset(size_t buffer_offset) {
  if (UseCompressedOops) {
    return BitMap::idx_t(buffer_offset / sizeof(narrowOop));
  } else {
    return BitMap::idx_t(buffer_offset / sizeof(HeapWord));
  }
}

oop StreamingArchiveHeapLoader::heap_object_for_object_index(int object_index, bool allow_gc) {
  if (allow_gc) {
    oop* handle = (oop*)_object_index_to_heap_object_table[object_index];
    if (handle == nullptr) {
      return nullptr;
    }
    return NativeAccess<>::oop_load(handle);
  } else {
    return cast_to_oop(_object_index_to_heap_object_table[object_index]);
  }
}

oop StreamingArchiveHeapLoader::heap_object_for_object_index(int object_index) {
  assert_lock_strong(CDSHeapLoading_lock);
  return heap_object_for_object_index(object_index, _allow_gc);
}

void StreamingArchiveHeapLoader::set_heap_object_for_object_index(int object_index, oop heap_object, bool allow_gc) {
  assert(heap_object_for_object_index(object_index, allow_gc) == nullptr, "Should only set once with this API");
  if (allow_gc) {
    oop* handle = Universe::vm_global()->allocate();
    NativeAccess<>::oop_store(handle, heap_object);
    _object_index_to_heap_object_table[object_index] = (void*)handle;
  } else {
    _object_index_to_heap_object_table[object_index] = cast_from_oop<void*>(heap_object);
  }
}

void StreamingArchiveHeapLoader::replace_heap_object_for_object_index(int object_index, oop heap_object, bool allow_gc) {
  if (allow_gc) {
    oop* handle = (oop*)_object_index_to_heap_object_table[object_index];
    NativeAccess<>::oop_store(handle, heap_object);
  } else {
    _object_index_to_heap_object_table[object_index] = cast_from_oop<void*>(heap_object);
  }
}

static int archive_array_length(oopDesc* archive_array) {
  return *(int*)(address(archive_array) + arrayOopDesc::length_offset_in_bytes());
}

static size_t archive_object_size(oopDesc* archive_object) {
  Klass* klass = archive_object->klass();
  int lh = klass->layout_helper();

  if (lh > Klass::_lh_neutral_value) {
    // Instance
    if (Klass::layout_helper_needs_slow_path(lh)) {
      return ((size_t*)(archive_object))[-1];
    } else {
      return lh >> LogHeapWordSize;
    }
  } else if (lh < Klass::_lh_neutral_value) {
    // Array
    size_t size_in_bytes;
    size_t array_length = (size_t)archive_array_length(archive_object);
    size_in_bytes = array_length << Klass::layout_helper_log2_element_size(lh);
    size_in_bytes += Klass::layout_helper_header_size(lh);

    return align_up(size_in_bytes, MinObjAlignmentInBytes) / HeapWordSize;
  } else {
    // Other
    return ((size_t*)(archive_object))[-1];
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
  }
}

oop StreamingArchiveHeapLoader::allocate_object(oopDesc* archive_object, size_t size, JavaThread* thread) {
  assert(!archive_object->is_instanceRef(), "no such objects are archived");
  assert(!archive_object->is_stackChunk(), "no such objects are archived");

  oop heap_object;

  if (archive_object->is_instance()) {
    heap_object = Universe::heap()->obj_allocate(archive_object->klass(), size, thread);
  } else if (archive_object->is_typeArray()) {
    int len = archive_array_length(archive_object);
    heap_object = TypeArrayKlass::cast(archive_object->klass())->allocate(len, thread);
  } else {
    assert(archive_object->is_objArray(), "must be");
    int len = archive_array_length(archive_object);
    heap_object = ObjArrayKlass::cast(archive_object->klass())->allocate(len, thread);
  }

  return heap_object;
}

void StreamingArchiveHeapLoader::install_root(int root_index, oop heap_object) {
  objArrayOop roots = objArrayOop((oop)NativeAccess<>::oop_load(_roots_heap));
  OrderAccess::release(); // Once the store below publishes an object, it can be concurrently picked up by another thread without using the lock
  roots->obj_at_put(root_index, heap_object);
}

void StreamingArchiveHeapLoader::TracingObjectLoader::wait_for_iterator() {
  if (JavaThread::current()->is_active_Java_thread()) {
    // When the main thread has bootstrapped past the point of allowing safepoints,
    // we can and indeed have to use safepoint checking waiting.
    CDSHeapLoading_lock->wait();
  } else {
    // If we have no bootstrapped the main thread far enough, then we cannot and
    // indeed also don't need to perform safepoint checking waiting.
    CDSHeapLoading_lock->wait_without_safepoint_check();
  }
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
  void do_oop_work(T* p, int object_index) {
    if (object_index != 0) {
      uintptr_t field_offset = uintptr_t(p) - cast_from_oop<uintptr_t>(_object);
      _dfs_stack.push({object_index, _object_index, field_offset});
    }
  }
};

template <bool COOPS>
void StreamingArchiveHeapLoader::TracingObjectLoader::copy_object(int object_index, oopDesc* archive_object, oop heap_object, size_t size, markWord mark, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread, bool allow_gc) {
  if (!allow_gc) {
    Copy::disjoint_words((HeapWord*)archive_object, cast_from_oop<HeapWord*>(heap_object), size);
    PushReferenceOopClosure cl(dfs_stack, heap_object, object_index);
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

  size_t header_size = COOPS ? 3 : 2;

  size_t buffer_offset = buffer_offset_for_archive_object(archive_object);
  const BitMap::idx_t start_bit = obj_bit_idx_for_buffer_offset(buffer_offset);
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
      RawElementT pointee_object_index = *archive_p;

      dfs_stack.push({(int)pointee_object_index, object_index, (unfinished_bit - start_bit) * sizeof(OopElementT)});

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

oop StreamingArchiveHeapLoader::TracingObjectLoader::materialize_object_inner(int object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread, bool allow_gc) {
  // Allocate object
  oopDesc* archive_object = archive_object_for_object_index(object_index);
  size_t size = archive_object_size(archive_object);
  markWord mark = *archive_object->mark_addr();
  oop heap_object = allocate_object(archive_object, size, thread);

  // Install forwarding
  set_heap_object_for_object_index(object_index, heap_object, allow_gc);

  // Fill in object contents, and recursively materialize
  if (UseCompressedOops) {
    copy_object<true>(object_index, archive_object, heap_object, size, mark, dfs_stack, thread, allow_gc);
  } else {
    copy_object<false>(object_index, archive_object, heap_object, size, mark, dfs_stack, thread, allow_gc);
  }

  return heap_object;
}

oop StreamingArchiveHeapLoader::TracingObjectLoader::materialize_object(int object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread, bool allow_gc) {
  oop heap_object = heap_object_for_object_index(object_index, allow_gc);

  if (object_index <= _previous_batch_last_object_index) {
    // The transitive closure of this object has been materialized; no need to do anything
    return heap_object;
  }

  if (object_index <= _current_batch_last_object_index) {
    // The CDSThread is currently materializing this object and its transitive closure; only need to wait for it to complete
    _waiting_for_iterator = true;
    while (object_index > _previous_batch_last_object_index) {
      wait_for_iterator();
    }
    _waiting_for_iterator = false;
    heap_object = heap_object_for_object_index(object_index, allow_gc);
    return heap_object;
  }

  if (heap_object != nullptr) {
    // Already materialized by mutator
    return heap_object;
  }

  heap_object = materialize_object_inner(object_index, dfs_stack, thread, allow_gc);

  if (java_lang_String::is_instance(heap_object)) {
    size_t buffer_offset = buffer_offset_for_object_index(object_index);
    BitMap::idx_t obj_bit = obj_bit_idx_for_buffer_offset(buffer_offset);
    if (_oopmap.at(obj_bit + 1)) {
      // Interned string... finish materializing and link it to the string table
      int value_object_index = object_index + 1;
      oop value_heap_object = materialize_object(value_object_index, dfs_stack, thread, allow_gc);

      heap_object = heap_object_for_object_index(object_index, allow_gc);
      if (allow_gc) {
        heap_object->obj_field_put(java_lang_String::value_offset(), value_heap_object);
      } else {
        // Allocated objects are not properly initialized when GC isn't allowed
        heap_object->obj_field_put_access<IS_DEST_UNINITIALIZED>(java_lang_String::value_offset(), value_heap_object);
      }

      // Replace string with interned string
      heap_object = StringTable::cds_intern(thread, heap_object);
      replace_heap_object_for_object_index(object_index, heap_object, allow_gc);
    }
  }

  return heap_object;
}

void StreamingArchiveHeapLoader::TracingObjectLoader::drain_stack(Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread, bool allow_gc) {
  while (!dfs_stack.is_empty()) {
    CDSHeapTraversalEntry entry = dfs_stack.pop();
    int pointee_object_index = entry._pointee_object_index;
    oop pointee_heap_object = materialize_object(pointee_object_index, dfs_stack, thread, allow_gc);
    oop heap_object = heap_object_for_object_index(entry._base_object_index, allow_gc);
    heap_object->obj_field_put_access<IS_DEST_UNINITIALIZED>((int)entry._heap_field_offset_bytes, pointee_heap_object);
  }
}

oop StreamingArchiveHeapLoader::TracingObjectLoader::materialize_object_transitive(int object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  assert_locked_or_safepoint(CDSHeapLoading_lock);
  while (_waiting_for_iterator) {
    wait_for_iterator();
  }

  Handle result(thread, materialize_object(object_index, dfs_stack, thread, _allow_gc));
  drain_stack(dfs_stack, thread, _allow_gc);

  return result();
}

oop StreamingArchiveHeapLoader::TracingObjectLoader::materialize_root(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  int root_object_index = object_index_for_root_index(root_index);

  return materialize_object_transitive(root_object_index, dfs_stack, thread);
}

oop StreamingArchiveHeapLoader::TracingObjectLoader::root(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  // Get the materialized roots array
  oop roots_obj = NativeAccess<>::oop_load(_roots_heap);
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

class InflateReferenceOopClosure : public BasicOopIterateClosure {
  bool _allow_gc;

public:
  InflateReferenceOopClosure(bool allow_gc) : _allow_gc(allow_gc) {}
  virtual void do_oop(oop* p) { do_oop_work(p, (int)*(intptr_t*)p); }
  virtual void do_oop(narrowOop* p) { do_oop_work(p, *(int*)p); }

  template <typename T>
  void do_oop_work(T* p, int object_index) {
    if (object_index != 0) {
      oop obj = StreamingArchiveHeapLoader::heap_object_for_object_index(object_index, _allow_gc);
      HeapAccess<IS_DEST_UNINITIALIZED>::oop_store(p, obj);
    }
  }
};

void StreamingArchiveHeapLoader::IterativeObjectLoader::copy_object(oopDesc* archive_object, oop heap_object, size_t size, bool allow_gc) {
  Copy::disjoint_words((HeapWord*)archive_object, cast_from_oop<HeapWord*>(heap_object), size);
  InflateReferenceOopClosure cl(allow_gc);
  heap_object->oop_iterate(&cl);
  patch_metadata(heap_object);
  intptr_t archive_hash = archive_object->mark().hash();
  if (archive_hash != 0) {
    heap_object->set_mark(heap_object->mark().copy_set_hash(archive_hash));
  }
}

// The range is inclusive
void StreamingArchiveHeapLoader::IterativeObjectLoader::initialize_range(int first_object_index, int last_object_index, JavaThread* thread, bool allow_gc) {
  bool last_object_was_interned_string = false;

  for (int i = first_object_index; i <= last_object_index; ++i) {
    oopDesc* archive_object = archive_object_for_object_index(i);
    size_t size = archive_object_size(archive_object);
    oop heap_object = heap_object_for_object_index(i, allow_gc);
    copy_object(archive_object, heap_object, size, allow_gc);

    // Link interned strings if necessary
    if (last_object_was_interned_string) {
      int string_object_index = i - 1;
      oop string_object = heap_object_for_object_index(string_object_index, allow_gc);
      replace_heap_object_for_object_index(string_object_index, StringTable::cds_intern(thread, string_object), allow_gc);
      last_object_was_interned_string = false;
    } else if (java_lang_String::is_instance(heap_object)) {
      size_t buffer_offset = buffer_offset_for_archive_object(archive_object);
      BitMap::idx_t obj_bit = obj_bit_idx_for_buffer_offset(buffer_offset);
      if (_oopmap.at(obj_bit + 1)) {
        last_object_was_interned_string = true;
      }
    }
  }
}

// The range is inclusive
size_t StreamingArchiveHeapLoader::IterativeObjectLoader::materialize_range(int first_object_index, int last_object_index, JavaThread* thread, bool allow_gc) {
  GrowableArrayCHeap<int, mtClassShared>* lazy_object_indices = nullptr;
  size_t materialized_words = 0;

  for (int i = first_object_index; i <= last_object_index; ++i) {
    oopDesc* archive_object = archive_object_for_object_index(i);
    size_t size = archive_object_size(archive_object);
    materialized_words += size;
    oop heap_object = heap_object_for_object_index(i, allow_gc);
    if (heap_object == nullptr) {
      // The normal case; no lazy loading have loaded the object yet
      heap_object = allocate_object(archive_object, size, thread);
      set_heap_object_for_object_index(i, heap_object, allow_gc);
    } else {
      // Lazy loading has already initialized the object; we must not mutate it
      if (lazy_object_indices == nullptr) {
        lazy_object_indices = new GrowableArrayCHeap<int, mtClassShared>();
      }
      lazy_object_indices->append(i);
    }
  }

  if (lazy_object_indices == nullptr) {
    // Normal case; no sprinkled lazy objects in the root subgraph
    initialize_range(first_object_index, last_object_index, thread, allow_gc);
  } else {
    // The user lazy initialized some objects that are already initialized; we have to initialize around them
    // to make sure they are not mutated.
    int previous_object_index = first_object_index - 1; // Exclusive start of initialization slice
    for (int i = 0; i < lazy_object_indices->length(); ++i) {
      int lazy_object_index = lazy_object_indices->at(i);
      int slice_start_object_index = previous_object_index;
      int slice_end_object_index = lazy_object_index;

      if (slice_end_object_index - slice_start_object_index > 1) { // Both markers are exclusive
        initialize_range(slice_start_object_index + 1, slice_end_object_index - 1, thread, allow_gc);
      }
      oop heap_object = heap_object_for_object_index(lazy_object_index, allow_gc);
      previous_object_index = lazy_object_index;
    }
    // Process tail range
    if (last_object_index - previous_object_index > 0) {
      initialize_range(previous_object_index + 1, last_object_index, thread, allow_gc);
    }
    delete lazy_object_indices;
  }

  return materialized_words;
}

void StreamingArchiveHeapLoader::IterativeObjectLoader::materialize_next_root(JavaThread* thread) {
  int current_root_index = _current_root_index;
  int highest_object_index = highest_object_index_for_root_index(current_root_index);
  int root_object_index = object_index_for_root_index(current_root_index);

  oop root = nullptr;
  bool allow_gc = _allow_gc;

  // Materialize objects of necessary, representing the transitive closure of the root
  if (highest_object_index > _previous_batch_last_object_index) {
    int first_object_index = _previous_batch_last_object_index + 1;
    _current_batch_last_object_index = highest_object_index;
    size_t allocated_words;
    {
      MutexUnlocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
      allocated_words = materialize_range(first_object_index, highest_object_index, thread, allow_gc);
    }
    _allocated_words += allocated_words;
    _previous_batch_last_object_index = _current_batch_last_object_index;
    if (_waiting_for_iterator) {
      CDSHeapLoading_lock->notify_all();
    }
  }

  root = heap_object_for_object_index(root_object_index, allow_gc);

  // Install the root
  install_root(current_root_index, root);
}

bool StreamingArchiveHeapLoader::materialize_early() {
  jlong start = os::javaTimeNanos();
  JavaThread* thread = JavaThread::current();
  int roots_length = compute_roots_length();

  size_t bootstrap_max_memory = Universe::heap()->bootstrap_max_memory();
  size_t bootstrap_min_memory = 2 * M;

  size_t before_gc_materialize_budget_bytes = (bootstrap_max_memory > bootstrap_min_memory) ? bootstrap_max_memory - bootstrap_min_memory : 0;
  size_t before_gc_materialize_budget_words = before_gc_materialize_budget_bytes / HeapWordSize;

  log_info(cds, heap)("Max bootstrapping memory: " SIZE_FORMAT "M, min bootstrapping memory: " SIZE_FORMAT "M, selected budget: " SIZE_FORMAT "M",
                      bootstrap_max_memory / M, bootstrap_min_memory / M, before_gc_materialize_budget_bytes / M);

  for (_current_root_index = 0; _current_root_index < roots_length; ++_current_root_index) {
    if (_stop_background_processing || _allow_gc || _allocated_words > before_gc_materialize_budget_words) {
      log_info(cds, heap)("Early object materialization interrupted at root %d", _current_root_index);
      break;
    }

    IterativeObjectLoader::materialize_next_root(thread);
  }

  _early_materialization_time_ns = os::javaTimeNanos() - start;

  bool finished_before_gc_allowed = !_allow_gc && _current_root_index == roots_length;

  return finished_before_gc_allowed;
}

void StreamingArchiveHeapLoader::materialize_late() {
  if (await_gc_enabled()) {
    // Materialization is signalled to stop
    return;
  }

  jlong start = os::javaTimeNanos();

  // Continue materializing with GC allowed
  JavaThread* thread = JavaThread::current();
  int roots_length = compute_roots_length();

  for (; _current_root_index < roots_length; ++_current_root_index) {
    if (_stop_background_processing) {
      log_info(cds, heap)("Late object materialization interrupted at root %d", _current_root_index);
      break;
    }

    IterativeObjectLoader::materialize_next_root(thread);
  }

  _late_materialization_time_ns = os::javaTimeNanos() - start;
}

void StreamingArchiveHeapLoader::cleanup(bool finished_before_gc_allowed) {
  JavaThread* thread = JavaThread::current();

  jlong start = os::javaTimeNanos();

  // Remove OopStorage roots
  if (!finished_before_gc_allowed) {
    size_t num_handles = _num_archived_objects;
    // Skip the null entry
    oop** handles = ((oop**)_object_index_to_heap_object_table) + 1;
    qsort(handles, num_handles, sizeof(oop*), (int (*)(const void*, const void*))oop_handle_cmp);
    for (size_t i = 0; i < num_handles; ++i) {
      oop* handle = handles[i];
      NativeAccess<>::oop_store(handle, nullptr);
    }
    Universe::vm_global()->release(handles, num_handles);
  }

  FREE_C_HEAP_ARRAY(void*, _object_index_to_heap_object_table);

  // Unmap regions
  FileMapInfo::current_info()->unmap_region(MetaspaceShared::hp);
  FileMapInfo::current_info()->unmap_region(MetaspaceShared::bm);

  _cleanup_materialization_time_ns = os::javaTimeNanos() - start;
}

void StreamingArchiveHeapLoader::log_telemetry() {
  log_info(cds,heap)("Early object materialization time (concurrent): " SIZE_FORMAT "us",
                     _early_materialization_time_ns / 1000);
  log_info(cds, heap)("Late object materialization time (concurrent): " SIZE_FORMAT "us",
                      _late_materialization_time_ns / 1000);
  log_info(cds, heap)("Object materialization cleanup time (concurrent): " SIZE_FORMAT "us",
                      _cleanup_materialization_time_ns / 1000);
  log_info(cds, heap)("Final object materialization time (synchronous): " SIZE_FORMAT "us",
                      _final_materialization_time_ns / 1000);
  log_info(cds, heap)("Bootstrapping lazy materialization time (synchronous): " SIZE_FORMAT "us",
                      _accumulated_lazy_materialization_time_ns / 1000);
  log_info(cds, heap)("Synchronous materialization time: " SIZE_FORMAT "us",
                      (_final_materialization_time_ns + _accumulated_lazy_materialization_time_ns) / 1000);
  log_info(cds, heap)("Concurrent materialization time: " SIZE_FORMAT "us",
                      (_early_materialization_time_ns + _late_materialization_time_ns + _cleanup_materialization_time_ns) / 1000);
}

void StreamingArchiveHeapLoader::materialize_objects() {
  JavaThread* thread = JavaThread::current();
  // Objects are laid out in DFS order; DFS traverse the roots by linearly walking all objects
  HandleMark hm(thread);
  // Early materialization with a budget before GC is allowed
  MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
  bool finished_before_gc_allowed = materialize_early();
  materialize_late();
  await_finished_processing();
  cleanup(finished_before_gc_allowed);
  log_telemetry();
}

void StreamingArchiveHeapLoader::switch_object_index_to_handle(int object_index) {
  oop heap_object = cast_to_oop(_object_index_to_heap_object_table[object_index]);
  if (heap_object == nullptr) {
    return;
  }

  oop* handle = Universe::vm_global()->allocate();
  NativeAccess<>::oop_store(handle, heap_object);
  _object_index_to_heap_object_table[object_index] = handle;
}

void StreamingArchiveHeapLoader::enable_gc() {
  // Need java.lang.Thread loaded to create the Thread object of the CDS thread
  // which starts way earlier due to its startup helping nature
  CDSThread::materialize_thread_object();

  MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
  // First wait until no tracing is active
  while (_waiting_for_iterator) {
    CDSHeapLoading_lock->wait();
  }

  _allow_gc = true;

  if (_current_root_index != compute_roots_length()) {
    // Inflate oop handles and continue materializing objects in a less efficient mode
    int num_handles = _num_archived_objects;
    int current_end = _current_batch_last_object_index;
    int last_end = _previous_batch_last_object_index;
    for (int i = current_end + 1; i <= num_handles; ++i) {
      // First upgrade handles in front of the iterative materialization
      switch_object_index_to_handle(i);
    }

    // The CDSThread is currently materializing this object and its transitive closure; only need to wait for it to complete
    _waiting_for_iterator = true;
    while (_previous_batch_last_object_index != _current_batch_last_object_index) {
      CDSHeapLoading_lock->wait();
    }
    for (int i = 1; i <= current_end; ++i) {
      switch_object_index_to_handle(i);
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

  jlong start = os::javaTimeNanos();

  MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
  _stop_background_processing = true;
  // Wait for the CDS thread to stop iterating
  while (_previous_batch_last_object_index != _current_batch_last_object_index) {
    CDSHeapLoading_lock->wait();
  }

  for (; _current_root_index < length; ++_current_root_index) {
    IterativeObjectLoader::materialize_next_root(thread);
  }

  // Notify CDS thread we are done
  _finished_processing = true;
  CDSHeapLoading_lock->notify_all();

  _final_materialization_time_ns = os::javaTimeNanos() - start;
}

void account_lazy_materialization_time_ns(jlong time, const char* description, int index) {
  Atomic::add(&_accumulated_lazy_materialization_time_ns, time);
  log_debug(cds, heap)("Lazy materialization of %s: %d end (%ld us of %ld us)", description, index, time / 1000, _accumulated_lazy_materialization_time_ns / 1000);
}

// Initialize an empty array of CDS heap roots; materialize them lazily
void StreamingArchiveHeapLoader::initialize() {
  JavaThread* thread = JavaThread::current();

  FileMapInfo::current_info()->map_bitmap_region();

  _heap_region = FileMapInfo::current_info()->region_at(MetaspaceShared::hp);
  _bitmap_region = FileMapInfo::current_info()->region_at(MetaspaceShared::bm);

  if (_heap_region->used() == 0) {
    // No objects to load
    return;
  }

  _is_loaded = true;

  // archived roots are at this offset in the stream.
  size_t heap_roots_offset = FileMapInfo::current_info()->heap_roots_offset();
  size_t forwarding_offset = FileMapInfo::current_info()->forwarding_offset();
  size_t root_highest_object_index_table_offset = FileMapInfo::current_info()->root_highest_object_index_table_offset();
  _num_archived_objects = FileMapInfo::current_info()->num_archived_objects();

  // The first int is the length of the array
  _roots_archive = ((int*)(((address)_heap_region->mapped_base()) + heap_roots_offset)) + 1;
  int length = compute_roots_length();

  objArrayOop roots = ObjArrayKlass::cast(Universe::objectArrayKlassObj())->allocate(length, thread);
  if (roots == nullptr) {
    fatal("Not enough memory available to initialize JVM");
  }
  _roots_heap = Universe::vm_global()->allocate();
  NativeAccess<>::oop_store(_roots_heap, roots);

  _object_index_to_buffer_offset_table = (size_t*)(((address)_heap_region->mapped_base()) + forwarding_offset);
  // We allocate the first entry for "null"
  _object_index_to_heap_object_table = NEW_C_HEAP_ARRAY(void*, _num_archived_objects + 1, mtClassShared);
  Copy::zero_to_bytes(_object_index_to_heap_object_table, (_num_archived_objects + 1) * sizeof(void*));

  _root_highest_object_index_table = (int*)(((address)_heap_region->mapped_base()) + root_highest_object_index_table_offset);

  address start = (address)(_bitmap_region->mapped_base()) + _heap_region->oopmap_offset();
  _oopmap = BitMapView((BitMap::bm_word_t*)start, _heap_region->oopmap_size_in_bits());

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

    if (_current_root_index == compute_roots_length()) {
      objArrayOop roots = objArrayOop((oop)NativeAccess<>::oop_load(_roots_heap));
      result = roots->obj_at(root_index);
    } else {
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

  return _roots_archive[-1];
}

bool StreamingArchiveHeapLoader::await_gc_enabled() {
  // Signal that we have stopped working
  CDSHeapLoading_lock->notify_all();
  while (!_allow_gc || _waiting_for_iterator) {
    CDSHeapLoading_lock->wait();
  }

  return _stop_background_processing;
}

void StreamingArchiveHeapLoader::await_finished_processing() {
  // Signal that we have stopped working
  CDSHeapLoading_lock->notify_all();

  // Wait for bootstrapping thread to finish any remaining work
  while (!_finished_processing) {
    CDSHeapLoading_lock->wait();
  }
}

#endif // INCLUDE_CDS_JAVA_HEAP
