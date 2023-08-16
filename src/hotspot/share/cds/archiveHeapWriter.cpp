/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "cds/archiveHeapWriter.hpp"
#include "cds/filemap.hpp"
#include "cds/heapShared.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/oopFactory.hpp"
#include "memory/universe.hpp"
#include "oops/compressedOops.hpp"
#include "oops/oop.inline.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "oops/oopHandle.inline.hpp"
#include "oops/typeArrayKlass.hpp"
#include "oops/typeArrayOop.hpp"
#include "runtime/java.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/bitMap.inline.hpp"

#if INCLUDE_CDS_JAVA_HEAP

GrowableArrayCHeap<u1, mtClassShared>* ArchiveHeapWriter::_buffer = nullptr;

// The following are offsets from buffer_bottom()
size_t ArchiveHeapWriter::_buffer_used;
size_t ArchiveHeapWriter::_heap_roots_offset;

size_t ArchiveHeapWriter::_heap_roots_word_size;

GrowableArrayCHeap<oop, mtClassShared>* ArchiveHeapWriter::_source_objs;

ArchiveHeapWriter::BufferOffsetToSourceObjectTable*
  ArchiveHeapWriter::_buffer_offset_to_source_obj_table = nullptr;


typedef ResourceHashtable<address, size_t,
      127, // prime number
      AnyObj::C_HEAP,
      mtClassShared> FillersTable;

void ArchiveHeapWriter::init() {
  if (HeapShared::can_write()) {
    _buffer_offset_to_source_obj_table = new BufferOffsetToSourceObjectTable();

    _source_objs = new GrowableArrayCHeap<oop, mtClassShared>(10000);
  }
}

void ArchiveHeapWriter::add_source_obj(oop src_obj) {
  _source_objs->append(src_obj);
}

void ArchiveHeapWriter::write(GrowableArrayCHeap<oop, mtClassShared>* roots,
                              ArchiveHeapInfo* heap_info) {
  assert(HeapShared::can_write(), "sanity");
  allocate_buffer();
  copy_source_objs_to_buffer(roots);
  relocate_embedded_oops(roots, heap_info);
  populate_archive_heap_info(heap_info);
}

void ArchiveHeapWriter::allocate_buffer() {
  int initial_buffer_size = 100000;
  _buffer = new GrowableArrayCHeap<u1, mtClassShared>(initial_buffer_size);
  _buffer_used = 0;
  ensure_buffer_space(1); // so that buffer_bottom() works
}

void ArchiveHeapWriter::ensure_buffer_space(size_t min_bytes) {
  // We usually have very small heaps. If we get a huge one it's probably caused by a bug.
  guarantee(min_bytes <= max_jint, "we dont support archiving more than 2G of objects");
  _buffer->at_grow(to_array_index(min_bytes));
}

void ArchiveHeapWriter::copy_roots_to_buffer(GrowableArrayCHeap<oop, mtClassShared>* roots) {
  Klass* k = Universe::objectArrayKlassObj(); // already relocated to point to archived klass
  int length = roots->length();
  _heap_roots_word_size = objArrayOopDesc::object_size(length);
  size_t byte_size = _heap_roots_word_size * HeapWordSize;

  size_t new_used = _buffer_used + byte_size;
  ensure_buffer_space(new_used);

  HeapWord* mem = offset_to_buffered_address<HeapWord*>(_buffer_used);
  memset(mem, 0, byte_size);
  {
    // This is copied from MemAllocator::finish
    oopDesc::set_mark(mem, markWord::prototype());
    oopDesc::release_set_klass(mem, k);
  }
  {
    // This is copied from ObjArrayAllocator::initialize
    arrayOopDesc::set_length(mem, length);
  }

  objArrayOop arrayOop = objArrayOop(cast_to_oop(mem));
  for (int i = 0; i < length; i++) {
    // Do not use arrayOop->obj_at_put(i, o) as arrayOop is outside of the real heap!
    oop o = roots->at(i);
    if (UseCompressedOops) {
      *arrayOop->obj_at_addr<uint32_t>(i) = (uint32_t)CompressedOops::encode_not_null(o);
    } else {
      *arrayOop->obj_at_addr<uintptr_t>(i) = cast_from_oop<uintptr_t>(o);
    }
  }
  log_info(cds, heap)("archived obj roots[%d] = " SIZE_FORMAT " bytes, klass = %p, obj = %p", length, byte_size, k, mem);

  _heap_roots_offset = _buffer_used;
  _buffer_used = new_used;
}

void ArchiveHeapWriter::copy_source_objs_to_buffer(GrowableArrayCHeap<oop, mtClassShared>* roots) {
  for (int i = 0; i < _source_objs->length(); i++) {
    oop src_obj = _source_objs->at(i);
    HeapShared::CachedOopInfo* info = HeapShared::archived_object_cache()->get(src_obj);
    assert(info != nullptr, "must be");
    size_t buffer_offset = copy_one_source_obj_to_buffer(src_obj);
    info->set_buffer_offset(buffer_offset);

    _buffer_offset_to_source_obj_table->put(buffer_offset, src_obj);
  }

  copy_roots_to_buffer(roots);

  log_info(cds)("Size of heap region = " SIZE_FORMAT " bytes, %d objects, %d roots",
                _buffer_used, _source_objs->length() + 1, roots->length());
}

