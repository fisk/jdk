/*
 * Copyright (c) 2012, 2023, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "classfile/javaThreadStatus.hpp"
#include "code/debugInfoRec.hpp"
#include "jfr/jfrEvents.hpp"
#include "jfr/recorder/jfrRecorder.hpp"
#include "jfr/periodic/sampling/jfrThreadSampler.hpp"
#include "jfr/recorder/checkpoint/types/traceid/jfrTraceIdLoadBarrier.inline.hpp"
#include "jfr/recorder/service/jfrOptionSet.hpp"
#include "jfr/recorder/stacktrace/jfrStackTraceRepository.hpp"
#include "jfr/recorder/storage/jfrBuffer.hpp"
#include "jfr/support/jfrThreadLocal.hpp"
#include "jfr/utilities/jfrTime.hpp"
#include "jfrfiles/jfrEventClasses.hpp"
#include "logging/log.hpp"
#include "runtime/atomic.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/globals.hpp"
#include "runtime/javaThread.inline.hpp"
#include "runtime/os.hpp"
#include "runtime/safepointMechanism.inline.hpp"
#include "runtime/semaphore.hpp"
#include "runtime/stackFrameStream.inline.hpp"
#include "runtime/stackWatermark.hpp"
#include "runtime/suspendedThreadTask.hpp"
#include "runtime/threadSMR.hpp"
#include "utilities/systemMemoryBarrier.hpp"

enum JfrSampleType {
  NO_SAMPLE = 0,
  JAVA_SAMPLE = 1,
  NATIVE_SAMPLE = 2
};

class OSThreadSampler : public SuspendedThreadTask {
 public:
  OSThreadSampler(JavaThread* thread, JfrSampleType type)
    : SuspendedThreadTask(thread),
      _type(type),
      _thread_state() {}

  void request_sample();
  void do_task(const SuspendedThreadTaskContext& context);

  JavaThreadState thread_state() const { return _thread_state; }

 private:
  JfrSampleType _type;
  JavaThreadState _thread_state;
};

void OSThreadSampler::do_task(const SuspendedThreadTaskContext& context) {
  JavaThread* const jt = JavaThread::cast(context.thread());
  JavaThreadState state = jt->thread_state();

  _thread_state = state;

  if (_type == JAVA_SAMPLE && state != _thread_in_Java) {
    // Sample miss
    jt->set_jfr_sample_state(0);
    return;
  }

  if (_type == NATIVE_SAMPLE && state != _thread_in_native) {
    // Sample miss
    jt->set_jfr_sample_state(0);
    return;
  }

  JfrSampleRequest request;

  request._sample_ticks = JfrTicks::now();
  intptr_t* last_sp = jt->last_Java_sp();

  if (last_sp != nullptr) {
    // Last Java frame is available, but might not be walkable; fix
    address last_pc = jt->last_Java_pc();
    if (last_pc == nullptr) {
      last_pc = address(last_sp[-1]);
    }
    request._sample_sp = last_sp;
    request._sample_pc = last_pc;
  } else {
    // Find top managed frame
    frame last_frame = os::fetch_frame_from_context(context.ucontext());
    request._sample_sp = last_frame.sp();
    request._sample_pc = last_frame.pc();
  }

  jt->set_jfr_sample_request(request);
  jt->set_jfr_sample_state(_type); // Request a sample of type _type

  SafepointMechanism::arm_local_poll_release(jt);
}

void OSThreadSampler::request_sample() {
  run();
}

class JfrThreadSampler : public NonJavaThread {
  friend class JfrThreadSampling;
 private:
  Semaphore _sample;
  Thread* _sampler_thread;
  JfrStackFrame* const _frames;
  JavaThread* _last_thread_java;
  JavaThread* _last_thread_native;
  int64_t _java_period_millis;
  int64_t _native_period_millis;
  const size_t _min_size; // for enqueue buffer monitoring
  int _cur_index;
  const u4 _max_frames;
  volatile bool _disenrolled;

  const JfrBuffer* get_enqueue_buffer();
  const JfrBuffer* renew_if_full(const JfrBuffer* enqueue_buffer);

  JavaThread* next_thread(ThreadsList* t_list, JavaThread* first_sampled, JavaThread* current);
  void task_stacktrace(JfrSampleType type, JavaThread** last_thread);
  JfrThreadSampler(int64_t java_period_millis, int64_t native_period_millis, u4 max_frames);
  ~JfrThreadSampler();

  void start_thread();

  void enroll();
  void disenroll();
  void set_java_period(int64_t period_millis);
  void set_native_period(int64_t period_millis);

  bool record(JavaThread* thread, JfrStackTrace& stacktrace, frame top_frame);

  bool request_sample_thread_in_java(JavaThread* thread);
  bool sample_thread_in_java(JavaThread* thread, JfrSampleRequest request);
  bool sample_thread_in_native(JavaThread* thread);

  void handle_requested_sampling(JavaThread* thread);
 protected:
  virtual void post_run();
 public:
  virtual const char* name() const { return "JFR Thread Sampler"; }
  virtual const char* type_name() const { return "JfrThreadSampler"; }
  bool is_JfrSampler_thread() const { return true; }
  void run();
  static Monitor* transition_block() { return JfrThreadSampler_lock; }
  int64_t get_java_period() const { return Atomic::load(&_java_period_millis); };
  int64_t get_native_period() const { return Atomic::load(&_native_period_millis); };
};

static bool is_excluded(JavaThread* thread) {
  assert(thread != nullptr, "invariant");
  return thread->is_Compiler_thread() || thread->is_hidden_from_external_view() || thread->jfr_thread_local()->is_excluded();
}

bool JfrThreadSampler::record(JavaThread* thread, JfrStackTrace& stacktrace, frame top_frame) {
  // If we sample from the JFR sampler thread; use the record_inner function to avoid
  // setting up handle marks and what not, which isn't necessary there.
  if (Thread::current() != thread) {
    return stacktrace.record_inner(thread, top_frame, 0, -1);
  } else {
    return stacktrace.record(thread, top_frame, 0, -1);
  }
}

static volatile int _g_sample_accurate = 0;
static volatile int _g_sample_safepoint = 0;
static volatile int _g_sample_cold_interpreter = 0;
static volatile int _g_sample_cold_stub = 0;
static volatile int _g_sample_cold_native = 0;
static volatile int _g_sample_cold_other = 0;
static volatile int _g_sample_normal = 0;

static bool compute_top_java_frame(JavaThread* thread, JfrSampleRequest request, frame* top_frame) {
  if (!thread->has_last_Java_frame()) {
    Atomic::inc(&_g_sample_accurate);
    return false;
  }

  void* sampled_sp = request._sample_sp;
  void* sampled_pc = request._sample_pc;
  const char* sampler = (thread == Thread::current()) ? "self" : "remote";

  CodeBlob* sampled_cb = CodeCache::find_blob(sampled_pc);

  if (sampled_cb == nullptr) {
    // No code blob... probably native code. Perform a biased sample
    *top_frame = thread->last_frame();
    Atomic::inc(&_g_sample_cold_native);
    return true;
  }

  if (!sampled_cb->is_nmethod() &&
      !sampled_cb->is_vtable_blob() &&
      !sampled_cb->is_adapter_blob() &&
      !sampled_cb->is_method_handles_adapter_blob()) {
    // Cold code blob... perform a biased sample
    *top_frame = thread->last_frame();

    address interpreter_start = Interpreter::code()->code_start();
    address interpreter_end = Interpreter::code()->code_end();

    if (sampled_pc >= interpreter_start && sampled_pc < interpreter_end) {
      Atomic::inc(&_g_sample_cold_interpreter);
    } else if (sampled_cb->is_runtime_stub()) {
      Atomic::inc(&_g_sample_cold_stub);
    } else {
      Atomic::inc(&_g_sample_cold_other);
    }

    return true;
  }

  // For nmethods, vtable stubs, itable stubs, adapter blobs and method handle intrinsic blobs,
  // want to perform an accurate unbiased sample
  nmethod* sampled_nm = sampled_cb->as_nmethod_or_null();

  // We sampled an nmethod. Let's find the frame it came from.
  RegisterMap map(thread,
                  RegisterMap::UpdateMap::skip,
                  RegisterMap::ProcessFrames::skip,
                  RegisterMap::WalkContinuation::skip);

  // Search the first frame that is above the sampled sp
  for (StackFrameStream frame_stream(thread, false /* update_registers */, false /* process_frames */);
       !frame_stream.is_done();
       frame_stream.next()) {
    frame* f = frame_stream.current();

    if (f->is_safepoint_blob_frame() || f->is_runtime_frame()) {
      // Skip runtime stubs
      continue;
    }

    // Seek the first matching frame
    if (f->real_fp() <= sampled_sp) {
      // Continue searching the matching frame or its caller
      continue;
    }

    if (sampled_nm == nullptr) {
      // The sample didn't have an nmethod; we decided to trace from its caller
      Atomic::inc(&_g_sample_accurate);
      *top_frame = *f;
      return true;
    }

    // We might have a matching frame; check it
    if (f->cb()->as_nmethod_or_null() == sampled_nm) {
      // We found the sampled nmethod! Let's correct the safepoint bias
      PcDesc* pc_desc = sampled_nm->pc_desc_near(address(sampled_pc) + 1);
      if (pc_desc == nullptr || pc_desc->scope_decode_offset() == DebugInformationRecorder::serialized_null) {
        // Bogus PC at frame boundary; we are close enough to the caller; trace from there
        continue;
      }
      f->set_pc(pc_desc->real_pc(sampled_nm));
      assert(sampled_nm->pc_desc_at(f->pc()) != nullptr, "invalid pc");

      Atomic::inc(&_g_sample_accurate);
      *top_frame = *f;
      return true;
    } else {
      // Frame not matching... possibly due to polling after unwinding.
      address saved_exception_pc = thread->saved_exception_pc();
      nmethod* exception_nm = saved_exception_pc == nullptr ? nullptr : CodeCache::find_blob(saved_exception_pc)->as_nmethod_or_null();

      if (exception_nm == sampled_nm && sampled_nm->is_at_poll_return(saved_exception_pc)) {
        // We have polled at an unwind site in the compiled method. Let's reconstruct what the frame
        // would have looked like before unwinding. This will point into garbage stack memory, but
        // is safe, as the stack sampling only cares about PCs, and not the content of the stack.
        intptr_t* previous_sp = f->sp() - sampled_nm->frame_size();

        // We found the sampled nmethod! Let's correct the safepoint bias
        PcDesc* pc_desc = sampled_nm->pc_desc_near(address(sampled_pc) + 1);
        if (pc_desc == nullptr || pc_desc->scope_decode_offset() == DebugInformationRecorder::serialized_null) {
          // Bogus PC at frame boundary; we are close enough to the caller; trace from there
          *top_frame = *f;
        } else {
          *top_frame = frame(previous_sp, previous_sp, (intptr_t*)f->sp(), (address)pc_desc->real_pc(sampled_nm), sampled_nm);
        }
        Atomic::inc(&_g_sample_accurate);
      } else {
        // Mismatched sample; trace from caller
        *top_frame = *f;
        if (f->is_safepoint_blob_frame()) {
          Atomic::inc(&_g_sample_safepoint);
        } else {
          Atomic::inc(&_g_sample_normal);
        }
      }

      return true;
    }
  }

  Atomic::inc(&_g_sample_normal);

  // No frame found
  return false;
}

