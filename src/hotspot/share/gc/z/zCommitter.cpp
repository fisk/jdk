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

int ZHeatingRequestTreeComparator::cmp(zoffset first, zoffset second) {
  if (first < second) {
    // Start before second
    return -1;
  }

  if (first > second) {
    // Start after second
    return 1;
  }

  // Same position
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

  // Don't allocate things that are larger than the largest medium page size, in the lower address space
  return clamp(align_up(target_capacity / 64, ZGranuleSize), smallest_granule, largest_granule);
}

bool ZCommitter::should_commit(size_t granule, size_t capacity, size_t target_capacity, size_t curr_max_capacity, const ZMemoryPressureMetrics& metrics) {
  if (capacity > target_capacity) {
    return false;
  }

  const size_t new_capacity = capacity + granule;

  if (!ZAdaptiveHeap::explicit_max_capacity()) {
    // Be more mindful about increasing capacity with automatic heap sizing on

    if (ZAdaptiveHeap::is_memory_pressure_high(metrics)) {
      // When the pressure is "high", we resort to lazyness for committing to ensure that we
      // are not stepping off a cliff when the memory isn't needed.
      return false;
    }

    if (ZAdaptiveHeap::is_memory_pressure_concerning(metrics) &&
        new_capacity > ZHeap::heap()->heuristic_max_capacity()) {
      // When memory pressure gets concerning, we don't eagerly commit *past* the heuristic
      // max heap size, because we migh not need it and we want to avoid a ping pong situation.
      return false;
    }
  }

  return new_capacity <= target_capacity;
}

