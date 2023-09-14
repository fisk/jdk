/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/shared/gcCallbackStackWatermark.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "memory/universe.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/registerMap.hpp"

GCCallbackStackWatermark::GCCallbackStackWatermark(JavaThread* jt)
  : StackWatermark(jt, StackWatermarkKind::gc_callback, Universe::heap()->total_collections_ended()),
    _record_process(false),
    _recorded_fp(nullptr),
    _recorded_epoch(Universe::heap()->total_collections_ended()),
    _unwind_detected(false) {
}

uint32_t GCCallbackStackWatermark::epoch_id() const {
  return Universe::heap()->total_collections_ended();
}

void GCCallbackStackWatermark::start_processing_impl(void* context) {
  _record_process = false;
  _recorded_epoch = epoch_id();
  _unwind_detected = false;
  _recorded_fp = nullptr;

  if (_jt->has_last_Java_frame()) {
    frame f = _jt->last_frame();

    RegisterMap map(_jt,
                    RegisterMap::UpdateMap::skip,
                    RegisterMap::ProcessFrames::skip,
                    RegisterMap::WalkContinuation::skip);
    if (f.is_safepoint_blob_frame() || f.is_runtime_frame()) {
      f = f.sender(&map);
    }

    _recorded_fp = f.fp();
  }

  // Publishes the processing start to concurrent threads
  StackWatermark::start_processing_impl(context);

  _record_process = true;
}

void GCCallbackStackWatermark::process(const frame& fr, RegisterMap& register_map, void* context) {
  if (!_record_process) {
    return;
  }

  _unwind_detected = true;
  abort_iteration(); // No point continuing to detect unwinding; one unwind will do
}

bool GCCallbackStackWatermark::make_safe(JavaThread* jt, uint32_t epoch, bool trigger_deopt) {
  if (!jt->has_last_Java_frame()) {
    return true;
  }

  if (_recorded_epoch >= epoch && _unwind_detected) {
    // We provably unwound past the point of racyness
    return true;
  }

  frame f = jt->last_frame();

  RegisterMap map(jt,
                  RegisterMap::UpdateMap::skip,
                  RegisterMap::ProcessFrames::skip,
                  RegisterMap::WalkContinuation::skip);
  if (f.is_safepoint_blob_frame() || f.is_runtime_frame()) {
    f = f.sender(&map);
  }

  if (!f.is_compiled_frame()) {
    return true;
  }

  nmethod* nm = f.cb()->as_nmethod_or_null();

  if (nm == nullptr) {
    return true;
  }

  if (nm->is_native_method()) {
    return true;
  }

  if (!nm->may_have_gc_callback_type()) {
    // We can prove the top frame doesn't interact with Reference with queue
    return true;
  }

  if (_recorded_fp != f.fp()) {
    return true;
  }

  if (!trigger_deopt) {
    return false;
  }

  if (f.can_be_deoptimized()) {
    // All bets are off; deoptimize
    Deoptimization::deoptimize(jt, f);
  }

  return true;
}
