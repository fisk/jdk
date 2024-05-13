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
#include "gc/shared/gc_globals.hpp"
#include "gc/z/zAdaptiveHeap.hpp"
#include "gc/z/zDriver.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zStat.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals_extension.hpp"
#include "utilities/debug.hpp"

#include <math.h>

bool ZAdaptiveHeap::_enabled = false;
volatile double ZAdaptiveHeap::_young_to_old_gc_time = 1.0;
double ZAdaptiveHeap::_accumulated_young_gc_time = 0.0;
ZAdaptiveHeap::ZGenerationOverhead ZAdaptiveHeap::_young_data;
ZAdaptiveHeap::ZGenerationOverhead ZAdaptiveHeap::_old_data;

void ZAdaptiveHeap::enable() {
  double time_now = os::elapsed_process_vtime();
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
  const double time_since_last = cycle_stats._time_since_last;

  const double process_time_last = generation_data._last_process_time;
  const double process_time_now = os::elapsed_process_vtime();
  const double process_time = process_time_now - process_time_last;
  generation_data._last_process_time = process_time_now;

  const bool is_major = Thread::current() == ZDriver::major();
  const GCCause::Cause cause = is_major ? ZDriver::major()->gc_cause() : ZDriver::minor()->gc_cause();
  const bool is_heap_pressure_gc = cause == GCCause::_z_allocation_rate ||
                                   cause == GCCause::_z_high_usage ||
                                   cause == GCCause::_z_warmup;

  if (!is_heap_pressure_gc) {
    return;
  }

  const double gc_time = serial_gc_time + parallel_gc_time + (is_young ? 0.0 : _accumulated_young_gc_time);

  generation_data._process_time.add(process_time);
  generation_data._gc_time.add(gc_time);

  const double avg_gc_time = generation_data._gc_time.avg();
  const double avg_process_time = generation_data._process_time.avg();

  const double avg_cpu_overhead = avg_gc_time / avg_process_time;

  log_debug(gc, heap)("Adaptive avg gc time %.3f, avg total time %.3f (%.3f%%)",
                      avg_gc_time, avg_process_time, avg_cpu_overhead * 100.0);

  // High GC frequencies lead to extra overheads such as barrier storms
  // Therefore, we add a factor that ensures there is at least some social
  // distancing between GCs, even when the GC overhead is small. The size of
  // the factor scales with the level of load induced on the machine.
  const double machine_load = (process_time / time_since_last) / double(os::active_processor_count());

  const double p = gc_pressure(machine_load);

  // When GC pressure is 10, the implication is that we want 25% of the
  // process CPU to be spent on doing GC when the process uses 100% of the
  // available CPU cores.. The ConcGCThreads sizing by default goes up to
  // a maximum of 25% of the available cores. So all ConcGCThreads would
  // be running back to back then.
  const double target_cpu_overhead = p / 40.0;
  const double cpu_overhead_error = avg_cpu_overhead - target_cpu_overhead;

  const double min_fully_loaded_gc_interval = 5.0 / p;
  const double min_gc_interval = min_fully_loaded_gc_interval / 4.0;
  const double gc_frequency_error = MAX2(min_gc_interval, machine_load * min_fully_loaded_gc_interval) - cycle_stats._time_since_last;

  const double sigmoid_error = sigmoid_function(MAX2(cpu_overhead_error, gc_frequency_error));
  double correction_factor = sigmoid_error + 0.5;

  log_info(gc, heap)("CPU Overhead Error: %.3f, GC Frequency Error: %.3f, Correction factor %.3f, Pressure: %.3f",
                     cpu_overhead_error, gc_frequency_error, correction_factor, p);

  if (is_young) {
    _accumulated_young_gc_time += gc_time;
    // Don't have enough data to shrink in young collections, so we try to
    // avoid it if we can. But in desperate times, when the machine is running
    // dry on memory, we will try to shrink even in young collections.
    double available_memory = (double)os::available_memory();
    double total_memory = (double)os::physical_memory();
    double memory_reserve_fraction = double(available_memory) / double(total_memory);

    if (memory_reserve_fraction > ZMemoryHighThreshold) {
      correction_factor = MAX2(correction_factor, 1.0);
    }
  } else {
    const double young_to_old_gc_time = _accumulated_young_gc_time / (_accumulated_young_gc_time + serial_gc_time + parallel_gc_time);
    Atomic::store(&_young_to_old_gc_time, young_to_old_gc_time);
    _accumulated_young_gc_time = 0.0;
  }

  ZHeap::heap()->resize_heap(correction_factor, p);
}

bool ZAdaptiveHeap::is_enabled() {
  return _enabled;
}

double ZAdaptiveHeap::young_to_old_gc_time() {
  return Atomic::load(&_young_to_old_gc_time);
}

// Exponentially increases as the last 15% of memory on the machine gets eaten.
double ZAdaptiveHeap::memory_pressure(double total_memory) {
  const double available_memory = (double)os::available_memory();
  const double memory_reserve_fraction = double(2.0 * available_memory / 3.0) / double(total_memory);
  const double linear_scaling = 1.0 - MIN2(ZMemoryHighThreshold, memory_reserve_fraction) / ZMemoryHighThreshold;

  // The natural exponential function seemed like a... natural choice.
  return pow(M_E, ZGCPressure * linear_scaling);
}

double ZAdaptiveHeap::gc_pressure(double cpu_usage) {
  const double total_memory = (double)os::physical_memory();
  const double memory_down_scaling = memory_pressure(total_memory);

  const double used_memory = (double)ZHeap::heap()->heuristic_max_capacity();
  const double memory_usage = used_memory / total_memory;

  const double cpu_up_scaling = MAX2(1.0, memory_usage / cpu_usage / 2.0);

  return ZGCPressure * cpu_up_scaling * memory_down_scaling;
}

uint64_t ZAdaptiveHeap::uncommit_delay() {
  if (!is_enabled()) {
    return ZUncommitDelay;
  }

  const double total_memory = (double)os::physical_memory();
  return uint64_t(ZUncommitDelay / memory_pressure(total_memory));
}
