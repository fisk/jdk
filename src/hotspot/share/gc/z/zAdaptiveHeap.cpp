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

#include "gc/shared/gc_globals.hpp"
#include "gc/shared/gcLogPrecious.hpp"
#include "gc/z/zAdaptiveHeap.hpp"
#include "gc/z/zDriver.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zStat.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals_extension.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

#include <limits>
#include <math.h>

bool ZAdaptiveHeap::_explicit_max_capacity;
TruncatedSeq ZAdaptiveHeap::_gc_pressures;

volatile double ZAdaptiveHeap::_young_to_old_gc_time = 1.0;
double ZAdaptiveHeap::_accumulated_young_gc_time = 0.0;
ZAdaptiveHeap::ZGenerationOverhead ZAdaptiveHeap::_young_data;
ZAdaptiveHeap::ZGenerationOverhead ZAdaptiveHeap::_old_data;

static ZLock* _stat_lock;

bool ZAdaptiveHeap::can_adapt() {
  bool static_heap = ZAdaptiveHeap::explicit_max_capacity() && MinHeapSize == MaxHeapSize;
  return !static_heap && Atomic::load(&ZGCPressure) != 0.0;
}

void ZAdaptiveHeap::initialize(bool explicit_max_capacity) {
  double process_time_now = os::elapsed_process_cpu_time();
  double system_time_now = os::elapsed_system_cpu_time();
  double time_now = os::elapsedTime();
  _young_data._last_system_time = process_time_now;
  _old_data._last_system_time = process_time_now;
  _young_data._last_process_time = process_time_now;
  _old_data._last_process_time = process_time_now;
  _young_data._last_time = time_now;
  _old_data._last_time = time_now;
  _explicit_max_capacity = explicit_max_capacity;
  _stat_lock = new ZLock();
}

double ZAdaptiveHeap::young_to_old_gc_time() {
  return Atomic::load(&_young_to_old_gc_time);
}

ZMemoryPressureMetrics ZAdaptiveHeap::memory_pressure_metrics() {
  const size_t system_max_memory = os::physical_memory();
  const size_t system_used_memory = os::used_memory();
  const size_t system_compressed_memory = MIN2(size_t(os::compressed_memory()), system_used_memory);
  const double unscaled_gc_pressure = Atomic::load(&ZGCPressure);

  return {unscaled_gc_pressure, system_used_memory, system_compressed_memory, system_max_memory};
}

static double concerning_threshold(const ZMemoryPressureMetrics& metrics) {
  // The concerning threshold is after which memory utilization we start trying
  // harder to keep the memory down. There are multiple reasons for letting the GC
  // run hotter:
  // 1) We want to maintain some headeroom on the machine so that we can deal with
  //    spikes without getting allocation stalls.
  // 2) It's good to let the OS keep some file system cache memory around
  // 3) On systems that compress used memory, using compressed memory is not a
  //    free lunch as it leads to page faults that compress and decompress memory.
  //    This is extra painful for a tracing GC to traverse.
  const double compression_rate = double(metrics._system_compressed_memory) / double(metrics._system_max_memory);

  return MIN2(ZMemoryConcerningThreshold + compression_rate, 1.0);
}

static double high_threshold(const ZMemoryPressureMetrics& metrics) {
  const double concerning_vs_high_diff = ZMemoryConcerningThreshold - ZMemoryHighThreshold;

  return concerning_threshold(metrics) - concerning_vs_high_diff;
}

static double critical_threshold(const ZMemoryPressureMetrics& metrics) {
  return ZMemoryCriticalThreshold;
}

