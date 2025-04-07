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
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAllocator.hpp"
#include "gc/z/zCommitter.hpp"
#include "runtime/init.hpp"
#include "utilities/rbTree.inline.hpp"

ZCommitter::ZCommitter(ZPageAllocator* page_allocator)
  : _page_allocator(page_allocator),
    _lock(),
    _heating_requests(),
    _target_capacity(0),
    _stop(false),
    _currently_heating(zaddress::null) {
  set_name("ZCommitter");
  create_and_start();
}

bool ZCommitter::is_stop_requested() {
  ZLocker<ZConditionLock> locker(&_lock);
  return _stop;
}

size_t ZCommitter::commit_granule(size_t capacity, size_t target_capacity) {
  const size_t smallest_granule = ZGranuleSize;
  const size_t largest_granule = MAX2(ZPageSizeMedium, smallest_granule);

  const size_t heuristic_max_capacity = _page_allocator->heuristic_max_capacity();

  // Don't allocate things that are larger than the largest medium page size, in the lower address space
  return clamp(round_down_power_of_2(heuristic_max_capacity / 64), smallest_granule, largest_granule);
}

bool ZCommitter::should_commit(size_t granule, size_t capacity, size_t target_capacity, size_t curr_max_capacity) {
  const size_t new_capacity = capacity + granule;

  if (!ZAdaptiveHeap::explicit_max_capacity() &&
      new_capacity > size_t(curr_max_capacity * (1.0 - ZMemoryCriticalThreshold))) {
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
    const size_t capacity = _page_allocator->capacity();
    const size_t curr_max_capacity = ZHeap::heap()->current_max_capacity();
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
    // This allows growing to be more vigilent and not have to wait for
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
  const size_t curr_max_capacity = ZHeap::heap()->current_max_capacity();

  ZLocker<ZConditionLock> locker(&_lock);
  Atomic::store(&_target_capacity, target_capacity);

  const size_t capacity = _page_allocator->capacity();
  target_capacity = MIN2(Atomic::load(&_target_capacity), curr_max_capacity);
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

void ZCommitter::register_heating_request(const ZPage* page) {
  ZLocker<ZConditionLock> locker(&_lock);
  if (_stop) {
    // Don't add more requests during termination
    return;
  }
  for (size_t granule = 0; granule < page->size(); granule += ZGranuleSize) {
    _heating_requests.upsert(page->start() + granule, ZGranuleSize);
  }
}

zoffset ZCommitter::pop_heating_request(size_t& size) {
  assert(has_heating_request(), "precondition");

  ZHeatingRequestNode* node = _heating_requests.leftmost();

  zoffset offset = node->key();
  size = node->val();

  _heating_requests.remove(offset);

  return offset;
}

void ZCommitter::remove_heating_request(const ZPage* page) {
  for (size_t granule = 0; granule < page->size(); granule += ZGranuleSize) {
    const zoffset offset = page->start() + granule;
    const zaddress addr = ZOffset::address(offset);

    ZLocker<ZConditionLock> locker(&_lock);
    while (_currently_heating == addr) {
      // Trying to unmap what's currently being heated; calm down!
      _lock.wait();
    }

    ZHeatingRequestNode* node = _heating_requests.find_node(offset);
    if (node != nullptr) {
      _heating_requests.remove(node);
    }
  }
}

size_t ZCommitter::process_heating_request() {
  zoffset offset;
  size_t size;
  {
    ZLocker<ZConditionLock> locker(&_lock);
    if (!has_heating_request()) {
      // Unmapping removed the request; bail
      return 0;
    }

    assert(has_heating_request(), "who else processed it?");
    offset = pop_heating_request(size);

    _currently_heating = ZOffset::address(offset);
  }

  _page_allocator->heat_memory(offset, size);

  {
    ZLocker<ZConditionLock> locker(&_lock);
    _lock.notify_all();
    _currently_heating = zaddress::null;
  }

  return size;
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
      const size_t capacity = _page_allocator->capacity();
      const size_t curr_max_capacity = ZHeap::heap()->current_max_capacity();
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
        if (_page_allocator->prime_alloc_page(granule)) {
          committed += granule;
        }
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
        uncommitted += _page_allocator->uncommit(nullptr, granule);
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

  while (_currently_heating != zaddress::null) {
    // Trying to unmap what's currently being heated; calm down!
    _lock.wait();
  }
}
