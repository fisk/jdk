/*
 * Copyright (c) 2015, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zBarrier.inline.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zRootsIterator.hpp"
#include "gc/z/zTask.hpp"
#include "gc/z/zWeakRootsProcessor.hpp"
#include "gc/z/zWorkers.hpp"
#include "memory/iterator.hpp"
#include "runtime/atomic.hpp"
#include "utilities/debug.hpp"

class ZPhantomCleanOopClosure : public OopClosure {
private:
  ZGeneration* _generation;

public:
  ZPhantomCleanOopClosure(ZGeneration* generation) :
      _generation(generation) {}

  virtual void do_oop(oop* p) {
    ZBarrier::clean_barrier_on_phantom_root_oop_field((zpointer*)p, _generation);
    SuspendibleThreadSet::yield();
  }

  virtual void do_oop(narrowOop* p) {
    ShouldNotReachHere();
  }
};

ZWeakRootsProcessor::ZWeakRootsProcessor(ZWorkers* workers)
  : _workers(workers) {}

class ZProcessWeakRootsTask : public ZTask {
private:
  ZRootsIteratorWeakColored _roots_weak_colored;
  ZGeneration*              _generation;

public:
  ZProcessWeakRootsTask(ZGeneration* generation)
    : ZTask("ZProcessWeakRootsTask"),
      _roots_weak_colored(generation->is_young() ?
                          ZGenerationIdOptional::young :
                          ZGenerationIdOptional::old),
      _generation(generation) {}

  ~ZProcessWeakRootsTask() {
    _roots_weak_colored.report_num_dead();
  }

  virtual void work() {
    SuspendibleThreadSetJoiner sts_joiner;
    ZPhantomCleanOopClosure cl(_generation);
    _roots_weak_colored.apply(&cl);
  }
};

void ZWeakRootsProcessor::process_weak_roots(ZGeneration* generation) {
  ZProcessWeakRootsTask task(generation);
  _workers->run(&task);
}
