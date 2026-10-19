/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "DNA_collection_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_layer.hh"
#include "BKE_library.hh"

#include "BLI_listbase.h"
#include "BLI_listbase_wrapper.hh"

#include "../outliner_intern.hh"
#include "common.hh"
#include "tree_display.hh"

namespace blender::ed::outliner {

/* -------------------------------------------------------------------- */
/** \name Tree-Display for a View Layer.
 * \{ */

TreeDisplayViewLayer::TreeDisplayViewLayer(SpaceOutliner &space_outliner)
    : AbstractTreeDisplay(space_outliner)
{
}

bool TreeDisplayViewLayer::supports_mode_column() const
{
  return true;
}

ListBaseT<TreeElement> TreeDisplayViewLayer::build_tree(const TreeSourceData &source_data)
{
  ListBaseT<TreeElement> tree = {nullptr};
  Scene *scene = source_data.scene;
  scene_ = scene;
  bmain_ = source_data.bmain;
  show_objects_ = !(space_outliner_.filter & SO_FILTER_NO_OBJECT);

  for (auto *view_layer : ListBaseWrapper<ViewLayer>(scene->view_layers)) {
    view_layer_ = view_layer;

    if (space_outliner_.filter & SO_FILTER_NO_VIEW_LAYERS) {
      if (view_layer != source_data.view_layer) {
        continue;
      }

      add_view_layer(*scene, tree, static_cast<TreeElement *>(nullptr));
    }
    else {
      TreeElement &te_view_layer = *add_element(
          &tree, reinterpret_cast<ID *>(scene), &view_layer, nullptr, TSE_R_LAYER, 0);

      TreeStoreElem *tselem = TREESTORE(&te_view_layer);

      if (!tselem->used) {
        tselem->flag &= ~TSE_CLOSED;
      }

      te_view_layer.name = view_layer->name;
      te_view_layer.directdata = view_layer;

      add_view_layer(*scene, te_view_layer.subtree, &te_view_layer);
    }
  }

  return tree;
}

void TreeDisplayViewLayer::add_view_layer(Scene &scene,
                                          ListBaseT<TreeElement> &tree,
                                          TreeElement *parent)
{
  if (space_outliner_.filter & SO_FILTER_NO_COLLECTION) {
    /* Show objects in the view layer. */
    BKE_view_layer_synced_ensure(*bmain_, &scene, view_layer_);
    for (Base &base : *BKE_view_layer_object_bases_get(view_layer_)) {
      TreeElement *te_object = add_element(
          &tree, reinterpret_cast<ID *>(base.object), nullptr, parent, TSE_SOME_ID, 0);
      te_object->directdata = &base;
    }

  }
  else {
    /* Show collections in the view layer. */
    TreeElement &ten = *AbstractTreeDisplay::add_element(
        &space_outliner_, &tree, &scene.id, nullptr, parent, TSE_VIEW_COLLECTION_BASE, 0);
    TREESTORE(&ten)->flag &= ~TSE_CLOSED;

    /* First layer collection is for master collection, don't show it. */
    LayerCollection *lc = static_cast<LayerCollection *>(view_layer_->layer_collections.first);
    if (lc == nullptr) {
      return;
    }

    add_layer_collections_recursive(ten.subtree, lc->layer_collections, ten);
    if (show_objects_) {
      add_layer_collection_objects(ten.subtree, *lc, ten);
    }
  }
}

void TreeDisplayViewLayer::add_layer_collections_recursive(
    ListBaseT<TreeElement> &tree,
    ListBaseT<LayerCollection> &layer_collections,
    TreeElement &parent_ten)
{
  for (LayerCollection &lc : layer_collections) {
    const bool exclude = (lc.flag & LAYER_COLLECTION_EXCLUDE) != 0;
    TreeElement *ten;

    if (exclude && ((space_outliner_.show_restrict_flags & SO_RESTRICT_ENABLE) == 0)) {
      ten = &parent_ten;
    }
    else {
      ID *id = &lc.collection->id;
      ten = add_element(&tree, id, &lc, &parent_ten, TSE_LAYER_COLLECTION, 0);

      /* Open by default, except linked collections, which may contain many elements. */
      TreeStoreElem *tselem = TREESTORE(ten);
      if (!(tselem->used || !ID_IS_EDITABLE(id) || ID_IS_OVERRIDE_LIBRARY(id))) {
        tselem->flag &= ~TSE_CLOSED;
      }
    }

    add_layer_collections_recursive(ten->subtree, lc.layer_collections, *ten);
    if (!exclude && show_objects_) {
      add_layer_collection_objects(ten->subtree, lc, *ten);
    }
  }
}

void TreeDisplayViewLayer::add_layer_collection_objects(ListBaseT<TreeElement> &tree,
                                                        LayerCollection &lc,
                                                        TreeElement &ten)
{
  BKE_view_layer_synced_ensure(*bmain_, scene_, view_layer_);
  for (CollectionObject &cob : lc.collection->gobject) {
    Base *base = BKE_view_layer_base_find(view_layer_, cob.ob);
    TreeElement *te_object = add_element(
        &tree, reinterpret_cast<ID *>(base->object), nullptr, &ten, TSE_SOME_ID, 0);
    te_object->directdata = base;
  }
}

/** \} */

}  // namespace blender::ed::outliner
