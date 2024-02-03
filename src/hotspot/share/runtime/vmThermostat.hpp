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
 *
 */

#ifndef SHARE_RUNTIME_VMTHERMOSTAT_HPP
#define SHARE_RUNTIME_VMTHERMOSTAT_HPP

#include "runtime/javaThread.hpp"

class VMThermostat : public JavaThread {
private:
  static VMThermostat* _thermostat;

  uint64_t _start_nanos;
  uint64_t _nticks;

  int sample(int overslept, float& the_sample);
  void sample_window(int& overflowed_idle_samples, float& overflow_sample, float* samples);
  void report_window(float* samples, int sample_number);
  int wait_for_tick();
  VMThermostat();

public:
  static void initialize();
  static void thread_entry(JavaThread* thread, TRAPS);
  bool is_hidden_from_external_view() const override { return true; }
  void run_loop();
};

#endif // SHARE_RUNTIME_VMTHERMOSTAT_HPP
