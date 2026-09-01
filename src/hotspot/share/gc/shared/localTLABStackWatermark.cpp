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

#include "gc/shared/localTLABStackWatermark.hpp"
#include "logging/log.hpp"
#include "memory/allocation.inline.hpp"
#include "oops/access.inline.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/osThread.hpp"
#include "runtime/registerMap.hpp"
#include "runtime/safepointMechanism.inline.hpp"
#include "runtime/stackWatermark.inline.hpp"

frame LocalTLABStackWatermark::top_frame(const frame& top) {
  frame f = _jt->last_frame();

  RegisterMap map(_jt,
                  RegisterMap::UpdateMap::skip,
                  RegisterMap::ProcessFrames::skip,
                  RegisterMap::WalkContinuation::skip);
  while (top.id() != f.id()) {
    f = f.sender(&map);
  }

  while (!has_barrier(f)) {
    f = f.sender(&map);
  }

  assert(has_barrier(f), "The top frame must have stack watermark barriers");

  return f;
}

frame LocalTLABStackWatermark::top_frame() {
  frame f = _jt->last_frame();

  // Skip any stub frames etc up until the frame that triggered before_unwind().
  RegisterMap map(_jt,
                  RegisterMap::UpdateMap::skip,
                  RegisterMap::ProcessFrames::skip,
                  RegisterMap::WalkContinuation::skip);
  if (f.is_safepoint_blob_frame() || f.is_runtime_frame()) {
    f = f.sender(&map);
  }

  while (!has_barrier(f)) {
    f = f.sender(&map);
  }

  assert(has_barrier(f), "The top frame must have stack watermark barriers");

  return f;
}

LocalTLABStackWatermark::LocalTLABStackWatermark(JavaThread* jt)
  : StackWatermark(jt, StackWatermarkKind::local_tlab, 0),
    _used_head(nullptr),
    _unused_head(nullptr),
    _retired_sp_watermark(0),
    _retired_fp_watermark(0),
    _vertical_start(nullptr),
    _vertical_end(nullptr) {
}

void LocalTLABStackWatermark::start_processing_impl(void* context) {
  ShouldNotReachHere();
}

void LocalTLABStackWatermark::update_watermark() {
  constexpr uintptr_t max_watermark = uintptr_t(0) - 1;
  uintptr_t watermark = max_watermark;
  if (_used_head != nullptr) {
    watermark = MIN2(watermark, _used_head->_sp_watermark);
  }
  if (_retired_sp_watermark != 0) {
    watermark = MIN2(watermark, _retired_sp_watermark);
  }
  if (watermark == max_watermark) {
    watermark = 0;
  }
  set_watermark0(watermark);
  if (Thread::current() == _jt) {
    SafepointMechanism::update_poll_values(_jt);
  } else {
    // A remote thread is never allowed to relax the arm value, because
    // it could racingly be updated in the opposite way.
    SafepointMechanism::arm_local_poll_release(_jt);
  }
}

bool LocalTLABStackWatermark::is_mixed_frame(const frame& fr) {
  if (_retired_fp_watermark == 0) {
    return false;
  }

  assert(has_barrier(fr), "Must have stack watermark barrier");

  return uintptr_t(fr.real_fp()) >= _retired_fp_watermark;
}

