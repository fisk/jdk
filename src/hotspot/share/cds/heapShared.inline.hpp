/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_CDS_HEAPSHARED_INLINE_HPP
#define SHARE_CDS_HEAPSHARED_INLINE_HPP

#include "cds/heapShared.hpp"

#include "classfile/javaClasses.hpp"
#include "utilities/macros.hpp"

#if INCLUDE_CDS_JAVA_HEAP

// Keep the knowledge about which objects have what metadata in one single place
template <typename T>
void HeapShared::do_metadata_offsets(oop src_obj, T callback) {
  if (java_lang_Class::is_instance(src_obj)) {
    callback(java_lang_Class::klass_offset());
    callback(java_lang_Class::array_klass_offset());
  } else if (java_lang_invoke_ResolvedMethodName::is_instance(src_obj)) {
    callback(java_lang_invoke_ResolvedMethodName::vmtarget_offset());
  }
}

inline void HeapShared::remap_loaded_metadata(oop src_obj) {
  do_metadata_offsets(src_obj, [&](int offset) {
    Metadata* metadata = src_obj->metadata_field(offset);
    if (metadata != nullptr) {
      metadata = (Metadata*)(address(metadata) + MetaspaceShared::relocation_delta());
      src_obj->metadata_field_put(offset, metadata);
    }
  });
}

#endif // INCLUDE_CDS_JAVA_HEAP

#endif // SHARE_CDS_HEAPSHARED_INLINE_HPP
