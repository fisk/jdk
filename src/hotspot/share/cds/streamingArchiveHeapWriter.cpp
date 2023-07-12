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
#include "cds/streamingArchiveHeapWriter.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/filemap.hpp"
#include "cds/heapShared.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/systemDictionary.hpp"
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
#include "utilities/stack.inline.hpp"

#if INCLUDE_CDS_JAVA_HEAP

GrowableArrayCHeap<u1, mtClassShared>* StreamingArchiveHeapWriter::_buffer = nullptr;

// The following are offsets from buffer_bottom()
size_t StreamingArchiveHeapWriter::_buffer_used;

size_t StreamingArchiveHeapWriter::_heap_roots_offset;
size_t StreamingArchiveHeapWriter::_heap_roots_word_size;

size_t StreamingArchiveHeapWriter::_forwarding_offset;
size_t StreamingArchiveHeapWriter::_roots_highest_dfs_offset;

GrowableArrayCHeap<oop, mtClassShared>* StreamingArchiveHeapWriter::_source_objs;

StreamingArchiveHeapWriter::BufferOffsetToSourceObjectTable* StreamingArchiveHeapWriter::_buffer_offset_to_source_obj_table;

int* StreamingArchiveHeapWriter::_roots_highest_dfs;
size_t* StreamingArchiveHeapWriter::_dfs_to_archive_object_table;


typedef ResourceHashtable<address, size_t,
      127, // prime number
      AnyObj::C_HEAP,
      mtClassShared> FillersTable;

void StreamingArchiveHeapWriter::init() {
  if (HeapShared::can_write()) {
    _buffer_offset_to_source_obj_table = new BufferOffsetToSourceObjectTable();

    _source_objs = new GrowableArrayCHeap<oop, mtClassShared>(10000);
  }
}

void StreamingArchiveHeapWriter::add_source_obj(oop src_obj) {
  _source_objs->append(src_obj);
}

class FollowOopIterateClosure: public BasicOopIterateClosure {
  Stack<oop, mtClassShared>* _dfs_stack;

public:
  FollowOopIterateClosure(Stack<oop, mtClassShared>* dfs_stack) :
    _dfs_stack(dfs_stack) {}

  void do_oop(narrowOop *p) { do_oop_work(p); }
  void do_oop(      oop *p) { do_oop_work(p); }

private:
  template <class T> void do_oop_work(T *p) {
    oop obj = HeapAccess<>::oop_load(p);
    if (obj != nullptr) {
      if (java_lang_Class::is_instance(obj)) {
        obj = HeapShared::scratch_java_mirror(obj);
        assert(obj != nullptr, "must be");
      }
      _dfs_stack->push(obj);
    }
  }
};

typedef ResourceHashtable<void*, int,
                          36137, // prime number
                          AnyObj::C_HEAP,
                          mtClassShared> SourceObjectToDFSOrderTable;

static SourceObjectToDFSOrderTable* _dfs_order_table;

static int cmp_dfs_order(oop* o1, oop* o2) {
  int* o1_dfs = _dfs_order_table->get(*o1);
  int* o2_dfs = _dfs_order_table->get(*o2);
  return *o1_dfs - *o2_dfs;
}

void StreamingArchiveHeapWriter::order_source_objs(GrowableArrayCHeap<oop, mtClassShared>* roots) {
  Stack<oop, mtClassShared> dfs_stack;
  _dfs_order_table = new SourceObjectToDFSOrderTable();
  _roots_highest_dfs = NEW_C_HEAP_ARRAY(int, roots->length(), mtClassShared);
  _dfs_to_archive_object_table = NEW_C_HEAP_ARRAY(size_t, _source_objs->length() + 1, mtClassShared);

  for (int i = 0; i < _source_objs->length(); ++i) {
    oop obj = _source_objs->at(i);
    _dfs_order_table->put(cast_from_oop<void*>(obj), -1);
  }

  int dfs_order = 0;
  int max_dfs_index = 0;

  for (int i = 0; i < roots->length(); ++i) {
    oop root = roots->at(i);

    if (root == nullptr) {
      log_info(cds,heap)("null root at %d", i);
      continue;
    }

    dfs_stack.push(root);

    while (!dfs_stack.is_empty()) {
      oop obj = dfs_stack.pop();
      assert(obj != nullptr, "null root");
      int* dfs_number = _dfs_order_table->get(cast_from_oop<void*>(obj));
      if (*dfs_number != -1) {
        // Already visited in the traversal
        continue;
      }
      _dfs_order_table->put(cast_from_oop<void*>(obj), ++dfs_order);
      max_dfs_index = MAX2(dfs_order, max_dfs_index);

      FollowOopIterateClosure cl(&dfs_stack);
      obj->oop_iterate(&cl);
    }

    _roots_highest_dfs[i] = max_dfs_index;
  }

  _source_objs->sort(cmp_dfs_order);
}

