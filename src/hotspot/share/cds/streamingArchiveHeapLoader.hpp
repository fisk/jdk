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
  template <bool allow_gc> friend class InflateReferenceOopClosure;
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
  static size_t _num_archived_objects;

  static size_t* _dfs_to_archive_offset_table;
  static void** _dfs_to_heap_object_table;

  static int* _roots_highest_dfs;

  static bool _waiting_for_iterator;

  static oop allocate_object(oopDesc* archive_object, size_t size, JavaThread* thread);

  static void switch_index_to_handle(int object_index);
  template <bool allow_gc> static oop heap_object_for_index(int object_index);
  template <bool allow_gc, bool is_dumping_cached_code> static void set_heap_object_for_index(int object_index, oop heap_object);
  template <bool allow_gc> static void replace_heap_object_for_index(int object_index, oop heap_object);

  class TracingObjectLoader {
    template <bool COOPS, bool allow_gc, bool is_dumping_cached_code> static oop materialize_object(oopDesc* archive_object, int archive_object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
    template <bool COOPS, bool allow_gc, bool is_dumping_cached_code> static oop materialize_object_inner(oopDesc* archive_object, int archive_object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
    template <bool COOPS, bool allow_gc> static void copy_object(oopDesc* archive_object, int archive_object_index, oop heap_object, size_t size, markWord mark, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
    template <bool COOPS, bool allow_gc, bool is_dumping_cached_code> static void drain_stack(Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
    static oop materialize_root(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);

  public:
    static oop materialize_object_transitive(oopDesc* archive_object, int object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
    static oop root(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
  };

  class IterativeObjectLoader {
    template <bool COOPS, bool allow_gc> static size_t initialize_range(int first_dfs, size_t first_offset, int last_dfs, JavaThread* thread);
    template <bool COOPS, bool allow_gc, bool is_dumping_cached_code> static size_t materialize_range(int first_dfs, size_t first_offset, int last_dfs, JavaThread* thread);
    template <bool COOPS, bool allow_gc> static void copy_object(oopDesc* archive_object, oop heap_object, size_t size);
    template <bool COOPS, bool allow_gc> static void initialize_object(oopDesc* archive_object, oop heap_object, int dfs_index, size_t size, JavaThread* thread);

  public:
    static void materialize_next_root(JavaThread* thread);
  };

  static void install_root(int root_index, oop heap_object);

  static int compute_roots_length();

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
};

#endif // SHARE_CDS_STREAMINGARCHIVEHEAPLOADER_HPP
