/*
 * Copyright (c) 2020, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "cds/cdsThread.hpp"
#include "cds/streamingArchiveHeapLoader.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/javaThreadStatus.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.hpp"
#include "runtime/threads.hpp"

CDSThread* CDSThread::_cds_thread;

void CDSThread::initialize() {
  if (!UseSharedSpaces) {
    return;
  }

  EXCEPTION_MARK;

  // Spin up thread without thread oop
  _cds_thread = new CDSThread(&cds_thread_entry);
  JavaThread::vm_exit_on_osthread_failure(_cds_thread);

  {
    MutexLocker mu(JavaThread::current(), Threads_lock);
    Threads::add(_cds_thread);
  }

  os::start_thread(_cds_thread);
}

void CDSThread::materialize_thread_object() {
  if (!UseSharedSpaces) {
    return;
  }

  EXCEPTION_MARK;

  // Bind the thread_oop to the CDS JavaThread.
  const char* name = "CDS Thread";
  Handle thread_oop = JavaThread::create_system_thread_object(name, JavaThread::current());

  _cds_thread->set_threadOopHandles(thread_oop());
  java_lang_Thread::release_set_thread(thread_oop(), _cds_thread);
  java_lang_Thread::set_thread_status(thread_oop(), JavaThreadStatus::RUNNABLE);
}

void CDSThread::cds_thread_entry(JavaThread* jt, TRAPS) {
#if INCLUDE_CDS_JAVA_HEAP
  StreamingArchiveHeapLoader::materialize_objects();
#endif
}