double ZAdaptiveHeap::memory_pressure(const ZMemoryPressureMetrics& metrics) {
  const size_t available_memory = metrics._system_max_memory - metrics._system_used_memory;

  // The remaining memory reserve of the machine
  const double availability = double(available_memory) / double(metrics._system_max_memory);

  // A number indicating how much the memory pressure should grow as the
  // memory unavailability grows
  const double pressure_rate = MAX2(metrics._unscaled_gc_pressure, 2.0);

  const double concerning = concerning_threshold(metrics);
  const double high = high_threshold(metrics);

  if (availability < high) {
    // When memory pressure is "high", we exponentially scale up memory pressure,
    // from the already "high" pressure induced by "concerning" memory pressure.
    const double progression = 1.0 - availability / high;
    const double exponent = 1.0 + progression * pressure_rate;
    return pressure_rate + pow(pressure_rate, exponent);
  }

  if (availability < concerning) {
    // When memory pressure is "concerning", we linearly scale up memory pressure to the
    // "high" GC pressure (i.e. gc pressure squared).
    const double progression = 1.0 - (availability - high) / (concerning - high);
    const double pressure_factor = (pressure_rate - 1.0) * progression;

    return 1.0 + pressure_factor;
  }

  return 1.0;
}

bool ZAdaptiveHeap::is_memory_pressure_concerning(const ZMemoryPressureMetrics& metrics) {
  const size_t available_memory = metrics._system_max_memory - metrics._system_used_memory;
  const double availability = double(available_memory) / double(metrics._system_max_memory);

  return availability < concerning_threshold(metrics);
}

bool ZAdaptiveHeap::is_memory_pressure_high(const ZMemoryPressureMetrics& metrics) {
  const size_t available_memory = metrics._system_max_memory - metrics._system_used_memory;
  const double availability = double(available_memory) / double(metrics._system_max_memory);

  return availability < high_threshold(metrics);
}

bool ZAdaptiveHeap::is_memory_pressure_critical(const ZMemoryPressureMetrics& metrics) {
  const size_t available_memory = metrics._system_max_memory - metrics._system_used_memory;
  const double availability = double(available_memory) / double(metrics._system_max_memory);

  return availability < critical_threshold(metrics);
}

double ZAdaptiveHeap::gc_pressure(double unscaled_gc_pressure, double process_cpu_usage, double system_cpu_usage, double& mem_pressure) {
  const size_t system_max_memory = os::physical_memory();
  const size_t system_used_memory = os::used_memory();
  const double system_memory_usage = double(system_used_memory) / double(system_max_memory);
  const size_t heap_capacity = ZHeap::heap()->capacity();
  const size_t system_compressed_memory = MIN2(size_t(os::compressed_memory()), system_used_memory);

  const ZMemoryPressureMetrics mem_pressure_metrics{unscaled_gc_pressure,
                                                    system_used_memory,
                                                    system_compressed_memory,
                                                    system_max_memory};

  mem_pressure = memory_pressure(mem_pressure_metrics);

  const size_t heuristic_max_capacity = ZHeap::heap()->heuristic_max_capacity();
  const size_t process_used_memory = os::rss();
  const size_t process_non_heap_memory = process_used_memory > heap_capacity ? process_used_memory - heap_capacity : 0;
  const size_t projected_process_used_memory = heuristic_max_capacity + process_non_heap_memory;

  const double process_memory_usage_ratio = double(projected_process_used_memory) / double(system_used_memory);
  const double process_cpu_usage_ratio = process_cpu_usage / system_cpu_usage;

  // The GC pressure is scaled by the relationship of how many of the system's
  // used bytes belong to this process compared to how many of the used system
  // CPU ticks belong to this process. For a single application deployment this
  // has effectively no effect, while for a multi process deployment, processes
  // that are unproportionally memory bloated compared to other processes will
  // rebalance themselves better to provide more memory for other processes.
  const double process_cpu_pressure = 1.0 / (1.0 + clamp(process_cpu_usage_ratio - process_memory_usage_ratio, -0.1, 1.0));

  // The GC pressure is scaled by what portion of system CPU resources are being
  // used. As CPU utilization of the machine gets higher, there will be more
  // fighting between mutator threads for CPU time, affecting latencies.
  // Then we want GC to increasingly stay out of the way. If the process is
  // using much of the CPU resources, don't bother trying to squish the
  // heap too much. In fact, then we can conversely increase the heap size
  // so that CPU can decrease a bit, avoiding latency issues due to too high
  // CPU utilization, to some reasonable limit.
  const double responsive_system_cpu_usage = system_cpu_usage / ZCPUConcerningThreshold;
  const double system_cpu_pressure = 1.0 / (1.0 + clamp(responsive_system_cpu_usage - system_memory_usage, -0.1, 1.0));

  // Balance the forces of resource share imbalance across processes with the
  // forces of system lvel resource usage imbalance.
  const double cpu_pressure = process_cpu_pressure * system_cpu_pressure;

  // The combined forces of memory vs CPU.
  const double scale = mem_pressure * cpu_pressure;

  const double scaled_pressure = unscaled_gc_pressure * scale;
  double gc_pressure;

  {
    ZLocker<ZLock> locker(_stat_lock);
    _gc_pressures.add(scaled_pressure);
    gc_pressure = MAX2(_gc_pressures.avg(), scaled_pressure);
  }

  if (can_adapt()) {
    log_debug(gc, heap)("Process CPU Pressure: %.1f, System CPU Pressure: %.1f, System Memory Pressure: %.1f",
                        process_cpu_pressure, system_cpu_pressure, mem_pressure);
    log_debug(gc, heap)("GC Pressure: %.1f, GC Pressure Scaling: %.1f",
                        scaled_pressure, scale);
  }

  log_info(gc, load)("System Memory Load: %.1f%%, Process Memory Load: %.1f%%, Heap Memory Load: %.1f%%",
                     double(system_used_memory) / double(system_max_memory) * 100.0,
                     double(projected_process_used_memory) / double(system_max_memory) * 100.0,
                     double(heuristic_max_capacity) / double(system_max_memory) * 100.0);

  return gc_pressure;
}

