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
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAllocator.hpp"
#include "gc/z/zMapper.hpp"

ZMapper::ZMapper(ZPageAllocator* page_allocator)
  : _page_allocator(page_allocator),
    _lock(),
    _target_capacity(0),
    _stop(false) {
  set_name("ZMapper");
  create_and_start();
}

bool ZMapper::dequeue() {
  ZLocker<ZConditionLock> locker(&_lock);

  for (;;) {
    if (_stop) {
      return false;
    }

    if (_page_allocator->capacity() < _target_capacity) {
      return true;
    }

    _lock.wait();
  }
}

void ZMapper::set_target_capacity(size_t target_capacity) {
  ZLocker<ZConditionLock> locker(&_lock);
  Atomic::store(&_target_capacity, target_capacity);
  if (_page_allocator->capacity() < _target_capacity) {
    _lock.notify_all();
  }
}

void ZMapper::run_thread() {
  for (;;) {
    if (!dequeue()) {
      // Stop
      return;
    }

    for (;;) {
      const size_t target_capacity = Atomic::load(&_target_capacity);
      const size_t capacity = _page_allocator->capacity();
      const size_t used = _page_allocator->used();
      const size_t heuristic_max_capacity = _page_allocator->heuristic_max_capacity();

      if (capacity >= target_capacity) {
        break;
      }

      const size_t remaining = target_capacity - capacity;
      const size_t available = used > capacity ? 0 : capacity - used;
      // Don't allocate things that are larger than the largest medium page size, in the lower address space
      const size_t granule_upper_bound = clamp(round_down_power_of_2(remaining), ZGranuleSize, ZGranuleSize * 16);
      const size_t granule = clamp(round_down_power_of_2(heuristic_max_capacity / 128), ZGranuleSize, granule_upper_bound);

      const size_t size = available < granule ? ZPageSizeSmall : granule;

      log_debug(gc, heap)("Mapping " SIZE_FORMAT "M page (" SIZE_FORMAT "M / " SIZE_FORMAT "M target)",
                          size / M, capacity / M, target_capacity / M);

      _page_allocator->prime_alloc_page(size);
    }
  }
}

void ZMapper::terminate() {
  ZLocker<ZConditionLock> locker(&_lock);
  _stop = true;
  _lock.notify_all();
}
