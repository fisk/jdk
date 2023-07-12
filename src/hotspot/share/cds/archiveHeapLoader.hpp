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
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License * version 2 for more details (a copy is included in the LICENSE file that
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

#ifndef SHARE_CDS_ARCHIVEHEAPLOADER_HPP
#define SHARE_CDS_ARCHIVEHEAPLOADER_HPP

#include "cds/filemap.hpp"
#include "memory/allocation.hpp"
#include "memory/allStatic.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/macros.hpp"
#include "utilities/stack.hpp"

class FileMapInfo;
class OopStorage;
class Thread;

struct CDSHeapTraversalEntry {
  oopDesc* _archive_pointee_object;
  void* _heap_object_handle;
  uintptr_t _heap_field_offset_bytes;
};

class ArchiveHeapLoader {
  friend class OopPatcherBase;
private:
  static FileMapRegion* _heap_region;
  static FileMapRegion* _bitmap_region;
  static OopStorage* _oop_storage;
  static address _roots_old_addr;
  static oop* _roots;
  static BitMapView _oopmap;
  static bool _is_loaded;
  static bool _allow_gc;
  static bool _stop_background_processing;
  static int _lowest_finished_root;

  template <bool allow_gc> static void* create_raw_handle(oop* handle, oop obj);
  template <bool allow_gc> static oop resolve_raw_handle(void* handle);

  template <bool allow_gc> static void* allocate_object(oopDesc* archive_object, size_t size, JavaThread* thread);
  template <bool COOPS, bool allow_gc> static oop materialize_object(oopDesc* archive_object, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
  template <bool COOPS, bool allow_gc> static oop materialize_object_inner(oopDesc* archive_object, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
  template <bool COOPS, bool allow_gc> static void copy_object(oopDesc* archive_object, void* heap_object_raw_handle, size_t size, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
  template <bool COOPS, bool allow_gc> static void drain_dfs_stack(Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);

  static int compute_roots_length();

  static oop materialize_root(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
  static oop root(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);

  static bool await_gc_enabled();

public:
  static void initialize_oop_storage();
  static void initialize_roots();
  static void enable_gc();
  static oop root(int root_index);
  static void materialize_objects();
  static void finish_materialize_objects();
  static bool is_loaded() { return _is_loaded; }
};

//class ArchiveHeapLoader : AllStatic {
//public:
//  // Can this VM load the objects from archived heap region into the heap at start-up?
//  static bool can_load()  NOT_CDS_JAVA_HEAP_RETURN_(false);
//
//  static bool is_loaded() {
//    CDS_JAVA_HEAP_ONLY(return _is_loaded;)
//    NOT_CDS_JAVA_HEAP(return false;)
//  }
//
//#if INCLUDE_CDS_JAVA_HEAP
//
//private:
//  static bool _is_loaded;
//public:
//  static bool load_heap_region(char* stream, size_t bytesize);
//
//#endif // INCLUDE_CDS_JAVA_HEAP
//
//};

#endif // SHARE_CDS_ARCHIVEHEAPLOADER_HPP
