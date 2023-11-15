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
#include "code/icBuffer.hpp"
#include "code/nmethod.hpp"
#include "code/vtableStubs.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/linkResolver.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/klass.inline.hpp"
#include "oops/method.inline.hpp"
#include "oops/oop.inline.hpp"
#include "oops/symbol.hpp"
#include "runtime/continuationEntry.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/icache.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "sanitizers/leak.hpp"
#include "utilities/events.hpp"


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

void CompiledIC::set_holder(CompiledICHolder* holder) {
  assert(entry_point != nullptr, "must set legal entry point");
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");
  assert (!is_optimized() || cache == nullptr, "an optimized virtual call does not have a cached metadata");

  if (holder != nullptr) {
    // TODO remove in cleanup
    holder()->release();
  }

  _call->set_data(_value, holder);
}

//-----------------------------------------------------------------------------
// High-level access to an inline cache. Guaranteed to be MT-safe.

void CompiledIC::initialize_from_iter(RelocIterator* iter) {
  assert(iter->addr() == _call->instruction_address(), "must find ic_call");

  if (iter->type() == relocInfo::virtual_call_type) {
    virtual_call_Relocation* r = iter->virtual_call_reloc();
    _is_optimized = false;
    _value = _call->get_load_instruction(r);
  } else {
    assert(iter->type() == relocInfo::opt_virtual_call_type, "must be a virtual call");
    _is_optimized = true;
    _value = nullptr;
  }
}

CompiledIC::CompiledIC(CompiledMethod* cm, NativeCall* call)
  : _method(cm)
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
    // LSan appears unable to follow malloc-based memory consistently when embedded as an immediate
    // in generated machine code. So we have to ignore it.
    LSAN_IGNORE_OBJECT(holder);
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

  if (TraceICs) {
    ResourceMark rm;
    assert(call_info->selected_method() != nullptr, "Unexpected null selected method");
    tty->print_cr ("IC@" INTPTR_FORMAT ": to megamorphic %s entry: " INTPTR_FORMAT,
                   p2i(instruction_address()), call_info->selected_method()->print_value_string(), p2i(entry));
  }

  set_holder(holder);

  // We can't check this anymore. With lazy deopt we could have already
  // cleaned this IC entry before we even return. This is possible if
  // we ran out of space in the inline cache buffer trying to do the
  // set_next and we safepointed to free up space. This is a benign
  // race because the IC entry was complete when we safepointed so
  // cleaning it immediately is harmless.
  // assert(is_megamorphic(), "sanity check");
  return true;
}


// true if destination is megamorphic stub
bool CompiledIC::is_megamorphic() const {
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");
  assert(!is_optimized(), "an optimized call cannot be megamorphic");

  // Cannot rely on cached_value. It is either an interface or a method.
  return holder()->is_megamorphic();
}

void CompiledIC::set_to_clean() {
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");
  if (TraceInlineCacheClearing || TraceICs) {
    tty->print_cr("IC@" INTPTR_FORMAT ": set to clean", p2i(instruction_address()));
    print();
  }

  address entry = SharedRuntime::get_resolve_virtual_call_stub();
  set_holder(new CompiledICHolder(nullptr, nullptr, entry, CompiledICState::_clean));
}

bool CompiledIC::is_clean() const {
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");
  return holder()->is_clean();
}

