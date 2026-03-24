/*
 * Copyright (c) 2020, 2022, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

#ifndef SHARE_RUNTIME_STACKWATERMARK_INLINE_HPP
#define SHARE_RUNTIME_STACKWATERMARK_INLINE_HPP

#include "runtime/stackWatermark.hpp"

#include "code/nmethod.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/registerMap.hpp"

static inline bool is_above_watermark(uintptr_t sp, uintptr_t watermark) {
  if (watermark == 0) {
    return false;
  }
// TODO: use frame::is_older()?
  return sp > watermark;
}

// Returns true for frames where stack watermark barriers have been inserted.
// This function may return false negatives, but may never return true if a
// frame has no barrier.
inline bool StackWatermark::has_barrier(const frame& f) {
  if (f.is_entry_frame()) {
    return true;
  }
  if (f.is_interpreted_frame()) {
    return true;
  }
  if (f.is_compiled_frame()) {
    nmethod* nm = f.cb()->as_nmethod();
    if (nm->is_compiled_by_c1() || nm->is_compiled_by_c2()) {
      return true;
    }
  }
  if (f.is_native_frame()) {
    return true;
  }
  return false;
}

inline bool StackWatermark::processing_started(uint32_t state) const {
  return StackWatermarkState::epoch(state) == epoch_id();
}

inline bool StackWatermark::processing_completed(uint32_t state) const {
  assert(processing_started(state), "Check is only valid if processing has been started");
  return StackWatermarkState::is_done(state);
}

inline void StackWatermark::ensure_safe(const frame& f) {
  assert(processing_started(), "Processing should already have started");

#if 1
if (!has_barrier(f)) {
  assert_is_frame_safe(f);
}
#endif
  if (processing_completed_acquire()) {
    return;
  }

  uintptr_t f_fp = reinterpret_cast<uintptr_t>(f.real_fp());

  if (is_above_watermark(f_fp, watermark())) {
    process_one();
  }

  assert_is_frame_safe(f);
}

#if 0
static bool is_good_frame(const frame& f) {
if (f.is_java_frame()) return true;
if (f.is_native_frame()) return true;
if (f.is_entry_frame()) return true;
if (f.is_upcall_stub_frame()) return true;
CodeBlob* cb = f.cb();
if (cb != nullptr) {
  if (cb->is_uncommon_trap_stub()) return true;
  if (cb->is_deoptimization_stub()) return true;
}
return false;
}
#endif

inline void StackWatermark::before_unwind() {
  frame f = _jt->last_frame();
#if 1
assert(f.is_interpreted_frame() || f.is_native_frame() || !has_barrier(f), "!");
assert_is_frame_safe(f);
#endif

  // Skip any stub frames etc up until the frame that triggered before_unwind().
  RegisterMap map(_jt,
                  RegisterMap::UpdateMap::skip,
                  RegisterMap::ProcessFrames::skip,
                  RegisterMap::WalkContinuation::skip);
  if (!has_barrier(f)) {
    f = f.sender(&map);
  }
#if 1
assert(!f.is_compiled_frame(), "!");
#endif

  assert_is_frame_safe(f);
  assert(!f.is_runtime_frame(), "should have skipped all runtime stubs");
#if 1
assert(has_barrier(f), "!");
#endif

  // before_unwind() potentially exposes a new frame. The new exposed frame is
  // always the caller of the top frame.
  if (!f.is_first_frame()) {
    f = f.sender(&map);
#if 1
if (!has_barrier(f)) {
  assert_is_frame_safe(f);
}
#endif
#if 1
assert(has_barrier(f), "redundant before_unwind?"); // should we skip more frames until we find has_barrier()?
#endif
    ensure_safe(f);
  }
}

inline void StackWatermark::after_unwind() {
  frame f = _jt->last_frame();
#if 1
assert(f.is_interpreted_frame() || f.is_native_frame() || !has_barrier(f), "!");
assert_is_frame_safe(f);
#endif

  if (!has_barrier(f)) {
    // Skip safepoint blob.
    RegisterMap map(_jt,
                    RegisterMap::UpdateMap::skip,
                    RegisterMap::ProcessFrames::skip,
                    RegisterMap::WalkContinuation::skip);
    f = f.sender(&map);
  }
  assert(!f.is_runtime_frame(), "should have skipped all runtime stubs");
#if 1
assert(has_barrier(f), "!");
#endif
#if 1
if (!has_barrier(f)) {
  assert_is_frame_safe(f);
}
#endif

  // after_unwind() potentially exposes the top frame.
  ensure_safe(f);
}

inline void StackWatermark::on_iteration(const frame& f) {
  if (process_on_iteration()) {
    ensure_safe(f);
  }
}

#endif // SHARE_RUNTIME_STACKWATERMARK_INLINE_HPP
