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

#include "precompiled.hpp"
#include "gc/z/zAdaptiveHeap.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAllocator.hpp"
#include "gc/z/zCommitter.hpp"

ZCommitter::ZCommitter(ZPageAllocator* page_allocator)
  : _page_allocator(page_allocator),
    _lock(),
    _target_capacity(0),
    _stop(false) {
  set_name("ZCommitter");
  create_and_start();
}

size_t ZCommitter::commit_granule(size_t capacity, size_t target_capacity) {
  const size_t smallest_granule = ZGranuleSize;
  const size_t largest_granule = ZPageSizeMedium;

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

bool ZCommitter::dequeue() {
  for (;;) {
    if (_stop) {
      return false;
    }

    const size_t capacity = _page_allocator->capacity();
    const size_t curr_max_capacity = ZHeap::heap()->current_max_capacity();
    const size_t target_capacity = MIN2(Atomic::load(&_target_capacity), curr_max_capacity);
    const size_t granule = commit_granule(capacity, target_capacity);

    if (should_commit(granule, capacity, target_capacity, curr_max_capacity)) {
      // At least one granule to commit
      return true;
    }

    ZLocker<ZConditionLock> locker(&_lock);
    _lock.wait();
  }
}

size_t ZCommitter::target_capacity() {
  return Atomic::load(&_target_capacity);
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
}

void ZCommitter::run_thread() {
  for (;;) {
    if (!dequeue()) {
      // Stop
      return;
    }

    size_t committed = 0;

    for (;;) {
      const size_t capacity = _page_allocator->capacity();
      const size_t curr_max_capacity = ZHeap::heap()->current_max_capacity();
      const size_t target_capacity = MIN2(Atomic::load(&_target_capacity), curr_max_capacity);
      const size_t granule = commit_granule(capacity, target_capacity);

      if (!should_commit(granule, capacity, target_capacity, curr_max_capacity)) {
        // We are done with committing
        break;
      }

      if (_page_allocator->prime_alloc_page(granule)) {
        committed += granule;
      }
    }

    if (committed > 0) {
      log_info(gc, heap)("Committed: " SIZE_FORMAT "M(%.0f%%)",
                         committed / M, percent_of(committed, ZHeap::heap()->dynamic_max_capacity()));
    }
  }
}

void ZCommitter::terminate() {
  ZLocker<ZConditionLock> locker(&_lock);
  _stop = true;
  _lock.notify_all();
}