bool CompiledIC::set_to_monomorphic(CompiledICInfo& info) {
  assert(CompiledICLocker::is_safe(_method), "mt unsafe call");
  // Updating a cache to the wrong entry can cause bugs that are very hard
  // to track down - if cache entry gets invalid - we just clean it. In
  // this way it is always the same code path that is responsible for
  // updating and resolving an inline cache
  //
  // The above is no longer true. SharedRuntime::fixup_callers_callsite will change optimized
  // callsites. In addition ic_miss code will update a site to monomorphic if it determines
  // that an monomorphic call to the interpreter can now be monomorphic to compiled code.
  //
  // In both of these cases the only thing being modified is the jump/call target and these
  // transitions are mt_safe

  Thread *thread = Thread::current();
  if (info.to_interpreter()) {
    // Call to interpreter
    if (info.is_optimized() && is_optimized()) {
      assert(is_clean(), "unsafe IC path");
      // the call analysis (callee structure) specifies that the call is optimized
      // (either because of CHA or the static target is final)
      // At code generation time, this call has been emitted as static call
      // Call via stub
      assert(info.cached_metadata() != nullptr && info.cached_metadata()->is_method(), "sanity check");
      methodHandle method (thread, (Method*)info.cached_metadata());
      _call->set_to_interpreted(method, info);

      if (TraceICs) {
         ResourceMark rm(thread);
         tty->print_cr ("IC@" INTPTR_FORMAT ": monomorphic to interpreter: %s",
           p2i(instruction_address()),
           method->print_value_string());
      }
    } else {
      // Call via method-klass-holder
      CompiledICHolder* holder = info.claim_cached_icholder();
      if (!InlineCacheBuffer::create_transition_stub(this, holder, info.entry())) {
        delete holder;
        return false;
      }
      // LSan appears unable to follow malloc-based memory consistently when embedded as an
      // immediate in generated machine code. So we have to ignore it.
      LSAN_IGNORE_OBJECT(holder);
      if (TraceICs) {
         ResourceMark rm(thread);
         tty->print_cr ("IC@" INTPTR_FORMAT ": monomorphic to interpreter via icholder ", p2i(instruction_address()));
      }
    }
  } else {
    // Call to compiled code
    bool static_bound = info.is_optimized() || (info.cached_metadata() == nullptr);
#ifdef ASSERT
    CodeBlob* cb = CodeCache::find_blob(info.entry());
    assert (cb != nullptr && cb->is_compiled(), "must be compiled!");
#endif /* ASSERT */

    // This is MT safe if we come from a clean-cache and go through a
    // non-verified entry point
    bool safe = SafepointSynchronize::is_at_safepoint() ||
                (!is_in_transition_state() && (info.is_optimized() || static_bound || is_clean()));

    if (!safe) {
      if (!InlineCacheBuffer::create_transition_stub(this, info.cached_metadata(), info.entry())) {
        return false;
      }
    } else {
      if (is_optimized()) {
        set_ic_destination(info.entry());
      } else {
        set_ic_destination_and_value(info.entry(), info.cached_metadata());
      }
    }

    if (TraceICs) {
      ResourceMark rm(thread);
      assert(info.cached_metadata() == nullptr || info.cached_metadata()->is_klass(), "must be");
      tty->print_cr ("IC@" INTPTR_FORMAT ": monomorphic to compiled (rcvr klass = %s) %s",
        p2i(instruction_address()),
        (info.cached_metadata() != nullptr) ? ((Klass*)info.cached_metadata())->print_value_string() : "nullptr",
        (safe) ? "" : " via stub");
    }
  }
  // We can't check this anymore. With lazy deopt we could have already
  // cleaned this IC entry before we even return. This is possible if
  // we ran out of space in the inline cache buffer trying to do the
  // set_next and we safepointed to free up space. This is a benign
  // race because the IC entry was complete when we safepointed so
  // cleaning it immediately is harmless.
  // assert(is_call_to_compiled() || is_call_to_interpreted(), "sanity check");
  return true;
}


