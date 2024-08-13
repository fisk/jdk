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

#ifndef SHARE_CDS_BOOTSTRAPCAPTURE_HPP
#define SHARE_CDS_BOOTSTRAPCAPTURE_HPP

#include "memory/allocation.hpp"
#include "runtime/frame.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/sizes.hpp"

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

  bool try_resolve();

  void register_unresolved_dependency(BootstrapNode* dependency);
  void register_unresolved_dependency(InstanceKlass* dependency);

  BootstrapNode* prev() { return _prev; }
  void set_prev(BootstrapNode* prev) { _prev = prev; }
  static ByteSize runtime_dependent_offset() { return byte_offset_of(BootstrapNode, _runtime_dependent); }
  static ByteSize immutable_data_dependent_offset()   { return byte_offset_of(BootstrapNode, _immutable_data_dependent); }
  static ByteSize allowed_statics_offset()   { return byte_offset_of(BootstrapNode, _allowed_statics); }

  virtual void print_resolution() = 0;
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
  void print_resolution() override;
};

class IndyBootstrapNode : public BootstrapNode {
  ResolvedIndyEntry* _entry;

public:
  IndyBootstrapNode(ResolvedIndyEntry* entry);

  void* entity() override { return _entry; }
  void* diagnosis() override;
  void set_diagnosis(void* diagnosis) override;
  void print_resolution() override;
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
  void taint_klass_and_interfaces(JavaThread* current, InstanceKlass* klass);
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
