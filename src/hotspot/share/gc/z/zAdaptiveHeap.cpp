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
#include "gc/shared/gc_globals.hpp"
#include "gc/z/zAdaptiveHeap.hpp"
#include "gc/z/zDriver.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zStat.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "runtime/atomic.hpp"
#include "utilities/debug.hpp"

#ifdef _WINDOWS
#include <processthreadsapi.h>
#include <timezoneapi.h>
#else
#include <time.h>
#endif

bool ZAdaptiveHeap::_enabled = false;
double ZAdaptiveHeap::_accumulated_young_gc_time = 0.0;
ZAdaptiveHeap::ZGenerationOverhead ZAdaptiveHeap::_young_data;
ZAdaptiveHeap::ZGenerationOverhead ZAdaptiveHeap::_old_data;

// TODO: OS abstractions
double ZAdaptiveHeap::process_cpu_time() {
#ifdef _WINDOWS
  FILETIME create;
  FILETIME exit;
  FILETIME kernel;
  FILETIME user;

  if (GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user) == -1) {
    return -1,0;
  }

  SYSTEMTIME user_total;
  if (FileTimeToSystemTime(&user, &user_total) == -1) {
    return -1.0;
  }

  SYSTEMTIME kernel_total;
  if (FileTimeToSystemTime(&kernel, &kernel_total) == -1) {
    return -1.0;
  }

  double user_seconds = double(user_total.wHour) * 3600.0 +
                        double(user_total.wMinute) * 60.0 +
                        double(user_total.wSecond) +
                        double(user_total.wMilliseconds) / 1000.0;

  double kernel_seconds = double(kernel_total.wHour) * 3600.0 +
                          double(kernel_total.wMinute) * 60.0 +
                          double(kernel_total.wSecond) +
                          double(kernel_total.wMilliseconds) / 1000.0;

  return user_seconds + kernel_seconds;
#else
  timespec tp;
  int status = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &tp);
  assert(status == 0, "clock_gettime error: %s", os::strerror(errno));
  if (status != 0) {
    return -1.0;
  }

  return double(tp.tv_sec) + double(tp.tv_nsec) / NANOSECS_PER_SEC;
#endif
}

void ZAdaptiveHeap::try_enable() {
  double time_now = process_cpu_time();
  if (time_now < 0.0) {
    return;
  }

  _enabled = true;
  _young_data._last_process_time = time_now;
  _old_data._last_process_time = time_now;
}

// Produces values in the range 0 - 1 in an S shape
static double sigmoid_function(double value) {
  return 1.0 / (1.0 + pow(M_E, -value));
}

void ZAdaptiveHeap::adapt(ZGenerationId generation) {
  assert(is_enabled(), "Adapting heap even though adaptation is disabled");
  ZStatWorkersStats worker_stats = ZGeneration::generation(generation)->stat_workers()->stats();
  ZStatCycleStats cycle_stats = ZGeneration::generation(generation)->stat_cycle()->stats();

  const bool is_young = generation == ZGenerationId::young;
  ZGenerationOverhead& generation_data = is_young ? _young_data : _old_data;

  const double parallel_gc_duration = worker_stats._accumulated_duration;
  const double parallel_gc_time = worker_stats._accumulated_time;
  const double serial_gc_time = cycle_stats._duration_since_start - parallel_gc_duration;

  const double process_time_last = generation_data._last_process_time;
  const double process_time_now = process_cpu_time();
  const double process_time = process_time_now - process_time_last;
  generation_data._last_process_time = process_time_now;

  const bool is_major = Thread::current() == ZDriver::major();
  const GCCause::Cause cause = is_major ? ZDriver::major()->gc_cause() : ZDriver::minor()->gc_cause();
  const bool is_proactive = cause == GCCause::_z_proactive;
  const bool is_heap_pressure_gc = cause == GCCause::_z_allocation_rate ||
                                   cause == GCCause::_z_high_usage ||
                                   cause == GCCause::_z_warmup;

  if (is_proactive && !is_young) {
    // Proactive GCs imply that the heap is excessively large; let's try
    // to shrink it more aggressively that we otherwise might
    ZHeap::heap()->resize_heap(0.0);
    return;
  }

  if (!is_heap_pressure_gc) {
    return;
  }

  const double gc_time = serial_gc_time + parallel_gc_time + (is_young ? 0.0 : _accumulated_young_gc_time);

  generation_data._process_time.add(process_time);
  generation_data._gc_time.add(gc_time);

  const double avg_gc_time = generation_data._gc_time.avg();
  const double avg_process_time = generation_data._process_time.avg();

  const double avg_cpu_overhead = avg_gc_time / avg_process_time;

  log_debug(gc, adaptive)("Adaptive avg gc time %.3f, avg total time %.3f (%.3f%%)", avg_gc_time, avg_process_time, avg_cpu_overhead * 100.0);

  const double cpu_overhead_fraction = ZCPUOverheadPercent / 100.0;
  const double target_cpu_overhead = cpu_overhead_fraction / (1.0 + cpu_overhead_fraction);

  const double cpu_overhead_error = is_proactive ? 0.0 : avg_cpu_overhead - target_cpu_overhead;

  // High GC frequencies lead to extra overheads such as barrier storms
  // Therefore, we add a factor that ensures there is at least some social
  // distancing between GCs, even when the GC overhead is small.
  const double gc_frequency_error = MAX2(0.0, 0.25 - cycle_stats._time_since_last);

  const double sigmoid_error = sigmoid_function(cpu_overhead_error + gc_frequency_error);
  const double correction_factor = sigmoid_error + 0.5;

  log_debug(gc, adaptive)("CPU Overhead Error: %.3f, GC Frequency Error: %.3f, Correction factor %.3f",
                          cpu_overhead_error, gc_frequency_error, correction_factor);

  if (is_young) {
    _accumulated_young_gc_time += gc_time;
    if (correction_factor < 1.0) {
      // Don't have enough data to shrink in minor collections
      return;
    }
  } else {
    _accumulated_young_gc_time = 0.0;
  }

  ZHeap::heap()->resize_heap(correction_factor);
}

bool ZAdaptiveHeap::is_enabled() {
  return _enabled;
}
