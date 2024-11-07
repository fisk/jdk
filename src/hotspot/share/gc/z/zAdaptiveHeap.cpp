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
#include "utilities/debug.hpp"

#include <math.h>

bool ZAdaptiveHeap::_explicit_max_capacity;
volatile double ZAdaptiveHeap::_young_to_old_gc_time = 1.0;
double ZAdaptiveHeap::_accumulated_young_gc_time = 0.0;
ZAdaptiveHeap::ZGenerationOverhead ZAdaptiveHeap::_young_data;
ZAdaptiveHeap::ZGenerationOverhead ZAdaptiveHeap::_old_data;

void ZAdaptiveHeap::initialize(bool explicit_max_capacity) {
  double process_time_now = os::elapsed_process_vtime();
  double time_now = os::elapsedTime();
  _young_data._last_process_time = process_time_now;
  _old_data._last_process_time = process_time_now;
  _young_data._last_time = time_now;
  _old_data._last_time = time_now;
  _explicit_max_capacity = explicit_max_capacity;
}

double ZAdaptiveHeap::young_to_old_gc_time() {
  return Atomic::load(&_young_to_old_gc_time);
}

// Exponentially increases as the last 5% of memory on the machine gets eaten.
double ZAdaptiveHeap::memory_pressure(double unscaled_pressure, size_t used_memory, size_t compressed_memory, size_t total_memory) {
  const size_t available_memory = total_memory - used_memory;

  // The remaining memory reserve of the machine
  const double memory_reserve_fraction = double(available_memory) / double(total_memory);

  // Squared GC pressure is "high"
  const double high_pressure = MAX2(unscaled_pressure, 2.0);

  // The concerning threshold is after which memory utilization we start trying
  // harder to keep the memory down. There are multiple reasons for letting the GC
  // run hotter:
  // 1) We want to maintain some headeroom on the machine so that we can deal with
  //    spikes without getting allocation stalls.
  // 2) It's good to let the OS keep some file system cache memory around
  // 3) On systems that compress used memory, using compressed memory is not a
  //    free lunch as it leads to page faults that compress and decompress memory.
  //    This is extra painful for a tracing GC to traverse.
  const double concerning_compressed_threshold = double(compressed_memory) / double(used_memory) * (1.0 - ZMemoryConcerningThreshold);
  const double concerning_threshold = ZMemoryConcerningThreshold + concerning_compressed_threshold;

  if (memory_reserve_fraction < ZMemoryHighThreshold) {
    // When memory pressure is "high", we exponentially scale up memory pressure,
    // from the already "high" pressure induced by "concerning" memory pressure.
    const double progression = 1.0 - memory_reserve_fraction / ZMemoryHighThreshold;

    return high_pressure + pow(high_pressure, high_pressure * (1.0 + progression));
  }

  if (memory_reserve_fraction < concerning_threshold) {
    // When memory pressure is "concerning", we linearly scale up memory pressure to the
    // "high" GC pressure (i.e. gc pressure squared).
    const double progression = 1.0 - (memory_reserve_fraction - ZMemoryHighThreshold) / (concerning_threshold - ZMemoryHighThreshold);

    return 1.0 + ((high_pressure - 1.0) * progression);
  }

  return 1.0;
}

double ZAdaptiveHeap::gc_pressure(double unscaled_pressure, double cpu_usage) {
  const size_t total_memory = os::physical_memory();
  const size_t used_memory = os::used_memory();
  const size_t compressed_memory = MIN2(os::compressed_memory(), used_memory);
  const double mem_pressure = memory_pressure(unscaled_pressure, used_memory, compressed_memory, total_memory);

  const size_t heuristic_max_capacity = ZHeap::heap()->heuristic_max_capacity();
  const double memory_usage = double(heuristic_max_capacity) / double(total_memory);

  // The CPU overhead is scaled by what portion of CPU resources are being
  // used. As CPU utilization of the machine gets higher, there will be more
  // fighting between mutator threads for CPU time, affecting latencies.
  // Then we want to increasingly stay out of the way. If the process is
  // using over much of the CPU resources, don't bother trying to squish the
  // heap too much. In fact, then we can conversely increase the heap size
  // so that CPU can decrease a bit, avoiding latency issues due to too high
  // CPU utilization, to some reasonable limit.
  const double responsive_cpu_usage = cpu_usage / ZCPUConcerningThreshold;
  const double cpu_memory_usage_ratio = memory_usage / (responsive_cpu_usage + memory_usage);
  const double cpu_pressure = cpu_memory_usage_ratio * 2.0;

  const double scale = mem_pressure * cpu_pressure;

  const double result = MAX2(unscaled_pressure * scale, 1.0);

  log_info(gc, heap)("Scaled GC Pressure: %.1f, CPU Pressure: %.1f, Memory Pressure: %.1f, CPU Load: %.1f%%, Heap Memory: %.1f%%",
                     result, cpu_pressure, mem_pressure, cpu_usage * 100.0, memory_usage * 100.0);

  return result;
}