// Logistic function, produces values in the range 0 - 1 in an S shape
static double sigmoid_function(double value) {
  return 1.0 / (1.0 + pow(M_E, -value));
}

// This function smoothens out measured error signals to make the incremental heap
// sizing converge better. During an initial warmup period, a more aggressive function
// is used, which doesn't try to reduce the error signals. This reduces the number of
// early GCs before the system has had any chance to converge to a stable heap size.
static double smoothing_function(double value, double warmness) {
  const double sigmoid = sigmoid_function(value);
  const double aggressive = MIN2(MAX2(sigmoid, 0.5 + value), 2.0);

  return sigmoid * warmness + aggressive * (1.0 - warmness);
}

size_t ZAdaptiveHeap::compute_heap_size(ZHeapResizeMetrics* metrics, ZGenerationId generation) {
  double unscaled_gc_pressure = Atomic::load(&ZGCPressure);

  const bool is_major = Thread::current() == ZDriver::major();
  const GCCause::Cause cause = is_major ? ZDriver::major()->gc_cause() : ZDriver::minor()->gc_cause();
  const bool is_heap_anti_pressure_gc = cause == GCCause::_z_proactive;
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
  const double system_time_last = generation_data._last_system_time;
  const double process_time_now = os::elapsed_process_cpu_time();
  // Note that the system time might have poor accuracy early on; it typically
  // has 100 ms granularity. So take it with a large grain of salt early on...
  const double system_time_now = os::elapsed_system_cpu_time();
  const double process_time = process_time_now - process_time_last;
  const double system_time = system_time_now - system_time_last;
  const double time_now = os::elapsedTime();
  const double time_last = generation_data._last_time;
  const double time_since_last = time_now - time_last;
  generation_data._last_process_time = process_time_now;
  generation_data._last_system_time = system_time_now;
  generation_data._last_time = time_now;

  // Heap size metrics
  const size_t soft_max_capacity = metrics->_soft_max_capacity;
  const size_t current_max_capacity = metrics->_current_max_capacity;
  const size_t heuristic_max_capacity = metrics->_heuristic_max_capacity;
  const size_t capacity = metrics->_capacity;
  const size_t min_capacity = metrics->_min_capacity;
  const size_t used = metrics->_used;

  if (is_heap_anti_pressure_gc) {
    // The GC is bored. The impact of shrinking should not cost a considerable amount of
    // CPU, or we would not get here.
    const size_t selected_capacity = MAX2(size_t(double(heuristic_max_capacity) * 0.95), used);
    return clamp(align_down(selected_capacity, ZGranuleSize), min_capacity, current_max_capacity);
  }

  double ncpus = double(os::active_processor_count());
  const double warmup_time_seconds = 3.0;
  const double warmness = MIN2(os::elapsedTime(), warmup_time_seconds) / warmup_time_seconds;
  const double warmness_squared = warmness * warmness;

  const double gc_time = cycle_stats._last_total_vtime + (is_young ? 0.0 : _accumulated_young_gc_time);

  const double gc_cpu_load = clamp((gc_time / time_since_last) / ncpus, 0.0, 1.0);
  const double process_cpu_load = clamp((process_time / time_since_last) / ncpus, 0.0, 1.0);
  const double system_cpu_load = clamp((system_time / time_since_last) / ncpus, 0.0, 1.0);

  generation_data._process_times.add(process_time);
  generation_data._system_times.add(system_time);
  generation_data._gc_times.add(gc_time);
  generation_data._gc_times_since_last.add(time_since_last);

  const double avg_gc_time = generation_data._gc_times.avg();
  const double avg_time_since_last = generation_data._gc_times_since_last.avg();
  const double avg_process_time = generation_data._process_times.avg();
  const double avg_system_time = generation_data._system_times.avg();
  const double avg_cpu_overhead = avg_gc_time / avg_process_time;
  const double avg_process_cpu_load = clamp((avg_process_time / avg_time_since_last) / ncpus, 0.0, 1.0);
  const double avg_system_cpu_load = clamp((avg_system_time / avg_time_since_last) / ncpus, 0.0, 1.0);

  // Calculate the GC pressure that scales the rest of the heuristics
  double mem_pressure = 1.0;
  const double pressure = gc_pressure(unscaled_gc_pressure, avg_process_cpu_load, avg_system_cpu_load, mem_pressure);

  // Calculate the heuristic lower bound for the heuristic heap
  const double alloc_rate = metrics->_alloc_rate;
  // Since a GC cycle is obviously round, we can estimate the minimum bytes due to
  // a particular allocation rate and GC pressure by calculating GC pressure * pi.
  const double alloc_rate_scaling = warmness_squared / (pressure * M_PI);
  const size_t heuristic_low = align_down(size_t(double(MAX2(size_t(double(used) * 1.1), size_t(alloc_rate * alloc_rate_scaling))) / mem_pressure), ZGranuleSize);

  const size_t upper_bound = MIN2(soft_max_capacity, current_max_capacity);
  const size_t lower_bound = clamp(heuristic_low, min_capacity, upper_bound);

  const double current_cpu_overhead = gc_time / process_time;

  // Account for the overhead of old generation collections when evaluating
  // the heap efficiency for young generation collections.
  const double avg_cpu_overhead_conservative = avg_cpu_overhead / (is_young ? Atomic::load(&_young_to_old_gc_time) : 1.0);
  const double current_cpu_overhead_conservative = current_cpu_overhead / (is_young ? Atomic::load(&_young_to_old_gc_time) : 1.0);

  // When GC pressure is 10, the implication is that we want 25% of the
  // process CPU to be spent on doing GC when the process uses 100% of the
  // available CPU cores.. The ConcGCThreads sizing by default goes up to
  // a maximum of 25% of the available cores. So all ConcGCThreads would
  // be running back to back then.
  const double target_cpu_overhead = pressure / 40.0;

  const double upper_cpu_overhead = MAX2(avg_cpu_overhead_conservative, current_cpu_overhead);
  const double upper_cpu_overhead_error = upper_cpu_overhead - target_cpu_overhead;

  const double lower_cpu_overhead = MIN2(avg_cpu_overhead_conservative, current_cpu_overhead);
  const double lower_cpu_overhead_error = lower_cpu_overhead - target_cpu_overhead;

  // High GC frequencies lead to extra overheads such as barrier storms
  // Therefore, we add a factor that ensures there is at least some social
  // distancing between GCs, even when the GC overhead is small. The size of
  // the factor scales with the level of load induced on the machine.
  const double min_fully_loaded_gc_interval = 5.0 / unscaled_gc_pressure;
  const double min_gc_interval = min_fully_loaded_gc_interval / 4.0 / mem_pressure;
  const double target_gc_interval = MAX2(min_gc_interval, process_cpu_load * min_fully_loaded_gc_interval);
  const double upper_gc_interval_error = MAX2(target_gc_interval - avg_time_since_last, target_gc_interval - time_since_last);
  const double lower_gc_interval_error = MIN2(target_gc_interval - avg_time_since_last, target_gc_interval - time_since_last);

  const double upper_error_signal = MAX2(upper_cpu_overhead_error, upper_gc_interval_error);
  const double lower_error_signal = MAX2(lower_cpu_overhead_error, lower_gc_interval_error);

  if (is_young) {
    _accumulated_young_gc_time += gc_time;
  } else {
    const double young_to_old_gc_time = _accumulated_young_gc_time / (_accumulated_young_gc_time + cycle_stats._last_total_vtime);
    Atomic::store(&_young_to_old_gc_time, young_to_old_gc_time);
    _accumulated_young_gc_time = 0.0;
  }

  const double upper_smoothened_error = smoothing_function(upper_error_signal, warmness);
  const double upper_correction_factor = upper_smoothened_error + 0.5;

  const double lower_smoothened_error = smoothing_function(lower_error_signal, warmness);
  const double lower_correction_factor = lower_smoothened_error + 0.5;

  const size_t upper_suggested_capacity = align_up(size_t(double(heuristic_max_capacity) * upper_correction_factor), ZGranuleSize);
  const size_t lower_suggested_capacity = align_up(size_t(double(heuristic_max_capacity) * lower_correction_factor), ZGranuleSize);

  const size_t upper_bounded_capacity = clamp(upper_suggested_capacity, lower_bound, upper_bound);
  const size_t lower_bounded_capacity = clamp(lower_suggested_capacity, lower_bound, upper_bound);

  // Grow if we experience short term *and* long term pressure on the heap
  const bool should_grow = lower_bounded_capacity > heuristic_max_capacity && upper_bounded_capacity > heuristic_max_capacity;
  // Grow if we experience short term *and* long term reverse pressure on the heap
  const bool should_shrink = lower_bounded_capacity < heuristic_max_capacity && upper_bounded_capacity < heuristic_max_capacity;

  log_info(gc, load)("System CPU Load: %.1f%%, Process CPU Load: %.1f%%, GC CPU Load: %.1f%%",
                     avg_system_cpu_load * 100.0, avg_process_cpu_load * 100.0, avg_cpu_overhead_conservative * avg_process_cpu_load * 100.0);

  if (can_adapt()) {
    log_info(gc, heap)("Process GC CPU Overhead: %.1f%%, Target Process GC CPU Overhead: %.1f%%",
                       avg_cpu_overhead_conservative * 100.0, target_cpu_overhead * 100.0);
    log_debug(gc, heap)("GC Interval: %.3fs, Target Minimum: %.3fs",
                        avg_time_since_last, target_gc_interval);
    log_debug(gc, heap)("Target heap lower bound: %zuM, upper bound: %zuM",
                        lower_bound / M, upper_bound / M);
    log_debug(gc, heap)("Suggested capacity range: %zuM - %zuM, heuristic capacity: %zuM",
                        lower_suggested_capacity / M, upper_suggested_capacity / M, heuristic_max_capacity / M);
  }

  if (should_grow) {
    const size_t selected_capacity = MIN2(upper_bounded_capacity, lower_bounded_capacity);
    const size_t capacity_resize = selected_capacity - heuristic_max_capacity;

    log_debug(gc, heap)("Updated heuristic max capacity: %zuM (%.3f%%), current capacity: %zuM",
                        selected_capacity / M, double(selected_capacity) / double(heuristic_max_capacity) * 100.0 - 100.0, capacity / M);

    log_info(gc, heap)("Heap Increase %zuM (%.1f%%)", capacity_resize / M, double(capacity_resize) / double(heuristic_max_capacity) * 100.0);

    return selected_capacity;
  } else if (should_shrink) {
    // We want to shrink slower than we grow; by splitting the proposed shrinking into a fraction,
    // we get a slower tail of shrinking, which avoids unnecessary fluctuations up and down.
    const size_t shrinking_fraction = 5;

    const size_t proposed_selected_capacity = MAX2(upper_bounded_capacity, lower_bounded_capacity);
    const size_t capacity_resize = align_up((heuristic_max_capacity - proposed_selected_capacity) / shrinking_fraction, ZGranuleSize);

    const size_t selected_capacity = heuristic_max_capacity - capacity_resize;

    log_debug(gc, heap)("Updated heuristic max capacity: %zuM (%.3f%%), current capacity: %zuM",
                        selected_capacity / M, double(selected_capacity) / double(heuristic_max_capacity) * 100.0 - 100.0, capacity / M);

    log_info(gc, heap)("Heap Decrease %zuM (%.1f%%)", capacity_resize / M, double(capacity_resize) / double(heuristic_max_capacity) * 100.0);
    return selected_capacity;
  }

  return heuristic_max_capacity;
}

