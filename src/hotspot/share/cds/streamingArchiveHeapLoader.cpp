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
oop** StreamingArchiveHeapLoader::_handles;
size_t StreamingArchiveHeapLoader::_num_handles;

size_t* StreamingArchiveHeapLoader::_dfs_to_archive_offset_table;
void** StreamingArchiveHeapLoader::_dfs_to_heap_object_table;
int* StreamingArchiveHeapLoader::_roots_highest_dfs;

void StreamingArchiveHeapLoader::add_handle(oop* handle) {
  assert(_num_handles <= FileMapInfo::current_info()->num_archived_objects(),
         "Too many handles: " SIZE_FORMAT " vs " SIZE_FORMAT,
         _num_handles, FileMapInfo::current_info()->num_archived_objects());
  _handles[_num_handles++] = handle;
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
  size_t num_archived_objects = FileMapInfo::current_info()->num_archived_objects();

  // The materialized address of the HeapShared::roots()
  _roots_old_addr = ((address)_heap_region->mapped_base()) + heap_roots_offset;
  typeArrayOopDesc* roots_old = (typeArrayOopDesc *)_roots_old_addr;
  int length = roots_old->length();

  objArrayOop roots = ObjArrayKlass::cast(Universe::objectArrayKlassObj())->allocate(length, thread);
  if (roots == nullptr) {
    fatal("Not enough memory available to initialize JVM");
  }
  _handles = NEW_C_HEAP_ARRAY(oop*, num_archived_objects, mtClassShared);
  _roots = Universe::vm_global()->allocate();
  NativeAccess<>::oop_store(_roots, roots);
  add_handle(_roots);

  _dfs_to_archive_offset_table = (size_t*)(((address)_heap_region->mapped_base()) + forwarding_offset);
  _dfs_to_heap_object_table = NEW_C_HEAP_ARRAY(void*, num_archived_objects + 1, mtClassShared);
  Copy::zero_to_bytes(_dfs_to_heap_object_table, (num_archived_objects + 1) * sizeof(void*));

  _roots_highest_dfs = (int*)(((address)_heap_region->mapped_base()) + roots_highest_dfs_offset);

  address start = (address)(_bitmap_region->mapped_base()) + _heap_region->oopmap_offset();
  _oopmap = BitMapView((BitMap::bm_word_t*)start, _heap_region->oopmap_size_in_bits());

  _is_loaded = true;
  HeapShared::init_roots(roots);

  CDSThread::initialize();
}

