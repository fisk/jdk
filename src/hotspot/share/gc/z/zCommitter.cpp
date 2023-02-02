/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/z/zAdaptiveHeap.hpp"
#include "gc/z/zCommitter.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAllocator.hpp"
#include "gc/z/zVirtualMemory.inline.hpp"
#include "runtime/init.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/rbTree.inline.hpp"

int ZHeatingRequestTreeComparator::cmp(ZVirtualMemory first, ZVirtualMemory second) {
  const zoffset start = first.start();
  if (start < second.start()) {
    // Start before second
    return -1;
  }

  if (start >= second.end()) {
    // Start after second
    return 1;
  }

  return 0;
}

ZCommitter::ZCommitter(uint32_t id, ZPartition* partition)
  : _id(id),
    _partition(partition),
    _lock(),
    _heating_requests(),
    _target_capacity(0),
    _stop(false),
    _currently_heating() {
  set_name("ZCommitter#%u", _id);
  create_and_start();
}

bool ZCommitter::is_stop_requested() {
  ZLocker<ZConditionLock> locker(&_lock);
  return _stop;
}

size_t ZCommitter::commit_granule(size_t capacity, size_t target_capacity) {
  const size_t smallest_granule = ZGranuleSize;
  const size_t largest_granule = MAX2(ZPageSizeMedium, smallest_granule);

  const size_t heuristic_max_capacity = _partition->heuristic_max_capacity();

  // Don't allocate things that are larger than the largest medium page size, in the lower address space
  return clamp(align_up(heuristic_max_capacity / 64, ZGranuleSize), smallest_granule, largest_granule);
}

bool ZCommitter::should_commit(size_t granule, size_t capacity, size_t target_capacity, size_t curr_max_capacity) {
  const size_t new_capacity = capacity + granule;

  if (!ZAdaptiveHeap::explicit_max_capacity() &&
      new_capacity > size_t(double(curr_max_capacity) * (1.0 - ZMemoryCriticalThreshold))) {
    // Don't speculatively commit memory around the machine boundaries; it interacts poorly with
    // panic uncommitting around the same boundaries. When a user is this close to falling over,
    // this instead acts as an implicit allocation pacer to try to avoid an allocation stall.
    return false;
  }

  return new_capacity <= target_capacity;
}

bool ZCommitter::should_uncommit(size_t granule, size_t capacity, size_t target_capacity, size_t curr_max_capacity) {
  if (!ZUncommit) {
    // Uncommit explicitly disabled; don't uncommit.
    return false;
  }

  if (granule > capacity) {
    // Seems certainly small enough
    return false;
  }

  const size_t new_capacity = capacity - granule;

  return new_capacity > target_capacity;
}

bool ZCommitter::should_heat() {
  ZLocker<ZConditionLock> locker(&_lock);
  return has_heating_request();
}

bool ZCommitter::has_heating_request() {
  return _heating_requests.size() != 0;
}

bool ZCommitter::peek() {
  for (;;) {
    const size_t capacity = _partition->capacity();
    const size_t curr_max_capacity = _partition->current_max_capacity();
    const size_t target_capacity = MIN2(Atomic::load(&_target_capacity), curr_max_capacity);
    const size_t granule = commit_granule(capacity, target_capacity);

    ZLocker<ZConditionLock> locker(&_lock);
    if (_stop) {
      return false;
    }

    if (!is_init_completed() || !ZAdaptiveHeap::can_adapt()) {
      // Don't start working until JVM is bootstrapped
      _lock.wait();
      continue;
    }

    if (should_commit(granule, capacity, target_capacity, curr_max_capacity)) {
      // At least one granule to commit
      return true;
    }

    if (should_uncommit(granule, capacity, target_capacity, curr_max_capacity)) {
      // At least one granule to uncommit
      return true;
    }

    if (has_heating_request()) {
      return true;
    }

    _lock.wait();
  }
}

size_t ZCommitter::target_capacity() {
  return Atomic::load(&_target_capacity);
}