uint64_t ZAdaptiveHeap::uncommit_delay(size_t used_memory, size_t total_memory) {
  if (explicit_max_capacity()) {
    return ZUncommitDelay;
  }

  const size_t available_memory = total_memory - used_memory;

  // If we are critically low on memory, aggressively free up memory
  if (double(used_memory) / double(total_memory) >= 1.0 - ZMemoryCriticalThreshold) {
    return 0;
  }

  // If we aren't low on memory, disable timer based uncommit; let
  // the GC heuristics guide the heap down instead, as part of the
  // natural control system.
  if (double(used_memory) / double(total_memory) < 1.0 - ZMemoryHighThreshold) {
    return std::numeric_limits<uint64_t>::max();
  }

  // If we are low on memory, start the clocks for uncommitting memory
  // We use a policy where the uncommit delay drops off farily quickly
  // as the memory pressure gets "high" to let uncommitting react before
  // the next GC, but still without being brutal.
  // When the memory availability becomes critical, more brutal uncommitting
  // will commence.

  // The remaining memory reserve of the machine
  const double available_fraction = double(available_memory) / double(total_memory);

  // Progression until critical uncommitting starts
  const double progression = 1.0 - (available_fraction - ZMemoryCriticalThreshold) / (ZMemoryHighThreshold - ZMemoryCriticalThreshold);

  // Select an nth root based on the progression
  const double root = 1.0 / (1.0 + progression);

  return (uint64_t)pow(ZUncommitDelay, root);
}