template <bool allow_gc> void* StreamingArchiveHeapLoader::allocate_object_dfs(oopDesc* archive_object, size_t size, JavaThread* thread) {
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

  oop* handle = Universe::vm_global()->allocate();
  add_handle(handle);
  NativeAccess<>::oop_store(handle, heap_object);

  return create_raw_handle<allow_gc>(handle, heap_object);
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

template <bool allow_gc> void* StreamingArchiveHeapLoader::create_raw_handle(oop* handle, oop obj) {
  if (allow_gc) {
    return (void*)handle;
  } else {
    return cast_from_oop<void*>(obj);
  }
}

template <bool allow_gc> oop StreamingArchiveHeapLoader::resolve_raw_handle(void* raw_handle) {
  if (allow_gc) {
    oop* handle = (oop*)raw_handle;
    return NativeAccess<>::oop_load(handle);
  } else {
    return cast_to_oop(raw_handle);
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
  void do_oop_work(T* p, int dfs_index) {
    if (dfs_index != 0) {
      uintptr_t field_offset = uintptr_t(p) - cast_from_oop<uintptr_t>(_object);
      _dfs_stack.push({dfs_index, _object_index, field_offset});
    }
  }
};

template <bool COOPS, bool allow_gc>
void StreamingArchiveHeapLoader::copy_object_dfs(oopDesc* archive_object, int archive_object_index, void* heap_object_raw_handle, size_t size, markWord mark, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  if (!allow_gc) {
    // TODO: Custom allocator
    oop heap_object = resolve_raw_handle<allow_gc>(heap_object_raw_handle);
    Copy::disjoint_words((HeapWord*)archive_object, (HeapWord*)heap_object, size);
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
      oop heap_object = resolve_raw_handle<allow_gc>(heap_object_raw_handle);;
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

  oop heap_object = resolve_raw_handle<allow_gc>(heap_object_raw_handle);

  if (!COOPS && oopDesc::has_klass_gap()) {
    oopDesc::set_klass_gap(cast_from_oop<HeapWord*>(heap_object), *(int*)(address(archive_object) + oopDesc::klass_gap_offset_in_bytes()));
  }

  patch_metadata(heap_object);

  intptr_t archive_hash = mark.hash();
  if (archive_hash != 0) {
    heap_object->set_mark(heap_object->mark().copy_set_hash(archive_hash));
  }
}

template <bool COOPS, bool allow_gc>
oop StreamingArchiveHeapLoader::materialize_object_inner_dfs(oopDesc* archive_object, int archive_object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  // Allocate object
  size_t size = archive_object->size();
  markWord mark = *archive_object->mark_addr();
  void* heap_object_raw_handle = allocate_object_dfs<allow_gc>(archive_object, size, thread);

  // Install forwarding
  _dfs_to_heap_object_table[archive_object_index] = heap_object_raw_handle;

  // Fill in object contents, and recursively materialize
  copy_object_dfs<COOPS, allow_gc>(archive_object, archive_object_index, heap_object_raw_handle, size, mark, dfs_stack, thread);

  return resolve_raw_handle<allow_gc>(heap_object_raw_handle);
}

template <bool COOPS, bool allow_gc>
oop StreamingArchiveHeapLoader::materialize_object_dfs(oopDesc* archive_object, int archive_object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  oop heap_object = cast_to_oop(_dfs_to_heap_object_table[archive_object_index]);

  if (archive_object_index <= _last_batch_last_object) {
    // The transitive closure of this object has been materialized; no need to do anything
    return heap_object;
  }

  if (archive_object_index <= _current_batch_last_object) {
    // The CDSThread is currently materializing this object and its transitive closure; only need to wait for it to complete
    while (heap_object == nullptr) {
      // TODO: What happens if another root is lazy expanded while we wait?
      CDSHeapLoading_lock->wait();
      heap_object = cast_to_oop(_dfs_to_heap_object_table[archive_object_index]);
    }
    return heap_object;
  }

  if (heap_object != nullptr) {
    // Already materialized by mutator
    return heap_object;
  }

  heap_object = materialize_object_inner_dfs<COOPS, allow_gc>(archive_object, archive_object_index, dfs_stack, thread);

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
      oop value_heap_object = materialize_object_dfs<COOPS, allow_gc>(value_archive_object, value_dfs_index, dfs_stack, thread);

      heap_object = cast_to_oop(_dfs_to_heap_object_table[archive_object_index]);
      heap_object->obj_field_put(java_lang_String::value_offset(), value_heap_object);

      // Replace string with interned string
      heap_object = StringTable::intern(heap_object, thread);
      _dfs_to_heap_object_table[archive_object_index] = heap_object;
    }
  }

  return heap_object;
}

template <bool COOPS, bool allow_gc>
void StreamingArchiveHeapLoader::drain_dfs_stack(Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  address bottom = (address)_heap_region->mapped_base();
  while (!dfs_stack.is_empty()) {
    CDSHeapTraversalEntry entry = dfs_stack.pop();
    int pointee_index = entry._pointee_index;
    oopDesc* pointee_archive_object = (oopDesc*)(bottom + _dfs_to_archive_offset_table[pointee_index]);
    oop pointee_heap_object = materialize_object_dfs<COOPS, allow_gc>(pointee_archive_object, pointee_index, dfs_stack, thread);
    oop heap_object = cast_to_oop(_dfs_to_heap_object_table[entry._base_index]);
    heap_object->obj_field_put_access<IS_DEST_UNINITIALIZED>((int)entry._heap_field_offset_bytes, pointee_heap_object);
  }
}

oop StreamingArchiveHeapLoader::materialize_object_transitive_dfs(oopDesc* archive_object, int object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  assert_locked_or_safepoint(CDSHeapLoading_lock);

  objArrayOopDesc* roots_old = (objArrayOopDesc *)_roots_old_addr;

  Handle result;

  if (UseCompressedOops) {
    if (archive_object == roots_old) {
      return nullptr;
    }
    if (_allow_gc) {
      result = Handle(thread, materialize_object_dfs<true, true>(archive_object, object_index, dfs_stack, thread));
      drain_dfs_stack<true, true>(dfs_stack, thread);
    } else {
      result = Handle(thread, materialize_object_dfs<true, false>(archive_object, object_index, dfs_stack, thread));
      drain_dfs_stack<true, false>(dfs_stack, thread);
    }
  } else {
    if (archive_object == roots_old) {
      return nullptr;
    }
    if (_allow_gc) {
      result = Handle(thread, materialize_object_dfs<false, true>(archive_object, object_index, dfs_stack, thread));
      drain_dfs_stack<false, true>(dfs_stack, thread);
    } else {
      result = Handle(thread, materialize_object_dfs<false, false>(archive_object, object_index, dfs_stack, thread));
      drain_dfs_stack<false, false>(dfs_stack, thread);
    }
  }

  return result();
}

oop StreamingArchiveHeapLoader::materialize_root_dfs(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  address bottom = (address)_heap_region->mapped_base();
  typeArrayOopDesc* roots_old = (typeArrayOopDesc *)_roots_old_addr;
  int root_dfs_index = roots_old->int_at(root_index);
  size_t root_buffer_offset = _dfs_to_archive_offset_table[root_dfs_index];
  oopDesc* root_obj = (oopDesc*)(bottom + root_buffer_offset);

  return materialize_object_transitive_dfs(root_obj, root_dfs_index, dfs_stack, thread);
}

oop StreamingArchiveHeapLoader::root_dfs(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  // Get the materialized roots array
  oop roots_obj = NativeAccess<>::oop_load(_roots);
  objArrayOop roots = (objArrayOop)roots_obj;

  // Check if we got the corresponding root
  oop root = roots->obj_at(root_index);

  if (root == nullptr) {
    // If not, materialize the root
    root = materialize_root_dfs(root_index, dfs_stack, thread);
    if (root != nullptr) {
      roots->replace_if_null(root_index, root);
    } else {
      roots->replace_if_null(root_index, roots);
    }
  }

  return root;
}

static volatile jlong _lazy_time = 0;

oop StreamingArchiveHeapLoader::root(int root_index) {
  jlong start = os::javaTimeNanos();
  JavaThread* thread = JavaThread::current();
  Stack<CDSHeapTraversalEntry, mtClassShared> dfs_stack;
  HandleMark hm(thread);

  typeArrayOopDesc* roots_old = (typeArrayOopDesc *)_roots_old_addr;
  int root_dfs_index = roots_old->int_at(root_index);

  oop result;
  {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    result = root_dfs(root_index, dfs_stack, thread);
  }

  jlong elapsed = os::javaTimeNanos() - start;
  Atomic::add(&_lazy_time, elapsed);
  log_info(cds, heap)("Lazy materialization of root: %d end (%ld of %ld)", root_index, elapsed / 1000000, _lazy_time / 1000000);

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
  for (;;) {
    if (_allow_gc) {
      break;
    }
    CDSHeapLoading_lock->wait();
  }

  return _stop_background_processing;
}

void StreamingArchiveHeapLoader::await_finished_processing() {
  for (;;) {
    if (_finished_processing) {
      break;
    }
    CDSHeapLoading_lock->wait();
  }
}

int oop_handle_cmp(const void* left, const void* right) {
  oop* left_handle = *(oop**)left;
  oop* right_handle = *(oop**)right;
  return uintptr_t(right_handle) - uintptr_t(left_handle);
}

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
      oop obj = cast_to_oop(Atomic::load(&_dfs_to_heap_object_table[dfs_index]));
      HeapAccess<IS_DEST_UNINITIALIZED>::oop_store(p, obj);
    }
  }
};

