/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_CDS_BOOTSTRAPCAPTURE_HPP
#define SHARE_CDS_BOOTSTRAPCAPTURE_HPP

#include "memory/allocation.hpp"
#include "runtime/frame.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/sizes.hpp"

// The BootstrapCapture class is used to analyze whether bootstrap functions such
// as clinit or *ndy bytecode BSMs are runtime independent, or not. Runtime independent
// functions may have the result of their computation shifted AOT.
//
// Tricky operations such as mutations or use of native code are tracked in the interpreter
// while the function is executing, and yields symptoms of runtime dependence that may
// either lead to a runtime dependence diagnosis or not.
//
// Symptoms such as cyclic class initialization and reading state produced by other
// bootstrap functions, do not go well together and yield a diagnosis. Yet, only one
// of the two symtpoms is okay, without getting a diagnosis.
//
// When functions do not observe any runtime dependent operations, the function is
// determined to be runtime independent. This has proven that every invocation is
// going to deterministically yield the same result as the one just computed. Such
// results may be shifted AOT.
//
// The diagnosis poitner of an entity after its first use is null iff the entity
// is runtime independent. If it is not null, then the runtime dependence is either
// diagnosed (self looped diagnosis pointer), or not yet known due to lazily resolved
// dependencies to another type (a growable array).
//
// The lazy resolved dependencies never have cycles and form a tree of unresolved
// dependencies through the diagnosis pointers. The direction of the children is from
// the nodes that are dependent on to the nodes that depend on them. In other words,
// these are anti dependencies. This direction is more helpful when a dependency that
// has been depended upon finally gets resolved. Then the tree is traversed to resolve
// more dependencies. This direction is also helpful for guaranteeing the invariant
// that after clinit barriers, the used class does not need tracking in the interpreter,
// if the diagnosis pointer is null.
//
// Each BootstrapNode tracks the number of unresolved dependencies, such that they
// can be eventually resolved when the transitively dependent nodes are resolved.

class BootstrapInfo;
class ConstantPool;
class ClassInitializerNode;
class InstanceKlass;
class JavaThread;
class ResolvedIndyEntry;

class BootstrapCapture : public AllStatic {
  friend class BootstrapNode;
  friend class BootstrapCaptureScope;

  static void escape_runtime_independent_objects(JavaThread* current);

public:
  static void track_allocation(JavaThread* current, oop obj);
  static void track_cyclic_class_initialization(JavaThread* current, InstanceKlass* dependency);
  static void track_class_use(JavaThread* current, InstanceKlass* dependency);

  static void assess_runtime_dependence_diagnosis(JavaThread* current);

  static bool is_enabled();

  static void whitelist_indy();
};

class BootstrapNode : public CHeapObj<mtClass> {
  bool _runtime_dependent;
  bool _immutable_data_dependent;
  BootstrapNode* _prev;
  InstanceKlass* _allowed_statics;
  uint64_t _unresolved_dependencies;
  frame _last_frame;

public:
  BootstrapNode(InstanceKlass* allowed_statics);

  void set_runtime_dependent() { _runtime_dependent = true; }
  void clear_runtime_dependent() { _runtime_dependent = false; }
  bool runtime_dependent() { return _runtime_dependent; }
  bool runtime_independent() { return !_runtime_dependent; }

  void inc_unresolved_dependencies(uint64_t amount = 1) { _unresolved_dependencies += amount; }
  bool dec_unresolved_dependencies() { return --_unresolved_dependencies == 0; }
  uint64_t unresolved_dependencies() { return _unresolved_dependencies; }

  frame last_frame() { return _last_frame; }

  void assess_runtime_dependence_diagnosis(JavaThread* current);

  void set_immutable_data_dependent() { _immutable_data_dependent = true; }
  bool is_immutable_data_dependent() const { return _immutable_data_dependent; }

  virtual oop escaping_root() { return nullptr; }

  virtual void* entity() = 0;
  virtual void* diagnosis() = 0;
  virtual void set_diagnosis(void* diagnosis)  = 0;

  bool try_resolve(GrowableArrayCHeap<BootstrapNode*, mtClass>** node_dependencies);

  void register_unresolved_dependency(BootstrapNode* dependency);
  void register_unresolved_dependency(InstanceKlass* dependency);

  BootstrapNode* prev() { return _prev; }
  void set_prev(BootstrapNode* prev) { _prev = prev; }
  static ByteSize runtime_dependent_offset() { return byte_offset_of(BootstrapNode, _runtime_dependent); }
  static ByteSize immutable_data_dependent_offset()   { return byte_offset_of(BootstrapNode, _immutable_data_dependent); }
  static ByteSize allowed_statics_offset()   { return byte_offset_of(BootstrapNode, _allowed_statics); }

  virtual const char* name() = 0;
  virtual const char* details() = 0;
};

class ClassInitializerNode : public BootstrapNode {
  InstanceKlass* _klass;

public:
  ClassInitializerNode(InstanceKlass* klass);

  InstanceKlass* klass() { return _klass; }

  void* entity() override { return _klass; }
  void* diagnosis() override;
  void set_diagnosis(void* diagnosis) override;
  oop escaping_root() override;
  const char* name() override;
  const char* details() override;
};

class IndyBootstrapNode : public BootstrapNode {
  ResolvedIndyEntry* _entry;

public:
  IndyBootstrapNode(ResolvedIndyEntry* entry);

  void* entity() override { return _entry; }
  void* diagnosis() override;
  void set_diagnosis(void* diagnosis) override;
  const char* name() override;
  const char* details() override;
};

class BootstrapCaptureScope : public StackObj {
protected:
  BootstrapNode* _node;
  BootstrapNode* _prev;

public:
  BootstrapCaptureScope(BootstrapNode* node);
  ~BootstrapCaptureScope();
};

class ClassInitializerCaptureScope : public BootstrapCaptureScope {
private:
  void taint_klass(JavaThread* current, InstanceKlass* klass);
  void taint_from_supers();

public:
  ClassInitializerCaptureScope(InstanceKlass* klass)
    : BootstrapCaptureScope(new ClassInitializerNode(klass)) {}

  ~ClassInitializerCaptureScope() {
    taint_from_supers();
  }
};

class IndyBootstrapCaptureScope : public BootstrapCaptureScope {
public:
  IndyBootstrapCaptureScope(ResolvedIndyEntry* entry)
    : BootstrapCaptureScope(new IndyBootstrapNode(entry)) {}
};

#endif // SHARE_CDS_BOOTSTRAPCAPTURE_HPP
