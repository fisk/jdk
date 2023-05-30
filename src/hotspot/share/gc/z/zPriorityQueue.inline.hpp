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

#ifndef SHARE_GC_Z_ZPRIORITYQUEUE_INLINE_HPP
#define SHARE_GC_Z_ZPRIORITYQUEUE_INLINE_HPP

#include "gc/z/zPriorityQueue.hpp"

template <typename T>
inline ZPriorityQueue<T>::ZPriorityQueue(IsHigherPriority is_higher_priority)
  : _array(),
    _is_higher_priority(is_higher_priority) {}

template <typename T>
inline T* ZPriorityQueue<T>::elem(int current_index) const {
  if (current_index >= _array.length() || current_index < 0) {
    // Indices outside the array have not been populated, and correspond to
    // null elements
    return nullptr;
  }
  return _array.at(current_index);
}

template <typename T>
inline int ZPriorityQueue<T>::parent(int current_index) const {
  if (current_index == 0) {
    // No parent for the root element
    return -1;
  }
  return (current_index - 1) / 2;
}

template <typename T>
inline int ZPriorityQueue<T>::left(int current_index) const {
  return current_index * 2 + 1;
}

template <typename T>
inline int ZPriorityQueue<T>::right(int current_index) const {
  return current_index * 2 + 2;
}

template <typename T>
inline void ZPriorityQueue<T>::swap(int n1_index, int n2_index) {
  T* n1_elem = elem(n1_index);
  T* n2_elem = elem(n2_index);
  _array.at_put(n1_index, n2_elem);
  _array.at_put(n2_index, n1_elem);
}

template <typename T>
inline void ZPriorityQueue<T>::move_down(int current_index) {
  for (;;) {
    T* const current_elem = elem(current_index);
    const int left_index = left(current_index);
    const int right_index = right(current_index);
    T* const left_elem = elem(left_index);
    T* const right_elem = elem(right_index);

    // Find highest_priority among current node and its immediate children
    int highest_priority_index = current_index;
    T* highest_priority_elem = current_elem;

    if (right_elem != nullptr && _is_higher_priority(right_elem, highest_priority_elem)) {
      highest_priority_index = right_index;
      highest_priority_elem = right_elem;
    }
    if (left_elem != nullptr && _is_higher_priority(left_elem, highest_priority_elem)) {
      highest_priority_index = left_index;
      highest_priority_elem = left_elem;
    }

    // If the highest priority is the current, we are good
    if (highest_priority_elem == current_elem) {
      return;
    }

    // Otherwise, rotate the tree and continue
    swap(current_index, highest_priority_index);

    current_index = highest_priority_index;
  }
}

template <typename T>
inline void ZPriorityQueue<T>::move_up(int current_index) {
  for (;;) {
    const int parent_index = parent(current_index);
    T* const current_elem = elem(current_index);
    T* const parent_elem = elem(parent_index);

    if (current_elem == nullptr ||
        parent_elem == nullptr ||
        !_is_higher_priority(current_elem, parent_elem)) {
      // Found a point where we can stop moving nodes up
      return;
    }

    // Move the node up in the tree
    swap(current_index, parent_index);
    current_index = parent_index;
  }
}

template <typename T>
void ZPriorityQueue<T>::insert(T* element) {
  int current_index = _array.length();
  _array.append(element);

  int parent_index = parent(current_index);
  T* parent_elem = elem(parent_index);
  move_up(current_index);
}

template <typename T>
T* ZPriorityQueue<T>::first() const {
  if (_array.length() == 0) {
    return nullptr;
  }

  return _array.at(0);
}

template <typename T>
T* ZPriorityQueue<T>::remove_first() {
  T* const first_element = first();
  if (first_element == nullptr) {
    // Empty list; bail
    return nullptr;
  }

  const int first_index = 0;
  const int last_index = _array.length() - 1;

  // Swap first and last elements
  swap(first_index, last_index);
  _array.pop();
  move_down(first_index);

  return first_element;
}

template <typename T>
int ZPriorityQueue<T>::length() const {
  return _array.length();
}

#endif // SHARE_GC_Z_ZPRIORITYQUEUE_INLINE_HPP