void StreamingArchiveHeapWriter::write(GrowableArrayCHeap<oop, mtClassShared>* roots,
                                       ArchiveHeapInfo* heap_info) {
  ResourceMark rm;
  assert(HeapShared::can_write(), "sanity");
  allocate_buffer();
  order_source_objs(roots);
  copy_source_objs_to_buffer(roots);
  relocate_embedded_oops(roots, heap_info);
  populate_archive_heap_info(heap_info);
}

void StreamingArchiveHeapWriter::allocate_buffer() {
  int initial_buffer_size = 100000;
  _buffer = new GrowableArrayCHeap<u1, mtClassShared>(initial_buffer_size);
  _buffer_used = 0;
  ensure_buffer_space(1); // so that buffer_bottom() works
}

void StreamingArchiveHeapWriter::ensure_buffer_space(size_t min_bytes) {
  // We usually have very small heaps. If we get a huge one it's probably caused by a bug.
  guarantee(min_bytes <= max_jint, "we dont support archiving more than 2G of objects");
  _buffer->at_grow(to_array_index(min_bytes));
}

void StreamingArchiveHeapWriter::copy_roots_to_buffer(GrowableArrayCHeap<oop, mtClassShared>* roots) {
  Klass* k = Universe::intArrayKlassObj(); // already relocated to point to archived klass
  int length = roots->length();
  _heap_roots_word_size = typeArrayOopDesc::object_size(k->layout_helper(), length);
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

  typeArrayOop arrayOop = typeArrayOop(cast_to_oop(mem));
  for (int i = 0; i < length; i++) {
    // Do not use arrayOop->obj_at_put(i, o) as arrayOop is outside of the real heap!
    oop o = roots->at(i);
    int dfs_index = o == nullptr ? 0 : *_dfs_order_table->get(cast_from_oop<void*>(o));
    *arrayOop->int_at_addr(i) = dfs_index;
  }
  log_info(cds, heap)("archived obj roots[%d] = " SIZE_FORMAT " bytes, klass = %p, obj = %p", length, byte_size, k, mem);

  _heap_roots_offset = _buffer_used;
  _buffer_used = new_used;
}

template <typename T>
void StreamingArchiveHeapWriter::write(T value) {
  size_t new_used = _buffer_used + sizeof(T);
  ensure_buffer_space(new_used);
  T* mem = offset_to_buffered_address<T*>(_buffer_used);
  *mem = value;
  _buffer_used = new_used;
}

void StreamingArchiveHeapWriter::copy_forwarding_to_buffer() {
  _forwarding_offset = _buffer_used;

  write<size_t>(0); // The first entry is the null entry

  // Write a mapping from object index to buffer offset
  for (int i = 1; i <= _source_objs->length(); i++) {
    size_t buffer_offset = _dfs_to_archive_object_table[i];
    write(buffer_offset);
  }
}

void StreamingArchiveHeapWriter::copy_roots_max_dfs_to_buffer(int roots_length) {
  _roots_highest_dfs_offset = _buffer_used;

  for (int i = 0; i < roots_length; ++i) {
    int highest_dfs = _roots_highest_dfs[i];
    write(highest_dfs);
  }

  if ((roots_length % 2) != 0) {
    write(-1); // Align up to a 64 bit word
  }
}

static bool is_interned_string(oop obj) {
  if (!java_lang_String::is_instance(obj)) {
    return false;
  }

  ResourceMark rm;
  int len;
  jchar* name = java_lang_String::as_unicode_string_or_null(obj, len);
  if (name == nullptr) {
    fatal("Insufficient memory for dumping");
  }
  return StringTable::lookup(name, len) == obj;
}

static void mark_interned_string(size_t buffer_offset, BitMap* oopmap) {
  BitMap::idx_t obj_idx = buffer_offset >> (UseCompressedOops ? 2 : 3);
  BitMap::idx_t intern_idx = obj_idx + 1;
  assert(intern_idx < oopmap->size(), "overflow");
  oopmap->set_bit(intern_idx);
}

