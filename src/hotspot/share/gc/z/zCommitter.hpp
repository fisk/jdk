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
#include "gc/z/zVirtualMemory.hpp"
#include "memory/allocation.hpp"
#include "utilities/rbTree.hpp"

class ZPartition;

class ZHeatingRequestTreeComparator : public AllStatic {
public:
  // Until the RBTree gets support for different key comparisons we only check
  // if the first.start() value is contained in second.
  static int cmp(ZVirtualMemory first, ZVirtualMemory second);
};

using ZHeatingRequestTree = RBTreeCHeap<ZVirtualMemory, bool, ZHeatingRequestTreeComparator, mtGC>;
using ZHeatingRequestNode = RBNode<ZVirtualMemory, bool>;

class ZCommitter : public ZThread {
private:
  const uint32_t      _id;
  ZPartition* const   _partition;
  ZConditionLock      _lock;
  ZHeatingRequestTree _heating_requests;
  volatile size_t     _target_capacity;
  bool                _stop;
  ZVirtualMemory      _currently_heating;

  bool is_stop_requested();
  size_t commit_granule(size_t capacity, size_t target_capacity);
  bool should_commit(size_t granule, size_t capacity, size_t target_capacity, size_t curr_max_capacity);
  bool should_uncommit(size_t granule, size_t capacity, size_t target_capacity, size_t curr_max_capacity);
  bool should_heat();
  bool has_heating_request();
  ZVirtualMemory pop_heating_request();
  size_t process_heating_request();
  bool peek();

protected:
  virtual void run_thread();
  virtual void terminate();

public:
  ZCommitter(uint32_t id, ZPartition* partition);

  void heap_resized(size_t capacity, size_t heuristic_max_capacity);
  void set_target_capacity(size_t target_capacity);
  size_t target_capacity();

  void register_heating_request(const ZVirtualMemory& vmem);
  void remove_heating_request(const ZVirtualMemory& vmem);
};

#endif // SHARE_GC_Z_ZCOMMITTER_HPP
