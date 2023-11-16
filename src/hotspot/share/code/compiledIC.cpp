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
#include "interpreter/interpreter.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/klass.inline.hpp"
#include "oops/method.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/continuationEntry.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/sharedRuntime.hpp"
#include "sanitizers/leak.hpp"
#include "utilities/interfaceSupport.inline.hpp"


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

CompiledICHolder::CompiledICHolder(Metadata* metadata, Klass* klass, address destination, CompiledICState state)
  : _holder_metadata(metadata), _holder_klass(klass), _destination(destination), _next(nullptr), _state(state) {
}

void CompiledICHolder::release() {
  for (;;) {
    CompiledICHolder* head = Atomic::load(&_unlink_list);
    set_next(head);
    if (Atomic::cmpxchg(&_unlink_list, head, this) == head) {
      return;
    }
  }
}

bool CompiledICHolder::is_loader_alive() {
  if (_holder_metadata != nullptr) {
    Klass* k = _state == CompiledICState::_monomorphic ? ((Method*)_holder_metadata)->method_holder() : (Klass*)_holder_metadata;
    if (!k->is_loader_alive()) {
      return false;
    }
  }

  if (_holder_klass != nullptr && !_holder_klass->is_loader_alive()) {
    return false;
  }
  return true;
}

void CompiledICHolder::trigger_cleanup() {
  assert(SafepointSynchronize::is_at_safepoint(), "Intended to trigger in safepoint cleanup");
  MonitorLocker ml(Service_lock, Mutex::_no_safepoint_check_flag);
  _purge_list = _unlink_list;
  _unlink_list = nullptr;
  if (_purge_list != nullptr) {
    ml.notify_all();
  }
}

bool CompiledICHolder::has_cleanup_work() {
  return _purge_list != nullptr;
}

// The service thread takes care of deleting released CompiledICHolders
void CompiledICHolder::do_cleanup_work() {
  CompiledICHolder* current = _purge_list;
  _purge_list = nullptr;

  ThreadBlockInVM tbivm(JavaThread::current());
  while (current != nullptr) {
    CompiledICHolder* next = current->next();
    delete current;
    current = next;
  }
}

CompiledICHolder* CompiledIC::holder() const {
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");
  return (CompiledICHolder*)_call->get_data(_value);
}

void CompiledIC::set_holder(CompiledICHolder* holder) {
  assert(entry_point != nullptr, "must set legal entry point");
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");
  assert (!is_optimized() || cache == nullptr, "an optimized virtual call does not have a cached metadata");

  if (holder != nullptr) {
    holder()->release();
  }

  _call->set_data(_value, holder);
  // LSan doesn't understand that the holder escapes into an instruction immediate
  LSAN_IGNORE_OBJECT(holder);
}

//-----------------------------------------------------------------------------
// High-level access to an inline cache. Guaranteed to be MT-safe.

void CompiledIC::initialize_from_iter(RelocIterator* iter) {
  assert(iter->addr() == _call->instruction_address(), "must find ic_call");

  virtual_call_Relocation* r = iter->virtual_call_reloc();
  _value = _call->get_load_instruction(r);
}

CompiledIC::CompiledIC(CompiledMethod* cm, NativeCall* call)
  : _method(cm),
    _call(call)
{
  _call = _method->call_wrapper_at((address) call);
  address ic_call = _call->instruction_address();

  assert(ic_call != nullptr, "ic_call address must be set");
  assert(cm != nullptr, "must pass compiled method");
  assert(cm->contains(ic_call), "must be in compiled method");

  // Search for the ic_call at the given address.
  RelocIterator iter(cm, ic_call, ic_call+1);
  bool ret = iter.next();
  assert(ret == true, "relocInfo must exist at this address");
  assert(iter.addr() == ic_call, "must find ic_call");

  initialize_from_iter(&iter);
}