void StreamingArchiveHeapWriter::copy_source_objs_to_buffer(GrowableArrayCHeap<oop, mtClassShared>* roots) {
  for (int i = 0; i < _source_objs->length(); i++) {
    oop src_obj = _source_objs->at(i);
    HeapShared::CachedOopInfo* info = HeapShared::archived_object_cache()->get(src_obj);
    assert(info != nullptr, "must be");
    size_t buffer_offset = copy_one_source_obj_to_buffer(src_obj);
    info->set_buffer_offset(buffer_offset);

    _buffer_offset_to_source_obj_table->put(buffer_offset, src_obj);

    size_t dfs_order = i + 1;
    _dfs_to_archive_object_table[dfs_order] = buffer_offset;
  }

  copy_roots_to_buffer(roots);
  copy_forwarding_to_buffer();
  copy_roots_max_dfs_to_buffer(roots->length());

  log_info(cds)("Size of heap region = " SIZE_FORMAT " bytes, %d objects, %d roots",
                _buffer_used, _source_objs->length() + 1, roots->length());
}

template <typename T>
void update_buffered_object_field(address buffered_obj, int field_offset, T value) {
  T* field_addr = cast_to_oop(buffered_obj)->field_addr<T>(field_offset);
  *field_addr = value;
}

size_t StreamingArchiveHeapWriter::copy_one_source_obj_to_buffer(oop src_obj) {
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

  // These native pointers will be restored explicitly at run time.
  if (java_lang_Module::is_instance(src_obj)) {
    update_buffered_object_field<ModuleEntry*>(to, java_lang_Module::module_entry_offset(), nullptr);
  } else if (java_lang_ClassLoader::is_instance(src_obj)) {
#ifdef ASSERT
    // We only archive these loaders
    if (src_obj != SystemDictionary::java_platform_loader() &&
        src_obj != SystemDictionary::java_system_loader()) {
      assert(src_obj->klass()->name()->equals("jdk/internal/loader/ClassLoaders$BootClassLoader"), "must be");
    }
#endif
    update_buffered_object_field<ClassLoaderData*>(to, java_lang_ClassLoader::loader_data_offset(), nullptr);
  }

  size_t buffered_obj_offset = _buffer_used;
  _buffer_used = new_used;

  return buffered_obj_offset;
}

// Oop relocation

inline void StreamingArchiveHeapWriter::store_oop_in_buffer(oop* buffered_addr, int dfs_index) {
  *(ssize_t*)buffered_addr = dfs_index;
}

inline void StreamingArchiveHeapWriter::store_oop_in_buffer(narrowOop* buffered_addr, int dfs_index) {
  *(int32_t*)buffered_addr = (int32_t)dfs_index;
}

template <typename T> void StreamingArchiveHeapWriter::relocate_field_in_buffer(oop obj, T* field_addr_in_buffer, CHeapBitMap* oopmap) {
  if (obj != nullptr) {
    if (java_lang_Class::is_instance(obj)) {
      obj = HeapShared::scratch_java_mirror(obj);
      assert(obj != nullptr, "must be");
    }
    int dfs_index = *_dfs_order_table->get(obj);
    store_oop_in_buffer(field_addr_in_buffer, dfs_index);
    mark_oop_pointer<T>(field_addr_in_buffer, oopmap);
  } else {
    store_oop_in_buffer(field_addr_in_buffer, 0);
    mark_oop_pointer<T>(field_addr_in_buffer, oopmap);
  }
}

template <typename T> void StreamingArchiveHeapWriter::mark_oop_pointer(T* buffered_addr, CHeapBitMap* oopmap) {
  // Mark the pointer in the oopmap
  size_t buffered_offset = buffered_address_to_offset((address)buffered_addr);
  BitMap::idx_t idx = buffered_offset >> (UseCompressedOops ? 2 : 3);
  assert(idx < oopmap->size(), "overflow");
  oopmap->set_bit(idx);
}

void StreamingArchiveHeapWriter::update_header_for_buffered_addr(address buffered_addr, oop src_obj,  Klass* src_klass) {
  assert(UseCompressedClassPointers, "Archived heap only supported for compressed klasses");
  narrowKlass nk = ArchiveBuilder::current()->get_requested_narrow_klass(src_klass);

  oop fake_oop = cast_to_oop(buffered_addr);
  fake_oop->set_narrow_klass(nk);

  // We need to retain the identity_hash, because it may have been used by some hashtables
  // in the shared heap. This also has the side effect of pre-initializing the
  // identity_hash for all shared objects, so they are less likely to be written
  // into during run time, increasing the potential of memory sharing.
  if (src_obj != nullptr) {
    intptr_t src_hash = src_obj->identity_hash();
    fake_oop->set_mark(markWord::prototype().copy_set_hash(src_hash));
    assert(fake_oop->mark().is_unlocked(), "sanity");

    DEBUG_ONLY(intptr_t archived_hash = fake_oop->identity_hash());
    assert(src_hash == archived_hash, "Different hash codes: original " INTPTR_FORMAT ", archived " INTPTR_FORMAT, src_hash, archived_hash);
  }
}

