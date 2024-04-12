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


class ZAdaptiveHeap : public AllStatic {
private:
  static bool _enabled;

  struct ZGenerationOverhead {
    double       _last_process_time;
    TruncatedSeq _process_time;
    TruncatedSeq _gc_time;

    ZGenerationOverhead() :
        _last_process_time(),
        _process_time(),
        _gc_time() {}
  };

  static volatile double _young_to_old_gc_time;
  static double _accumulated_young_gc_time;
  static ZGenerationOverhead _young_data;
  static ZGenerationOverhead _old_data;

public:
  static bool is_enabled();
  static void enable();

  static void adapt(ZGenerationId generation);
  static double young_to_old_gc_time();
};

#endif // SHARE_GC_Z_ZADAPTIVEHEAP_HPP
