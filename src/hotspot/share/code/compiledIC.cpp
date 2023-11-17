/*
 * Copyright (c) 1997, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "code/codeBehaviours.hpp"
#include "code/codeCache.hpp"
#include "code/compiledIC.hpp"
#include "code/nmethod.hpp"
#include "code/vtableStubs.hpp"
#include "memory/resourceArea.hpp"
#include "oops/klass.inline.hpp"
#include "oops/method.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/continuationEntry.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/sharedRuntime.hpp"
#include "sanitizers/leak.hpp"


// Every time a compiled IC is changed or its type is being accessed,
// either the CompiledIC_lock must be set or we must be at a safe point.

CompiledICLocker::CompiledICLocker(CompiledMethod* method)
  : _method(method),
    _behaviour(CompiledICProtectionBehaviour::current()),
    _locked(_behaviour->lock(_method)) {
}

CompiledICLocker::~CompiledICLocker() {
  if (_locked) {
    _behaviour->unlock(_method);
  }
}

bool CompiledICLocker::is_safe(CompiledMethod* method) {
  return CompiledICProtectionBehaviour::current()->is_safe(method);
}

bool CompiledICLocker::is_safe(address code) {
  CodeBlob* cb = CodeCache::find_blob(code);
  assert(cb != nullptr && cb->is_compiled(), "must be compiled");
  CompiledMethod* cm = cb->as_compiled_method();
  return CompiledICProtectionBehaviour::current()->is_safe(cm);
}

CompiledICHolder::CompiledICHolder()
  : _speculated_method(),
    _speculated_klass(),
    _itable_defc_klass(),
    _itable_refc_klass(),
    _destination(SharedRuntime::get_resolve_virtual_call_stub()),
    _state(CompiledICState::_clean) {}

void CompiledICHolder::set_to_clean() {
  Atomic::store(&_destination, SharedRuntime::get_resolve_virtual_call_stub());
  _state = CompiledICState::_clean;
}

void CompiledICHolder::set_to_monomorphic(const methodHandle& callee_method, Klass* receiver_klass, address destination) {
  if (_speculated_method == nullptr) {
    // Only transition monotonically from null to a single speculated class
    Atomic::store(&_speculated_method, callee_method());
    Atomic::store(&_speculated_klass, receiver_klass);
  }
  Atomic::release_store(&_destination, destination);
  _state = CompiledICState::_monomorphic;
}

void CompiledICHolder::set_to_itable(Klass* defc, Klass* refc, address destination) {
  Atomic::store(&_itable_refc_klass, refc);
  Atomic::store(&_itable_defc_klass, defc);

  Atomic::release_store(&_destination, destination);
  _state = CompiledICState::_itable;
}

void CompiledICHolder::set_to_vtable(address destination) {
  Atomic::store(&_destination, destination);
  _state = CompiledICState::_vtable;
}

void CompiledICHolder::clean() {
  // GC cleaning doesn't need to change the state of the inline cache,
  // only nuke stale speculated metadata if it gets unloaded. If the
  // inline cache is monomorphic, the verified entries will miss, and
  // subsequent miss handlers will upgrade the callsite to megamorphic,
  // which makes sense as it obviously is megamorphic then.
  if (_speculated_klass != nullptr && !_speculated_klass->is_loader_alive()) {
    Atomic::store(&_speculated_method, (Method*)nullptr);
    Atomic::store(&_speculated_klass, (Klass*)nullptr);
  }
}

CompiledICHolder* CompiledIC::holder() const {
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");
  return _holder;
}

//-----------------------------------------------------------------------------
// High-level access to an inline cache. Guaranteed to be MT-safe.

CompiledICHolder* holder_from_reloc_iter(RelocIterator* iter) {
  assert(iter->type() == relocInfo::virtual_call_type, "wrong reloc. info");

  virtual_call_Relocation* r = iter->virtual_call_reloc();
  NativeMovConstReg* value = nativeMovConstReg_at(r->cached_value());

  return (CompiledICHolder*)value->data();
}

CompiledIC::CompiledIC(RelocIterator* iter)
  : _method(iter->code()),
    _holder(holder_from_reloc_iter(iter)),
    _call_instruction(iter->addr())
{
  assert(_method != nullptr, "must pass compiled method");
  assert(_method->contains(iter->addr()), "must be in compiled method");
}

CompiledIC* CompiledIC_before(CompiledMethod* nm, address return_addr) {
  address call_site = return_addr - 3; // TODO: Better constant for 3
  RelocIterator iter(nm, call_site, call_site + 1);
  CompiledIC* c_ic = new CompiledIC(&iter);
  c_ic->verify();
  return c_ic;
}

CompiledIC* CompiledIC_at(CompiledMethod* nm, address call_site) {
  RelocIterator iter(nm, call_site, call_site + 1);
  CompiledIC* c_ic = new CompiledIC(&iter);
  c_ic->verify();
  return c_ic;
}

CompiledIC* CompiledIC_at(Relocation* call_reloc) {
  address call_site = call_reloc->addr();
  CompiledMethod* cm = CodeCache::find_blob(call_reloc->addr())->as_compiled_method();
  RelocIterator iter(cm, call_site, call_site + 1);
  CompiledIC* c_ic = new CompiledIC(&iter);
  c_ic->verify();
  return c_ic;
}

CompiledIC* CompiledIC_at(RelocIterator* reloc_iter) {
  CompiledIC* c_ic = new CompiledIC(reloc_iter);
  c_ic->verify();
  return c_ic;
}

// This function may fail for two reasons: either due to running out of vtable
// stubs, or due to running out of IC stubs in an attempted transition to a
// transitional state. The needs_ic_stub_refill value will be set if the failure
// was due to running out of IC stubs, in which case the caller will refill IC
// stubs and retry.
bool CompiledIC::set_to_megamorphic(CallInfo* call_info, Bytecodes::Code bytecode) {
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");
  assert(is_monomorphic(), "going directly to megamorphic?");

  address entry;
  if (call_info->call_kind() == CallInfo::itable_call) {
    assert(bytecode == Bytecodes::_invokeinterface, "");
    int itable_index = call_info->itable_index();
    entry = VtableStubs::find_itable_stub(itable_index);
    if (entry == nullptr) {
      return false;
    }
#ifdef ASSERT
    int index = call_info->resolved_method()->itable_index();
    assert(index == itable_index, "CallInfo pre-computes this");
    InstanceKlass* k = call_info->resolved_method()->method_holder();
    assert(k->verify_itable_index(itable_index), "sanity check");
#endif //ASSERT
    _holder->set_to_itable(call_info->resolved_method()->method_holder(),
                           call_info->resolved_klass(),
                           entry);
  } else {
    assert(call_info->call_kind() == CallInfo::vtable_call, "either itable or vtable");
    // Can be different than selected_method->vtable_index(), due to package-private etc.
    int vtable_index = call_info->vtable_index();
    assert(call_info->resolved_klass()->verify_vtable_index(vtable_index), "sanity check");
    entry = VtableStubs::find_vtable_stub(vtable_index);
    if (entry == nullptr) {
      return false;
    }
    _holder->set_to_vtable(entry);
  }

  assert(is_megamorphic(), "sanity check");
  return true;
}

void CompiledIC::set_to_clean() {
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");

  // TODO: Clear with GC and see who else is cleaning really
  _holder->set_to_clean();
}

void CompiledIC::set_to_monomorphic(const methodHandle& callee_method, Klass* receiver_klass) {
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");

  // We speculate the receiver_klass is the only dynamic receiver_klass
  CompiledMethod* code = callee_method->code();
  address entry;
  if (code != nullptr && code->is_in_use() && !code->is_unloading()) {
    entry = code->entry_point();
  } else {
    entry = callee_method->get_c2i_unverified_entry();
  }
  _holder->set_to_monomorphic(callee_method, receiver_klass, entry);
}

#ifdef ASSERT
void CompiledIC::print() {
  // TODO: implement
}
void CompiledIC::verify() {
  // TODO: implement
}
#endif

// ----------------------------------------------------------------------------

void CompiledDirectCall::set_to_clean() {
  // in_use is unused but needed to match template function in CompiledMethod
  assert(CompiledICLocker::is_safe(instruction_address()), "mt unsafe call");
  // Reset call site
  RelocIterator iter((nmethod*)nullptr, instruction_address());
  while (iter.next()) {
    if (iter.addr() == instruction_address()) {
      switch(iter.type()) {
      case relocInfo::static_call_type:
        _call->set_destination_mt_safe(SharedRuntime::get_resolve_static_call_stub());
        break;
      case relocInfo::opt_virtual_call_type:
        _call->set_destination_mt_safe(SharedRuntime::get_resolve_opt_virtual_call_stub());
        break;
      default:
        ShouldNotReachHere();
      }
    }
  }
}

void CompiledDirectCall::set(const methodHandle& callee_method) {
  CompiledMethod* code = callee_method->code();
  CompiledMethod* caller = CodeCache::find_blob(instruction_address())->as_compiled_method();

  if (code != nullptr && code->is_in_use() && !code->is_unloading() &&
      (!caller->method()->is_continuation_enter_intrinsic() ||
       !ContinuationEntry::is_interpreted_call(instruction_address()))) {
    _call->set_destination_mt_safe(code->verified_entry_point());
  } else {
    // Patch call site to C2I adapter if code is deoptimized or unloaded.
    // We also need to patch the static call stub to set the rmethod register
    // to the callee_method so the c2i adapter knows how to build the frame
    set_to_interpreted(callee_method, callee_method->get_c2i_entry());
  }
}

bool CompiledDirectCall::is_clean() const {
  return destination() == SharedRuntime::get_resolve_static_call_stub() ||
         destination() == SharedRuntime::get_resolve_opt_virtual_call_stub();
}

bool CompiledDirectCall::is_call_to_interpreted() const {
  // It is a call to interpreted, if it calls to a stub. Hence, the destination
  // must be in the stub part of the nmethod that contains the call
  CompiledMethod* cm = CodeCache::find_compiled(instruction_address());
  return cm->stub_contains(destination());
}

bool CompiledDirectCall::is_call_to_compiled() const {
  return !is_clean() && !is_call_to_interpreted();
}

address CompiledDirectCall::find_stub_for(address instruction) {
  // Find reloc. information containing this call-site
  RelocIterator iter((nmethod*)nullptr, instruction);
  while (iter.next()) {
    if (iter.addr() == instruction) {
      switch(iter.type()) {
        case relocInfo::static_call_type:
          return iter.static_call_reloc()->static_stub();
        // We check here for opt_virtual_call_type, since we reuse the code
        // from the CompiledIC implementation
        case relocInfo::opt_virtual_call_type:
          return iter.opt_virtual_call_reloc()->static_stub();
        default:
          ShouldNotReachHere();
      }
    }
  }
  return nullptr;
}

address CompiledDirectCall::find_stub() {
  return find_stub_for(instruction_address());
}

#ifdef ASSERT
void CompiledDirectCall::print() {
  // TODO: implement
}
void CompiledDirectCall::verify_mt_safe(const methodHandle& callee, address entry,
                                        NativeMovConstReg* method_holder,
                                        NativeJump*        jump) {
  // TODO: implement
}
#endif
