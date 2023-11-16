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

#ifndef SHARE_CODE_COMPILEDIC_HPP
#define SHARE_CODE_COMPILEDIC_HPP

#include "code/nativeInst.hpp"
#include "interpreter/linkResolver.hpp"
#include "oops/compiledICHolder.hpp"
#include "runtime/safepointVerifiers.hpp"

//-----------------------------------------------------------------------------
// The CompiledIC represents a compiled inline cache.
//
// In order to make patching of the inline cache MT-safe, we only allow the following
// transitions (when not at a safepoint):
//
//
//         [1] --<--  Clean -->---  [1]
//            /       (null)      \
//           /                     \      /-<-\
//          /          [2]          \    /     \
//      Interpreted  ---------> Monomorphic     | [3]
//  (CompiledICHolder*)            (Klass*)     |
//          \                        /   \     /
//       [4] \                      / [4] \->-/
//            \->-  Megamorphic -<-/
//              (CompiledICHolder*)
//
// The text in parentheses () refers to the value of the inline cache receiver (mov instruction)
//
// The numbers in square brackets refer to the kind of transition:
// [1]: Initial fixup. Receiver it found from debug information
// [2]: Compilation of a method
// [3]: Recompilation of a method (note: only entry is changed. The Klass* must stay the same)
// [4]: Inline cache miss. We go directly to megamorphic call.
//
// The class automatically inserts transition stubs (using the InlineCacheBuffer) when an MT-unsafe
// transition is made to a stub.
//
class CompiledIC;
class CompiledICProtectionBehaviour;
class CompiledMethod;

class CompiledICLocker: public StackObj {
  CompiledMethod* _method;
  CompiledICProtectionBehaviour* _behaviour;
  bool _locked;
  NoSafepointVerifier _nsv;

public:
  CompiledICLocker(CompiledMethod* method);
  ~CompiledICLocker();
  static bool is_safe(CompiledMethod* method);
  static bool is_safe(address code);
};

// A CompiledICHolder* is a helper object for the inline cache implementation.
// It holds:
//   (1) (dest) when the inline cache is clean
//   (2) (method+klass+dest) when the inline cache is monomorphic
//   (3) (klass+klass+dest) when calling itable stub from megamorphic compiled call
//   (4) (dest) when calling vtable stub from megamorphic compiled call
//

enum class CompiledICState {
  _clean,
  _monomorphic,
  _vtable,
  _itable
};

class CompiledICHolder : public CHeapObj<mtCompiler> {
  friend class VMStructs;
 private:
  static CompiledICHolder* volatile _unlink_list;
  static CompiledICHolder* _purge_list;

  Metadata* _holder_metadata;
  Klass*    _holder_klass;    // to avoid name conflict with oopDesc::_klass
  address   _destination;
  CompiledICHolder* _next;
  CompiledICState _state;

 public:
  // Constructor
  CompiledICHolder(Metadata* metadata, Klass* klass, address destination, CompiledICState state);

  // accessors
  Klass*    holder_klass()  const     { return _holder_klass; }
  Metadata* holder_metadata() const   { return _holder_metadata; }
  address   destination() const       { return _destination; }

  static ByteSize holder_metadata_offset() { return byte_offset_of(CompiledICHolder, _holder_metadata); }
  static ByteSize holder_klass_offset()    { return byte_offset_of(CompiledICHolder, _holder_klass); }
  static ByteSize destination_offset()     { return byte_offset_of(CompiledICHolder, _destination); }

  bool is_clean()       const { return _state == CompiledICState::_clean; }
  bool is_monomorphic() const { return _state == CompiledICState::_monomorphic; }
  bool is_megamorphic() const { return _state == CompiledICState::_itable || _state == CompiledICState::_vtable; }

  CompiledICHolder* next()     { return _next; }
  void set_next(CompiledICHolder* n) { _next = n; }

  void release();

  bool is_loader_alive();
};

class CompiledIC: public ResourceObj {
 private:
  NativeInstruction* _value;    // patchable value cell for this IC
  NativeCall* _call;
  CompiledMethod* _method;

  CompiledIC(CompiledMethod* cm, NativeCall* ic_call);
  CompiledIC(RelocIterator* iter);

  void initialize_from_iter(RelocIterator* iter);

  void set_holder(CompiledICHolder* value);

 public:
  // conversion (machine PC to CompiledIC*)
  friend CompiledIC* CompiledIC_before(CompiledMethod* nm, address return_addr);
  friend CompiledIC* CompiledIC_at(CompiledMethod* nm, address call_site);
  friend CompiledIC* CompiledIC_at(Relocation* call_site);
  friend CompiledIC* CompiledIC_at(RelocIterator* reloc_iter);

  CompiledICHolder* holder() const {return (CompiledICHolder*)_call->get_data(_value);}

  // State
  bool is_clean()       const { return holder()->is_clean(); }
  bool is_monomorphic() const { return holder()->is_monomorphic(); }
  bool is_megamorphic() const { return holder()->is_megamorphic(); }

