/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

#include "SOL_geometry.hh"

namespace blender::solid {

/* Temporary lookup over immutable model storage. Compact IDs use contiguous slots;
 * sparse or very large IDs retain bounded storage through a hash-table fallback. */
template<typename T> class IdIndex {
  Id first_ = 0;
  std::vector<const T *> slots_;
  std::unordered_map<Id, const T *> sparse_;

  const T *lookup(Id id) const
  {
    if (!slots_.empty()) {
      return id >= first_ && id - first_ < slots_.size() ? slots_[id - first_] : nullptr;
    }
    const auto it = sparse_.find(id);
    return it == sparse_.end() ? nullptr : it->second;
  }

 public:
  IdIndex() = default;
  explicit IdIndex(const std::vector<T> &items)
  {
    if (items.empty())
      return;
    const auto bounds = std::minmax_element(items.begin(), items.end(), [](const T &a, const T &b) {
      return a.id < b.id;
    });
    first_ = bounds.first->id;
    const Id span = bounds.second->id - first_;
    if (span / 8 < items.size()) {
      slots_.resize(size_t(span) + 1, nullptr);
      for (const T &item : items) {
        auto &slot = slots_[item.id - first_];
        if (!slot)
          slot = &item;
      }
    }
    else {
      sparse_.reserve(items.size());
      for (const T &item : items)
        sparse_.emplace(item.id, &item);
    }
  }
  bool contains(Id id) const
  {
    return lookup(id) != nullptr;
  }
  const T *at(Id id) const
  {
    const T *item = lookup(id);
    if (!item)
      throw std::out_of_range("Unknown analytic source ID");
    return item;
  }
};

}  // namespace blender::solid
