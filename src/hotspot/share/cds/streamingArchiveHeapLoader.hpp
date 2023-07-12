/*
 * Copyright (c) 2018, 2024, Oracle and/or its affiliates. All rights reserved.
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

// The streaming archive heap loader loads Java objects using normal allocations. It requires the objects
// to be ordered in DFS order already at dump time, given the set of roots into the archived heap.
// Since the objects are ordered in DFS order, that means that walking them linearly through the archive
// is equivalent to performing a DFS traversal, but without pushing and popping anything.
//
// The advantage of this pre-ordering, other than the obvious locality improvement, is that we can have
// a separate thread, the CDSThread, perform this walk, in a way that allows us to split the archived
// heap into three separate zones. The first zone contains objects that have been transitively materialized,
// the second zone contains objects that are currently being materialized, and the last zone contains
// objects that have not and are not about to be touched by the CDS thread.
// Whenever a new root is traversed by the CDS thread, the zones are shifted atomically under a lock.
//
// Visualization of the three zones:
//
// +--------------------------------------+-------------------------+----------------------------------+
// |      transitively materialized       | currently materializing |        not yet materialized      |
// +--------------------------------------+-------------------------+----------------------------------+
//
// Being able to split the memory into these three zones, allows the bootstrapping thread and potential
// other threads to be able to, under a lock, traverse a root, and know how to coordinate with the
// concurrent CDS thread. Whenever the traversal finds an object in the "transitively materialized"
// zone, then we know such objects don't need any processing at all. As for "currently materializing",
// we know that if we just stay out of the way and let the CDSThread finish its current root, then
// the transitive closure of such objects will be materialized. And the CDSThread can materialize faster
// then the rest as it doesn't need to perform any traversal. Finally, as for objects in the "not yet
// materialized" zone, we know that we can trace through it without stepping on the feed of the CDSThread
// which has published it won't be tracing anything in there.
//
// What we get from this, is fast iterative traversal from the CDS thread (IterativeObjectLoader)
// while allowing lazyness and concurrency with the rest of the program (TracingObjectLoader).
// This way the CDS thread can remove the bulk of the work of materializing the Java objects from
// the critical bootstrapping thread.
//
// When we start materializing objects, we have not yet come to the point in the bootstrapping where
// GC is allowed. This is a two edged sword. On the one hand side, we can materialize objects faster
// when we know there is no GC to coordinate with, but on the other hand side, if we need to perform
// a GC when allocating memory for archived objects, we will bring down the entire JVM. To deal with this,
// the CDS thread asks the GC for a budget of bytes it is allowed to allocate before GC is allowed.
// When we get to the point in the bootstrapping where GC is allowed, we resume materializing objects
// that didn't fit in the budget. Before we let the application run, we force materialization of any
// remaining objects that have not been materialized by the CDS thread yet, so that we don't get
// surprising OOMs due to object materialization while the program is running.
//
// The object format of the archived heap is similar to a normal object. However, references are encoded
// as DFS indices, which in the end map to what index the object is in the buffer, as they are laid out
// in DFS order. The DFS indices start at 1 for the first object, and hence the number 0 represents
// null. The DFS index of objects is a core identifier of objects in this approach. From this index
// it is possible to find out what offset the archived object has into the buffer, as well as finding
// mappings to Java heap objects that have been materialized.
//
// The table mapping DFS indices to Java heap objects is filled in when an object is allocated.
// Materializing objects involves allocating the object, initializing it, and linking it with other
// objects. Since linking the object requires whatever is being referenced to be at least allocated,
// the iterative traversal will first allocate all of the objects in its zone being worked on, and then
// perform initialization and linking in a second pass. What these passes have in common is that they
// are trivially parallelizable, should we ever need to do that. The tracing materialization links
// objects when going "back" in the DFS traversal.
//
// The forwarding information for the mechanism contains raw oops before GC is allowed, and as we
// enable GC in the bootstrapping, all raw oops are handleified using OopStorage. All handles are
// handed back from the CDS thread when materialization has finished. The switch from raw oops to
// using OopStorage handles, happens under a lock while no iteration nor tracing is allowed.
//
// The initialization code is also performed in a faster way when the GC is not allowed. In particular,
// before GC is allowed, we perform raw memcpy of the archived object into the Java heap. Then the
// object is initialized with IS_DEST_UNINITIALIZED stores. The assumption made here is that before
// any GC activity is allowed, we shouldn't have to worry about concurrent GC threads scanning the
// memory and getting tripped up by that. Once GC is enabled, we revert to a bit more careful approach
// that uses a pre-computed bitmap to find the holes where oops go, and carefully copy only the
// non-oop information with memcpy, while the oops are set separately with HeapAccess stores that
// should be able to cope well with concurrent activity.
//
// The same bitmap that tracks where there are oops, is reused also for signalling which string
// objects should be interned. From the dump, some referenced strings were interned. This is
// really an identity property. We don't need to dump the entire string table as a way of communicating
// this identity property. Instead we intern strings on-the-fly, exploiting the dynamic object
// level linking that this approach has chosen to our advantage.

class FileMapInfo;
class OopStorage;
class Thread;

struct CDSHeapTraversalEntry {
  int _pointee_object_index;
  int _base_object_index;
  uintptr_t _heap_field_offset_bytes;
};

class StreamingArchiveHeapLoader {
  friend class InflateReferenceOopClosure;
private:
  static FileMapRegion* _heap_region;
  static FileMapRegion* _bitmap_region;
  static OopStorage* _oop_storage;
  static int* _roots_archive;
  static oop* _roots_heap;
  static BitMapView _oopmap;
  static bool _is_loaded;
  static bool _allow_gc;
  static bool _stop_background_processing;
  static bool _finished_processing;
  static int _previous_batch_last_object_index;
  static int _current_batch_last_object_index;
  static size_t _allocated_words;
  static int _current_root_index;
  static size_t _num_archived_objects;

  static size_t* _object_index_to_buffer_offset_table;
  static void** _object_index_to_heap_object_table;
  static int* _root_highest_object_index_table;

  static bool _waiting_for_iterator;

  static oop allocate_object(oopDesc* archive_object, size_t size, JavaThread* thread);
  static int object_index_for_root_index(int root_index);
  static int highest_object_index_for_root_index(int root_index);
  static size_t buffer_offset_for_object_index(int object_index);
  static oopDesc* archive_object_for_object_index(int object_index);
  static size_t buffer_offset_for_archive_object(oopDesc* archive_object);
  static BitMap::idx_t obj_bit_idx_for_buffer_offset(size_t buffer_offset);

  static void switch_object_index_to_handle(int object_index);
  static oop heap_object_for_object_index(int object_index, bool allow_gc);
  static oop heap_object_for_object_index(int object_index);
  static void set_heap_object_for_object_index(int object_index, oop heap_object, bool allow_gc);
  static void replace_heap_object_for_object_index(int object_index, oop heap_object, bool allow_gc);

  class TracingObjectLoader {
    static oop materialize_object(int object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread, bool allow_gc);
    static oop materialize_object_inner(int object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread, bool allow_gc);
    template <bool COOPS>
    static void copy_object(int object_index, oopDesc* archive_object, oop heap_object, size_t size, markWord mark, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread, bool allow_gc);
    static void drain_stack(Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread, bool allow_gc);
    static oop materialize_root(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);

    static void wait_for_iterator();

  public:
    static oop materialize_object_transitive(int object_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
    static oop root(int root_index, Stack<CDSHeapTraversalEntry, mtClassShared>& dfs_stack, JavaThread* thread);
  };

  class IterativeObjectLoader {
    static void copy_object(oopDesc* archive_object, oop heap_object, size_t size, bool allow_gc);
    static void initialize_range(int first_object_index, int last_object_index, JavaThread* thread, bool allow_gc);
    static size_t materialize_range(int first_object_index, int last_object_index, JavaThread* thread, bool allow_gc);

  public:
    static void materialize_next_root(JavaThread* thread);
  };

  static void install_root(int root_index, oop heap_object);

  static int compute_roots_length();

  static bool await_gc_enabled();
  static void await_finished_processing();

public:
  static void initialize();
  static void enable_gc();
  static oop root(int root_index);
  static void materialize_objects();
  static void finish_materialize_objects();
  static bool is_loaded() { return _is_loaded; }
};

#endif // SHARE_CDS_STREAMINGARCHIVEHEAPLOADER_HPP
