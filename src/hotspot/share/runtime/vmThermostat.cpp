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

#include "logging/log.hpp"
#include "runtime/handshake.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/os.hpp"
#include "runtime/threadSMR.inline.hpp"
#include "runtime/vmThermostat.hpp"
#include "utilities/ticks.hpp"

static constexpr uint64_t sampling_interval_nanos = 100000; // 1000 us
static constexpr int samples_per_window = 1000;             // 100  ms

// Sampled relative performance difference between tiers in a sample program
static constexpr float tier0_relative_performance = 0.0318f;
static constexpr float tier1_relative_performance = 0.5f;
static constexpr float tier2_relative_performance = 0.4209f;
static constexpr float tier3_relative_performance = 0.2364f;
static constexpr float tier4_relative_performance = 1.0f;

VMThermostat* VMThermostat::_thermostat;

void VMThermostat::initialize() {
  LogTarget(Info, thermostat) lt;
  if (!lt.is_enabled()) {
    return;
  }

  HandleMark hm(JavaThread::current());
  EXCEPTION_MARK;

  const char* name = "VM Thermostat Thread";
  Handle thread_oop = JavaThread::create_system_thread_object(name, CHECK);

  _thermostat = new VMThermostat();
  JavaThread::vm_exit_on_osthread_failure(_thermostat);
  JavaThread::start_internal_daemon(THREAD, _thermostat, thread_oop, NearMaxPriority);
}

VMThermostat::VMThermostat()
  : JavaThread(thread_entry),
    _start_nanos(0),
    _nticks(0) {
}

void VMThermostat::thread_entry(JavaThread* thread, TRAPS) {
  static_cast<VMThermostat*>(thread)->run_loop();
}

// Returns how many sampling windows were missed due to latency problems
int VMThermostat::wait_for_tick() {
  if (_nticks++ == 0) {
    // First tick, set start time and record startup pause for premain
    const Ticks now = Ticks::now();
    _start_nanos = now.nanoseconds();
    return _start_nanos / sampling_interval_nanos;
  }

  for (;;) {
    // We might wake up spuriously from wait, so always recalculate
    // the timeout after a wakeup to see if we need to wait again.
    const Ticks now = Ticks::now();
    const uint64_t now_nanos = now.nanoseconds();
    const uint64_t next_nanos = _start_nanos + (sampling_interval_nanos * _nticks);
    const int64_t timeout_nanos = next_nanos - now_nanos;

    if (timeout_nanos > 0) {
      // Wait
      ThreadBlockInVM tbivm(_thermostat);
      if (timeout_nanos > NANOUNITS_PER_MILLIUNIT) {
        // More than a millisecond to sleep
        os::naked_short_sleep(timeout_nanos / NANOUNITS_PER_MILLIUNIT);
      } else {
        // Less than a millisecond to sleep
        os::naked_short_nanosleep(timeout_nanos);
      }
    } else {
      // Tick
      int overslept_ticks = 0;
      if (timeout_nanos < 0) {
        const uint64_t overslept = -timeout_nanos;
        if (overslept > sampling_interval_nanos) {
          // Missed one or more ticks. Bump _nticks accordingly to
          // avoid firing a string of immediate ticks to make up
          // for the ones we missed.
          overslept_ticks = overslept / sampling_interval_nanos;
          _nticks += overslept_ticks;
        }
      }

      return overslept_ticks;
    }
  }
}

class ThermostatHandshake : public HandshakeClosure {
private:
  float _sampled_relative_performance;

public:
  ThermostatHandshake()
    : HandshakeClosure("ThermostatHandshake"),
      _sampled_relative_performance(1.0f) {}
  virtual void do_thread(Thread* thread) {
    JavaThread* jt = JavaThread::cast(thread);
    if (!jt->has_last_Java_frame()) {
      // No java frame, no action
      _sampled_relative_performance = 0.0f;
      return;
    }

    // Sample top frame to see if the program is doing something
    // we know is slower than it could be
    frame f = jt->last_frame();

    // Skip any stub frames etc
    RegisterMap map(jt,
                    RegisterMap::UpdateMap::skip,
                    RegisterMap::ProcessFrames::skip,
                    RegisterMap::WalkContinuation::skip);
    if (f.is_safepoint_blob_frame() || f.is_runtime_frame()) {
      f = f.sender(&map);
    }

    if (f.is_interpreted_frame()) {
      _sampled_relative_performance = tier0_relative_performance;
      return;
    }

    if (f.is_compiled_frame()) {
      nmethod* nm = f.cb()->as_nmethod();

      switch (nm->comp_level()) {
      case CompLevel_simple:
        _sampled_relative_performance = tier1_relative_performance;
        break;
      case CompLevel_limited_profile:
        _sampled_relative_performance = tier2_relative_performance;
        break;
      case CompLevel_full_profile:
        _sampled_relative_performance = tier3_relative_performance;
        break;
      case CompLevel_full_optimization:
        _sampled_relative_performance = tier4_relative_performance;
        break;
      default:
        // Assume we are not slowed down by default
        break;
      }
    }
  }