static bool compute_top_native_frame(JavaThread* thread, JfrSampleRequest request, frame* top_frame) {
  if (!thread->has_last_Java_frame()) {
    return false;
  }

  *top_frame = thread->last_frame();

  // Frame found
  return true;
}

bool JfrThreadSampler::request_sample_thread_in_java(JavaThread* thread) {
  OSThreadSampler sampler(thread, JAVA_SAMPLE);
  sampler.request_sample();

  if (thread->jfr_sample_state() == 0) {
    // Not in java or mutator already took care of it
    return false;
  }

  if (!thread->jfr_sample_monitor()->try_lock()) {
    // If the mutator holds the lock, it will handle the sampling itself
    return false;
  }

  if (thread->jfr_sample_state() == 0) {
    // Double checked locking
    thread->jfr_sample_monitor()->unlock();
    return false;
  }

  // Move request from signal handler to request queue
  JfrSampleRequest request = thread->jfr_sample_request();
  thread->jfr_sample_requests()->append(request);
  thread->set_jfr_sample_state(0);

  if (sampler.thread_state() == _thread_in_native) {
    // If the thread was in native, it was in a walkable state, and will
    // hit a safepoint poll on the way back from native. Therefore, any
    // requests in the queue can be safely processed now. Just process
    // them, to ensure timely progress.
    for (JfrSampleRequest request: *thread->jfr_sample_requests()) {
      sample_thread_in_java(thread, request);
    }
    thread->jfr_sample_requests()->clear();
  }

  thread->jfr_sample_monitor()->unlock();

  return true;
}

