/*
 * Copyright (c) 2020, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/shared/gc_globals.hpp"
#include "gc/z/zList.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAllocator.hpp"
#include "gc/z/zPriorityQueue.inline.hpp"
#include "gc/z/zUnmapper.hpp"
#include "jfr/jfrEvents.hpp"
#include "runtime/globals.hpp"

static bool z_page_address_priority_function(ZPage* a, ZPage* b) {
  return a->start() < b->start();
}

ZUnmapper::ZUnmapper(ZPageAllocator* page_allocator)
  : _page_allocator(page_allocator),
    _lock(),
    _queue(&z_page_address_priority_function),
    _enqueued_bytes(0),
    _stop(false) {
  set_name("ZUnmapper");
  create_and_start();
}

bool ZUnmapper::try_dequeue(ZArray<ZPage*>* result) {
  ZLocker<ZConditionLock> locker(&_lock);

  for (;;) {
    if (_stop) {
      // Signal shutdown
      return false;
    }

    // Pick a page
    ZPage* last = _queue.remove_first();

    if (last != nullptr) {
      // We got at least one page enqueued
      result->append(last);
      _enqueued_bytes -= last->size();

      // Try to fill in any contiguous ranges following said page
      for (;;) {
        ZPage* const page = _queue.first();

        if (page == nullptr) {
          // Empty queue
          return true;
        }

        if (last->end() != page->start()) {
          // Not a consecutive range
          return true;
        }

        result->append(page);
        _enqueued_bytes -= page->size();

        last = _queue.remove_first();
      }
    }

    _lock.wait();
  }
}

bool ZUnmapper::try_enqueue(ZPage* page) {
  // Enqueue for asynchronous unmap and destroy
  ZLocker<ZConditionLock> locker(&_lock);
  if (is_saturated()) {
    // The unmapper thread is lagging behind and is unable to unmap memory fast enough
    return false;
  }

  _queue.insert(page);
  _enqueued_bytes += page->size();
  _lock.notify_all();

  return true;
}

bool ZUnmapper::is_saturated() const {
  return _enqueued_bytes > _page_allocator->max_capacity() / 4;
}

void ZUnmapper::do_unmap_and_destroy_page(ZPage* page) const {
  ZArray<ZPage*> singleton_list;
  singleton_list.append(page);
  do_unmap_and_destroy_consecutive_pages(&singleton_list);
}

void ZUnmapper::do_unmap_and_destroy_consecutive_pages(ZArray<ZPage*>* pages) const {
  EventZUnmap event;
  size_t unmapped = 0;

  // Unmap and destroy
  _page_allocator->unmap_consecutive_pages(pages);
  ZArrayIterator<ZPage*> iter(pages);
  for (ZPage* page; iter.next(&page);) {
    _page_allocator->destroy_page(page);
    unmapped += page->size();
  }
  log_debug(gc, unmap)("Unmapping %d pages covering " SIZE_FORMAT "M, %d pages still enqueued",
                       pages->length(),
                       unmapped / M,
                       _queue.length());

  // Send event
  event.commit(unmapped);
}

void ZUnmapper::unmap_and_destroy_page(ZPage* page) {
  if (try_enqueue(page)) {
    // Asynchronous unmap and destroy
    return;
  }

  // When the unmapper thread can't keep up we switch to synchronous unmapping
  do_unmap_and_destroy_page(page);
}

void ZUnmapper::run_thread() {
  for (;;) {
    ZArray<ZPage*> dequeued;
    if (!try_dequeue(&dequeued)) {
      // Stop
      return;
    }

    do_unmap_and_destroy_consecutive_pages(&dequeued);
  }
}

void ZUnmapper::terminate() {
  ZLocker<ZConditionLock> locker(&_lock);
  _stop = true;
  _lock.notify_all();
}