size_t ArchiveHeapWriter::copy_one_source_obj_to_buffer(oop src_obj) {
  size_t byte_size = src_obj->size() * HeapWordSize;
  assert(byte_size > 0, "no zero-size objects");

  size_t new_used = _buffer_used + byte_size;
  assert(new_used > _buffer_used, "no wrap around");

  ensure_buffer_space(new_used);

  address from = cast_from_oop<address>(src_obj);
  address to = offset_to_buffered_address<address>(_buffer_used);
  assert(is_object_aligned(_buffer_used), "sanity");
  assert(is_object_aligned(byte_size), "sanity");
  memcpy(to, from, byte_size);

  size_t buffered_obj_offset = _buffer_used;
  _buffer_used = new_used;

  return buffered_obj_offset;
}

// Oop relocation

inline void ArchiveHeapWriter::store_oop_in_buffer(oop* buffered_addr, size_t buffer_offset) {
  *(size_t*)buffered_addr = buffer_offset;
}

inline void ArchiveHeapWriter::store_oop_in_buffer(narrowOop* buffered_addr, size_t buffer_offset) {
  *(uint32_t*)buffered_addr = (uint32_t)buffer_offset >> 3;
}

oop ArchiveHeapWriter::load_oop_from_buffer(oop* buffered_addr) {
  return *buffered_addr;
}

oop ArchiveHeapWriter::load_oop_from_buffer(narrowOop* buffered_addr) {
  return CompressedOops::decode(*buffered_addr);
}

template <typename T> void ArchiveHeapWriter::relocate_field_in_buffer(T* field_addr_in_buffer, CHeapBitMap* oopmap) {
  oop obj = load_oop_from_buffer(field_addr_in_buffer);
  if (obj != nullptr) {
    size_t buffered_offset = source_obj_to_buffered_offset(obj);
    store_oop_in_buffer(field_addr_in_buffer, buffered_offset);
    mark_oop_pointer<T>(field_addr_in_buffer, oopmap);
  }
}

template <typename T> void ArchiveHeapWriter::mark_oop_pointer(T* buffered_addr, CHeapBitMap* oopmap) {
  // Mark the pointer in the oopmap
  size_t buffered_offset = buffered_address_to_offset((address)buffered_addr);
  BitMap::idx_t idx = buffered_offset >> (UseCompressedOops ? 2 : 3);
  assert(idx < oopmap->size(), "overflow");
  oopmap->set_bit(idx);
}

void ArchiveHeapWriter::update_header_for_buffered_addr(address buffered_addr, oop src_obj,  Klass* src_klass) {
  assert(UseCompressedClassPointers, "Archived heap only supported for compressed klasses");
  narrowKlass nk = ArchiveBuilder::current()->get_requested_narrow_klass(src_klass);

  oop fake_oop = cast_to_oop(buffered_addr);
  fake_oop->set_narrow_klass(nk);

  // We need to retain the identity_hash, because it may have been used by some hashtables
  // in the shared heap. This also has the side effect of pre-initializing the
  // identity_hash for all shared objects, so they are less likely to be written
  // into during run time, increasing the potential of memory sharing.
  if (src_obj != nullptr) {
    int src_hash = src_obj->identity_hash();
    fake_oop->set_mark(markWord::prototype().copy_set_hash(src_hash));
    assert(fake_oop->mark().is_unlocked(), "sanity");

    DEBUG_ONLY(int archived_hash = fake_oop->identity_hash());
    assert(src_hash == archived_hash, "Different hash codes: original %x, archived %x", src_hash, archived_hash);
  }
}

// Relocate an element in the buffered copy of HeapShared::roots()
template <typename T> void ArchiveHeapWriter::relocate_root_at(address buffered_roots, int index, CHeapBitMap* oopmap) {
  size_t offset = (size_t)((objArrayOopDesc*)buffered_roots)->obj_at_offset<T>(index);
  relocate_field_in_buffer<T>((T*)(buffered_heap_roots_addr() + offset), oopmap);
}

class ArchiveHeapWriter::EmbeddedOopRelocator: public BasicOopIterateClosure {
  oop _src_obj;
  address _buffered_obj;
  CHeapBitMap* _oopmap;

public:
  EmbeddedOopRelocator(oop src_obj, address buffered_obj, CHeapBitMap* oopmap) :
    _src_obj(src_obj), _buffered_obj(buffered_obj), _oopmap(oopmap) {}

  void do_oop(narrowOop *p) { EmbeddedOopRelocator::do_oop_work(p); }
  void do_oop(      oop *p) { EmbeddedOopRelocator::do_oop_work(p); }

private:
  template <class T> void do_oop_work(T *p) {
    size_t field_offset = pointer_delta(p, _src_obj, sizeof(char));
    ArchiveHeapWriter::relocate_field_in_buffer<T>((T*)(_buffered_obj + field_offset), _oopmap);
  }
};

