/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 */

#include "intern/builder/deg_builder_rna.h"

#include <cstring>

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_key_types.h"
#include "DNA_object_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "intern/builder/deg_builder.h"
#include "intern/depsgraph.hh"
#include "intern/node/deg_node.hh"
#include "intern/node/deg_node_component.hh"
#include "intern/node/deg_node_id.hh"
#include "intern/node/deg_node_operation.hh"

namespace blender::deg {

/* ***************************** Node Identifier **************************** */

RNANodeIdentifier::RNANodeIdentifier()
    : id(nullptr),
      type(NodeType::UNDEFINED),
      component_name(""),
      operation_code(OperationCode::OPERATION),
      operation_name(),
      operation_name_tag(-1)
{
}

bool RNANodeIdentifier::is_valid() const
{
  return id != nullptr && type != NodeType::UNDEFINED;
}

/* ********************************** Query ********************************* */

RNANodeQuery::RNANodeQuery(Depsgraph *depsgraph, DepsgraphBuilder *builder)
    : depsgraph_(depsgraph), builder_(builder)
{
}

RNANodeQuery::~RNANodeQuery() = default;

Node *RNANodeQuery::find_node(const PointerRNA *ptr,
                              const PropertyRNA *prop,
                              RNAPointerSource source)
{
  const RNANodeIdentifier node_identifier = construct_node_identifier(ptr, prop, source);
  if (!node_identifier.is_valid()) {
    return nullptr;
  }
  IDNode *id_node = depsgraph_->find_id_node(node_identifier.id);
  if (id_node == nullptr) {
    return nullptr;
  }
  ComponentNode *comp_node = id_node->find_component(node_identifier.type,
                                                     node_identifier.component_name);
  if (comp_node == nullptr) {
    return nullptr;
  }
  if (node_identifier.operation_code == OperationCode::OPERATION) {
    return comp_node;
  }
  return comp_node->find_operation(node_identifier.operation_code,
                                   node_identifier.operation_name,
                                   node_identifier.operation_name_tag);
}

bool RNANodeQuery::contains(const char *prop_identifier, const char *rna_path_component)
{
  const char *substr = strstr(prop_identifier, rna_path_component);
  if (substr == nullptr) {
    return false;
  }

  /* If `substr != prop_identifier`, it means that the sub-string is found further in
   * `prop_identifier`, and that thus index -1 is a valid memory location. */
  const bool start_ok = substr == prop_identifier || substr[-1] == '.';
  if (!start_ok) {
    return false;
  }

  const size_t component_len = strlen(rna_path_component);
  const bool end_ok = ELEM(substr[component_len], '\0', '.', '[');
  return end_ok;
}

RNANodeIdentifier RNANodeQuery::construct_node_identifier(const PointerRNA *ptr,
                                                          const PropertyRNA *prop,
                                                          RNAPointerSource source)
{
  RNANodeIdentifier node_identifier;
  if (ptr->type == nullptr) {
    return node_identifier;
  }
  /* Set default values for returns. */
  node_identifier.id = ptr->owner_id;
  node_identifier.component_name = "";
  node_identifier.operation_code = OperationCode::OPERATION;
  node_identifier.operation_name = "";
  node_identifier.operation_name_tag = -1;
  /* Handling of commonly known scenarios. */
  if (rna_prop_affects_parameters_node(ptr, prop)) {
    node_identifier.type = NodeType::PARAMETERS;
    node_identifier.operation_code = OperationCode::ID_PROPERTY;
    node_identifier.operation_name = RNA_property_identifier(prop);
    return node_identifier;
  }
  const char *prop_identifier = prop != nullptr ?
                                    RNA_property_identifier(const_cast<PropertyRNA *>(prop)) :
                                    "";

  if (RNA_struct_is_a(ptr->type, RNA_Constraint)) {
    node_identifier.type = NodeType::TRANSFORM;
    node_identifier.operation_code = OperationCode::TRANSFORM_LOCAL;
    return node_identifier;
  }
  else if (RNA_struct_is_a(ptr->type, RNA_Modifier) &&
           (contains(prop_identifier, "show_viewport") ||
            contains(prop_identifier, "show_render")))
  {
    node_identifier.type = NodeType::GEOMETRY;
    node_identifier.operation_code = OperationCode::VISIBILITY;
    return node_identifier;
  }
  else if (RNA_struct_is_a(ptr->type, RNA_Mesh) || RNA_struct_is_a(ptr->type, RNA_Modifier) ||
           RNA_struct_is_a(ptr->type, RNA_Spline) || RNA_struct_is_a(ptr->type, RNA_TextBox) ||
           RNA_struct_is_a(ptr->type, RNA_LatticePoint) ||
           RNA_struct_is_a(ptr->type, RNA_MeshUVLoop) ||
           RNA_struct_is_a(ptr->type, RNA_MeshLoopColor) ||
           RNA_struct_is_a(ptr->type, RNA_VertexGroupElement) ||
           (prop &&
            RNA_property_flag(const_cast<PropertyRNA *>(prop)) & PROP_FORCE_GEOMETRY_EVAL) != 0)
  {
    /* When modifier is used as FROM operation this is likely referencing to
     * the property (for example, modifier's influence).
     * But when it's used as TO operation, this is geometry component. */
    switch (source) {
      case RNAPointerSource::ENTRY:
        node_identifier.type = NodeType::GEOMETRY;
        break;
      case RNAPointerSource::EXIT:
        node_identifier.type = NodeType::PARAMETERS;
        node_identifier.operation_code = OperationCode::PARAMETERS_EVAL;
        break;
    }
    return node_identifier;
  }
  else if (ptr->type == RNA_Object) {
    /* Transforms props? */
    if (prop != nullptr) {
      /* TODO(sergey): How to optimize this? */
      if (contains(prop_identifier, "location") || contains(prop_identifier, "matrix_basis") ||
          contains(prop_identifier, "matrix_channel") ||
          contains(prop_identifier, "matrix_inverse") ||
          contains(prop_identifier, "matrix_local") ||
          contains(prop_identifier, "matrix_world") ||
          contains(prop_identifier, "rotation_axis_angle") ||
          contains(prop_identifier, "rotation_euler") ||
          contains(prop_identifier, "rotation_mode") ||
          contains(prop_identifier, "rotation_quaternion") || contains(prop_identifier, "scale") ||
          contains(prop_identifier, "delta_location") ||
          contains(prop_identifier, "delta_rotation_euler") ||
          contains(prop_identifier, "delta_rotation_quaternion") ||
          contains(prop_identifier, "delta_scale"))
      {
        node_identifier.type = NodeType::TRANSFORM;
        return node_identifier;
      }
      if (contains(prop_identifier, "data")) {
        /* We access object.data, most likely a geometry. */
        node_identifier.type = NodeType::GEOMETRY;
        return node_identifier;
      }
      if (STR_ELEM(prop_identifier, "hide_viewport", "hide_render")) {
        node_identifier.type = NodeType::OBJECT_FROM_LAYER;
        return node_identifier;
      }
      if (STREQ(prop_identifier, "dimensions")) {
        node_identifier.type = NodeType::PARAMETERS;
        node_identifier.operation_code = OperationCode::DIMENSIONS;
        return node_identifier;
      }
    }
  }
  else if (ptr->type == RNA_ShapeKey) {
    KeyBlock *key_block = static_cast<KeyBlock *>(ptr->data);
    node_identifier.id = ptr->owner_id;
    node_identifier.type = NodeType::PARAMETERS;
    node_identifier.operation_code = OperationCode::PARAMETERS_EVAL;
    node_identifier.operation_name = key_block->name;
    return node_identifier;
  }
  else if (ptr->type == RNA_Key) {
    node_identifier.id = ptr->owner_id;
    node_identifier.type = NodeType::GEOMETRY;
    return node_identifier;
  }
  else if (RNA_struct_is_a(ptr->type, RNA_Strip) ||
           RNA_struct_is_a(ptr->type, RNA_StripModifier) ||
           ELEM(ptr->type, RNA_StripTransform, RNA_StripCrop, RNA_StripColorBalanceData))
  {
    /* Sequencer strip or related nested data. */
    node_identifier.type = NodeType::SEQUENCER;
    return node_identifier;
  }
  else if (RNA_struct_is_a(ptr->type, RNA_NodeSocket)) {
    node_identifier.type = NodeType::NTREE_OUTPUT;
    return node_identifier;
  }
  else if (RNA_struct_is_a(ptr->type, RNA_ShaderNode)) {
    node_identifier.type = NodeType::SHADING;
    return node_identifier;
  }
  else if (ELEM(ptr->type, RNA_Curve, RNA_TextCurve)) {
    node_identifier.id = ptr->owner_id;
    node_identifier.type = NodeType::GEOMETRY;
    return node_identifier;
  }
  else if (ELEM(ptr->type, RNA_BezierSplinePoint, RNA_SplinePoint)) {
    node_identifier.id = ptr->owner_id;
    node_identifier.type = NodeType::GEOMETRY;
    return node_identifier;
  }
  else if (RNA_struct_is_a(ptr->type, RNA_ImageUser)) {
    if (GS(node_identifier.id->name) == ID_NT) {
      node_identifier.type = NodeType::IMAGE_ANIMATION;
      node_identifier.operation_code = OperationCode::IMAGE_ANIMATION;
      return node_identifier;
    }
  }
  else if (ELEM(ptr->type, RNA_MeshVertex, RNA_MeshEdge, RNA_MeshLoop, RNA_MeshPolygon)) {
    node_identifier.type = NodeType::GEOMETRY;
    return node_identifier;
  }
  if (prop != nullptr) {
    /* All unknown data effectively falls under "parameter evaluation". */
    node_identifier.type = NodeType::PARAMETERS;
    node_identifier.operation_code = OperationCode::PARAMETERS_EVAL;
    node_identifier.operation_name = "";
    node_identifier.operation_name_tag = -1;
    return node_identifier;
  }
  return node_identifier;
}

bool rna_prop_affects_parameters_node(const PointerRNA *ptr, const PropertyRNA *prop)
{
  return prop != nullptr && RNA_property_is_idprop(prop) &&
         /* ID properties in the geometry nodes modifier don't affect that parameters node.
          * Instead they affect the modifier and therefore the geometry node directly. */
         !RNA_struct_is_a(ptr->type, RNA_NodesModifier);
}

}  // namespace blender::deg