void LocalTLABStackWatermark::ensure_safe(const frame& after_unwind_frame) {
  if (!is_above_watermark(uintptr_t(after_unwind_frame.real_fp()), watermark())) {
    // Not above the watermark yet; we are good
    return;
  }

  assert(has_barrier(after_unwind_frame), "Should have stack watermark barrier");

  frame f = top_frame(after_unwind_frame);
  uintptr_t fp = reinterpret_cast<uintptr_t>(f.real_fp());
  uintptr_t sp = reinterpret_cast<uintptr_t>(f.sp());

  log_info(stackbarrier)("Unwinding for tid %d: [%lx, %lx), watermark: %lx", _jt->osthread()->thread_id(), fp, sp, watermark());

  bool is_mixed = is_mixed_frame(f);

  HeapWord* tlab_start;
  HeapWord* tlab_end;
  HeapWord* tlab_top;

  if (is_mixed) {
    if (_retired_fp_watermark != 0 && fp > _retired_fp_watermark) {
      // mixed to mixed
      tlab_start = _jt->local_tlab().start();
      tlab_end = _jt->local_tlab().end();
      tlab_top = _jt->saved_local_tlab_top();
      if (tlab_top == JavaThread::no_saved_local_tlab_top()) {
        // Unwind without restore (no local objects)
        tlab_top = _jt->local_tlab().top();
      }
      log_info(stackbarrier)("Mixed to mixed for tid %d: [%lx, %lx), watermark: %lx", _jt->osthread()->thread_id(), fp, sp, watermark());
    } else {
      // new to mixed
      if (_vertical_start == nullptr) {
        // new to not yet allocated mixed
        log_info(stackbarrier)("New to mixed non-vertical for tid %d: [%lx, %lx), watermark: %lx", _jt->osthread()->thread_id(), fp, sp, watermark());
        tlab_start = tlab_end = tlab_top = nullptr;
      } else {
        log_info(stackbarrier)("New to mixed vertical for tid %d: [%lx, %lx), vertical: [%lx, %ld), head: %lx, watermark: %lx",
                               _jt->osthread()->thread_id(), fp, sp, p2i(_vertical_start), p2i(_vertical_end), _used_head == nullptr ? 0 : p2i(_used_head->_start), watermark());
        for (LocalTLAB* tlab = _used_head; tlab != nullptr; tlab = tlab->_prev) {
          log_info(stackbarrier)("HEAD tid %d: [%lx, %lx)", _jt->osthread()->thread_id(), p2i(tlab->_start), p2i(tlab->_end));
        }
        tlab_start = _vertical_start;
        tlab_end = _vertical_end - ThreadLocalAllocBuffer::alignment_reserve();
        if (fp == _retired_fp_watermark) {
          // Returned back previously processed mixed frame after upcall to a new frame
          tlab_top = _jt->local_tlab().top();
        } else {
          tlab_top = tlab_start;
        }
      }
    }
  } else {
    log_info(stackbarrier)("New to new for tid %d: [%lx, %lx), watermark: %lx", _jt->osthread()->thread_id(), fp, sp, watermark());
    tlab_start = _jt->local_tlab().start();
    tlab_end = _jt->local_tlab().end();
    tlab_top = _jt->local_tlab().top();
  }

  for (;;) {
    LocalTLAB* head = _used_head;
    if (head == nullptr) {
      break;
    }

    if (fp > head->_fp_watermark) {
      // Recycle expired TLABs
      if (is_mixed) {
        // Nuke all horizontal TLABs
        log_info(stackbarrier)("Unwinding from retired Allocated for tid %d: [%lx, %lx), watermark: %lx", _jt->osthread()->thread_id(), fp, sp, watermark());
        _used_head = head->_prev;

        if (head->_prev == nullptr) {
          // Last TLAB; make it the vertical stack instead of popping it
          _vertical_start = head->_start;
          _vertical_end = head->_end;
          tlab_start = _vertical_start;
          tlab_end = _vertical_end - ThreadLocalAllocBuffer::alignment_reserve();
          tlab_top = tlab_start;
          delete head;
        } else {
          // Recycle the memory
          head->_sp_watermark = 0;
          head->_fp_watermark = 0;
          head->_prev = _unused_head;
          _unused_head = head;
          // Put in filler in case GC runs later
          Universe::heap()->fill_with_dummy_object(head->_start, head->_end, true);
        }
      } else if (head->_prev == nullptr) {
        // Last TLAB; move it up the stack instead of popping it
        log_info(stackbarrier)("Unwinding from leaf Allocated for tid %d: [%lx, %lx), watermark: %lx", _jt->osthread()->thread_id(), fp, sp, watermark());
        head->_sp_watermark = sp;
        head->_fp_watermark = fp;
        tlab_top = tlab_start;
        break;
      } else {
        // Popping TLAB with predecessor
        log_info(stackbarrier)("Unwinding from non-leaf Allocated for tid %d: [%lx, %lx), watermark: %lx", _jt->osthread()->thread_id(), fp, sp, watermark());
        // There was a previous used TLAB in the stack; make it current so previous tops make sense
        _used_head = head->_prev;
        // Pop to previous entry
        tlab_start = _used_head->_start;
        tlab_end = _used_head->_end - ThreadLocalAllocBuffer::alignment_reserve();

        // Put in filler in case GC runs
        Universe::heap()->fill_with_dummy_object(head->_start, head->_end, true);

        // Recycle the memory
        head->_sp_watermark = 0;
        head->_fp_watermark = 0;
        head->_prev = _unused_head;
        _unused_head = head;
      }
    } else {
      log_info(stackbarrier)("Unwinding from fishy Allocated for tid %d: [%lx, %lx), watermark: %lx", _jt->osthread()->thread_id(), fp, sp, watermark());
      for (LocalTLAB* curr = head; curr != nullptr; curr = curr->_prev) {
        assert(fp <= curr->_fp_watermark, "unsorted watermarks");
        if (fp > curr->_sp_watermark) {
          curr->_sp_watermark = sp;
        }
      }
      break;
    }
  }

  if (_retired_fp_watermark != 0) {
    if (fp > _retired_fp_watermark) {
      log_info(stackbarrier)("Unwinding into Retired Caller for tid %d: [%lx, %lx), watermark: %lx", _jt->osthread()->thread_id(), fp, sp, watermark());
      _retired_sp_watermark = sp;
      _retired_fp_watermark = fp;
    } else if (fp > _retired_sp_watermark) {
      log_info(stackbarrier)("Unwinding into Retired Callee for tid %d: [%lx, %lx), watermark: %lx", _jt->osthread()->thread_id(), fp, sp, watermark());
      _retired_sp_watermark = sp;
    }
  }

  _jt->local_tlab().initialize(tlab_start, tlab_top, tlab_end);
  _jt->local_tlab().invariants();
  update_watermark();
  assert(!is_above_watermark(sp, watermark()), "still above watermark?");
}

