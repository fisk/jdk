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

#ifndef SHARE_CDS_STREAMINGARCHIVEHEAPLOADER_HPP
#define SHARE_CDS_STREAMINGARCHIVEHEAPLOADER_HPP

#include "cds/filemap.hpp"
#include "memory/allocation.hpp"
#include "memory/allStatic.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"
#include "utilities/stack.hpp"

class FileMapInfo;
class OopStorage;
class Thread;

struct CDSHeapTraversalEntry {
  int _pointee_index;
  int _base_index;
  uintptr_t _heap_field_offset_bytes;
};

class StreamingArchiveHeapLoader {
  friend class OopPatcherBase;
private:
  static FileMapRegion* _heap_region;
  static FileMapRegion* _bitmap_region;
  static FileMapRegion* _forwarding_region;
  static OopStorage* _oop_storage;
  static address _roots_old_addr;
  static oop* _roots;
  static BitMapView _oopmap;
  static bool _is_loaded;
  static bool _allow_gc;
  static bool _stop_background_processing;
  static bool _finished_processing;
  static int _last_batch_last_object;
  static int _current_batch_last_object;
  static int _current_root;
  static size_t _current_buffer_offset;
  static oop** _handles;
  static size_t _num_handles;

  static size_t* _dfs_to_archive_offset_table;
  static void** _dfs_to_heap_object_table;

  static int* _roots_highest_dfs;

  static void add_handle(oop* handle);

  template <bool allow_gc> static void* create_raw_handle(oop* handle, oop obj);
  template <bool allow_gc> static oop resolve_raw_handle(void* handle);

  template <bool allow_gc> static void* allocate_object_dfs(oopDesc* archive_object, size_t size, JavaThread* thread);
  template <bool COOPS, bool allow_gc> static oop materialize_object_dfs(oopDesc* archive_object, int archive_object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
  template <bool COOPS, bool allow_gc> static oop materialize_object_inner_dfs(oopDesc* archive_object, int archive_object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
  template <bool COOPS, bool allow_gc> static void copy_object_dfs(oopDesc* archive_object, int archive_object_index, void* heap_object_raw_handle, size_t size, markWord mark, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
  template <bool COOPS, bool allow_gc> static void drain_dfs_stack(Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
  static oop materialize_object_transitive_dfs(oopDesc* archive_object, int object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
  static oop materialize_root_dfs(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
  static oop root_dfs(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);

  static void install_root(int root_index);
  static void materialize_root_iter(JavaThread* thread);
  static size_t initialize_range(int first_dfs, size_t first_offset, int last_dfs, JavaThread* thread);
  static size_t materialize_range(int first_dfs, size_t first_offset, int last_dfs, JavaThread* thread);

  // TODO: Clean up header
  template <bool COOPS> void static copy_object_iter(oopDesc* archive_object, oop heap_object, size_t size);
  oop static allocate_object_iter(oopDesc* archive_object, int dfs_index, size_t size, JavaThread* thread);
  template <bool COOPS> void static initialize_object_iter(oopDesc* archive_object, oop heap_object, int dfs_index, size_t size, JavaThread* thread);
  static oop interned_string(oop heap_object, int dfs_index, JavaThread* thread);

  static int compute_roots_length();

  static oop root(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);

  static bool await_gc_enabled();
  static void await_finished_processing();

public:
  static void initialize_roots();
  static void enable_gc();
  static oop root(int root_index);
  static void materialize_objects();
  static void finish_materialize_objects();
  static bool is_loaded() { return _is_loaded; }

  // Leyden support
  static oop get_archived_object(int permanent_index);
  static void populate_permanent_object_table();
};

#endif // SHARE_CDS_STREAMINGARCHIVEHEAPLOADER_HPP
