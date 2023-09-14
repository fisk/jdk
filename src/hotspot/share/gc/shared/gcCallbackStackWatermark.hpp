/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_GCCALLBACKSTACKWATERMARK_HPP
#define SHARE_GC_SHARED_GCCALLBACKSTACKWATERMARK_HPP

#include "runtime/stackWatermark.hpp"
#include "utilities/globalDefinitions.hpp"

class frame;
class JavaThread;
class RegisterMap;

class GCCallbackStackWatermark : public StackWatermark {
  bool _record_process;
  void* _recorded_fp;
  uint32_t _recorded_epoch;
  bool _unwind_detected;

  virtual uint32_t epoch_id() const;
  virtual void start_processing_impl(void* context);
  virtual void process(const frame& fr, RegisterMap& register_map, void* context);
  virtual bool process_on_iteration() { return false; }

public:
  GCCallbackStackWatermark(JavaThread* jt);

  bool make_safe(JavaThread* jt, uint32_t epoch, bool trigger_deopt);
};

#endif // SHARE_GC_SHARED_GCCALLBACKSTACKWATERMARK_HPP