uint64_t ZAdaptiveHeap::soft_ref_delay() {
  ZStatHeap* const stats = ZGeneration::old()->stat_heap();
  // Young generation should have mostly transient state;
  // consider it as basically free.
  const size_t old_used_reloc_end = stats->used_generation_at_relocate_end();
  const size_t target_capacity = MAX2(ZHeap::heap()->heuristic_max_capacity(), old_used_reloc_end);
  const size_t free_heap = target_capacity - old_used_reloc_end;

  const uint64_t explicit_delay = free_heap / M * uint64_t(SoftRefLRUPolicyMSPerMB);

  if (explicit_max_capacity()) {
    // Use the good old policy we all know and love so much when automatic heap
    // sizing is not in use.
    return explicit_delay;
  }

  // With automatic heap sizing, there is a risk for a feedback loop when the amount
  // free memory decides how long soft references survive. More soft references will
  // lead to the heap growing, hence creating more free memory and suddenly letting
  // soft references live for longer. In order to cut this feedback loop, a more
  // involved policy is used.
  //
  // The more involved strategy scales the delay with the time it would take for the
  // heap to get filled up by old generation allocations multiplied by a scaled
  // variation of SoftRefLRUPolicyMSPerMB. The scaling is more aggressive than linear
  // by computing the nth root of SoftRefLRUPolicyMSPerMB, where n is some memory
  // pressure.

  // Scale the delay by the old generation allocation rate; the faster it fills up,
  // the more rapidly we need to prune soft references
  const double avg_time_since_last = _old_data._gc_times_since_last.avg();
  const size_t old_live = stats->live_at_mark_end();
  const size_t old_used = ZHeap::heap()->used_old();
  const size_t old_allocated = old_used - old_live;
  const double old_alloc_rate = MAX2(old_allocated / M / avg_time_since_last, 1.0);

  const double time_to_old_oom = free_heap / M / old_alloc_rate;

  const double free_ratio = double(target_capacity) / double(free_heap);

  const double mem_pressure = free_ratio;

  // No point to clear more soft references due to external memory pressure if the
  // Scale the SoftRefLRUPolicyMSPerMB as an nth root where n is the memory pressure.
  // The reason for using the nth root is that it might not necessarily be that
  // linearly decreasing the interval with memory pressure yields linearly more
  // soft references being cleared. It rather depends on the access frequency.
  // If they get accessed very frequently, then it's likely that no soft reference
  // get cleared at all, until the interval is made *very* small. Therefore, the
  // more aggressive nth root is used.
  const uint64_t scaled_interval = uint64_t(pow(SoftRefLRUPolicyMSPerMB, 1.0 / mem_pressure));

  // Compute the potentially more aggressive delay that cuts the feedback loop.
  const uint64_t implicit_delay = uint64_t(time_to_old_oom * double(scaled_interval));

  // If the new policy yields earlier cut off points, then use that. Otherwise,
  // we still use the more relaxed policy to cut off soft references when they
  // have not been used for unreasonably long. While we could keep them around
  // forever, it might also be a bit pointless.
  const uint64_t delay = MIN2(implicit_delay, explicit_delay);

  log_info(gc, ref)("Soft ref timeout: %.3fs", double(delay) / 1000);

  LogTarget(Debug, gc, ref) lt;
  if (lt.is_enabled()) {
    LogStream ls(lt);

    ls.print_cr("Soft ref time to old generation OOM: %.3fs", time_to_old_oom);
    ls.print_cr("Soft ref explicit timeout: %.3fs", double(explicit_delay) / 1000);
    ls.print_cr("Soft ref implicit timeout: %.3fs", double(implicit_delay) / 1000);
    ls.print_cr("Soft ref memory pressure: %.3f", mem_pressure);
  }

  return delay;
}

size_t ZAdaptiveHeap::current_max_capacity(size_t capacity, size_t dynamic_max_capacity) {
  const size_t used_memory = os::used_memory();
  const ssize_t available_memory = ssize_t(dynamic_max_capacity) - ssize_t(used_memory);
  // It is a bit naive to assume all available memory can be directly turned
  // into our own heap memory. We need auxiliary GC data structures, and other
  // processes can also take the memory as we might not be alone. By scaling
  // the available memory we stay on the pessimistic size, and let the estimated
  // current max capacity grow gradually as we approach the limits instead.
  const size_t scaled_available_memory = available_memory >= 0 ? (available_memory * (1.0 - ZMemoryCriticalThreshold))
                                                               : 0;
  const size_t max_available = align_down(capacity + scaled_available_memory, ZGranuleSize);

  return MIN2(max_available, dynamic_max_capacity);
}

void ZAdaptiveHeap::print() {
  const char* status;
  if (!can_adapt()) {
    status = "Manual";
  } else if (explicit_max_capacity() ||
             FLAG_IS_CMDLINE(MinHeapSize)) {
    status = "Bounded Automatic";
  } else {
    status = "Automatic";
  }
  log_info_p(gc, init)("Heap Sizing: %s", status);
}