template <bool COOPS>
void StreamingArchiveHeapLoader::copy_object_iter(oopDesc* archive_object, oop heap_object, size_t size) {
  Copy::disjoint_words((HeapWord*)archive_object, (HeapWord*)heap_object, size);
  InflateReferenceOopClosure cl(_dfs_to_heap_object_table);
  heap_object->oop_iterate(&cl);
  patch_metadata(heap_object);
  intptr_t archive_hash = archive_object->mark().hash();
  if (archive_hash != 0) {
    heap_object->set_mark(heap_object->mark().copy_set_hash(archive_hash));
  }
}

oop StreamingArchiveHeapLoader::allocate_object_iter(oopDesc* archive_object, int dfs_index, size_t size, JavaThread* thread) {
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

template <bool COOPS>
void StreamingArchiveHeapLoader::initialize_object_iter(oopDesc* archive_object, oop heap_object, int dfs_index, size_t size, JavaThread* thread) {
  // Fill in object contents, and recursively materialize
  copy_object_iter<COOPS>(archive_object, heap_object, size);

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
      oop value_heap_object = cast_to_oop(Atomic::load(&_dfs_to_heap_object_table[value_dfs_index]));
      oopDesc* value_archive_object = (oopDesc*)(((HeapWord*)archive_object) + size);
      // TODO: avoid double initialization of string values
      initialize_object_iter<COOPS>(value_archive_object, value_heap_object, value_dfs_index, value_archive_object->size(), thread);
      _dfs_to_heap_object_table[dfs_index] = StringTable::intern(heap_object, thread);
    }
  }
}

