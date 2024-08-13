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

#include "cds/bootstrapCapture.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/constantPool.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/resolvedIndyEntry.hpp"
#include "oops/markWord.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/stackFrameStream.inline.hpp"
#include "utilities/stack.inline.hpp"
#include "interpreter/bootstrapInfo.hpp"

// TODO: Check if analyzed classes were AOT loadable as precondition

static volatile uint64_t _initialized_classes = 0;
static volatile uint64_t _trivial_classes = 0;
static volatile uint64_t _simple_classes = 0;
static volatile uint64_t _runtime_independent_initialized_classes = 0;

static volatile uint64_t _initialized_indys = 0;
static volatile uint64_t _runtime_independent_initialized_indys = 0;

void BootstrapCapture::track_allocation(JavaThread* current, oop obj) {
  if (!is_enabled()) {
    return;
  }

  BootstrapNode* node = current->active_bootstrap();
  if (node != nullptr && node->runtime_independent()) {
    // We are currently performing runtime independence analysis.
    for (;;) {
      // Color the allocated object "runtime independent" by setting its identity hash code
      // to 1. Since the identityHashCode method is a native method, and its intrinsics are
      // only used by compilers, we always have the opportunity to clean up all objects with
      // 1 hash codes before exposing the objects to a system capable of reading the identity
      // hash codes. This trick allows us to not claim an entire bit in the object header for
      // runtime independence analysis - only a single value in the large number range for
      // identity hash codes.
      markWord old_mark = obj->mark();
      markWord new_mark = old_mark.copy_set_hash(1);
      if (obj->cas_set_mark(new_mark, old_mark, memory_order_relaxed) == old_mark) {
        break;
      }
    }
  }
}

// TODO: Commonalities?
static bool is_lazy_dependent(InstanceKlass* from, BootstrapNode* to) {
  // Treat self dependency is dependent
  if (to->entity() == from) {
    return true;
  }

  void* diagnosis = to->diagnosis();
  if (diagnosis == nullptr || diagnosis == to->entity()) {
    return false;
  }

  GrowableArrayCHeap<BootstrapNode*, mtClass>* node_dependencies = static_cast<GrowableArrayCHeap<BootstrapNode*, mtClass>*>(diagnosis);
  // Push children in the anti dependency tree
  for (BootstrapNode* dependency : *node_dependencies) {
    if (is_lazy_dependent(from, dependency)) {
      return true;
    }
  }

  return false;
}

static bool is_lazy_dependent(BootstrapNode* from, BootstrapNode* to) {
  // Treat self dependency is dependent
  if (to == from) {
    return true;
  }

  void* diagnosis = to->diagnosis();
  if (diagnosis == nullptr || diagnosis == to->entity()) {
    return false;
  }

  GrowableArrayCHeap<BootstrapNode*, mtClass>* node_dependencies = static_cast<GrowableArrayCHeap<BootstrapNode*, mtClass>*>(diagnosis);
  // Push children in the anti dependency tree
  for (BootstrapNode* dependency : *node_dependencies) {
    if (is_lazy_dependent(from, dependency)) {
      return true;
    }
  }

  return false;
}

void BootstrapCapture::track_class_use(JavaThread* current, InstanceKlass* dependency) {
  if (!is_enabled()) {
    return;
  }

  BootstrapNode* active_node = current->active_bootstrap();
  if (active_node == nullptr || active_node->runtime_dependent()) {
    return;
  }

  void* dependency_diagnosis = dependency->runtime_dependence_diagnosis();
  if (dependency_diagnosis == nullptr) {
    // No runtime dependence signs
    return;
  }

  if (dependency->has_aot_safe_initializer()) {
    // User declared runtime independence; trust it
    return;
  }

  MutexLocker ml(RuntimeIndependenceAnalysis_lock, Mutex::_no_safepoint_check_flag);
  dependency_diagnosis = dependency->runtime_dependence_diagnosis();
  if (dependency_diagnosis == nullptr) {
    // No runtime dependence signs
    return;
  }

  if (dependency_diagnosis == dependency) {
    // Runtime dependence diagnosis of dependent class assessed
    active_node->assess_runtime_dependence_diagnosis(current);
    return;
  }

  // There is an unresolved dependency
  active_node->register_unresolved_dependency(dependency);
}

