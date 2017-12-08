/*
 * Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zErrno.hpp"
#include "gc/z/zLargePages.inline.hpp"
#include "gc/z/zMemory.hpp"
#include "gc/z/zNUMA.hpp"
#include "gc/z/zPhysicalMemory.inline.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "zBackingFile_linux_x86.hpp"
#include "zPhysicalMemoryBacking_linux_x86.hpp"

#include <stdio.h>
#include <sys/mman.h>
#include <sys/types.h>

// Support for building on older Linux systems
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE                        14
#endif

// Proc file entry for max map mount
#define ZFILENAME_PROC_MAX_MAP_COUNT         "/proc/sys/vm/max_map_count"

ZPhysicalMemoryBacking::ZPhysicalMemoryBacking(size_t max_capacity, size_t granule_size) :
    _manager(),
    _file(),
    _granule_size(granule_size) {

  // Check and warn if max map count seems too low
  check_max_map_count(max_capacity, granule_size);
}

void ZPhysicalMemoryBacking::check_max_map_count(size_t max_capacity, size_t granule_size) const {
  const char* const filename = ZFILENAME_PROC_MAX_MAP_COUNT;
  FILE* const file = fopen(filename, "r");
  if (file == NULL) {
    // Failed to open file, skip check
    log_debug(gc)("Failed to open %s", filename);
    return;
  }

  size_t actual_max_map_count = 0;
  const int result = fscanf(file, SIZE_FORMAT, &actual_max_map_count);
  fclose(file);
  if (result != 1) {
    // Failed to read file, skip check
    log_debug(gc)("Failed to read %s", filename);
    return;
  }

  // The required max map count is impossible to calculate exactly since subsystems
  // other than ZGC are also creating memory mappings, and we have no control over that.
  // However, ZGC tends to create the most mappings and dominate the total count.
  // In the worst cases, ZGC will map each granule three times, i.e. once per heap view.
  // We speculate that we need another 20% to allow for non-ZGC subsystems to map memory.
  const size_t required_max_map_count = (max_capacity / granule_size) * 3 * 1.2;
  if (actual_max_map_count < required_max_map_count) {
    log_warning(gc)("The system limit on number of memory mappings "
                    "per process might be too low for the given");
    log_warning(gc)("Java heap size (" SIZE_FORMAT "M). Please "
                    "adjust %s to allow for at least", max_capacity / M, filename);
    log_warning(gc)(SIZE_FORMAT " mappings (current limit is " SIZE_FORMAT "). "
                    "Continuing execution with the current limit could",
                    required_max_map_count, actual_max_map_count);
    log_warning(gc)("lead to a fatal error down the line, due to failed "
                    "attempts to map memory.");
  }
}

bool ZPhysicalMemoryBacking::is_initialized() const {
  return _file.is_initialized();
}

bool ZPhysicalMemoryBacking::expand(size_t from, size_t to) {
  const size_t size = to - from;

  // Expand
  if (!_file.expand(from, size)) {
    return false;
  }

  // Add expanded space to free list
  _manager.free(from, size);

  return true;
}

ZPhysicalMemory ZPhysicalMemoryBacking::alloc(size_t size) {
  assert(is_aligned(size, _granule_size), "Invalid size");

  ZPhysicalMemory pmem;

  // Allocate segments
  for (size_t allocated = 0; allocated < size; allocated += _granule_size) {
    const uintptr_t start = _manager.alloc_from_front(_granule_size);
    assert(start != UINTPTR_MAX, "Allocation should never fail");
    pmem.add_segment(ZPhysicalMemorySegment(start, _granule_size));
  }

  return pmem;
}

void ZPhysicalMemoryBacking::free(ZPhysicalMemory pmem) {
  const size_t nsegments = pmem.nsegments();

  // Free segments
  for (size_t i = 0; i < nsegments; i++) {
    const ZPhysicalMemorySegment segment = pmem.segment(i);
    _manager.free(segment.start(), segment.size());
  }
}

void ZPhysicalMemoryBacking::map_failed(ZErrno err) const {
  if (err == ENOMEM) {
    fatal("Failed to map memory. Please check the system limit on number of "
          "memory mappings allowed per process (see %s)", ZFILENAME_PROC_MAX_MAP_COUNT);
  } else {
    fatal("Failed to map memory (%s)", err.to_string());
  }
}

void ZPhysicalMemoryBacking::advise_view(uintptr_t addr, size_t size) const {
  if (madvise((void*)addr, size, MADV_HUGEPAGE) == -1) {
    ZErrno err;
    log_error(gc)("Failed to advise use of transparent huge pages (%s)", err.to_string());
  }
}

void ZPhysicalMemoryBacking::pretouch_view(uintptr_t addr, size_t size) const {
  const size_t page_size = ZLargePages::is_explicit() ? os::large_page_size() : os::vm_page_size();
  os::pretouch_memory((void*)addr, (void*)(addr + size), page_size);
}

void ZPhysicalMemoryBacking::map_view(ZPhysicalMemory pmem, uintptr_t addr, bool pretouch) const {
  const size_t nsegments = pmem.nsegments();

  // Map segments
  for (size_t i = 0; i < nsegments; i++) {
    const ZPhysicalMemorySegment segment = pmem.segment(i);
    const size_t size = segment.size();
    const void* const res = mmap((void*)addr, size, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_SHARED, _file.fd(), segment.start());
    if (res == MAP_FAILED) {
      ZErrno err;
      map_failed(err);
    }

    // Advise on use of transparent huge pages before touching it
    if (ZLargePages::is_transparent()) {
      advise_view(addr, size);
    }

    // NUMA interleave memory before touching it
    ZNUMA::memory_interleave(addr, size);

    if (pretouch) {
      pretouch_view(addr, size);
    }

    addr += size;
  }
}

void ZPhysicalMemoryBacking::unmap_view(ZPhysicalMemory pmem, uintptr_t addr) const {
  // Note that we must keep the address space reservation intact and just detach
  // the backing memory. For this reason we map a new anonymous, non-accessible
  // and non-reserved page over the mapping instead of actually unmapping.
  const size_t size = pmem.size();
  const void* const res = mmap((void*)addr, size, PROT_NONE, MAP_FIXED|MAP_ANONYMOUS|MAP_PRIVATE|MAP_NORESERVE, -1, 0);
  if (res == MAP_FAILED) {
    ZErrno err;
    map_failed(err);
  }
}

void ZPhysicalMemoryBacking::map(ZPhysicalMemory pmem, uintptr_t offset) const {
  if (ZUnmapBadViews) {
    // Only map the good view, for debugging only
    map_view(pmem, ZAddress::good(offset), AlwaysPreTouch);
  } else {
    // Map all views
    map_view(pmem, ZAddress::marked0(offset), AlwaysPreTouch);
    map_view(pmem, ZAddress::marked1(offset), AlwaysPreTouch);
    map_view(pmem, ZAddress::remapped(offset), AlwaysPreTouch);
  }
}

void ZPhysicalMemoryBacking::unmap(ZPhysicalMemory pmem, uintptr_t offset) const {
  if (ZUnmapBadViews) {
    // Only map the good view, for debugging only
    unmap_view(pmem, ZAddress::good(offset));
  } else {
    // Unmap all views
    unmap_view(pmem, ZAddress::marked0(offset));
    unmap_view(pmem, ZAddress::marked1(offset));
    unmap_view(pmem, ZAddress::remapped(offset));
  }
}

void ZPhysicalMemoryBacking::flip(ZPhysicalMemory pmem, uintptr_t offset) const {
  assert(ZUnmapBadViews, "Should be enabled");
  const uintptr_t addr_good = ZAddress::good(offset);
  const uintptr_t addr_bad = ZAddress::is_marked(ZAddressGoodMask) ? ZAddress::remapped(offset) : ZAddress::marked(offset);
  // Map/Unmap views
  map_view(pmem, addr_good, false /* pretouch */);
  unmap_view(pmem, addr_bad);
}