  address end_of_call() const { return  _call->return_address(); }

  // MT-safe patching of inline caches. Note: Only safe to call is_xxx when holding the CompiledICLocker
  // so you are guaranteed that no patching takes place. The same goes for verify.
  void set_to_clean();
  void set_to_monomorphic(Method* callee_method, Klass* receiver_klass);

  // Returns true if successful and false otherwise. The call can fail if memory
  // allocation in the code cache fails, or ic stub refill is required.
  bool set_to_megamorphic(CallInfo* call_info, Bytecodes::Code bytecode, TRAPS);


  // Location
  address instruction_address() const { return _call->instruction_address(); }

  // Misc
  void print()             PRODUCT_RETURN;
  void print_compiled_ic() PRODUCT_RETURN;
  void verify()            PRODUCT_RETURN;
};

inline CompiledIC* CompiledIC_before(CompiledMethod* nm, address return_addr) {
  CompiledIC* c_ic = new CompiledIC(nm, nativeCall_before(return_addr));
  c_ic->verify();
  return c_ic;
}

inline CompiledIC* CompiledIC_at(CompiledMethod* nm, address call_site) {
  CompiledIC* c_ic = new CompiledIC(nm, nativeCall_at(call_site));
  c_ic->verify();
  return c_ic;
}

inline CompiledIC* CompiledIC_at(Relocation* call_site) {
  assert(call_site->type() == relocInfo::virtual_call_type ||
         call_site->type() == relocInfo::opt_virtual_call_type, "wrong reloc. info");
  CompiledIC* c_ic = new CompiledIC(call_site->code(), nativeCall_at(call_site->addr()));
  c_ic->verify();
  return c_ic;
}

inline CompiledIC* CompiledIC_at(RelocIterator* reloc_iter) {
  assert(reloc_iter->type() == relocInfo::virtual_call_type ||
      reloc_iter->type() == relocInfo::opt_virtual_call_type, "wrong reloc. info");
  CompiledIC* c_ic = new CompiledIC(reloc_iter);
  c_ic->verify();
  return c_ic;
}

//-----------------------------------------------------------------------------
// The CompiledDirectStaticCall represents a call to a static method in the compiled
//
// Transition diagram of a static call site is somewhat simpler than for an inlined cache:
//
//
//           -----<----- Clean ----->-----
//          /                             \
//         /                               \
//    compilled code <------------> interpreted code
//
//  Clean:            Calls directly to runtime method for fixup
//  Compiled code:    Calls directly to compiled code
//  Interpreted code: Calls to stub that set Method* reference
//
//

class CompiledDirectStaticCall : public ResourceObj {
private:
  friend class CompiledIC;
  friend class DirectNativeCallWrapper;

  // Also used by CompiledIC
  void set_to_interpreted(const methodHandle& callee, address entry);
  void verify_mt_safe(const methodHandle& callee, address entry,
                      NativeMovConstReg* method_holder,
                      NativeJump*        jump) PRODUCT_RETURN;
  address instruction_address() const { return _call->instruction_address(); }
  void set_destination_mt_safe(address dest) { _call->set_destination_mt_safe(dest); }

  NativeCall* _call;

  CompiledDirectStaticCall(NativeCall* call) : _call(call) {}

 public:
  // Returns null if CodeBuffer::expand fails
  static address emit_to_interp_stub(CodeBuffer &cbuf, address mark = nullptr);
  static int to_interp_stub_size();
  static int to_trampoline_stub_size();
  static int reloc_to_interp_stub();

  static inline CompiledDirectStaticCall* before(address return_addr) {
    CompiledDirectStaticCall* st = new CompiledDirectStaticCall(nativeCall_before(return_addr));
    st->verify();
    return st;
  }

  static inline CompiledDirectStaticCall* at(address native_call) {
    CompiledDirectStaticCall* st = new CompiledDirectStaticCall(nativeCall_at(native_call));
    st->verify();
    return st;
  }

  static inline CompiledDirectStaticCall* at(Relocation* call_site) {
    return at(call_site->addr());
  }

  // Delegation
  address destination() const { return _call->destination(); }
  address end_of_call() const { return _call->return_address(); }

  // Clean static call (will force resolving on next use)
  void set_to_clean();

  void set(Method* method);
  void set_to_interpreted(const methodHandle& callee, address entry);

  // State
  bool is_clean() const;

  // State
  bool is_call_to_interpreted() const;

  // Stub support
  static address find_stub_for(address instruction);
  address find_stub();
  static void set_stub_to_clean(static_stub_Relocation* static_stub);

  // Misc.
  void print()  PRODUCT_RETURN;
  void verify() PRODUCT_RETURN;

 protected:
  virtual address resolve_call_stub() const;
  virtual const char* name() const { return "CompiledDirectStaticCall"; }
};

#endif // SHARE_CODE_COMPILEDIC_HPP