void BootstrapCapture::track_cyclic_class_initialization(JavaThread* current, InstanceKlass* dependency) {
  if (!is_enabled()) {
    return;
  }

  BootstrapNode* active_node = current->active_bootstrap();
  if (active_node == nullptr || active_node->runtime_dependent()) {
    return;
  }

  if (active_node->entity() == dependency) {
    // Cyclic but only to the owner... doesn't count as a real cycle really
    return;
  }

  if (active_node->is_immutable_data_dependent()) {
    // Immutable data dependencies and clinit cycles combined is a bad smell: disallow
    assess_runtime_dependence_diagnosis(current);
    return;
  }

  MutexLocker ml(RuntimeIndependenceAnalysis_lock, Mutex::_no_safepoint_check_flag);
  void* dependency_diagnosis = dependency->runtime_dependence_diagnosis();

  if (dependency_diagnosis == dependency) {
    // No need to create lazy dependency; already toast
    active_node->assess_runtime_dependence_diagnosis(current);
    return;
  }

  // Walk previous dependencies and install lazy dependencies along the way
  // until the root of the cycle ("dependency") which is always a class
  BootstrapNode* prev_dependency = active_node;
  BootstrapNode* curr_dependency = active_node->prev();
  bool found_root = true;
  while (curr_dependency->entity() != dependency) {
    if (curr_dependency->entity() == curr_dependency->diagnosis()) {
      found_root = false;
      break;
    }

    prev_dependency = curr_dependency;
    curr_dependency = curr_dependency->prev();
  }

  if (found_root) {
    assert(curr_dependency->entity() == dependency, "should be root of cycle");
    active_node->register_unresolved_dependency(curr_dependency);
  } else {
    assess_runtime_dependence_diagnosis(current);
  }
}

void BootstrapCapture::assess_runtime_dependence_diagnosis(JavaThread* current) {
  if (!is_enabled()) {
    return;
  }

  BootstrapNode* active_node = current->active_bootstrap();
  if (active_node == nullptr) {
    return;
  }

  active_node->assess_runtime_dependence_diagnosis(JavaThread::current());
}

bool BootstrapCapture::is_enabled() {
  return AnalyzeRuntimeIndependence;
}

void BootstrapCapture::whitelist_indy() {
  // If you promise a node can be trusted, I will treat it as such.
  // Hope you know what you are doing!
  // WARNING: You probably don't know what you are doing. Stop right now.
  // WARNING: If you are still reading these comments, you should stop and do something else.
  JavaThread* current = JavaThread::current();
  BootstrapNode* active_node = current->active_bootstrap();
  void* diagnosis = active_node->diagnosis();
  if (diagnosis != nullptr && diagnosis != active_node->entity()) {
    delete (GrowableArrayCHeap<BootstrapNode*, mtClass>*) diagnosis;
  }
  active_node->set_diagnosis(nullptr);

  if (active_node->runtime_dependent()) {
    current->increment_interp_only_mode();
    active_node->clear_runtime_dependent();
  }
}

class FollowRICleaningOopClosure : public BasicOopIterateClosure {
private:
  oop _base;
  Stack<oop, mtClass>* _traversal_stack;

public:
  FollowRICleaningOopClosure(oop base, Stack<oop, mtClass>* traversal_stack)
    : _base(base),
      _traversal_stack(traversal_stack) {}

  template <typename T> void do_oop_work(T* p) {
    uintptr_t offset = ((uintptr_t)p) - cast_from_oop<uintptr_t>(_base);
    oop obj = HeapAccess<ON_UNKNOWN_OOP_REF>::oop_load_at(_base, offset);
    if (obj != nullptr) {
      _traversal_stack->push(obj);
    }
  }

  virtual void do_oop(oop* p) {
    do_oop_work(p);
  }

  virtual void do_oop(narrowOop* p) {
    do_oop_work(p);
  }
};