CompiledIC::CompiledIC(RelocIterator* iter)
  : _method(iter->code())
{
  _call = _method->call_wrapper_at(iter->addr());
  address ic_call = _call->instruction_address();

  CompiledMethod* nm = iter->code();
  assert(ic_call != nullptr, "ic_call address must be set");
  assert(nm != nullptr, "must pass compiled method");
  assert(nm->contains(ic_call), "must be in compiled method");

  initialize_from_iter(iter);
}

// This function may fail for two reasons: either due to running out of vtable
// stubs, or due to running out of IC stubs in an attempted transition to a
// transitional state. The needs_ic_stub_refill value will be set if the failure
// was due to running out of IC stubs, in which case the caller will refill IC
// stubs and retry.
bool CompiledIC::set_to_megamorphic(CallInfo* call_info, Bytecodes::Code bytecode, TRAPS) {
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");
  assert(!is_optimized(), "cannot set an optimized virtual call to megamorphic");
  assert(is_call_to_compiled() || is_call_to_interpreted(), "going directly to megamorphic?");

  address entry;
  CompiledICHolder* holder;
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
    holder = new CompiledICHolder(call_info->resolved_method()->method_holder(),
                                  call_info->resolved_klass(),
                                  entry,
                                  CompiledICState::_itable);
  } else {
    assert(call_info->call_kind() == CallInfo::vtable_call, "either itable or vtable");
    // Can be different than selected_method->vtable_index(), due to package-private etc.
    int vtable_index = call_info->vtable_index();
    assert(call_info->resolved_klass()->verify_vtable_index(vtable_index), "sanity check");
    entry = VtableStubs::find_vtable_stub(vtable_index);
    if (entry == nullptr) {
      return false;
    }
    holder = new CompiledICHolder(nullptr,
                                  nullptr,
                                  entry,
                                  CompiledICState::_vtable);
  }

  set_holder(holder);

  assert(is_megamorphic(), "sanity check");
  return true;
}

void CompiledIC::set_to_clean() {
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");

  address entry = SharedRuntime::get_resolve_virtual_call_stub();
  set_holder(new CompiledICHolder(nullptr, nullptr, entry, CompiledICState::_clean));
}

bool CompiledIC::set_to_monomorphic(Method* callee_method, Klass* receiver_klass) {
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");

  // We speculate the receiver_klass is the only dynamic receiver_klass
  nmethod* code = callee_method->code();
  address entry;
  if (code != nullptr && code->is_in_use && !code->is_unloading()) {
    entry = code->entry_point();
  } else {
    entry = callee_method->get_c2i_entry();
  }
  set_holder(new CompiledICHolder(callee_method, receiver_klass, entry, CompiledICState::_monomorphic));
}

// ----------------------------------------------------------------------------

void CompiledDirectCall::set_to_clean() {
  // in_use is unused but needed to match template function in CompiledMethod
  assert(CompiledICLocker::is_safe(instruction_address()), "mt unsafe call");
  // Reset call site
  _call->set_destination_mt_safe(resolve_call_stub());
}

void CompiledDirectCall::set(const methodHandle& callee_method) {
  CompiledMethod* code = callee_method->code();

  if (code != nullptr && code->is_in_use() && !code->is_unloading() &&
      !ContinuationEntry::is_interpreted_call(ncall->instruction_address())) {
    _call->set_destination_mt_safe(callee_method->verified_entry_point());
  } else {
    // Patch call site to C2I adapter if code is deoptimized or unloaded.
    // We also need to patch the static call stub to set the rmethod register
    // to the callee_method so the c2i adapter knows how to build the frame
    set_to_interpreted(callee, callee_method->get_c2i_entry());
  }
}

bool CompiledDirectCall::is_clean() const {
  return destination() == SharedRuntime::get_resolve_static_call_stub() ||
         destination() == SharedRuntime::get_resolve_opt_virtual_call_stub();
}

bool CompiledStaticCall::is_call_to_compiled() const {
  // TODO: This check is incorrect
  return CodeCache::contains(destination());
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