  float sampled_relative_performance() const {
    return _sampled_relative_performance;
  }
};

static JavaThread* select_target(ThreadsListHandle& tlh, JavaThread* current) {
  int length = tlh.length();
  GrowableArray<JavaThread*> candidates(length);

  for (int i = 0; i < length; i++) {
    JavaThread* target = tlh.thread_at(i);
    // Hidden threads are not so interesting
    if (target->is_hidden_from_external_view()) {
      continue;
    }

    // Threads not calling Java or not so interesting
    if (!target->can_call_java()) {
      continue;
    }

    // Exiting threads are not so interesting
    oop thread_oop = target->threadObj();
    if (thread_oop == nullptr) {
      continue;
    }

    // Daemon threads are not so interesting
    if (java_lang_Thread::is_daemon(thread_oop)) {
      continue;
    }

    // Threads potentially blocking are not so interesting
    JavaThreadState state = target->thread_state();
    if (state == _thread_in_native) {
      continue;
    }

    if (state == _thread_blocked) {
      continue;
    }

    candidates.append(target);
  }

  if (candidates.length() == 0) {
    return nullptr;
  }

  // Pick a random candidate
  return candidates.at(os::random() % candidates.length());
}

int VMThermostat::sample(int overslept, float& the_sample) {
  int idle_ticks = overslept;

  ThermostatHandshake op;
  ThreadsListHandle tlh;
  JavaThread* target = select_target(tlh, _thermostat);

  if (target == nullptr) {
    // No interesting threads running? Well there is no known slowdown
    // on any threads at the moment then
    the_sample = 1.0f;
    return idle_ticks;
  }

  const Ticks before = Ticks::now();
  Handshake::execute(&op, &tlh, target);
  const Ticks after = Ticks::now();

  Tickspan duration = after - before;
  idle_ticks += duration.nanoseconds() / sampling_interval_nanos;

  the_sample = op.sampled_relative_performance();

  return idle_ticks;
}

// Sample a window
void VMThermostat::sample_window(int& overflowed_idle_samples, float& overflow_sample, float* samples) {
  ResourceMark rm(JavaThread::current());

  int current = 0;

  // Idle samples "stand still"; relative performance is 0
  while (current < MIN2(overflowed_idle_samples, samples_per_window)) {
    samples[current++] = 0.0f;
  }

  if (current == samples_per_window) {
    overflowed_idle_samples -= samples_per_window;
    return;
  }

  if (overflow_sample > -0.5f) {
    // Fill in the overflow sample from last time
    samples[current++] = overflow_sample;
    if (current == samples_per_window) {
      overflow_sample = -1.0f;
      overflowed_idle_samples = 0;
    }
  }

  while (current < samples_per_window) {
    int result = wait_for_tick();

    float the_sample;
    int idle_ticks = sample(result, the_sample);
    int remaining_ticks = samples_per_window - current;

    int consumed_idle_ticks = MIN2(idle_ticks, remaining_ticks);

    // Count idle samples as standing still; relative performance is 0
    for (int i = 0; i < consumed_idle_ticks; ++i) {
      samples[current++] = 0.0f;
    }

    if (current == samples_per_window) {
      overflow_sample = the_sample;
      overflowed_idle_samples = idle_ticks - consumed_idle_ticks;
      return;
    }

    samples[current++] = the_sample;
  }

  // No overflow to the next window
  overflowed_idle_samples = 0;
  overflow_sample = -1.0f;
}

int order_floats(const float* v1, const float* v2) {
  if (*v1 > *v2) {
    return -1;
  }

  if (*v1 < *v2) {
    return 1;
  }

  return 0;
}

float calculate_average(float* samples, int from) {
  float sum = 0.0f;
  for (int i = from; i < samples_per_window; ++i) {
    sum += samples[i];
  }
  float avg = sum / (samples_per_window - from);
  return avg;
}

float calculate_percentile(float* samples, int percentile) {
  int ignore = percentile * samples_per_window / 1000;
  return int((1.0f - calculate_average(samples, ignore)) * 100.0f);
}

void VMThermostat::report_window(float* samples, int window_number) {
  qsort(samples, samples_per_window, sizeof(float), (_sort_Fn)order_floats);
  int p0 = calculate_percentile(samples, 0);
  int p50 = calculate_percentile(samples, 500);
  int p90 = calculate_percentile(samples, 900);
  log_info(thermostat)(SIZE_FORMAT "\t%d\t%d\t%d", size_t(window_number) * samples_per_window * sampling_interval_nanos / NANOUNITS_PER_MILLIUNIT, p0, p50, p90);
}

void VMThermostat::run_loop() {
  log_info(thermostat)("Time ms\tP0\tP50\tP90");
  int overflowed_idle_samples = 0;
  float overflow_sample = -1.0f;
  float samples[samples_per_window];
  int window_number = 0;
  for (;;) {
    sample_window(overflowed_idle_samples, overflow_sample, samples);
    report_window(samples, window_number++);
  }
}