static void follow(oop obj, Stack<oop, mtClass>* traversal_stack) {
  FollowRICleaningOopClosure cl(obj, traversal_stack);
  obj->oop_iterate(&cl);
}

static bool clean_runtime_independence(oop obj) {
  for (;;) {
    // Color the allocated object "runtime independent" by setting its identity hash code
    // to 1. Since the identityHashCode method is a native method, and its intrinsics are
    // only used by compilers, we always have the opportunity to clean up all objects with
    // 1 hash codes before exposing the objects to a system capable of reading the identity
    // hash codes. This trick allows us to not claim an entire bit in the object header for
    // runtime independence analysis - only a single value in the large number range for
    // identity hash codes.
    markWord old_mark = obj->mark();
    if (old_mark.hash() != 1) {
      return false;
    }
    markWord new_mark = old_mark.copy_set_hash(0);
    if (obj->cas_set_mark(new_mark, old_mark, memory_order_relaxed) == old_mark) {
      return true;
    }
  }
}

class FollowRICleaningRootOopClosure : public OopClosure {
private:
  Stack<oop, mtClass>* _traversal_stack;

public:
  FollowRICleaningRootOopClosure(Stack<oop, mtClass>* traversal_stack)
    : _traversal_stack(traversal_stack) {}

  virtual void do_oop(oop* p) {
    oop obj = *p;
    if (obj != nullptr) {
      _traversal_stack->push(obj);
    }
  }

  virtual void do_oop(narrowOop* p) {
    ShouldNotReachHere();
  }
};

void BootstrapCapture::escape_runtime_independent_objects(JavaThread* current) {
  Stack<oop, mtClass> traversal_stack;
  BootstrapNode* active_node = current->active_bootstrap();
  frame bootstrap_trigger = active_node->last_frame();
  oop entity_root = active_node->escaping_root();

  if (entity_root != nullptr) {
    follow(entity_root, &traversal_stack);
  }

  if (current->has_last_Java_frame()) {
    // Look for roots in the frames of the current BSM node
    for (StackFrameStream fst(current, true /* update */, true /* process_frames */); !fst.is_done(); fst.next()) {
      frame& fr = *fst.current();
      if (fr.id() == bootstrap_trigger.id()) {
        // Only objects up to the caller of the BSM will escape; don't clean up further or we
        // risk over cleaning into a BSM that depends on the currently executing one.
        break;
      }
      FollowRICleaningRootOopClosure cl(&traversal_stack);
      fr.oops_do(&cl, nullptr, fst.register_map());
    }
  }

  // Traverse and clean transitive closure
  while (!traversal_stack.is_empty()) {
    oop obj = traversal_stack.pop();
    if (clean_runtime_independence(obj)) {
      follow(obj, &traversal_stack);
    }
  }
}

BootstrapNode::BootstrapNode(InstanceKlass* allowed_statics)
  : _runtime_dependent(),
    _immutable_data_dependent(),
    _prev(),
    _allowed_statics(allowed_statics),
    _unresolved_dependencies(1),
    _last_frame() {
  if (BootstrapCapture::is_enabled()) {
    JavaThread* current = JavaThread::current();
    if (current->has_last_Java_frame()) {
      _last_frame = JavaThread::current()->last_frame();
    }
  }
}

// TODO: Unify below two methods or delete one
void BootstrapNode::register_unresolved_dependency(BootstrapNode* dependency) {
  assert_lock_strong(RuntimeIndependenceAnalysis_lock);

  // TODO: Resource mark hygiene
  ResourceMark rm;

  if (is_lazy_dependent(dependency, this)) {
    // No need to record cycles
    log_info(class, init)("Noting cyclic dependency from %s to %s", name(), dependency->name());
    return;
  }

  log_info(class, init)("Registering unresolved dependency from %s to %s", name(), dependency->name());

  void* dependency_diagnosis = dependency->diagnosis();
  assert(dependency_diagnosis != dependency, "should not register after diagnosis");
  if (dependency_diagnosis == nullptr) {
    GrowableArrayCHeap<BootstrapNode*, mtClass>* dependencies = new GrowableArrayCHeap<BootstrapNode*, mtClass>();
    inc_unresolved_dependencies();
    dependencies->append(this);
    dependency->set_diagnosis(dependencies);
  } else {
    GrowableArrayCHeap<BootstrapNode*, mtClass>* dependencies = (GrowableArrayCHeap<BootstrapNode*, mtClass>*)dependency_diagnosis;
    if (!dependencies->contains(this)) {
      inc_unresolved_dependencies();
      dependencies->append(this);
    }
  }
}