// is_optimized: Compiler has generated an optimized call (i.e. fixed, no inline cache)
// static_bound: The call can be static bound. If it isn't also optimized, the property
// wasn't provable at time of compilation. An optimized call will have any necessary
// null check, while a static_bound won't. A static_bound (but not optimized) must
// therefore use the unverified entry point.
void CompiledIC::compute_monomorphic_entry(const methodHandle& method,
                                           Klass* receiver_klass,
                                           bool is_optimized,
                                           bool static_bound,
                                           bool caller_is_nmethod,
                                           CompiledICInfo& info,
                                           TRAPS) {
  CompiledMethod* method_code = method->code();

  address entry = nullptr;
  if (method_code != nullptr && method_code->is_in_use() && !method_code->is_unloading()) {
    assert(method_code->is_compiled(), "must be compiled");
    // Call to compiled code
    //
    // Note: the following problem exists with Compiler1:
    //   - at compile time we may or may not know if the destination is final
    //   - if we know that the destination is final (is_optimized), we will emit
    //     an optimized virtual call (no inline cache), and need a Method* to make
    //     a call to the interpreter
    //   - if we don't know if the destination is final, we emit a standard
    //     virtual call, and use CompiledICHolder to call interpreted code
    //     (no static call stub has been generated)
    //   - In the case that we here notice the call is static bound we
    //     convert the call into what looks to be an optimized virtual call,
    //     but we must use the unverified entry point (since there will be no
    //     null check on a call when the target isn't loaded).
    //     This causes problems when verifying the IC because
    //     it looks vanilla but is optimized. Code in is_call_to_interpreted
    //     is aware of this and weakens its asserts.
    if (is_optimized) {
      entry      = method_code->verified_entry_point();
    } else {
      entry      = method_code->entry_point();
    }
  }
  if (entry != nullptr) {
    // Call to near compiled code.
    info.set_compiled_entry(entry, is_optimized ? nullptr : receiver_klass, is_optimized);
  } else {
    if (is_optimized) {
      // Use stub entry
      info.set_interpreter_entry(method()->get_c2i_entry(), method());
    } else {
      // Use icholder entry
      assert(method_code == nullptr || method_code->is_compiled(), "must be compiled");
      CompiledICHolder* holder = new CompiledICHolder(method(), receiver_klass);
      info.set_icholder_entry(method()->get_c2i_unverified_entry(), holder);
    }
  }
  assert(info.is_optimized() == is_optimized, "must agree");
}

// ----------------------------------------------------------------------------

bool CompiledStaticCall::set_to_clean(bool in_use) {
  // in_use is unused but needed to match template function in CompiledMethod
  assert(CompiledICLocker::is_safe(instruction_address()), "mt unsafe call");
  // Reset call site
  set_destination_mt_safe(resolve_call_stub());

  // Do not reset stub here:  It is too expensive to call find_stub.
  // Instead, rely on caller (nmethod::clear_inline_caches) to clear
  // both the call and its stub.
  return true;
}

bool CompiledStaticCall::is_clean() const {
  return destination() == resolve_call_stub();
}

bool CompiledStaticCall::is_call_to_compiled() const {
  return CodeCache::contains(destination());
}

bool CompiledDirectStaticCall::is_call_to_interpreted() const {
  // It is a call to interpreted, if it calls to a stub. Hence, the destination
  // must be in the stub part of the nmethod that contains the call
  CompiledMethod* cm = CodeCache::find_compiled(instruction_address());
  return cm->stub_contains(destination());
}

void CompiledStaticCall::set_to_compiled(address entry) {
  if (TraceICs) {
    ResourceMark rm;
    tty->print_cr("%s@" INTPTR_FORMAT ": set_to_compiled " INTPTR_FORMAT,
        name(),
        p2i(instruction_address()),
        p2i(entry));
  }
  // Call to compiled code
  assert(CodeCache::contains(entry), "wrong entry point");
  set_destination_mt_safe(entry);
}

void CompiledStaticCall::set(const StaticCallInfo& info) {
  assert(CompiledICLocker::is_safe(instruction_address()), "mt unsafe call");
  // Updating a cache to the wrong entry can cause bugs that are very hard
  // to track down - if cache entry gets invalid - we just clean it. In
  // this way it is always the same code path that is responsible for
  // updating and resolving an inline cache
  assert(is_clean(), "do not update a call entry - use clean");

  if (info._to_interpreter) {
    // Call to interpreted code
    set_to_interpreted(info.callee(), info.entry());
  } else {
    set_to_compiled(info.entry());
  }
}