size_t StreamingArchiveHeapLoader::initialize_range(int first_dfs, size_t first_offset, int last_dfs, JavaThread* thread) {
  HeapWord* bottom = ((HeapWord*)_heap_region->mapped_base()) + first_offset;
  size_t initialized_words = 0;

  for (int i = first_dfs; i <= last_dfs; ++i) {
    oopDesc* archive_object = (oopDesc*)(((HeapWord*)bottom) + initialized_words);
    size_t size = archive_object->size();
    oop heap_object = cast_to_oop(Atomic::load(&_dfs_to_heap_object_table[i]));
    if (UseCompressedOops) {
      initialize_object_iter<true>(archive_object, heap_object, i, size, thread);
    } else {
      initialize_object_iter<false>(archive_object, heap_object, i, size, thread);
    }
    initialized_words += size;
  }

  return initialized_words;
}

size_t StreamingArchiveHeapLoader::materialize_range(int first_dfs, size_t first_offset, int last_dfs, JavaThread* thread) {
  HeapWord* bottom = ((HeapWord*)_heap_region->mapped_base()) + first_offset;

  size_t allocated_words = 0;

  GrowableArrayCHeap<int, mtClassShared>* lazy_object_indices = nullptr;

  for (int i = first_dfs; i <= last_dfs; ++i) {
    oopDesc* archive_object = (oopDesc*)(((HeapWord*)bottom) + allocated_words);
    size_t size = archive_object->size();
    oop heap_object = cast_to_oop(Atomic::load(&_dfs_to_heap_object_table[i]));
    if (heap_object == nullptr) {
      // The normal case; no lazy loading have loaded the object yet
      heap_object = allocate_object_iter(archive_object, i, size, thread);
      Atomic::store(&_dfs_to_heap_object_table[i], cast_from_oop<void*>(heap_object));
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
    initialized_words = initialize_range(first_dfs, first_offset, last_dfs, thread);
  } else {
    // The user lazy initialized some objects that are already initialized; we have to initialize around them
    // to make sure they are not mutated.
    int previous_index = first_dfs - 1; // Exclusive start of initialization slice
    for (int i = 0; i < lazy_object_indices->length(); ++i) {
      int lazy_object_index = lazy_object_indices->at(i);
      int slice_start = previous_index;
      int slice_end = lazy_object_index;

      if (slice_end - slice_start > 1) { // Both markers are exclusive
        initialized_words += initialize_range(slice_start + 1, first_offset + initialized_words, slice_end - 1, thread);
      }
      oop heap_object = cast_to_oop(Atomic::load(&_dfs_to_heap_object_table[lazy_object_index]));
      initialized_words += heap_object->size();
      previous_index = lazy_object_index;
    }
    // Process tail range
    if (last_dfs - previous_index > 0) {
      initialized_words += initialize_range(previous_index + 1, first_offset + initialized_words, last_dfs, thread);
    }
    delete lazy_object_indices;
  }

  return initialized_words;
}

void StreamingArchiveHeapLoader::install_root(int root_index) {
  typeArrayOopDesc* roots_old = (typeArrayOopDesc *)_roots_old_addr;
  int root_dfs_index = roots_old->int_at(root_index);
  oop obj = cast_to_oop(_dfs_to_heap_object_table[root_dfs_index]);
  objArrayOop roots = objArrayOop((oop)NativeAccess<>::oop_load(_roots));
  roots->obj_at_put(root_index, obj);
}

void StreamingArchiveHeapLoader::materialize_root_iter(JavaThread* thread) {
  int last_dfs_index = _roots_highest_dfs[_current_root];

  // Materialize objects of necessary, representing the transitive closure of the root
  if (last_dfs_index > _last_batch_last_object) {
    int first_dfs_index = _last_batch_last_object + 1;
    _current_batch_last_object = last_dfs_index;
    {
      CDSHeapLoading_lock->notify_all();
      MutexUnlocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
      _current_buffer_offset += materialize_range(first_dfs_index, _current_buffer_offset, last_dfs_index, thread);
    }
    _last_batch_last_object = _current_batch_last_object;
  }

  // Install the root
  install_root(_current_root);
}

void StreamingArchiveHeapLoader::materialize_objects() {
  // Get the materialized roots array
  JavaThread* thread = JavaThread::current();

  size_t num_archived_objects = FileMapInfo::current_info()->num_archived_objects();
  size_t before_gc_materialize_budget_bytes = 10 * M;
  size_t before_gc_materialize_budget_words = before_gc_materialize_budget_bytes / HeapWordSize;

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

    materialize_root_iter(thread);
  }

  jlong end = os::javaTimeNanos();

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

      materialize_root_iter(thread);
    }

    log_info(cds, heap)("Concurrent object materialization end");
  }

  await_finished_processing();

  log_info(cds, heap)("Concurrent object materialization cleanup start");
  // Remove OopStorage roots
  qsort(_handles, _num_handles, sizeof(oop*), (int (*)(const void*, const void*))oop_handle_cmp);
  for (size_t i = 0; i < _num_handles; ++i) {
    oop* handle = _handles[i];
    NativeAccess<>::oop_store(handle, nullptr);
  }
  Universe::vm_global()->release(_handles, _num_handles);
  FREE_C_HEAP_ARRAY(oop*, _handles);
  FREE_C_HEAP_ARRAY(void*, _dfs_to_heap_object_table);

  // Unmap regions
  FileMapInfo::current_info()->unmap_region(MetaspaceShared::hp);
  FileMapInfo::current_info()->unmap_region(MetaspaceShared::bm);

  log_info(cds, heap)("Concurrent object materialization cleanup end");
}