class StreamingArchiveHeapWriter::EmbeddedOopRelocator: public BasicOopIterateClosure {
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
    oop obj = HeapAccess<>::oop_load(p);
    StreamingArchiveHeapWriter::relocate_field_in_buffer<T>(obj, (T*)(_buffered_obj + field_offset), _oopmap);
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
  if (java_lang_Class::is_instance(heap_object)) {
    patch_metadata(heap_object, archived_object, java_lang_Class::klass_offset());
    patch_metadata(heap_object, archived_object, java_lang_Class::array_klass_offset());
  } else if (java_lang_invoke_ResolvedMethodName::is_instance(heap_object)) {
    patch_metadata(heap_object, archived_object, java_lang_invoke_ResolvedMethodName::vmtarget_offset());
  }
}

// Update all oop fields embedded in the buffered objects
void StreamingArchiveHeapWriter::relocate_embedded_oops(GrowableArrayCHeap<oop, mtClassShared>* roots,
                                                        ArchiveHeapInfo* heap_info) {
  size_t oopmap_unit = (UseCompressedOops ? sizeof(narrowOop) : sizeof(oop));
  size_t heap_region_byte_size = _buffer_used;
  heap_info->oopmap()->resize(heap_region_byte_size / oopmap_unit);

  for (int i = 0; i < _source_objs->length(); i++) {
    oop src_obj = _source_objs->at(i);
    HeapShared::CachedOopInfo* info = HeapShared::archived_object_cache()->get(src_obj);
    assert(info != nullptr, "must be");
    address buffered_obj = offset_to_buffered_address<address>(info->buffer_offset());

    if (is_interned_string(src_obj)) {
      mark_interned_string(info->buffer_offset(), heap_info->oopmap());
    }

    update_header_for_buffered_addr(buffered_obj, src_obj, src_obj->klass());
    EmbeddedOopRelocator relocator(src_obj, buffered_obj, heap_info->oopmap());
    src_obj->oop_iterate(&relocator);
    patch_metadata(src_obj, buffered_obj);
  };

  // Relocate HeapShared::roots(), which is created in copy_roots_to_buffer() and
  // doesn't have a corresponding src_obj, so we can't use EmbeddedOopRelocator on it.
  address buffered_roots = offset_to_buffered_address<address>(_heap_roots_offset);
  update_header_for_buffered_addr(buffered_roots, nullptr, Universe::intArrayKlassObj());

  size_t total_bytes = (size_t)_buffer->length();
  log_bitmap_usage("oopmap", heap_info->oopmap(), total_bytes / (UseCompressedOops ? sizeof(narrowOop) : sizeof(oop)));
}

size_t StreamingArchiveHeapWriter::source_obj_to_buffered_offset(oop src_obj) {
  HeapShared::CachedOopInfo* p = HeapShared::archived_object_cache()->get(src_obj);
  if (p != nullptr) {
    return p->buffer_offset();
  } else {
    ShouldNotReachHere();
    return 0;
  }
}

address StreamingArchiveHeapWriter::source_obj_to_buffered_addr(oop src_obj) {
  return offset_to_buffered_address<address>(source_obj_to_buffered_offset(src_obj));
}

oop StreamingArchiveHeapWriter::buffered_addr_to_source_obj(address buffered_addr) {
  oop* p = _buffer_offset_to_source_obj_table->get(buffered_address_to_offset(buffered_addr));
  if (p != nullptr) {
    return *p;
  } else {
    return nullptr;
  }
}

void StreamingArchiveHeapWriter::populate_archive_heap_info(ArchiveHeapInfo* info) {
  assert(!info->is_used(), "only set once");

  size_t heap_region_byte_size = _buffer_used;
  assert(heap_region_byte_size > 0, "must archived at least one object!");

  info->set_buffer_region(MemRegion(offset_to_buffered_address<HeapWord*>(0),
                                    offset_to_buffered_address<HeapWord*>(_buffer_used)));
  info->set_heap_roots_offset(_heap_roots_offset);
  info->set_forwarding_offset(_forwarding_offset);
  info->set_roots_highest_dfs_offset(_roots_highest_dfs_offset);
  info->set_num_archived_objects(_source_objs->length() + 1);
}

#endif // INCLUDE_CDS_JAVA_HEAP
