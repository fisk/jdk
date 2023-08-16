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
#include "cds/filemap.hpp"
#include "cds/archiveHeapLoader.hpp"
#include "cds/heapShared.hpp"
#include "cds/metaspaceShared.hpp"
#include "classfile/classLoaderDataShared.hpp"
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

  // The materialized address of the HeapShared::roots()
  _roots_old_addr = ((address)_heap_region->mapped_base()) + heap_roots_stream_offset;
  objArrayOopDesc* roots_old = (objArrayOopDesc *)_roots_old_addr;
  int length = roots_old->length();

  objArrayOop roots = ObjArrayKlass::cast(roots_old->klass())->allocate(length, thread);
  if (roots == nullptr) {
    fatal("Not enough memory available to initialize JVM");
  }
  _roots = _oop_storage->allocate();
  NativeAccess<IS_NOT_NULL>::oop_store(_roots, roots);
  *((oop**)_roots_old_addr) = _roots;

  address start = (address)(_bitmap_region->mapped_base()) + _heap_region->oopmap_offset();
  _oopmap = BitMapView((BitMap::bm_word_t*)start, _heap_region->oopmap_size_in_bits());

  _is_loaded = true;
  HeapShared::init_roots(roots);
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
  if (!heap_object->klass()->is_mirror_instance_klass()) {
    return;
  }

  patch_metadata(heap_object, java_lang_Class::klass_offset());
  patch_metadata(heap_object, java_lang_Class::array_klass_offset());
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
    oopDesc::set_klass_gap((HeapWord*)heap_object, *(int*)(address(archive_object) + oopDesc::klass_gap_offset_in_bytes()));
  }

  patch_metadata(heap_object);

  markWord mark = *archive_object->mark_addr();
  intptr_t archive_hash = mark.hash();
  if (archive_hash != 0) {
    heap_object->set_mark(heap_object->mark().copy_set_hash(archive_hash));
  }
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

  // Allocate object
  size_t size = archive_object->size();
  void* heap_object_raw_handle = allocate_object<allow_gc>(archive_object, size, thread);

  // Fill in object contents, and recursively materialize
  copy_object<COOPS, allow_gc>(archive_object, heap_object_raw_handle, size, dfs_stack, thread);

  return resolve_raw_handle<allow_gc>(heap_object_raw_handle);
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
    roots->obj_at_put(root_index, root);
  }

  return root;
}

oop ArchiveHeapLoader::root(int root_index) {
  Stack<CDSHeapTraversalEntry, mtClassShared> dfs_stack;

  MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
  return root(root_index, dfs_stack, JavaThread::current());
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

void ArchiveHeapLoader::materialize_objects() {
  // Get the materialized roots array
  _allow_gc = true;

  JavaThread* thread = JavaThread::current();
  Stack<CDSHeapTraversalEntry, mtClassShared> dfs_stack;
  int length = compute_roots_length();

  for (int i = 0; i < length; ++i) {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    if (_stop_background_processing) {
      _lowest_finished_root = i;
      break;
    }
    root(i, dfs_stack, thread);
  }
}

void ArchiveHeapLoader::finish_materialize_objects() {
  // Get the materialized roots array
  JavaThread* thread = JavaThread::current();
  Stack<CDSHeapTraversalEntry, mtClassShared> dfs_stack;
  int length = compute_roots_length();

  {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    _stop_background_processing = true;
  }

  for (int i = _lowest_finished_root; i < length; ++i) {
    MutexLocker ml(CDSHeapLoading_lock, Mutex::_safepoint_check_flag);
    root(i, dfs_stack, thread);
  }
}

#endif // INCLUDE_CDS_JAVA_HEAP