void BootstrapNode::register_unresolved_dependency(InstanceKlass* dependency) {
  assert_lock_strong(RuntimeIndependenceAnalysis_lock);

  // TODO: Resource mark hygiene
  ResourceMark rm;

  if (is_lazy_dependent(dependency, this)) {
    // No need to record cycles
    log_info(class, init)("Noting cyclic dependency from %s to %s", name(), dependency->name()->as_C_string());
    return;
  }

  log_info(class, init)("Registering unresolved dependency from %s to %s", name(), dependency->name()->as_C_string());

  void* dependency_diagnosis = dependency->runtime_dependence_diagnosis();
  assert(dependency_diagnosis != dependency, "should not register after diagnosis");
  if (dependency_diagnosis == nullptr) {
    GrowableArrayCHeap<BootstrapNode*, mtClass>* dependencies = new GrowableArrayCHeap<BootstrapNode*, mtClass>();
    inc_unresolved_dependencies();
    dependencies->append(this);
    dependency->set_runtime_dependence_diagnosis(dependencies);
  } else {
    GrowableArrayCHeap<BootstrapNode*, mtClass>* dependencies = (GrowableArrayCHeap<BootstrapNode*, mtClass>*)dependency_diagnosis;
    if (!dependencies->contains(this)) {
      inc_unresolved_dependencies();
      dependencies->append(this);
    }
  }
}

void BootstrapNode::assess_runtime_dependence_diagnosis(JavaThread* current) {
  if (runtime_dependent()) {
    // Already triggered
    return;
  }
  set_runtime_dependent();
  current->decrement_interp_only_mode();
  BootstrapCapture::escape_runtime_independent_objects(current);

  // TODO: Hygiene
  ResourceMark rm;
  log_info(class, init)("Early runtime dependence diagnosis for %s", name());
}

// Does <clinit> exist for ik or any of its supertypes?
static bool has_clinit(InstanceKlass* ik) {
  if (ik->class_initializer() != nullptr) {
    return true;
  }
  InstanceKlass* super = ik->java_super();
  if (super != nullptr && has_clinit(super)) {
    return true;
  }
  Array<InstanceKlass*>* interfaces = ik->local_interfaces();
  int num_interfaces = interfaces->length();
  for (int index = 0; index < num_interfaces; index++) {
    InstanceKlass* intf = interfaces->at(index);
    if (has_clinit(intf)) {
      return true;
    }
  }
  return false;
}

ClassInitializerNode::ClassInitializerNode(InstanceKlass* klass)
  : BootstrapNode(klass),
    _klass(klass) {
  LogTarget(Info, class, init) lt;
  if (!lt.is_enabled()) {
    return;
  }

  AtomicAccess::inc(&_initialized_classes);

  if (_klass->class_initializer() == nullptr) {
    AtomicAccess::inc(&_trivial_classes);
  }

  if (!has_clinit(_klass)) {
    AtomicAccess::inc(&_simple_classes);
  }
}

void* ClassInitializerNode::diagnosis() {
  assert_lock_strong(RuntimeIndependenceAnalysis_lock);
  return _klass->runtime_dependence_diagnosis();
}

void ClassInitializerNode::set_diagnosis(void* diagnosis) {
  assert_lock_strong(RuntimeIndependenceAnalysis_lock);
  _klass->set_runtime_dependence_diagnosis(diagnosis);
  if (diagnosis == nullptr) {
    _klass->set_has_aot_safe_initializer();
    AtomicAccess::inc(&_runtime_independent_initialized_classes);
  }
}