static void log_bitmap_usage(const char* which, BitMap* bitmap, size_t total_bits) {
  // The whole heap is covered by total_bits, but there are only non-zero bits within [start ... end).
  size_t start = bitmap->find_first_set_bit(0);
  size_t end = bitmap->size();
  log_info(cds)("%s = " SIZE_FORMAT_W(7) " ... " SIZE_FORMAT_W(7) " (%3zu%% ... %3zu%% = %3zu%%)", which,
                start, end,
                start * 100 / total_bits,
                end * 100 / total_bits,
                (end - start) * 100 / total_bits);
}

static void patch_metadata(oop heap_object, address archived_object, int offset) {
  Metadata** buffered_field_addr = (Metadata**)(archived_object + offset);
  Metadata* native_ptr = *buffered_field_addr;
  if (native_ptr == nullptr) {
    return;
  }
  address buffered_native_ptr = ArchiveBuilder::current()->get_buffered_addr((address)native_ptr);
  address requested_native_ptr = ArchiveBuilder::current()->to_requested(buffered_native_ptr);
  *buffered_field_addr = (Metadata*)requested_native_ptr;
}

static void patch_metadata(oop heap_object, address archived_object) {
  if (!heap_object->klass()->is_mirror_instance_klass()) {
    return;
  }

  patch_metadata(heap_object, archived_object, java_lang_Class::klass_offset());
  patch_metadata(heap_object, archived_object, java_lang_Class::array_klass_offset());
}

// Update all oop fields embedded in the buffered objects
void ArchiveHeapWriter::relocate_embedded_oops(GrowableArrayCHeap<oop, mtClassShared>* roots,
                                               ArchiveHeapInfo* heap_info) {
  size_t oopmap_unit = (UseCompressedOops ? sizeof(narrowOop) : sizeof(oop));
  size_t heap_region_byte_size = _buffer_used;
  heap_info->oopmap()->resize(heap_region_byte_size / oopmap_unit);

  for (int i = 0; i < _source_objs->length(); i++) {
    oop src_obj = _source_objs->at(i);
    HeapShared::CachedOopInfo* info = HeapShared::archived_object_cache()->get(src_obj);
    assert(info != nullptr, "must be");
    address buffered_obj = offset_to_buffered_address<address>(info->buffer_offset());
    update_header_for_buffered_addr(buffered_obj, src_obj, src_obj->klass());
    EmbeddedOopRelocator relocator(src_obj, buffered_obj, heap_info->oopmap());
    src_obj->oop_iterate(&relocator);
    patch_metadata(src_obj, buffered_obj);
  };

  // Relocate HeapShared::roots(), which is created in copy_roots_to_buffer() and
  // doesn't have a corresponding src_obj, so we can't use EmbeddedOopRelocator on it.
  address buffered_roots = offset_to_buffered_address<address>(_heap_roots_offset);
  update_header_for_buffered_addr(buffered_roots, nullptr, Universe::objectArrayKlassObj());
  int length = roots != nullptr ? roots->length() : 0;
  for (int i = 0; i < length; i++) {
    if (UseCompressedOops) {
      relocate_root_at<narrowOop>(buffered_roots, i, heap_info->oopmap());
    } else {
      relocate_root_at<oop>(buffered_roots, i, heap_info->oopmap());
    }
  }

  size_t total_bytes = (size_t)_buffer->length();
  log_bitmap_usage("oopmap", heap_info->oopmap(), total_bytes / (UseCompressedOops ? sizeof(narrowOop) : sizeof(oop)));
}

size_t ArchiveHeapWriter::source_obj_to_buffered_offset(oop src_obj) {
  HeapShared::CachedOopInfo* p = HeapShared::archived_object_cache()->get(src_obj);
  if (p != nullptr) {
    return p->buffer_offset();
  } else {
    ShouldNotReachHere();
  }
}

address ArchiveHeapWriter::source_obj_to_buffered_addr(oop src_obj) {
  return offset_to_buffered_address<address>(source_obj_to_buffered_offset(src_obj));
}

oop ArchiveHeapWriter::buffered_addr_to_source_obj(address buffered_addr) {
  oop* p = _buffer_offset_to_source_obj_table->get(buffered_address_to_offset(buffered_addr));
  if (p != nullptr) {
    return *p;
  } else {
    return nullptr;
  }
}

void ArchiveHeapWriter::populate_archive_heap_info(ArchiveHeapInfo* info) {
  assert(!info->is_used(), "only set once");

  size_t heap_region_byte_size = _buffer_used;
  assert(heap_region_byte_size > 0, "must archived at least one object!");

  info->set_buffer_region(MemRegion(offset_to_buffered_address<HeapWord*>(0),
                                    offset_to_buffered_address<HeapWord*>(_buffer_used)));
  info->set_heap_roots_offset(_heap_roots_offset);
}

#endif // INCLUDE_CDS_JAVA_HEAP