void StreamingArchiveHeapLoader::enable_gc() {
  log_info(cds, heap)("Enable GC");
  CDSThread::materialize_thread_object();
  MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
  _allow_gc = true;
  CDSHeapLoading_lock->notify_all();
}

void StreamingArchiveHeapLoader::finish_materialize_objects() {
  // Get the materialized roots array
  JavaThread* thread = JavaThread::current();
  HandleMark hm(thread);
  int length = compute_roots_length();

  log_info(cds, heap)("Synchronous object materialization start");

  {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    _stop_background_processing = true;
  }

  for (; _current_root < length; ++_current_root) {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    materialize_root_iter(thread);
  }

  {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    _finished_processing = true;
    CDSHeapLoading_lock->notify_all();
  }

  log_info(cds, heap)("Synchronous object materialization end");
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
      result = cast_to_oop(_dfs_to_heap_object_table[permanent_index]);
    } else {
      result = materialize_object_transitive_dfs(archive_object, permanent_index, dfs_stack, thread);
    }
  }

  jlong elapsed = os::javaTimeNanos() - start;
  Atomic::add(&_lazy_time, elapsed);
  log_info(cds, heap)("Lazy materialization of permanent object: %d end (%ld of %ld)", permanent_index, elapsed / 1000000, _lazy_time / 1000000);

  return result;
}

void StreamingArchiveHeapLoader::populate_permanent_object_table() {
  MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);

  // Ensure all objects are materialized, and then add them all to the table
  await_finished_processing();

  address bottom = (address)_heap_region->mapped_base();
  // Index 0 is the ""null" object
  for (size_t i = 1; i <= FileMapInfo::current_info()->num_archived_objects(); ++i) {
    void* object_raw_handle = _dfs_to_heap_object_table[i];
    oop heap_object;
    if (_allow_gc) {
      heap_object = resolve_raw_handle<true>(object_raw_handle);
    } else {
      heap_object = resolve_raw_handle<false>(object_raw_handle);
    }
    HeapShared::add_to_permanent_index_table(heap_object, i);
  }
}

#endif // INCLUDE_CDS_JAVA_HEAP