oop ClassInitializerNode::escaping_root() {
  return klass()->java_mirror();
}

IndyBootstrapNode::IndyBootstrapNode(ResolvedIndyEntry* entry)
  : BootstrapNode(nullptr),
    _entry(entry) {
  AtomicAccess::inc(&_initialized_indys);
}

void* IndyBootstrapNode::diagnosis() {
  return _entry->runtime_dependence_diagnosis();
}

void IndyBootstrapNode::set_diagnosis(void* diagnosis) {
  _entry->set_runtime_dependence_diagnosis(diagnosis);
  if (diagnosis == nullptr) {
    AtomicAccess::inc(&_runtime_independent_initialized_indys);
  }
}

const char* IndyBootstrapNode::name() {
  return "indy";
}

const char* IndyBootstrapNode::details() {
  return "indy";
}

BootstrapCaptureScope::BootstrapCaptureScope(BootstrapNode* node)
  : _node(node), _prev(nullptr) {
  if (!BootstrapCapture::is_enabled()) {
    return;
  }

  JavaThread* jt = JavaThread::current();
  _prev = jt->active_bootstrap();
  jt->set_active_bootstrap(node);
  if (_prev != nullptr) {
    node->set_prev(_prev);
  }

  if (_prev == nullptr || _prev->runtime_dependent()) {
    jt->increment_interp_only_mode();
  }
}

bool BootstrapNode::try_resolve(GrowableArrayCHeap<BootstrapNode*, mtClass>** node_dependencies) {
  void* d = diagnosis();

  if (d == entity()) {
    // Already diagnosed as runtime dependent
    log_info(class, init)("Resolved already runtime dependent node: %s", name());
    *node_dependencies = nullptr;
    return false;
  }

  *node_dependencies = static_cast<GrowableArrayCHeap<BootstrapNode*, mtClass>*>(d);

  if (runtime_dependent()) {
    _unresolved_dependencies = 0;
    set_diagnosis(entity());
    log_info(class, init)("Resolved runtime dependent node: %s %s", name(), details());
    return true;
  }

  precond(unresolved_dependencies() > 0);

  dec_unresolved_dependencies();

  if (unresolved_dependencies() != 0) {
    log_info(class, init)("Lazily resolving node: %s", name());
    return false;
  }

  log_info(class, init)("Resolved runtime independent node: %s %s", name(), details());
  set_diagnosis(nullptr);

  return true;
}

const char* ClassInitializerNode::name() {
  return _klass->name()->as_C_string();
}

const char* ClassInitializerNode::details() {
  if (_klass->class_initializer() != nullptr) {
    return " (with clinit)";
  }

  return "";
}

