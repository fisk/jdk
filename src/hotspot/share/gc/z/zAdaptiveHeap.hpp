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

#ifndef SHARE_GC_Z_ZADAPTIVEHEAP_HPP
#define SHARE_GC_Z_ZADAPTIVEHEAP_HPP

#include "gc/z/zGenerationId.hpp"
#include "memory/allocation.hpp"
#include "gc/z/zStat.hpp"
#include "utilities/numberSeq.hpp"

struct ZHeapResizeMetrics {
  const size_t _soft_max_capacity;
  const size_t _current_max_capacity;
  const size_t _heuristic_max_capacity;
  const size_t _min_capacity;
  const size_t _capacity;
  const size_t _used;
  const double _alloc_rate;
};

struct ZMemoryPressureMetrics {
  const double _unscaled_gc_pressure;
  const size_t _system_used_memory;
  const size_t _system_compressed_memory;
  const size_t _system_max_memory;
};

class ZAdaptiveHeap : public AllStatic {
private:
  static bool _explicit_max_capacity;
  static TruncatedSeq _gc_pressures;

  struct ZGenerationOverhead {
    double       _last_system_time;
    double       _last_process_time;
    double       _last_time;
    TruncatedSeq _process_times;
    TruncatedSeq _system_times;
    TruncatedSeq _gc_times;
    TruncatedSeq _gc_times_since_last;

    ZGenerationOverhead()
      : _last_system_time(),
        _last_process_time(),
        _process_times(),
        _system_times(),
        _gc_times(),
        _gc_times_since_last() {}
  };

  static volatile double _young_to_old_gc_time;
  static double _accumulated_young_gc_time;
  static ZGenerationOverhead _young_data;
  static ZGenerationOverhead _old_data;

  static double gc_pressure(double unscaled_pressure, double process_cpu_usage, double system_cpu_usage, double& mem_pressure);
  static double memory_pressure(const ZMemoryPressureMetrics& metrics);

public:
  static void initialize(bool explicit_max_heap_size);

  static size_t compute_heap_size(ZHeapResizeMetrics* metrics, ZGenerationId generation);
  static double young_to_old_gc_time();

  static double uncommit_urgency(size_t used_memory, size_t total_memory);
  static uint64_t soft_ref_delay();
  static ZMemoryPressureMetrics memory_pressure_metrics();
  static bool is_memory_pressure_concerning(const ZMemoryPressureMetrics& metrics);
  static bool is_memory_pressure_high(const ZMemoryPressureMetrics& metrics);
  static bool is_memory_pressure_critical(const ZMemoryPressureMetrics& metrics);

  static bool explicit_max_capacity() { return _explicit_max_capacity; }
  static bool can_adapt();
  static size_t current_max_capacity(size_t capacity, size_t dynamic_max_capacity);

  static void print();
};

#endif // SHARE_GC_Z_ZADAPTIVEHEAP_HPP