bool JfrThreadSampler::sample_thread_in_java(JavaThread* thread, JfrSampleRequest request) {
  JfrStackFrame* frames =  JfrCHeapObj::new_array<JfrStackFrame>(_max_frames);
  JfrStackTrace stacktrace(frames, _max_frames);

  frame top_frame;
  if (!compute_top_java_frame(thread, request, &top_frame)) {
    JfrCHeapObj::free(frames, sizeof(JfrStackFrame) * _max_frames);
    return false;
  }

  int normal = Atomic::load(&_g_sample_normal);
  int accurate = Atomic::load(&_g_sample_accurate);
  int safepoint = Atomic::load(&_g_sample_safepoint);
  int cold_interpreter = Atomic::load(&_g_sample_cold_interpreter);
  int cold_native = Atomic::load(&_g_sample_cold_native);
  int cold_stub = Atomic::load(&_g_sample_cold_stub);
  int cold_other = Atomic::load(&_g_sample_cold_other);
  int total = accurate + normal + safepoint + cold_interpreter + cold_native + cold_stub + cold_other;
  log_info(jfr)("accurate: %d (%f), normal: %d (%f), cold interpreter: %d (%f), cold native: %d (%f), cold stub: %d (%f), cold other: %d (%f), safepoint: %d (%f)",
                accurate, double(accurate) / double(total) * 100.0,
                normal, double(normal) / double(total) * 100.0,
                cold_interpreter, double(cold_interpreter) / double(total) * 100.0,
                cold_native, double(cold_native) / double(total) * 100.0,
                cold_stub, double(cold_stub) / double(total) * 100.0,
                cold_other, double(cold_other) / double(total) * 100.0,
                safepoint, double(safepoint) / double(total) * 100.0);

  if (!record(thread, stacktrace, top_frame)) {
    JfrCHeapObj::free(frames, sizeof(JfrStackFrame) * _max_frames);
    // Empty stack trace; fail
    return false;
  }

  traceid id = JfrStackTraceRepository::add(stacktrace);
  assert(id != 0, "Stacktrace id should not be 0");

  JfrCHeapObj::free(frames, sizeof(JfrStackFrame) * _max_frames);

  EventExecutionSample event;
  event.set_starttime(request._sample_ticks);
  event.set_endtime(JfrTicks::now());
  event.set_sampledThread(JfrThreadLocal::thread_id(thread));
  event.set_state((u8)JavaThreadStatus::RUNNABLE); // TODO: Weird; it's seemingly RUNNABLE by definition when in java
  event.set_stackTrace(id);
  event.commit();

  return true;
}

