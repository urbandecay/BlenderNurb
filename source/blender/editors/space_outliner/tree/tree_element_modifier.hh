/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#pragma once

#include "tree_element.hh"

namespace blender {

struct ModifierData;
struct Object;

struct ModifierDataStoreElem {
  ModifierData *md;

  ModifierDataStoreElem(ModifierData *md_) : md(md_) {}
};

namespace ed::outliner {

class TreeElementModifierBase final : public AbstractTreeElement {
  Object &object_;

 public:
  TreeElementModifierBase(TreeElement &legacy_te, Object &object);
  void expand(SpaceOutliner & /*soops*/) const override;
};

class TreeElementModifier final : public AbstractTreeElement {
  /* Not needed right now, avoid unused member variable warning. */
  Object &object_;
  ModifierDataStoreElem &md_;

 public:
  TreeElementModifier(TreeElement &legacy_te, Object &object, ModifierDataStoreElem &md);
  void expand(SpaceOutliner & /*soops*/) const override;
};

}  // namespace ed::outliner
}  // namespace blender