void ZCommitter::heap_resized(size_t capacity, size_t heuristic_max_capacity) {
  if (capacity <= heuristic_max_capacity) {
    // Heap increases are handled lazily through the director monitoring
    // This allows growing to be more vigilant and not have to wait for
    // a GC before growing can commence. Uncommitting though, is less urgent.
    return;
  }

  // If the heuristics have said the heap should shrink, and the shrinking
  // goes below the capacity, then we would like to uncommit a fraction of
  // that capacity, so that the heap memory usage slowly goes down over time,
  // converging at a lower capacity.

  // Set up direct uncommit to shrink the heap
  const size_t target_capacity = Atomic::load(&_target_capacity);
  const size_t surplus_capacity = capacity - heuristic_max_capacity;

  // Uncommit 5% of the surplus at a time for a smooth capacity decline
  const size_t uncommit_fraction = 20;
  const size_t uncommit_request = align_up(surplus_capacity / uncommit_fraction, ZGranuleSize);

  if (target_capacity != 0 && target_capacity < uncommit_request) {
    // Race; ignore uncommitting
    return;
  }

  // If the surplus capacity isn't over 5% of the capacity, the point of
  // uncommitting heuristically seems questionable and might just cause
  // pointless fluctuation.
  if (surplus_capacity < capacity / uncommit_fraction) {
    return;
  }

  set_target_capacity(target_capacity - uncommit_request);
}

void ZCommitter::set_target_capacity(size_t target_capacity) {
  const size_t curr_max_capacity = _partition->current_max_capacity();

  ZLocker<ZConditionLock> locker(&_lock);
  Atomic::store(&_target_capacity, target_capacity);

  const size_t capacity = _partition->capacity();
  target_capacity = MIN2(target_capacity, curr_max_capacity);
  const size_t granule = commit_granule(capacity, target_capacity);

  if (should_commit(granule, capacity, target_capacity, curr_max_capacity)) {
    // At least one granule to commit
    _lock.notify_all();
  }

  if (should_uncommit(granule, capacity, target_capacity, curr_max_capacity)) {
    // At least one granule to commit
    _lock.notify_all();
  }
}

void ZCommitter::register_heating_request(const ZVirtualMemory& vmem) {
  ZLocker<ZConditionLock> locker(&_lock);
  if (_stop) {
    // Don't add more requests during termination
    return;
  }
  _heating_requests.upsert(vmem, true);
}

ZVirtualMemory ZCommitter::pop_heating_request() {
  assert(has_heating_request(), "precondition");

  ZHeatingRequestNode* const node = _heating_requests.leftmost();

  ZVirtualMemory vmem = node->key();
  const ZVirtualMemory popped_vmem = vmem.shrink_from_front(ZGranuleSize);
  if (vmem.size() == 0) {
    // Popped the last memory in node
    _heating_requests.remove(node);
  } else {
    // Memory still left, create and replace node
    ZHeatingRequestNode* const new_node = _heating_requests.allocate_node(popped_vmem, true);
    _heating_requests.replace_at_cursor(new_node, _heating_requests.cursor(node));
  }

  return popped_vmem;
}

void ZCommitter::remove_heating_request(const ZVirtualMemory& vmem) {
  const auto remove_vmem_entires = [&](const ZVirtualMemory& remove_vmem) {
    ZArray<ZHeatingRequestNode*> to_remove;

    // ZHeatingRequestTreeComparator::cmp only checks if a node contains the
    // lookup keys start(). Construct virtual vmems representing the first and
    // last granule.
    const ZVirtualMemory first_vmem = remove_vmem.first_part(ZGranuleSize);
    const ZVirtualMemory last_vmem = remove_vmem.last_part(remove_vmem.size() - ZGranuleSize);
    _heating_requests.visit_range_in_order(first_vmem, last_vmem,[&](const ZHeatingRequestNode* node) {
      // Const cast the node, we only use it to modify the tree after
      // visit_range_in_order is completed.
      to_remove.push(const_cast<ZHeatingRequestNode*>(node));
    });

    for (ZHeatingRequestNode* node : to_remove) {
      assert(node->key().overlaps(remove_vmem), "must overlap");
      ZVirtualMemory node_vmem = node->key();
      // First remove the node
      _heating_requests.remove(node);
      if (remove_vmem.contains(node_vmem)) {
        // Memory in node is completely contain by remove_vmem,
        // nothing to reinsert.
        continue;
      }

      if (node_vmem.start() < remove_vmem.start()) {
        // Keep part of node_vmem front
        const size_t prefix_size = remove_vmem.start() - node_vmem.start();
        _heating_requests.upsert(node_vmem.shrink_from_front(prefix_size), true);
      }

      if (node_vmem.end() > remove_vmem.end()) {
        // Keep part of node_vmem back
        const size_t suffix_size = node_vmem.end() - remove_vmem.end();
        _heating_requests.upsert(node_vmem.shrink_from_back(suffix_size), true);
      }

      assert(remove_vmem.contains(node_vmem), "what is left must be a subset of remove_vmem");
    }
  };

  ZLocker<ZConditionLock> locker(&_lock);
  if (!_currently_heating.is_null() && vmem.overlaps(_currently_heating)) {
    ZVirtualMemory to_remove = vmem;

    if (to_remove.start() < _currently_heating.start()) {
      // Remove prefix
      const size_t prefix_size = _currently_heating.start() - to_remove.start();
      remove_vmem_entires(to_remove.shrink_from_front(prefix_size));
    }

    if (to_remove.end() > _currently_heating.end()) {
      // Remove suffix
        const size_t suffix_size = to_remove.end() - _currently_heating.end();
        remove_vmem_entires(to_remove.shrink_from_back(suffix_size));
    }

    assert(_currently_heating.contains(to_remove), "must only have _currently_heating left");

    do {
      // Wait until heating is finished
      _lock.wait();
    } while (!_currently_heating.is_null() && _currently_heating.contains(to_remove));
  } else {
    // No heating of memory we are removing, just remove everything
    remove_vmem_entires(vmem);
  }

  postcond(_currently_heating.is_null() || !vmem.overlaps(_currently_heating));
#ifdef ASSERT
  const ZVirtualMemory first_vmem = vmem.first_part(ZGranuleSize);
  const ZVirtualMemory last_vmem = vmem.last_part(vmem.size() - ZGranuleSize);
  _heating_requests.visit_range_in_order(first_vmem, last_vmem,[&](const ZHeatingRequestNode* node) {
    // Should contain no nodes with memory that overlaps with vmem
    ShouldNotReachHere();
  });
#endif
}