bool JfrThreadSampler::sample_thread_in_native(JavaThread* thread) {
  OSThreadSampler sampler(thread, NATIVE_SAMPLE);
  sampler.request_sample();

  if (thread->jfr_sample_state() == 0) {
    // Not in native
    return false;
  }

  JfrSampleRequest request = thread->jfr_sample_request();

  JfrStackTrace stacktrace(_frames, _max_frames);

  {
    MonitorLocker ml(thread->jfr_sample_monitor(), Monitor::_no_safepoint_check_flag);

    frame top_frame;
    if (!compute_top_native_frame(thread, request, &top_frame)) {
      // Notify that we are done with the sampling
      ml.notify_all();
      thread->set_jfr_sample_state(0);
      return false;
    }

    if (!record(thread, stacktrace, top_frame)) {
      // Empty stack trace; fail
      // Notify that we are done with the sampling
      ml.notify_all();
      thread->set_jfr_sample_state(0);
      return false;
    }

    // Notify that we are done with the sampling
    ml.notify_all();
    thread->set_jfr_sample_state(0);
  }

  traceid id = JfrStackTraceRepository::add(stacktrace);
  assert(id != 0, "Stacktrace id should not be 0");

  EventNativeMethodSample event;
  event.set_starttime(request._sample_ticks);
  event.set_endtime(JfrTicks::now());
  event.set_sampledThread(JfrThreadLocal::thread_id(thread));
  event.set_state((u8)JavaThreadStatus::RUNNABLE); // TODO: Weird to pass in... it's seemingly RUNNABLE by definition if in native
  event.set_stackTrace(id);
  event.commit();

  return true;
}

