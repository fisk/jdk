/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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
 */

#ifndef SHARE_GC_Z_ZCOMMITTER_HPP
#define SHARE_GC_Z_ZCOMMITTER_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zArray.hpp"
#include "gc/z/zList.hpp"
#include "gc/z/zLock.hpp"
#include "gc/z/zThread.hpp"
#include "memory/allocation.hpp"
#include "utilities/rbTree.hpp"

class ZPage;
class ZPageAllocator;

class ZHeatingRequestTreeComparator : public AllStatic {
public:
  static int cmp(zoffset first, zoffset second) {
    if (first < second) {
      return 1;
    }

    if (first > second) {
      return -1;
    }

    return 0;
  }
};

using ZHeatingRequestTree = RBTreeCHeap<zoffset, size_t, ZHeatingRequestTreeComparator, mtGC>;
using ZHeatingRequestNode = RBNode<zoffset, size_t>;

class ZCommitter : public ZThread {
private:
  ZPageAllocator* const _page_allocator;
  ZConditionLock        _lock;
  ZHeatingRequestTree   _heating_requests;
  volatile size_t       _target_capacity;
  bool                  _stop;
  zaddress              _currently_heating;

  bool is_stop_requested();
  size_t commit_granule(size_t capacity, size_t target_capacity);
  bool should_commit(size_t granule, size_t capacity, size_t target_capacity, size_t curr_max_capacity);
  bool should_uncommit(size_t granule, size_t capacity, size_t target_capacity, size_t curr_max_capacity);
  bool should_heat();
  bool has_heating_request();
  zoffset pop_heating_request(size_t& size);
  size_t process_heating_request();
  bool peek();

protected:
  virtual void run_thread();
  virtual void terminate();

public:
  ZCommitter(ZPageAllocator* page_allocator);

  void heap_resized(size_t capacity, size_t heuristic_max_capacity);
  void set_target_capacity(size_t target_capacity);
  size_t target_capacity();

  void register_heating_request(const ZPage* page);
  void remove_heating_request(const ZPage* page);
};

#endif // SHARE_GC_Z_ZCOMMITTER_HPP
