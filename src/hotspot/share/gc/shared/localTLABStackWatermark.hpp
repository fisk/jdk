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

#ifndef SHARE_GC_SHARED_LOCALTLABSTACKWATERMARK_HPP
#define SHARE_GC_SHARED_LOCALTLABSTACKWATERMARK_HPP

#include "gc/shared/barrierSet.hpp"
#include "memory/allocation.hpp"
#include "oops/oopsHierarchy.hpp"
#include "runtime/stackWatermark.hpp"
#include "utilities/globalDefinitions.hpp"

class frame;
class JavaThread;

class LocalTLABStackWatermark : public StackWatermark {
private:
  class LocalTLAB : public CHeapObj<mtGC> {
  public:
    LocalTLAB* _prev;
    uintptr_t _sp_watermark;
    uintptr_t _fp_watermark;
    HeapWord* _start;
    HeapWord* _end;
  };

  LocalTLAB* _used_head;
  LocalTLAB* _unused_head;

  LocalTLAB* _used_object_head;

  uintptr_t _retired_sp_watermark;
  uintptr_t _retired_fp_watermark;

  HeapWord* _vertical_start;
  HeapWord* _vertical_end;

  bool is_mixed_frame(const frame& fr);

  // Only for GCs really
  virtual uint32_t epoch_id() const { return 0; }
  virtual void start_processing_impl(void* context);
  virtual void ensure_safe(const frame& fr);
  // Only trigger when unwinding
  virtual bool process_on_iteration() { return false; }
  virtual void update_watermark();

  frame top_frame(const frame& top);
  frame top_frame();

public:
  LocalTLABStackWatermark(JavaThread* jt);

  void retire_tlabs();
  void alloc_tlab(HeapWord* start, HeapWord* end);
  void alloc_outside_tlab(HeapWord* start, HeapWord* end);
  bool try_refill(HeapWord*& start, size_t& size, size_t min_size);
};

#endif // SHARE_GC_SHARED_LOCALTLABSTACKWATERMARK_HPP
