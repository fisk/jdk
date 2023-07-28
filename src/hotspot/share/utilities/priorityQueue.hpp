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

#ifndef SHARE_UTILTIES_PRIORITYQUEUE_HPP
#define SHARE_UTILTIES_PRIORITYQUEUE_HPP

#include "memory/allocation.hpp"
#include "utilities/growableArray.hpp"

// This class implements a binary heap implemented with a growable array as backing storage
template <typename T, MEMFLAGS flags>
class PriorityQueue : public CHeapObj<flags> {
public:
  // Is a higher priority than b?
  typedef bool (*IsHigherPriority)(T* a, T* b);

private:
  GrowableArrayCHeap<T*, flags> _array;
  const IsHigherPriority _is_higher_priority;

  T* elem(int current_index) const;
  int parent(int current_index) const;
  int left(int current_index) const;
  int right(int current_index) const;

  void swap(int n1_index, int n2_index);

  void move_up(int current_index);
  void move_down(int current_index);

public:
  PriorityQueue(IsHigherPriority is_higher_priority);
  void insert(T* element);
  T* first() const;
  T* remove_first();
  int length() const;
  bool is_empty() const;
};

#endif // SHARE_UTILITIES_PRIORITYQUEUE_HPP