// Logistic function, produces values in the range 0 - 1 in an S shape
static double sigmoid_function(double value) {
  return 1.0 / (1.0 + pow(M_E, -value));
}

// This function smoothens out measured error signals to make the incremental heap
// sizing converge better. During an initial warmup period, a more aggressive function
// is used, which doesn't try to reduce the error signals. This reduces the number of
// early GCs before the system has had any chance to converge to a stable heap size.
static double smoothing_function(double value) {
  const double warmup_time_seconds = 3.0;
  const double sigmoid = sigmoid_function(value);
  const double aggressive = MAX2(sigmoid, 0.5 + value);
  const double heat = MIN2(os::elapsedTime(), warmup_time_seconds) / warmup_time_seconds;

  return sigmoid * heat + aggressive * (1.0 - heat);
}

size_t ZAdaptiveHeap::compute_heap_size(ZHeapResizeMetrics* metrics, ZGenerationId generation) {
  double unscaled_pressure = Atomic::load(&ZGCPressure);

  if (unscaled_pressure <= 0.0) {
    // Don't adapt anything when turned off
    return metrics->_heuristic_max_capacity;
  }

  const bool is_major = Thread::current() == ZDriver::major();
  const GCCause::Cause cause = is_major ? ZDriver::major()->gc_cause() : ZDriver::minor()->gc_cause();
  const bool is_heap_pressure_gc = cause == GCCause::_z_allocation_rate ||
                                   cause == GCCause::_z_high_usage ||
                                   cause == GCCause::_z_warmup;

  if (!is_heap_pressure_gc) {
    // If this isn't a GC pressure triggered GC, don't resize or learn anything
    return metrics->_heuristic_max_capacity;
  }

  ZStatWorkersStats worker_stats = ZGeneration::generation(generation)->stat_workers()->stats();
  ZStatCycleStats cycle_stats = ZGeneration::generation(generation)->stat_cycle()->stats();

  const bool is_young = generation == ZGenerationId::young;
  ZGenerationOverhead& generation_data = is_young ? _young_data : _old_data;

  // Time metrics
  const double process_time_last = generation_data._last_process_time;
  const double process_time_now = os::elapsed_process_vtime();
  const double process_time = process_time_now - process_time_last;
  const double time_now = os::elapsedTime();
  const double time_last = generation_data._last_time;
  const double time_since_last = time_now - time_last;
  generation_data._last_process_time = process_time_now;
  generation_data._last_time = time_now;

  // Heap size metrics
  const size_t soft_max_capacity = metrics->_soft_max_capacity;
  const size_t current_max_capacity = metrics->_current_max_capacity;
  const size_t heuristic_max_capacity = metrics->_heuristic_max_capacity;
  const size_t capacity = metrics->_capacity;
  const size_t min_capacity = metrics->_min_capacity;
  const size_t used = metrics->_used;

  double ncpus = double(os::active_processor_count());
  const double machine_load = clamp((process_time / time_since_last) / ncpus, 0.0, 1.0);
  const double scaled_pressure = gc_pressure(unscaled_pressure, machine_load);
  generation_data._gc_pressure.add(scaled_pressure);
  const double pressure = generation_data._gc_pressure.avg();

  // Since a GC cycle is obviously round, we can estimate the minimum bytes due to
  // a particular allocation rate and GC pressure by calculating GC pressure * pi
  const double alloc_rate_scaling = pressure * M_PI;
  const double alloc_rate = metrics->_alloc_rate;
  const size_t heuristic_low = MAX2(size_t(used * 1.1), size_t(alloc_rate / alloc_rate_scaling));

  const size_t upper_bound = MIN2(soft_max_capacity, current_max_capacity);
  size_t lower_bound = clamp(heuristic_low, min_capacity, upper_bound);

  const double gc_time = cycle_stats._last_total_vtime + (is_young ? 0.0 : _accumulated_young_gc_time);

  generation_data._process_time.add(process_time);
  generation_data._gc_time.add(gc_time);
  generation_data._gc_time_since_last.add(time_since_last);

  const double avg_gc_time = generation_data._gc_time.avg();
  const double avg_time_since_last = generation_data._gc_time_since_last.avg();
  const double avg_process_time = generation_data._process_time.avg();
  const double avg_cpu_overhead = avg_gc_time / avg_process_time;

  // When GC pressure is 10, the implication is that we want 25% of the
  // process CPU to be spent on doing GC when the process uses 100% of the
  // available CPU cores.. The ConcGCThreads sizing by default goes up to
  // a maximum of 25% of the available cores. So all ConcGCThreads would
  // be running back to back then.
  const double target_cpu_overhead = pressure / 40.0;
  const double cpu_overhead_error = avg_cpu_overhead - target_cpu_overhead;

  // High GC frequencies lead to extra overheads such as barrier storms
  // Therefore, we add a factor that ensures there is at least some social
  // distancing between GCs, even when the GC overhead is small. The size of
  // the factor scales with the level of load induced on the machine.
  const double min_fully_loaded_gc_interval = 5.0 / pressure;
  const double min_gc_interval = min_fully_loaded_gc_interval / 4.0;
  const double target_gc_interval = MAX2(min_gc_interval, machine_load * min_fully_loaded_gc_interval);
  const double gc_interval_error = target_gc_interval - avg_time_since_last;

  double error_signal = MAX2(cpu_overhead_error, gc_interval_error);

  if (is_young) {
    _accumulated_young_gc_time += gc_time;

    // Don't have enough data to shrink in young collections, so we don't do it.
    error_signal = MAX2(error_signal, 0.0);
  } else {
    const double young_to_old_gc_time = _accumulated_young_gc_time / (_accumulated_young_gc_time + cycle_stats._last_total_vtime);
    Atomic::store(&_young_to_old_gc_time, young_to_old_gc_time);
    _accumulated_young_gc_time = 0.0;
  }

  const double smoothened_error = smoothing_function(error_signal);
  const double correction_factor = smoothened_error + 0.5;

  const size_t suggested_capacity = align_up(size_t(heuristic_max_capacity * correction_factor), ZGranuleSize);
  const size_t selected_capacity = clamp(suggested_capacity, lower_bound, upper_bound);
  const ssize_t capacity_resize = ssize_t(selected_capacity) - ssize_t(heuristic_max_capacity);

  log_info(gc, heap)("GC CPU Overhead: %.1f%% (%.1f%%), Target GC CPU Overhead: %.1f%% (%.1f%%)",
                     avg_cpu_overhead * 100.0, avg_cpu_overhead * machine_load * 100.0,
                     target_cpu_overhead * 100.0, target_cpu_overhead * machine_load * 100.0);
  log_info(gc, heap)("GC Interval: %.3fs, Target Minimum: %.3fs",
                     avg_time_since_last, target_gc_interval);
  log_debug(gc, heap)("Target heap lower bound: " SIZE_FORMAT ", upper bound: " SIZE_FORMAT,
                      lower_bound / M, upper_bound / M);
  log_debug(gc, heap)("Suggested capacity: " SIZE_FORMAT ", selected capacity: " SIZE_FORMAT ", heuristic capacity: " SIZE_FORMAT,
                      suggested_capacity / M, selected_capacity / M, heuristic_max_capacity / M);
  log_debug(gc, heap)("Updated heuristic max capacity: " SIZE_FORMAT "M (%.3f%%), current capacity: " SIZE_FORMAT "M",
                      selected_capacity / M, double(selected_capacity) / double(heuristic_max_capacity) * 100.0 - 100.0, capacity / M);

  if (capacity_resize > 0) {
    log_info(gc, heap)("Heap Increase " SIZE_FORMAT "M (%.1f%%)", capacity_resize / M, double(capacity_resize) / double(heuristic_max_capacity) * 100.0);
  } else if (capacity_resize < 0) {
    log_info(gc, heap)("Heap Decrease " SIZE_FORMAT "M (%.1f%%)", -capacity_resize / M, double(-capacity_resize) / double(heuristic_max_capacity) * 100.0);
  }

  return selected_capacity;
}

