/*
 * Copyright (c) 1998, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_OOPS_COMPILEDICHOLDER_HPP
#define SHARE_OOPS_COMPILEDICHOLDER_HPP

#include "oops/oop.hpp"
#include "utilities/macros.hpp"
#include "oops/klass.hpp"
#include "oops/method.hpp"

// A CompiledICHolder* is a helper object for the inline cache implementation.
// It holds:
//   (1) (dest) when the inline cache is clean
//   (2) (method+klass+dest) when the inline cache is monomorphic
//   (3) (method+klass+dest) when converting from compiled to an interpreted call
//   (4) (klass+klass+dest) when calling itable stub from megamorphic compiled call
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
#ifdef ASSERT
  static volatile int _live_count; // allocated
  static volatile int _live_not_claimed_count; // allocated but not yet in use so not
                                               // reachable by iterating over nmethods
#endif

  Metadata* _holder_metadata;
  Klass*    _holder_klass;    // to avoid name conflict with oopDesc::_klass
  address   _destination;
  CompiledICHolder* _next;
  int _table_index;
  CompiledICState _state;

 public:
  // Constructor
  CompiledICHolder(Metadata* metadata, Klass* klass, address destination, int table_index, CompiledICState state);
  ~CompiledICHolder() NOT_DEBUG_RETURN;

#ifdef ASSERT
  static int live_count() { return _live_count; }
  static int live_not_claimed_count() { return _live_not_claimed_count; }
#endif

  // accessors
  Klass*    holder_klass()  const     { return _holder_klass; }
  Metadata* holder_metadata() const   { return _holder_metadata; }
  address   destination() const       { return _destination; }

  static ByteSize holder_metadata_offset() { return byte_offset_of(CompiledICHolder, _holder_metadata); }
  static ByteSize holder_klass_offset()    { return byte_offset_of(CompiledICHolder, _holder_klass); }
  static ByteSize destination_offset()     { return byte_offset_of(CompiledICHolder, _destination); }

  CompiledICHolder* next()     { return _next; }
  void set_next(CompiledICHolder* n) { _next = n; }

  void release();

  inline bool is_loader_alive();

  // Verify
  void verify_on(outputStream* st);

  // Printing
  void print_on(outputStream* st) const;
  void print_value_on(outputStream* st) const;

  const char* internal_name() const { return "{compiledICHolder}"; }

  void claim() NOT_DEBUG_RETURN;
};

#endif // SHARE_OOPS_COMPILEDICHOLDER_HPP
