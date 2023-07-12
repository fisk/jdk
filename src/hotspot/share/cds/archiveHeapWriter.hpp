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

#ifndef SHARE_CDS_ARCHIVEHEAPWRITER_HPP
#define SHARE_CDS_ARCHIVEHEAPWRITER_HPP

#include "cds/heapShared.hpp"
#include "memory/allocation.hpp"
#include "memory/allStatic.hpp"
#include "oops/oopHandle.hpp"
#include "utilities/bitMap.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"
#include "utilities/resourceHash.hpp"

class MemRegion;

class ArchiveHeapInfo {
  MemRegion _buffer_region;             // Contains the archived objects to be written into the CDS archive.
  CHeapBitMap _oopmap;
  size_t _heap_roots_offset;            // Offset of the HeapShared::roots() object, from the bottom
                                        // of the archived heap objects, in bytes.
  size_t _num_archived_objects;         // The number of archived objects written into the CDS archive.

public:
  ArchiveHeapInfo() : _buffer_region(), _oopmap(128, mtClassShared) {}
  bool is_used() { return !_buffer_region.is_empty(); }

  MemRegion buffer_region() { return _buffer_region; }
  void set_buffer_region(MemRegion r) { _buffer_region = r; }

  char* buffer_start() { return (char*)_buffer_region.start(); }
  size_t buffer_byte_size() { return _buffer_region.byte_size();    }

  CHeapBitMap* oopmap() { return &_oopmap; }

  void set_heap_roots_offset(size_t n) { _heap_roots_offset = n; }
  size_t heap_roots_offset() const { return _heap_roots_offset; }

  void set_num_archived_objects(size_t n) { _num_archived_objects = n; }
  size_t num_archived_objects() const { return _num_archived_objects; }
};

#if INCLUDE_CDS_JAVA_HEAP
class ArchiveHeapWriter : AllStatic {
  // ArchiveHeapWriter manipulates three types of addresses:
  //
  //     "source" vs "buffered"
  //
  // (Note: the design and convention is the same as for the archiving of Metaspace objects.
  //  See archiveBuilder.hpp.)
  //
  // - "source objects" are regular Java objects allocated during the execution
  //   of "java -Xshare:dump". They can be used as regular oops.
  //
  //   HeapShared::archive_objects() recursively searches for the oops that need to be
  //   stored into the CDS archive. These are entered into HeapShared::archived_object_cache().
  //
  // - "buffered objects" are copies of the "source objects", and are stored in into
  //   ArchiveHeapWriter::_buffer, which is a GrowableArray that sits outside of
  //   the valid heap range. Therefore we avoid using the addresses of these copies
  //   as oops. They are usually called "buffered_addr" in the code (of the type "address").
  //
  //   The buffered objects are stored contiguously.
  //
  // UseCompressedOops == true:
  //   The archived objects are stored assuming that the runtime COOPS compression
  //   scheme is exactly the same as in dump time (or else a more expensive runtime relocation
  //   would be needed.)

private:
  class EmbeddedOopRelocator;
  static GrowableArrayCHeap<u1, mtClassShared>* _buffer;

  // The number of bytes that have written into _buffer (may be smaller than _buffer->length()).
  static size_t _buffer_used;

  // The bottom of the copy of Heap::roots() inside this->_buffer.
  static size_t _heap_roots_offset;
  static size_t _heap_roots_word_size;

  static GrowableArrayCHeap<oop, mtClassShared>* _source_objs;

  typedef ResourceHashtable<size_t, oop,
      36137, // prime number
      AnyObj::C_HEAP,
      mtClassShared> BufferOffsetToSourceObjectTable;
  static BufferOffsetToSourceObjectTable* _buffer_offset_to_source_obj_table;

  static void allocate_buffer();
  static void ensure_buffer_space(size_t min_bytes);