void JfrThreadSampler::handle_requested_sampling(JavaThread* thread) {
  assert(JavaThread::current() == thread, "should be current thread");
  assert(thread->thread_state() == _thread_in_vm, "should be in VM, so we don't enqueue more work racingly");

  MonitorLocker ml(thread->jfr_sample_monitor(), Monitor::_no_safepoint_check_flag);

  for (;;) {
    int sample_state = thread->jfr_sample_state();
    if (sample_state == NATIVE_SAMPLE) {
      // Wait until stack trace is processed
      ml.wait();
    } else if (sample_state == JAVA_SAMPLE) {
      // Enqueue pending request from signal handler
      thread->jfr_sample_requests()->append(thread->jfr_sample_request());
      thread->set_jfr_sample_state(0);
      break;
    } else {
      // State has been processed
      break;
    }
  }

  assert(thread->jfr_sample_state() == 0, "invariant");

  ResourceMark rm;
  // Drain request queue for java samples
  for (JfrSampleRequest request: *thread->jfr_sample_requests()) {
    // Enqueued requests are Java sample requests
    sample_thread_in_java(thread, request);
  }
  thread->jfr_sample_requests()->clear();
}

JfrThreadSampler::JfrThreadSampler(int64_t java_period_millis, int64_t native_period_millis, u4 max_frames) :
  _sample(),
  _sampler_thread(nullptr),
  _frames(JfrCHeapObj::new_array<JfrStackFrame>(max_frames)),
  _last_thread_java(nullptr),
  _last_thread_native(nullptr),
  _java_period_millis(java_period_millis),
  _native_period_millis(native_period_millis),
  _min_size(max_frames * 2 * wordSize), // each frame tags at most 2 words, min size is a full stacktrace
  _cur_index(-1),
  _max_frames(max_frames),
  _disenrolled(true) {
  assert(_java_period_millis >= 0, "invariant");
  assert(_native_period_millis >= 0, "invariant");
}

JfrThreadSampler::~JfrThreadSampler() {
  JfrCHeapObj::free(_frames, sizeof(JfrStackFrame) * _max_frames);
}

void JfrThreadSampler::set_java_period(int64_t period_millis) {
  assert(period_millis >= 0, "invariant");
  Atomic::store(&_java_period_millis, period_millis);
}

void JfrThreadSampler::set_native_period(int64_t period_millis) {
  assert(period_millis >= 0, "invariant");
  Atomic::store(&_native_period_millis, period_millis);
}

static inline bool is_released(JavaThread* jt) {
  return !jt->is_trace_suspend();
}

JavaThread* JfrThreadSampler::next_thread(ThreadsList* t_list, JavaThread* first_sampled, JavaThread* current) {
  assert(t_list != nullptr, "invariant");
  assert(_cur_index >= -1 && (uint)_cur_index + 1 <= t_list->length(), "invariant");
  assert((current == nullptr && -1 == _cur_index) || (t_list->find_index_of_JavaThread(current) == _cur_index), "invariant");
  if ((uint)_cur_index + 1 == t_list->length()) {
    // wrap
    _cur_index = 0;
  } else {
    _cur_index++;
  }
  assert(_cur_index >= 0 && (uint)_cur_index < t_list->length(), "invariant");
  JavaThread* const next = t_list->thread_at(_cur_index);
  return next != first_sampled ? next : nullptr;
}

void JfrThreadSampler::start_thread() {
  if (os::create_thread(this, os::os_thread)) {
    os::start_thread(this);
  } else {
    log_error(jfr)("Failed to create thread for thread sampling");
  }
}

void JfrThreadSampler::enroll() {
  if (_disenrolled) {
    log_trace(jfr)("Enrolling thread sampler");
    _sample.signal();
    _disenrolled = false;
  }
}

void JfrThreadSampler::disenroll() {
  if (!_disenrolled) {
    _sample.wait();
    _disenrolled = true;
    log_trace(jfr)("Disenrolling thread sampler");
  }
}

static int64_t get_monotonic_ms() {
  return os::javaTimeNanos() / 1000000;
}

