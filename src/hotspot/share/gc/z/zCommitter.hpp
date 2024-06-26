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

#include "gc/z/zList.hpp"
#include "gc/z/zLock.hpp"
#include "gc/z/zThread.hpp"

class ZPage;
class ZPageAllocator;

class ZCommitter : public ZThread {
private:
  ZPageAllocator* const _page_allocator;
  ZConditionLock        _lock;
  volatile size_t       _target_capacity;
  bool                  _stop;

  size_t commit_granule(size_t capacity, size_t target_capacity);
  bool should_commit(size_t granule, size_t capacity, size_t target_capacity);
  bool dequeue();

protected:
  virtual void run_thread();
  virtual void terminate();

public:
  ZCommitter(ZPageAllocator* page_allocator);

  void set_target_capacity(size_t target_capacity);
};

#endif // SHARE_GC_Z_ZCOMMITTER_HPP
