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
#include "cds/cdsThread.hpp"
#include "cds/filemap.hpp"
#include "cds/archiveHeapLoader.hpp"
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
#include "runtime/java.hpp"
#include "runtime/thread.hpp"
#include "memory/iterator.inline.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/stack.inline.hpp"

#include <type_traits>

#if INCLUDE_CDS_JAVA_HEAP

FileMapRegion* ArchiveHeapLoader::_heap_region;
FileMapRegion* ArchiveHeapLoader::_bitmap_region;
OopStorage* ArchiveHeapLoader::_oop_storage;
address ArchiveHeapLoader::_roots_old_addr;
oop* ArchiveHeapLoader::_roots;
BitMapView ArchiveHeapLoader::_oopmap;
bool ArchiveHeapLoader::_is_loaded;
int ArchiveHeapLoader::_lowest_finished_root;
bool ArchiveHeapLoader::_allow_gc;
bool ArchiveHeapLoader::_stop_background_processing;
bool ArchiveHeapLoader::_finished_processing;
oop** ArchiveHeapLoader::_handles;
size_t ArchiveHeapLoader::_num_handles;

void ArchiveHeapLoader::add_handle(oop* handle) {
  assert(_num_handles < FileMapInfo::current_info()->num_archived_objects(),
         "Too many handles: " SIZE_FORMAT " vs " SIZE_FORMAT,
         _num_handles, FileMapInfo::current_info()->num_archived_objects());
  _handles[_num_handles++] = handle;
}

void ArchiveHeapLoader::initialize_oop_storage() {
  _oop_storage = OopStorageSet::create_strong("CDS Heap", mtClassShared);
}

// Initialize an empty array of CDS heap roots; materialize them lazily
void ArchiveHeapLoader::initialize_roots() {
  JavaThread* thread = JavaThread::current();

  FileMapInfo::current_info()->map_bitmap_region();

  _heap_region = FileMapInfo::current_info()->region_at(MetaspaceShared::hp);
  _bitmap_region = FileMapInfo::current_info()->region_at(MetaspaceShared::bm);
  assert(_oop_storage != nullptr, "Should be initialized now");

  // HeapShared::roots() is at this offset in the stream.
  size_t heap_roots_stream_offset = FileMapInfo::current_info()->heap_roots_offset();
  size_t num_archived_objects = FileMapInfo::current_info()->num_archived_objects();

  // The materialized address of the HeapShared::roots()
  _roots_old_addr = ((address)_heap_region->mapped_base()) + heap_roots_stream_offset;
  objArrayOopDesc* roots_old = (objArrayOopDesc *)_roots_old_addr;
  int length = roots_old->length();

  objArrayOop roots = ObjArrayKlass::cast(roots_old->klass())->allocate(length, thread);
  if (roots == nullptr) {
    fatal("Not enough memory available to initialize JVM");
  }
  _handles = NEW_C_HEAP_ARRAY(oop*, num_archived_objects, mtClassShared);
  _roots = _oop_storage->allocate();
  NativeAccess<IS_NOT_NULL>::oop_store(_roots, roots);
  add_handle(_roots);
  *((oop**)_roots_old_addr) = _roots;

  address start = (address)(_bitmap_region->mapped_base()) + _heap_region->oopmap_offset();
  _oopmap = BitMapView((BitMap::bm_word_t*)start, _heap_region->oopmap_size_in_bits());

  _is_loaded = true;
  HeapShared::init_roots(roots);

  CDSThread::initialize();
}

template <bool allow_gc> void* ArchiveHeapLoader::allocate_object(oopDesc* archive_object, size_t size, JavaThread* thread) {
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

  oop* handle = _oop_storage->allocate();
  add_handle(handle);
  NativeAccess<>::oop_store(handle, heap_object);

  // Install forwarding
  *(oop**)(archive_object) = handle;

  return create_raw_handle<allow_gc>(handle, heap_object);
}

static void patch_metadata(oop heap_object, int offset) {
  if (heap_object->metadata_field(offset) != nullptr) {
    heap_object->metadata_field_put(offset, (Metadata*)(address(heap_object->metadata_field(offset)) + MetaspaceShared::relocation_delta()));
  }
}

static void patch_metadata(oop heap_object) {
  if (heap_object->klass()->is_mirror_instance_klass()) {
    patch_metadata(heap_object, java_lang_Class::klass_offset());
    patch_metadata(heap_object, java_lang_Class::array_klass_offset());
  }
}

template <bool allow_gc> void* ArchiveHeapLoader::create_raw_handle(oop* handle, oop obj) {
  if (allow_gc) {
    return (void*)handle;
  } else {
    return cast_from_oop<void*>(obj);
  }
}

template <bool allow_gc> oop ArchiveHeapLoader::resolve_raw_handle(void* raw_handle) {
  if (allow_gc) {
    oop* handle = (oop*)raw_handle;
    return NativeAccess<>::oop_load(handle);
  } else {
    return cast_to_oop(raw_handle);
  }
}