void JfrThreadSampler::run() {
  assert(_sampler_thread == nullptr, "invariant");

  _sampler_thread = this;

  int64_t last_java_ms = get_monotonic_ms();
  int64_t last_native_ms = last_java_ms;
  while (true) {
    if (!_sample.trywait()) {
      // disenrolled
      _sample.wait();
      last_java_ms = get_monotonic_ms();
      last_native_ms = last_java_ms;
    }
    _sample.signal();

    int64_t java_period_millis = get_java_period();
    java_period_millis = java_period_millis == 0 ? max_jlong : MAX2<int64_t>(java_period_millis, 1);
    int64_t native_period_millis = get_native_period();
    native_period_millis = native_period_millis == 0 ? max_jlong : MAX2<int64_t>(native_period_millis, 1);

    // If both periods are max_jlong, it implies the sampler is in the process of
    // disenrolling. Loop back for graceful disenroll by means of the semaphore.
    if (java_period_millis == max_jlong && native_period_millis == max_jlong) {
      continue;
    }

    const int64_t now_ms = get_monotonic_ms();

    /*
     * Let I be java_period or native_period.
     * Let L be last_java_ms or last_native_ms.
     * Let N be now_ms.
     *
     * Interval, I, might be max_jlong so the addition
     * could potentially overflow without parenthesis (UB). Also note that
     * L - N < 0. Avoid UB, by adding parenthesis.
     */
    const int64_t next_j = java_period_millis + (last_java_ms - now_ms);
    const int64_t next_n = native_period_millis + (last_native_ms - now_ms);

    const int64_t sleep_to_next = MIN2<int64_t>(next_j, next_n);

    if (sleep_to_next > 0) {
      os::naked_sleep(sleep_to_next);
    }

    // Note, this code used to check (next_j - sleep_to_next) <= 0,
    // but that can overflow (UB) and cause a spurious sample.
    if (next_j <= sleep_to_next) {
      task_stacktrace(JAVA_SAMPLE, &_last_thread_java);
      last_java_ms = get_monotonic_ms();
    }
    if (next_n <= sleep_to_next) {
      task_stacktrace(NATIVE_SAMPLE, &_last_thread_native);
      last_native_ms = get_monotonic_ms();
    }
  }
}

void JfrThreadSampler::post_run() {
  this->NonJavaThread::post_run();
  delete this;
}

const JfrBuffer* JfrThreadSampler::get_enqueue_buffer() {
  const JfrBuffer* buffer = JfrTraceIdLoadBarrier::get_sampler_enqueue_buffer(this);
  return buffer != nullptr ? renew_if_full(buffer) : JfrTraceIdLoadBarrier::renew_sampler_enqueue_buffer(this);
}

const JfrBuffer* JfrThreadSampler::renew_if_full(const JfrBuffer* enqueue_buffer) {
  assert(enqueue_buffer != nullptr, "invariant");
  return enqueue_buffer->free_size() < _min_size ? JfrTraceIdLoadBarrier::renew_sampler_enqueue_buffer(this) : enqueue_buffer;
}

void JfrThreadSampler::task_stacktrace(JfrSampleType type, JavaThread** last_thread) {
  ResourceMark rm;

  // TODO: Figure out what this sample limit is all about
  const uint sample_limit = JAVA_SAMPLE == type ? 5 : 1;
  uint num_samples = 0;
  JavaThread* start = nullptr;
  {
    elapsedTimer sample_time;
    sample_time.start();
    {
      ThreadsListHandle tlh;
      // Resolve a sample session relative start position index into the thread list array.
      // In cases where the last sampled thread is null or not-null but stale, find_index() returns -1.
      _cur_index = tlh.list()->find_index_of_JavaThread(*last_thread);
      JavaThread* current = _cur_index != -1 ? *last_thread : nullptr;

      // Explicitly monitor the available space of the thread-local buffer used by the load barrier
      // for enqueuing klasses as part of tagging methods. We do this because if space becomes sparse,
      // we cannot rely on the implicit allocation of a new buffer as part of the regular tag mechanism.
      // If the free list is empty, a malloc could result, and the problem with that is that the thread
      // we have suspended could be the holder of the malloc lock. Instead, the buffer is pre-emptively
      // renewed before thread suspension.
      const JfrBuffer* enqueue_buffer = get_enqueue_buffer();
      assert(enqueue_buffer != nullptr, "invariant");

      while (num_samples < sample_limit) {
        current = next_thread(tlh.list(), start, current);
        if (current == nullptr) {
          break;
        }
        if (start == nullptr) {
          start = current;  // remember the thread where we started to attempt sampling
        }
        if (is_excluded(current)) {
          continue;
        }
        assert(enqueue_buffer->free_size() >= _min_size, "invariant");
        bool success;
        if (JAVA_SAMPLE == type) {
          success = request_sample_thread_in_java(current);
        } else {
          assert(NATIVE_SAMPLE == type, "invariant");
          success = sample_thread_in_native(current);
        }
        if (success) {
          num_samples++;
        }
        enqueue_buffer = renew_if_full(enqueue_buffer);
      }
      *last_thread = current;  // remember the thread we last attempted to sample
    }
    sample_time.stop();
    log_trace(jfr)("JFR thread sampling done in %3.7f secs", sample_time.seconds());
  }
}