BootstrapCaptureScope::~BootstrapCaptureScope() {
  if (!BootstrapCapture::is_enabled()) {
    return;
  }

  JavaThread* current = JavaThread::current();
  ResourceMark rm; // TODO: For printing below

  log_info(class, init)("Analysis finished for: %s", _node->name());

  MutexLocker ml(RuntimeIndependenceAnalysis_lock, Mutex::_no_safepoint_check_flag);

  // Transitive resolution of dependency
  Stack<BootstrapNode*, mtClass> resolution_stack;
  GrowableArrayCHeap<BootstrapNode*, mtClass>* node_dependencies;

  if (_node->try_resolve(&node_dependencies)) {
    if (node_dependencies != nullptr) {
      // Push children in the anti dependency tree
      for (BootstrapNode* dependency : *node_dependencies) {
        resolution_stack.push(dependency);
        // Propagate runtime dependence
        if (_node->runtime_dependent()) {
          dependency->set_runtime_dependent();
        }
      }

      delete node_dependencies;
    }
  } else if (_node->prev() != nullptr) {
    _node->prev()->register_unresolved_dependency(_node);
  }

  while (!resolution_stack.is_empty()) {
    BootstrapNode* node = resolution_stack.pop();

    if (!node->try_resolve(&node_dependencies)) {
      // Already resolved or pending unresolved dependency
      continue;
    }

    // TODO: Follow method abstraction
    if (node_dependencies != nullptr) {
      // Push children in the anti dependency tree
      for (BootstrapNode* dependency : *node_dependencies) {
        resolution_stack.push(dependency);
        // Propagate runtime dependence
        if (node->runtime_dependent()) {
          dependency->set_runtime_dependent();
        }
      }

      delete node_dependencies;
    }
  }

  uint64_t runtime_independent_classes = AtomicAccess::load(&_runtime_independent_initialized_classes);
  uint64_t trivial_classes = AtomicAccess::load(&_trivial_classes);
  uint64_t simple_classes = AtomicAccess::load(&_simple_classes);
  uint64_t classes = AtomicAccess::load(&_initialized_classes);

  uint64_t indys = AtomicAccess::load(&_initialized_indys);
  uint64_t runtime_independent_indys = AtomicAccess::load(&_runtime_independent_initialized_indys);

  log_info(class, init)("Runtime independent classes %zu / %zu (%.1f)",
                        runtime_independent_classes, classes, double(runtime_independent_classes) / double(classes) * 100.0);

  log_info(class, init)("Trivial classes %zu / %zu (%.1f)",
                        trivial_classes, classes, double(trivial_classes) / double(classes) * 100.0);

  log_info(class, init)("Simple classes %zu / %zu (%.1f)",
                        simple_classes, classes, double(simple_classes) / double(classes) * 100.0);

  log_info(class, init)("Runtime independent indys %zu / %zu (%.1f)",
                        runtime_independent_indys, indys, double(runtime_independent_indys) / double(indys) * 100.0);

  if (_node->runtime_dependent()) {
    current->set_active_bootstrap(_prev);
    if (_prev != nullptr && !_prev->runtime_dependent()) {
      _prev->set_runtime_dependent();
      BootstrapCapture::escape_runtime_independent_objects(current);
    }
  } else {
    // Clean away local shade of bootstrap objects if any
    BootstrapCapture::escape_runtime_independent_objects(current);

    if (_prev == nullptr) {
      current->decrement_interp_only_mode();
    } else if (_prev->runtime_dependent()) {
      current->decrement_interp_only_mode();
    } else if (_node->is_immutable_data_dependent()) {
      _prev->set_immutable_data_dependent();
      if (_prev->unresolved_dependencies() > 1) {
        // After running a dependent BSM, we have become tainted with both an immutable data dependency
        // and cyclic initialization. This is a combination that we can not support; diagnose this as
        // runtime dependent code.
        _prev->assess_runtime_dependence_diagnosis(current);
      }
    }
    current->set_active_bootstrap(_prev);
  }
}

void ClassInitializerCaptureScope::taint_klass(JavaThread* current, InstanceKlass* klass) {
  bool tainted_before = _node->runtime_dependent();

  if (klass->is_being_initialized() && klass->is_reentrant_initialization(current)) {
    BootstrapCapture::track_cyclic_class_initialization(current, klass);
  } else {
    BootstrapCapture::track_class_use(current, klass);
  }

  bool tainted_after = _node->runtime_dependent();

  if (tainted_after && !tainted_before) {
    ResourceMark rm;
    log_info(class, init)("Runtime Dependent Super: %s for %s",
                          klass->name()->as_C_string(),
                          static_cast<ClassInitializerNode*>(_node)->klass()->name()->as_C_string());
  }
}

void ClassInitializerCaptureScope::taint_from_supers() {
  JavaThread* current = JavaThread::current();

  if (!BootstrapCapture::is_enabled()) {
    return;
  }

  ClassInitializerNode* node = static_cast<ClassInitializerNode*>(_node);
  assert(node == current->active_bootstrap(), "strange destruction?");
  InstanceKlass* k = node->klass();

  for (InstanceKlass* super = k->java_super(); super != nullptr; super = super->java_super()) {
    taint_klass(current, super);
  }

  for (int i = 0; i < k->transitive_interfaces()->length(); ++i) {
    InstanceKlass* interface = k->transitive_interfaces()->at(i);
    taint_klass(current, interface);
  }
}