uint64_t ZAdaptiveHeap::uncommit_delay() {
  const size_t total_memory = os::physical_memory();
  const size_t used_memory = os::used_memory();
  const size_t compressed_memory = MIN2(os::compressed_memory(), used_memory);

  // If we are critically low on memory, aggressively free up memory
  if (double(used_memory) / double(total_memory) >= 1.0 - ZMemoryCriticalThreshold) {
    return 0;
  }

  const double unscaled_pressure = Atomic::load(&ZGCPressure);
  const double excess_pressure = memory_pressure(unscaled_pressure, used_memory, compressed_memory, total_memory) - 1.0;
  const double pressure = 1.0 + excess_pressure * unscaled_pressure;

  return uint64_t(ZUncommitDelay / pressure);
}

size_t ZAdaptiveHeap::current_max_capacity(size_t capacity) {
  const size_t machine_memory = os::physical_memory();
  const size_t used_memory = os::used_memory();
  const size_t hard_machine_memory_limit = machine_memory * (1.0 - ZMemoryCriticalThreshold);
  const size_t available_machine_memory = used_memory > hard_machine_memory_limit ? 0 : (hard_machine_memory_limit - used_memory);
  // It is a bit naive to assume all available memory can be directly turned
  // into our own heap memory. We need auxiliary GC data structures, and other
  // processes can also take the memory as we might not be alone. By scaling
  // the available memory we stay on the pessimistic size, and let the estimated
  // current max capacity grow gradually as we approach the limits instead.
  const size_t scaled_available_machine_memory = available_machine_memory * 0.2;
  const size_t max_capacity_available = align_down(capacity + scaled_available_machine_memory, ZGranuleSize);

  return MIN2(max_capacity_available, machine_memory);
}