bool ZCommitter::should_uncommit(size_t granule, size_t capacity, size_t target_capacity) {
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
    const ZMemoryPressureMetrics metrics = ZAdaptiveHeap::memory_pressure_metrics();

    ZLocker<ZConditionLock> locker(&_lock);
    if (_stop) {
      return false;
    }

    if (!is_init_completed() || !ZAdaptiveHeap::can_adapt()) {
      // Don't start working until JVM is bootstrapped
      _lock.wait();
      continue;
    }

    if (should_commit(granule, capacity, target_capacity, curr_max_capacity, metrics)) {
      // At least one granule to commit
      return true;
    }

    if (should_uncommit(granule, capacity, target_capacity)) {
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

  if (target_capacity == 0) {
    // It is fairly easy the first GC...
    grow_target_capacity(heuristic_max_capacity);
    return;
  }

  // Uncommit 5% of the surplus at a time for a smooth capacity decline
  const size_t uncommit_fraction = 20;
  const size_t uncommit_request = align_up(surplus_capacity / uncommit_fraction, ZGranuleSize);

  if (target_capacity <= uncommit_request) {
    // Race; ignore uncommitting
    return;
  }

  // If the surplus capacity isn't over 5% of the capacity, the point of
  // uncommitting heuristically seems questionable and might just cause
  // pointless fluctuation.
  if (surplus_capacity < capacity / uncommit_fraction) {
    return;
  }

  shrink_target_capacity(target_capacity - uncommit_request);
}

void ZCommitter::heap_truncated(size_t capacity) {
  shrink_target_capacity(capacity);
}

void ZCommitter::set_target_capacity(size_t target_capacity) {
  ZLocker<ZConditionLock> locker(&_lock);
  Atomic::store(&_target_capacity, target_capacity);
}

void ZCommitter::grow_target_capacity(size_t target_capacity) {
  const size_t curr_max_capacity = _partition->current_max_capacity();
  const ZMemoryPressureMetrics metrics = ZAdaptiveHeap::memory_pressure_metrics();

  ZLocker<ZConditionLock> locker(&_lock);
  if (target_capacity < _target_capacity) {
    // Doesn't seem to be growing any more
    return;
  }
  Atomic::store(&_target_capacity, target_capacity);

  const size_t capacity = _partition->capacity();
  target_capacity = MIN2(target_capacity, curr_max_capacity);
  const size_t granule = commit_granule(capacity, target_capacity);

  if (should_commit(granule, capacity, target_capacity, curr_max_capacity, metrics)) {
    // At least one granule to commit
    _lock.notify_all();
  }
}

void ZCommitter::shrink_target_capacity(size_t target_capacity) {
  ZLocker<ZConditionLock> locker(&_lock);
  if (target_capacity > _target_capacity) {
    // Doesn't seem to be shrinking any more
    return;
  }
  Atomic::store(&_target_capacity, target_capacity);

  const size_t capacity = _partition->capacity();
  const size_t granule = commit_granule(capacity, target_capacity);

  if (should_uncommit(granule, capacity, target_capacity)) {
    // At least one granule to commit
    _lock.notify_all();
  }
}

void ZCommitter::remove_heating_request_range(const ZVirtualMemory& vmem) {
  ZArray<ZHeatingRequestNode*> to_remove;

  const zoffset start_inclusive = vmem.start();
  const zoffset end_inclusive = vmem.start() + (vmem.size() - ZGranuleSize);
  _heating_requests.visit_range_in_order(start_inclusive, end_inclusive, [&](const ZHeatingRequestNode* node) {
    // Const cast the node, we only use it to modify the tree after
    // visit_range_in_order is completed.
    to_remove.push(const_cast<ZHeatingRequestNode*>(node));
  });

  for (ZHeatingRequestNode* node: to_remove) {
    _heating_requests.remove(node);
  }
}

void ZCommitter::register_heating_request(const ZVirtualMemory& vmem) {
  ZLocker<ZConditionLock> locker(&_lock);
  if (_stop) {
    // Don't add more requests during termination
    return;
  }

  zoffset insert_start = vmem.start();
  zoffset_end insert_end = zoffset_end(vmem.start()) + vmem.size();

  // Remove overlap on the left
  if (vmem.start() > zoffset(0)) {
    ZHeatingRequestNode* left_node = _heating_requests.closest_leq(vmem.start() - ZGranuleSize);
    if (left_node != nullptr) {
      ZVirtualMemory closest_left(left_node->key(), left_node->val());
      if (closest_left.overlaps(vmem)) {
        insert_start = closest_left.start();
        insert_end = MAX2(zoffset_end(left_node->key()) + left_node->val(), insert_end);
        _heating_requests.remove(left_node);
      }
    }
  }

  // Remove overlap on the right
  if (size_t(vmem.start()) + vmem.size() < ZAddressOffsetMax) {
    ZHeatingRequestNode* right_node = _heating_requests.closest_leq(vmem.start() + vmem.size());
    if (right_node != nullptr) {
      insert_end = MAX2(zoffset_end(right_node->key()) + right_node->val(), insert_end);
      _heating_requests.remove(right_node);
    }
  }

  remove_heating_request_range(vmem);

  ZHeatingRequestNode* const new_node = _heating_requests.allocate_node(insert_start, size_t(insert_end) - size_t(insert_start));
  _heating_requests.insert(insert_start, new_node);
}

ZVirtualMemory ZCommitter::pop_heating_request() {
  assert(has_heating_request(), "precondition");

  ZHeatingRequestNode* const node = _heating_requests.leftmost();

  ZVirtualMemory vmem(node->key(), node->val());
  _heating_requests.remove(node);

  const size_t max_heating = 16 * ZGranuleSize;

  if (vmem.size() > max_heating) {
    // Reinsert remaining part if there is a lot of work to do
    const ZVirtualMemory remaining = vmem.shrink_from_back(vmem.size() - max_heating);
    ZHeatingRequestNode* const new_node = _heating_requests.allocate_node(remaining.start(), remaining.size());
    _heating_requests.insert(remaining.start(), new_node);
  }

  return vmem;
}

void ZCommitter::remove_heating_request(const ZVirtualMemory& vmem) {
  ZLocker<ZConditionLock> locker(&_lock);

  while (!_currently_heating.is_null() && vmem.overlaps(_currently_heating)) {
    // Wait while currently heating overlaps with this vmem
    _lock.wait();
  }

  // Cut off overlap on the left
  if (vmem.start() > zoffset(0)) {
    ZHeatingRequestNode* left_node = _heating_requests.closest_leq(vmem.start() - ZGranuleSize);
    if (left_node != nullptr) {
      const ZVirtualMemory closest_left(left_node->key(), left_node->val());
      if (closest_left.overlaps(vmem)) {
        const size_t overlap_size = size_t(closest_left.start()) + size_t(closest_left.size()) - size_t(vmem.start());
        _heating_requests.upsert(closest_left.start(), closest_left.size() - overlap_size);
      }
    }
  }

  // Cut off overlap on the right
  if (size_t(vmem.start()) + vmem.size() < ZAddressOffsetMax) {
    ZHeatingRequestNode* right_node = _heating_requests.closest_leq(vmem.start() + vmem.size());
    if (right_node != nullptr) {
      const ZVirtualMemory closest_right(right_node->key(), right_node->val());
      if (closest_right.overlaps(vmem)) {
        const size_t overlap_size = size_t(vmem.start()) + size_t(vmem.end()) - size_t(closest_right.start());
        _heating_requests.remove(right_node);
        const zoffset new_node_start = vmem.start() + vmem.size();
        ZHeatingRequestNode* const new_node = _heating_requests.allocate_node(new_node_start, closest_right.size() - overlap_size);
        _heating_requests.insert(new_node_start, new_node);
      }
    }
  }

  remove_heating_request_range(vmem);
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
      const ZMemoryPressureMetrics metrics = ZAdaptiveHeap::memory_pressure_metrics();

      if (is_stop_requested()) {
        return;
      }

      if (last_target_capacity != 0 && last_target_capacity != target_capacity) {
        // Printouts look better when flushing across target capacity changes
        break;
      }

      last_target_capacity = target_capacity;

      // Prioritize committing memory if needed
      if (uncommitted == 0 && should_commit(granule, capacity, target_capacity, curr_max_capacity, metrics)) {
        committed += _partition->commit(granule, target_capacity);
        assert(!should_uncommit(granule, capacity + granule, target_capacity), "commit rule mismatch");
        continue;
      }

      // Secondary priority is to heat pages
      if (should_heat()) {
        heated += process_heating_request();
        continue;
      }

      // The lowest priority is uncommitting memory if needed
      if (committed == 0 && should_uncommit(granule, capacity, target_capacity)) {
        uncommitted += _partition->uncommit(granule);
        assert(!should_commit(granule, capacity - granule, target_capacity, curr_max_capacity, metrics), "uncommit rule mismatch");
        continue;
      }

      break;
    }

    if (committed > 0) {
      log_info(gc, heap)("Memory Worker (%d) Committed: %zuM(%.0f%%)",
                         _id, committed / M, percent_of(committed, last_target_capacity));
    }

    if (uncommitted > 0) {
      log_info(gc, heap)("Memory Worker (%d) Uncommitted: %zuM(%.0f%%)",
                         _id, uncommitted / M, percent_of(uncommitted, last_target_capacity));
    }

    if (heated > 0) {
      log_info(gc, heap)("Memory Worker (%d) Heated: %zuM(%.0f%%)",
                         _id, heated / M, percent_of(heated, last_target_capacity));
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