template <bool COOPS, bool allow_gc>
void ArchiveHeapLoader::copy_object(oopDesc* archive_object, void* heap_object_raw_handle, size_t size, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  size_t scale = COOPS ? 2 : 1;
  using RawElementT = std::conditional_t<COOPS, uint32_t, uint64_t>;
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
      RawElementT pointee_byte_offset = (*archive_p) << (COOPS ? 3 : 0);
      oopDesc* pointee_archive_object = (oopDesc*)(bottom + pointee_byte_offset);

      dfs_stack.push({pointee_archive_object, heap_object_raw_handle, (unfinished_bit - start_bit) * sizeof(OopElementT)});

      unfinished_bit++;
      next_reference_bit = _oopmap.find_first_set_bit(unfinished_bit, end_bit);
    }
  }

  oop heap_object = resolve_raw_handle<allow_gc>(heap_object_raw_handle);

  if (!COOPS && oopDesc::has_klass_gap()) {
    oopDesc::set_klass_gap(cast_from_oop<HeapWord*>(heap_object), *(int*)(address(archive_object) + oopDesc::klass_gap_offset_in_bytes()));
  }

  patch_metadata(heap_object);

  markWord mark = *archive_object->mark_addr();
  intptr_t archive_hash = mark.hash();
  if (archive_hash != 0) {
    heap_object->set_mark(heap_object->mark().copy_set_hash(archive_hash));
  }
}

template <bool COOPS, bool allow_gc>
oop ArchiveHeapLoader::materialize_object_inner(oopDesc* archive_object, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  // Allocate object
  size_t size = archive_object->size();
  void* heap_object_raw_handle = allocate_object<allow_gc>(archive_object, size, thread);

  // Fill in object contents, and recursively materialize
  copy_object<COOPS, allow_gc>(archive_object, heap_object_raw_handle, size, dfs_stack, thread);

  return resolve_raw_handle<allow_gc>(heap_object_raw_handle);
}

static oop interned_string(oopDesc* archive_object, oop heap_object, JavaThread* thread) {
  oop interned_string = StringTable::intern(heap_object, thread);

  // Override forwarding
  oop* handle = *(oop**)(archive_object);
  NativeAccess<>::oop_store(handle, interned_string);

  return interned_string;
}

template <bool COOPS, bool allow_gc>
oop ArchiveHeapLoader::materialize_object(oopDesc* archive_object, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  address bottom = (address)_heap_region->mapped_base();
  size_t archive_object_offset = size_t(archive_object) - size_t(bottom);
  BitMap::idx_t obj_bit;
  if (COOPS) {
    obj_bit = BitMap::idx_t(archive_object_offset / sizeof(narrowOop));
  } else {
    obj_bit = BitMap::idx_t(archive_object_offset / sizeof(HeapWord));
  }

  if (_oopmap.at(obj_bit)) {
    // Already materialized
    return NativeAccess<>::oop_load(*(oop**)(archive_object));
  }

  _oopmap.set_bit(obj_bit);

  oop heap_object = materialize_object_inner<COOPS, allow_gc>(archive_object, dfs_stack, thread);

  if (java_lang_String::is_instance(archive_object) && _oopmap.at(obj_bit + 1)) {
    // Interned string... finish materializing and link it to the string table
    CDSHeapTraversalEntry value_entry = dfs_stack.pop();
    oop value_heap_object = materialize_object<COOPS, allow_gc>(value_entry._archive_pointee_object, dfs_stack, thread);

    heap_object = resolve_raw_handle<allow_gc>(value_entry._heap_object_handle);
    heap_object->obj_field_put((int)value_entry._heap_field_offset_bytes, value_heap_object);

    heap_object = interned_string(archive_object, heap_object, thread);
  }

  return heap_object;
}

template <bool COOPS, bool allow_gc>
void ArchiveHeapLoader::drain_dfs_stack(Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  while (!dfs_stack.is_empty()) {
    CDSHeapTraversalEntry entry = dfs_stack.pop();
    oop pointee_heap_object = materialize_object<COOPS, allow_gc>(entry._archive_pointee_object, dfs_stack, thread);
    oop heap_object = resolve_raw_handle<allow_gc>(entry._heap_object_handle);
    heap_object->obj_field_put((int)entry._heap_field_offset_bytes, pointee_heap_object);
  }
}

