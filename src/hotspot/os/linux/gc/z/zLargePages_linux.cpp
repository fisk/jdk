/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/shared/gcLogPrecious.hpp"
#include "gc/z/zErrno.hpp"
#include "gc/z/zLargePages.hpp"
#include "hugepages.hpp"
#include "logging/log.hpp"
#include "os_linux.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"

#include <sys/mman.h>

#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP	               4
#endif

static bool supports_anonymous_backing() {
  // Test if we can use mremap with MREMAP_DONTUNMAP, introduced in Linux 5.7
  size_t page_size = os::vm_page_size();
  void* const mapping1 = mmap(0, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (mapping1 == MAP_FAILED) {
    ZErrno err;
    fatal("Failed to map memory (%s)", err.to_string());
  }

  void* const mapping2 = mmap(0, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (mapping2 == MAP_FAILED) {
    ZErrno err;
    fatal("Failed to map memory (%s)", err.to_string());
  }

  // Try remapping one mapping to another
  bool supported = mremap(mapping1, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, mapping2) != MAP_FAILED;

  // Either way, let's clean up the pages
  if (munmap(mapping1, page_size) == -1) {
    ZErrno err;
    fatal("Failed to unmap memory (%s)", err.to_string());
  }

  if (munmap(mapping2, page_size) == -1) {
    ZErrno err;
    fatal("Failed to unmap memory (%s)", err.to_string());
  }

  return supported;
}

static bool should_select_anonymous_backing() {
  // Can't use anonymous memory with explicit large pages
  if (UseLargePages && !UseTransparentHugePages) {
    return false;
  }

  if (AllocateHeapAt != nullptr) {
    // Explicit file backing requires non-anonymous heap
    return false;
  }

  // We need Linux 5.7 to use anonymous memory
  if (!supports_anonymous_backing()) {
    // Without the appropriate Linux support, resort to using shared memory
    if (ZAnonymousMemoryBacking) {
      log_warning_p(gc)("The ZAnonymousMemoryBacking flag requires Linux 5.7; falling back to shared memory");
    }
    return false;
  }

  if (FLAG_IS_DEFAULT(ZAnonymousMemoryBacking)) {
    // If we have support for anonymous memory, let's use it by default
    return true;
  }

  // Otherwise, let the user decide what to do
  return ZAnonymousMemoryBacking;
}

void ZLargePages::pd_initialize() {
  // We need to know if we are going to use anonymous or shared memory, in order
  // to know how to initialize the large page configuration
  ZAnonymousMemoryBacking = should_select_anonymous_backing();

  if (os::Linux::thp_requested()) {
    if (ZAnonymousMemoryBacking) {
      // Check if the OS config turned off transparent huge pages for shmem.
      _os_enforced_transparent_mode = HugePages::thp_info().mode() == THPMode::never;
      _state = _os_enforced_transparent_mode ? Disabled : Transparent;
    } else {
      if (!HugePages::supports_shmem_thp()) {
        log_warning(pagesize)("Shared memory transparent huge pages are not enabled in the OS. "
                              "Set /sys/kernel/mm/transparent_hugepage/shmem_enabled to 'advise' to enable them.");
        // UseTransparentHugePages has historically been tightly coupled with
        // anonymous THPs. Fall through here and let the validity be determined
        // by the OS configuration for anonymous THPs. ZGC doesn't use the flag
        // but instead checks os::Linux::thp_requested().
      }

      // Check if the OS config turned off transparent huge pages for shmem.
      _os_enforced_transparent_mode = HugePages::shmem_thp_info().is_disabled();
      _state = _os_enforced_transparent_mode ? Disabled : Transparent;
    }
    return;
  }

  if (UseLargePages) {
    _state = Explicit;
    return;
  }

  if (ZAnonymousMemoryBacking) {
    // Check if the OS config turned on transparent huge pages for anonymous memory.
    _os_enforced_transparent_mode = HugePages::thp_info().mode() == THPMode::always;
    _state = _os_enforced_transparent_mode ? Transparent : Disabled;
  } else {
    // Check if the OS config turned on transparent huge pages for shmem.
    _os_enforced_transparent_mode = HugePages::shmem_thp_info().is_forced();
    _state = _os_enforced_transparent_mode ? Transparent : Disabled;
  }
}