// Compute settings for a CompiledStaticCall. Since we might have to set
// the stub when calling to the interpreter, we need to return arguments.
void CompiledStaticCall::compute_entry(const methodHandle& m, bool caller_is_nmethod, StaticCallInfo& info) {
  CompiledMethod* m_code = m->code();
  info._callee = m;
  if (m_code != nullptr && m_code->is_in_use() && !m_code->is_unloading()) {
    info._to_interpreter = false;
    info._entry  = m_code->verified_entry_point();
  } else {
    // Callee is interpreted code.  In any case entering the interpreter
    // puts a converter-frame on the stack to save arguments.
    assert(!m->is_method_handle_intrinsic(), "Compiled code should never call interpreter MH intrinsics");
    info._to_interpreter = true;
    info._entry      = m()->get_c2i_entry();
  }
}

address CompiledDirectStaticCall::find_stub_for(address instruction) {
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
        case relocInfo::poll_type:
        case relocInfo::poll_return_type: // A safepoint can't overlap a call.
        default:
          ShouldNotReachHere();
      }
    }
  }
  return nullptr;
}

address CompiledDirectStaticCall::find_stub() {
  return CompiledDirectStaticCall::find_stub_for(instruction_address());
}

address CompiledDirectStaticCall::resolve_call_stub() const {
  return SharedRuntime::get_resolve_static_call_stub();
}

//-----------------------------------------------------------------------------
// Non-product mode code
#ifndef PRODUCT

void CompiledIC::verify() {
  _call->verify();
  assert(is_clean() || is_call_to_compiled() || is_call_to_interpreted()
          || is_optimized() || is_megamorphic(), "sanity check");
}

void CompiledIC::print() {
  print_compiled_ic();
  tty->cr();
}

void CompiledIC::print_compiled_ic() {
  tty->print("Inline cache at " INTPTR_FORMAT ", calling %s " INTPTR_FORMAT " cached_value " INTPTR_FORMAT,
             p2i(instruction_address()), is_call_to_interpreted() ? "interpreted " : "", p2i(ic_destination()), p2i(is_optimized() ? nullptr : cached_value()));
}

void CompiledDirectStaticCall::print() {
  tty->print("static call at " INTPTR_FORMAT " -> ", p2i(instruction_address()));
  if (is_clean()) {
    tty->print("clean");
  } else if (is_call_to_compiled()) {
    tty->print("compiled");
  } else if (is_call_to_interpreted()) {
    tty->print("interpreted");
  }
  tty->cr();
}

void CompiledDirectStaticCall::verify_mt_safe(const methodHandle& callee, address entry,
                                              NativeMovConstReg* method_holder,
                                              NativeJump*        jump) {
  // A generated lambda form might be deleted from the Lambdaform
  // cache in MethodTypeForm.  If a jit compiled lambdaform method
  // becomes not entrant and the cache access returns null, the new
  // resolve will lead to a new generated LambdaForm.
  Method* old_method = reinterpret_cast<Method*>(method_holder->data());
  assert(old_method == nullptr || old_method == callee() ||
         callee->is_compiled_lambda_form() ||
         !old_method->method_holder()->is_loader_alive() ||
         old_method->is_old(),  // may be race patching deoptimized nmethod due to redefinition.
         "a) MT-unsafe modification of inline cache");

  address destination = jump->jump_destination();
  assert(destination == (address)-1 || destination == entry
         || old_method == nullptr || !old_method->method_holder()->is_loader_alive() // may have a race due to class unloading.
         || old_method->is_old(),  // may be race patching deoptimized nmethod due to redefinition.
         "b) MT-unsafe modification of inline cache");
}
#endif // !PRODUCT