size_t ZCommitter::process_heating_request() {
  ZVirtualMemory vmem;
  {
    ZLocker<ZConditionLock> locker(&_lock);
    if (!has_heating_request()) {
      // Unmapping removed the request; bail
      return 0;
    }

    assert(has_heating_request(), "who else processed it?");

    vmem = pop_heating_request();

    assert(_currently_heating.is_null(), "must be");
    _currently_heating = vmem;
  }

  _partition->heat_memory(vmem);

  {
    ZLocker<ZConditionLock> locker(&_lock);
    _lock.notify_all();
    _currently_heating = {};
  }

  return vmem.size();
}

void ZCommitter::run_thread() {
  for (;;) {
    if (!peek()) {
      // Stop
      return;
    }

    size_t committed = 0;
    size_t uncommitted = 0;
    size_t heated = 0;
    size_t last_target_capacity = 0;

    for (;;) {
      const size_t capacity = _partition->capacity();
      const size_t curr_max_capacity = _partition->current_max_capacity();
      const size_t target_capacity = MIN2(Atomic::load(&_target_capacity), curr_max_capacity);
      const size_t granule = commit_granule(capacity, target_capacity);

      if (is_stop_requested()) {
        return;
      }

      if (last_target_capacity != 0 && last_target_capacity != target_capacity) {
        // Printouts look better when flushing across target capacity changes
        break;
      }

      last_target_capacity = target_capacity;

      // Prioritize committing memory if needed
      if (uncommitted == 0 && should_commit(granule, capacity, target_capacity, curr_max_capacity)) {
        committed += _partition->commit(granule);
        assert(!should_uncommit(granule, capacity + granule, target_capacity, curr_max_capacity), "commit rule mismatch");
        continue;
      }

      // Secondary priority is to heat pages
      if (should_heat()) {
        heated += process_heating_request();
        continue;
      }

      // The lowest priority is uncommitting memory if needed
      if (committed == 0 && should_uncommit(granule, capacity, target_capacity, curr_max_capacity)) {
        uncommitted += _partition->uncommit(granule);
        assert(!should_commit(granule, capacity - granule, target_capacity, curr_max_capacity), "uncommit rule mismatch");
        continue;
      }

      break;
    }

    if (committed > 0) {
      log_info(gc, heap)("Committed: %zuM(%.0f%%)",
                         committed / M, percent_of(committed, last_target_capacity));
    }

    if (uncommitted > 0) {
      log_info(gc, heap)("Uncommitted: %zuM(%.0f%%)",
                         uncommitted / M, percent_of(uncommitted, last_target_capacity));
    }

    if (heated > 0) {
      log_info(gc, heap)("Heated: %zuM(%.0f%%)",
                         heated / M, percent_of(heated, last_target_capacity));
    }
  }
}

void ZCommitter::terminate() {
  ZLocker<ZConditionLock> locker(&_lock);
  _stop = true;
  _lock.notify_all();

  _heating_requests.remove_all();

  while (!_currently_heating.is_null()) {
    // Trying to unmap what's currently being heated; calm down!
    _lock.wait();
  }
}