static JfrThreadSampling* _instance = nullptr;

JfrThreadSampling& JfrThreadSampling::instance() {
  return *_instance;
}

JfrThreadSampling* JfrThreadSampling::create() {
  assert(_instance == nullptr, "invariant");
  _instance = new JfrThreadSampling();
  return _instance;
}

void JfrThreadSampling::destroy() {
  if (_instance != nullptr) {
    delete _instance;
    _instance = nullptr;
  }
}

JfrThreadSampling::JfrThreadSampling() : _sampler(nullptr) {}

JfrThreadSampling::~JfrThreadSampling() {
  if (_sampler != nullptr) {
    _sampler->disenroll();
  }
}

#ifdef ASSERT
void assert_periods(const JfrThreadSampler* sampler, int64_t java_period_millis, int64_t native_period_millis) {
  assert(sampler != nullptr, "invariant");
  assert(sampler->get_java_period() == java_period_millis, "invariant");
  assert(sampler->get_native_period() == native_period_millis, "invariant");
}
#endif

static void log(int64_t java_period_millis, int64_t native_period_millis) {
  log_trace(jfr)("Updated thread sampler for java: " INT64_FORMAT "  ms, native " INT64_FORMAT " ms", java_period_millis, native_period_millis);
}

void JfrThreadSampling::create_sampler(int64_t java_period_millis, int64_t native_period_millis) {
  assert(_sampler == nullptr, "invariant");
  log_trace(jfr)("Creating thread sampler for java:" INT64_FORMAT " ms, native " INT64_FORMAT " ms", java_period_millis, native_period_millis);
  _sampler = new JfrThreadSampler(java_period_millis, native_period_millis, JfrOptionSet::stackdepth());
  _sampler->start_thread();
  _sampler->enroll();
}

void JfrThreadSampling::update_run_state(int64_t java_period_millis, int64_t native_period_millis) {
  if (java_period_millis > 0 || native_period_millis > 0) {
    if (_sampler == nullptr) {
      create_sampler(java_period_millis, native_period_millis);
    } else {
      _sampler->enroll();
    }
    DEBUG_ONLY(assert_periods(_sampler, java_period_millis, native_period_millis);)
    log(java_period_millis, native_period_millis);
    return;
  }
  if (_sampler != nullptr) {
    DEBUG_ONLY(assert_periods(_sampler, java_period_millis, native_period_millis);)
    _sampler->disenroll();
  }
}

void JfrThreadSampling::set_sampling_period(bool is_java_period, int64_t period_millis) {
  int64_t java_period_millis = 0;
  int64_t native_period_millis = 0;
  if (is_java_period) {
    java_period_millis = period_millis;
    if (_sampler != nullptr) {
      _sampler->set_java_period(java_period_millis);
      native_period_millis = _sampler->get_native_period();
    }
  } else {
    native_period_millis = period_millis;
    if (_sampler != nullptr) {
      _sampler->set_native_period(native_period_millis);
      java_period_millis = _sampler->get_java_period();
    }
  }
  update_run_state(java_period_millis, native_period_millis);
}

void JfrThreadSampling::set_java_sample_period(int64_t period_millis) {
  assert(period_millis >= 0, "invariant");
  if (_instance == nullptr && 0 == period_millis) {
    return;
  }
  instance().set_sampling_period(true, period_millis);
}

void JfrThreadSampling::set_native_sample_period(int64_t period_millis) {
  assert(period_millis >= 0, "invariant");
  if (_instance == nullptr && 0 == period_millis) {
    return;
  }
  instance().set_sampling_period(false, period_millis);
}

void JfrThreadSampling::handle_requested_sampling(JavaThread* thread) {
  if (_instance == nullptr) {
    return;
  }

  JfrThreadSampler* sampler = instance()._sampler;

  if (sampler == nullptr) {
    return;
  }

  sampler->handle_requested_sampling(thread);
}

bool JfrThreadSampling::has_requested_sampling(JavaThread* thread) {
  return thread->jfr_sample_state() != 0 || thread->jfr_sample_requests()->length() != 0;
}