  // Both Java bytearray and GrowableArraty use int indices and lengths. Do a safe typecast with range check
  static int to_array_index(size_t i) {
    assert(i <= (size_t)max_jint, "must be");
    return (int)i;
  }
  static int to_array_length(size_t n) {
    return to_array_index(n);
  }

  template <typename T> static T offset_to_buffered_address(size_t offset) {
    return (T)(_buffer->adr_at(to_array_index(offset)));
  }

  static address buffer_bottom() {
    return offset_to_buffered_address<address>(0);
  }

  // The exclusive end of the last object that was copied into the buffer.
  static address buffer_top() {
    return buffer_bottom() + _buffer_used;
  }

  static bool in_buffer(address buffered_addr) {
    return (buffer_bottom() <= buffered_addr) && (buffered_addr < buffer_top());
  }

  static size_t buffered_address_to_offset(address buffered_addr) {
    assert(in_buffer(buffered_addr), "sanity");
    return buffered_addr - buffer_bottom();
  }

  static void copy_roots_to_buffer(GrowableArrayCHeap<oop, mtClassShared>* roots);
  static void copy_source_objs_to_buffer(GrowableArrayCHeap<oop, mtClassShared>* roots);
  static size_t copy_one_source_obj_to_buffer(oop src_obj);

  static void relocate_embedded_oops(GrowableArrayCHeap<oop, mtClassShared>* roots, ArchiveHeapInfo* info);
  static bool is_in_requested_range(oop o);
  static oop requested_obj_from_buffer_offset(size_t offset);

  static oop load_oop_from_buffer(oop* buffered_addr);
  static oop load_oop_from_buffer(narrowOop* buffered_addr);
  inline static void store_oop_in_buffer(oop* buffered_addr, size_t buffer_offset);
  inline static void store_oop_in_buffer(narrowOop* buffered_addr, size_t buffer_offset);

  template <typename T> static void relocate_field_in_buffer(T* field_addr_in_buffer, CHeapBitMap* oopmap);
  template <typename T> static void mark_oop_pointer(T* buffered_addr, CHeapBitMap* oopmap);
  template <typename T> static void relocate_root_at(address buffered_roots, int index, CHeapBitMap* oopmap);

  static void update_header_for_buffered_addr(address buffered_addr, oop src_obj, Klass* src_klass);

  static void populate_archive_heap_info(ArchiveHeapInfo* info);

public:
  static void init() NOT_CDS_JAVA_HEAP_RETURN;
  static void add_source_obj(oop src_obj);
  static void write(GrowableArrayCHeap<oop, mtClassShared>*, ArchiveHeapInfo* heap_info);
  static address buffered_heap_roots_addr() {
    return offset_to_buffered_address<address>(_heap_roots_offset);
  }
  static size_t heap_roots_word_size() {
    return _heap_roots_word_size;
  }

  static size_t source_obj_to_buffered_offset(oop src_obj);
  static address source_obj_to_buffered_addr(oop src_obj);
  static oop buffered_addr_to_source_obj(address buffered_addr);

  // Archived heap object headers carry pre-computed narrow Klass ids calculated with the
  // following scheme:
  // 1) the encoding base must be the mapping start address.
  // 2) shift must be large enough to result in an encoding range that covers the runtime Klass range.
  //    That Klass range is defined by CDS archive size and runtime class space size. Luckily, the maximum
  //    size can be predicted: archive size is assumed to be <1G, class space size capped at 3G, and at
  //    runtime we put both regions adjacent to each other. Therefore, runtime Klass range size < 4G.
  //    Since nKlass itself is 32 bit, our encoding range len is 4G, and since we set the base directly
  //    at mapping start, these 4G are enough. Therefore, we don't need to shift at all (shift=0).
  static constexpr int precomputed_narrow_klass_shift = 0;

};
#endif // INCLUDE_CDS_JAVA_HEAP
#endif // SHARE_CDS_ARCHIVEHEAPWRITER_HPP