oop ArchiveHeapLoader::materialize_root(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  address bottom = (address)_heap_region->mapped_base();
  objArrayOopDesc* roots_old = (objArrayOopDesc *)_roots_old_addr;

  oop result;

  if (UseCompressedOops) {
    oopDesc* root_old = (oopDesc*)(bottom + (*roots_old->obj_at_addr<uint32_t>(root_index) << 3));
    if (_allow_gc) {
      result = materialize_object<true, true>(root_old, dfs_stack, thread);
      drain_dfs_stack<true, true>(dfs_stack, thread);
    } else {
      result = materialize_object<true, false>(root_old, dfs_stack, thread);
      drain_dfs_stack<true, false>(dfs_stack, thread);
    }
  } else {
    oopDesc* root_old = (oopDesc*)(bottom + *roots_old->obj_at_addr<uintptr_t>(root_index));
    if (_allow_gc) {
      result = materialize_object<false, true>(root_old, dfs_stack, thread);
      drain_dfs_stack<false, true>(dfs_stack, thread);
    } else {
      result = materialize_object<false, false>(root_old, dfs_stack, thread);
      drain_dfs_stack<false, false>(dfs_stack, thread);
    }
  }

  return result;
}

oop ArchiveHeapLoader::root(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread) {
  // Get the materialized roots array
  oop roots_obj = NativeAccess<>::oop_load(_roots);
  objArrayOop roots = (objArrayOop)roots_obj;

  // Check if we got the corresponding root
  oop root = roots->obj_at(root_index);

  if (root == nullptr) {
    // If not, materialize the root
    root = materialize_root(root_index, dfs_stack, thread);
    roots->replace_if_null(root_index, root);
  }

  return root;
}

oop ArchiveHeapLoader::root(int root_index) {
  log_info(cds, heap)("Lazy materialization of root: %d start", root_index);
  Stack<CDSHeapTraversalEntry, mtClassShared> dfs_stack;

  oop result;
  {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    result = root(root_index, dfs_stack, JavaThread::current());
  }

  log_info(cds, heap)("Lazy materialization of root: %d end", root_index);

  return result;
}

int ArchiveHeapLoader::compute_roots_length() {
  if (!_is_loaded) {
    return 0;
  }

  oop roots_obj = NativeAccess<>::oop_load(_roots);
  objArrayOop roots = (objArrayOop)roots_obj;
  int length = roots->length();

  return length;
}

bool ArchiveHeapLoader::await_gc_enabled() {
  for (;;) {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    if (_allow_gc) {
      break;
    }
    CDSHeapLoading_lock->wait();
  }

  return _stop_background_processing;
}

void ArchiveHeapLoader::await_finished_processing() {
  for (;;) {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
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

void ArchiveHeapLoader::materialize_objects() {
  // Get the materialized roots array
  JavaThread* thread = JavaThread::current();
  Stack<CDSHeapTraversalEntry, mtClassShared> dfs_stack;
  int length = compute_roots_length();

  size_t before_gc_materialize_budget_bytes = NewSize - 512 * K;
  size_t before_gc_materialize_budget_words = before_gc_materialize_budget_bytes / HeapWordSize;
  size_t materialized_words = 0;

  log_info(cds, heap)("Concurrent object materialization start with budget " SIZE_FORMAT " K", before_gc_materialize_budget_bytes / K);

  // Early materialization with a budget before GC is allowed
  for (int i = 0; i < length; ++i) {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    if (_stop_background_processing || _allow_gc || materialized_words > before_gc_materialize_budget_words) {
      _lowest_finished_root = i;
      log_info(cds, heap)("Concurrent object materialization end at root %d", i);
      break;
    }
    oop obj = root(i, dfs_stack, thread);
    materialized_words += obj->size();
  }

  if (!await_gc_enabled()) {
    // Continue materializing with GC allowed
    log_info(cds, heap)("Concurrent object materialization start after gc enabling");

    for (int i = _lowest_finished_root; i < length; ++i) {
      MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
      if (_stop_background_processing) {
        _lowest_finished_root = i;
        break;
      }
      oop obj = root(i, dfs_stack, thread);
      materialized_words += obj->size();
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
  _oop_storage->release(_handles, _num_handles);
  FREE_C_HEAP_ARRAY(oop*, _handles);
  log_info(cds, heap)("Concurrent object materialization cleanup end");
}

void ArchiveHeapLoader::enable_gc() {
  log_info(cds, heap)("Enable GC");
  CDSThread::materialize_thread_object();
  MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
  _allow_gc = true;
  CDSHeapLoading_lock->notify_all();
}

void ArchiveHeapLoader::finish_materialize_objects() {
  // Get the materialized roots array
  JavaThread* thread = JavaThread::current();
  Stack<CDSHeapTraversalEntry, mtClassShared> dfs_stack;
  int length = compute_roots_length();

  log_info(cds, heap)("Synchronous object materialization start");

  {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    _stop_background_processing = true;
  }

  for (int i = _lowest_finished_root; i < length; ++i) {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    root(i, dfs_stack, thread);
  }

  {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    _finished_processing = true;
    CDSHeapLoading_lock->notify_all();
  }

  log_info(cds, heap)("Synchronous object materialization end");
}

#endif // INCLUDE_CDS_JAVA_HEAP