bool LocalTLABStackWatermark::try_refill(HeapWord** start, size_t* size, size_t min_size) {
  LocalTLAB* head = _unused_head;
  frame f = top_frame();
  uintptr_t sp = uintptr_t(f.sp());
  uintptr_t fp = uintptr_t(f.real_fp());
  if (head == nullptr) {
    log_info(stackbarrier)("EMPTY try_refill for tid %d: [%lx, %lx), watermark: %lx", _jt->osthread()->thread_id(), fp, sp, watermark());
    return false;
  }

  HeapWord* potential_start = head->_start;
  size_t potential_size = (size_t(head->_end) - size_t(head->_start)) / HeapWordSize;

  assert(head->_end == head->_start + potential_size, "invariant");

  if (potential_size < min_size) {
    log_info(stackbarrier)("SMALL %lx < %lx try_refill for tid %d: [%lx, %lx), watermark: %lx", potential_size, min_size, _jt->osthread()->thread_id(), fp, sp, watermark());
    return false;
  }

  LocalTLAB* next = head->_prev;
  delete head;
  _unused_head = next;

  log_info(stackbarrier)("Successful try_refill for tid %d: [%lx, %lx), watermark: %lx, [%lx, %lx)", _jt->osthread()->thread_id(), fp, sp, watermark(),
                         p2i(potential_start), p2i(potential_start + potential_size));

  *start = potential_start;
  *size = potential_size;

  return true;
}

void LocalTLABStackWatermark::retire_tlabs() {
  // Frames above this watermark (all) should not reuse previously used TLABs
  LocalTLAB* entry = _used_head;
  while (entry != nullptr) {
    LocalTLAB* next = entry->_prev;
    delete entry;
    entry = next;
  }
  _used_head = nullptr;

  entry = _unused_head;
  while (entry != nullptr) {
    LocalTLAB* next = entry->_prev;
    delete entry;
    entry = next;
  }
  _unused_head = nullptr;

  if (!_jt->has_last_Java_frame()) {
    _retired_sp_watermark = 0;
    _retired_fp_watermark = 0;
    update_watermark();
    return;
  }

  frame f = top_frame();

  uintptr_t sp = uintptr_t(f.sp());
  uintptr_t fp = uintptr_t(f.real_fp());

  _retired_sp_watermark = sp;
  _retired_fp_watermark = fp;
  _vertical_start = nullptr;
  _vertical_end = nullptr;
  assert(is_mixed_frame(f), "invariant");
  update_watermark();

  log_info(stackbarrier)("Retired local TLAB for tid %d: [%lx, %lx), watermark: %lx", _jt->osthread()->thread_id(), fp, sp, watermark());
}

void LocalTLABStackWatermark::register_allocated_tlab(HeapWord* start, HeapWord* end) {
  assert(_jt->has_last_Java_frame(), "only compiled frames allocating local objects");
  frame f = top_frame();

  uintptr_t sp = uintptr_t(f.sp());
  uintptr_t fp = uintptr_t(f.real_fp());

  if (is_mixed_frame(f)) {
    // Vertical allocations for mixed frames and the first TLAB after retire
    assert(_used_head == nullptr, "sanity");
    _vertical_start = start;
    _vertical_end = end;
  } else {
    // Horizontal allocations for new frames
    LocalTLAB* entry = new LocalTLAB();
    entry->_sp_watermark = sp;
    entry->_fp_watermark = fp;
    entry->_prev = _used_head;
    entry->_start = start;
    entry->_end = end;
    _used_head = entry;
  }

  update_watermark();
  log_info(stackbarrier)("Allocated %s TLAB for tid %d: [%lx, %lx), watermark: %lx, [%lx, %lx)", is_mixed_frame(f) ? "MIXED" : "NEW", _jt->osthread()->thread_id(), fp, sp, watermark(), p2i(start), p2i(end));
}

HeapWord* LocalTLABStackWatermark::allocate_new_tlab(size_t min_tlab_size, size_t new_tlab_size, size_t* size) {
  HeapWord* mem = nullptr;

  if (!try_refill(&mem, size, min_tlab_size)) {
    mem = Universe::heap()->allocate_new_tlab(min_tlab_size, new_tlab_size, size);
  }

  if (mem != nullptr) {
    assert(*size != 0, "successful TLAB allocation must report its size");
    register_allocated_tlab(mem, mem + *size);
  }

  return mem;
}

void LocalTLABStackWatermark::after_unwind() {
  StackWatermark::after_unwind();

  // After unwinding from a C2 frame, we should clear the saved local TLAB top
  // This essentially consumes the information used by our normal after_unwind processing
  _jt->clear_saved_local_tlab_top();
}
