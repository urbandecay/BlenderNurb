/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include "DNA_nurbs_topology_types.h"
#include "DNA_nurbs_types.h"

#include "RNA_define.hh"

#include "rna_internal.hh"

#ifdef RNA_RUNTIME

#  include <cstring>
#  include <string>

namespace blender {

static void rna_NurbsCurve_component_id_get(PointerRNA *ptr, char *value)
{
  const std::string id = std::to_string(static_cast<NurbsCurve *>(ptr->data)->id);
  std::memcpy(value, id.data(), id.size());
  value[id.size()] = '\0';
}

static int rna_NurbsCurve_component_id_length(PointerRNA *ptr)
{
  return int(std::to_string(static_cast<NurbsCurve *>(ptr->data)->id).size());
}

static void rna_NurbsControlVertex_component_id_get(PointerRNA *ptr, char *value)
{
  const std::string id = std::to_string(static_cast<NurbsControlVertex *>(ptr->data)->id);
  std::memcpy(value, id.data(), id.size());
  value[id.size()] = '\0';
}

static int rna_NurbsControlVertex_component_id_length(PointerRNA *ptr)
{
  return int(std::to_string(static_cast<NurbsControlVertex *>(ptr->data)->id).size());
}

static void rna_nurbs_id_get(const uint64_t id, char *value)
{
  const std::string text = std::to_string(id);
  std::memcpy(value, text.data(), text.size());
  value[text.size()] = '\0';
}

#  define NURBS_ID_ACCESSORS(type, name, member) \
    static void name##_get(PointerRNA *ptr, char *value) \
    { \
      rna_nurbs_id_get(static_cast<type *>(ptr->data)->member, value); \
    } \
    static int name##_length(PointerRNA *ptr) \
    { \
      return int(std::to_string(static_cast<type *>(ptr->data)->member).size()); \
    }

NURBS_ID_ACCESSORS(NurbsTopoVertex, rna_NurbsTopoVertex_component_id, id)
NURBS_ID_ACCESSORS(NurbsTopoEdge, rna_NurbsTopoEdge_component_id, id)
NURBS_ID_ACCESSORS(NurbsTopoEdge, rna_NurbsTopoEdge_support_curve_id, curve_id)
NURBS_ID_ACCESSORS(NurbsTopoFace, rna_NurbsTopoFace_component_id, id)
NURBS_ID_ACCESSORS(NurbsTopoFace, rna_NurbsTopoFace_surface_id, surface_id)
NURBS_ID_ACCESSORS(NurbsSelection, rna_NurbsSelection_component_id, component_id)

static void rna_NurbsGeometry_active_component_id_get(PointerRNA *ptr, char *value)
{
  const NurbsGeometry *geometry = static_cast<NurbsGeometry *>(ptr->data);
  const uint64_t id = geometry->topology ? geometry->topology->active_component_id :
                                           geometry->active_control_vertex_id;
  const std::string text = std::to_string(id);
  std::memcpy(value, text.data(), text.size());
  value[text.size()] = '\0';
}

static int rna_NurbsGeometry_active_component_id_length(PointerRNA *ptr)
{
  const NurbsGeometry *geometry = static_cast<NurbsGeometry *>(ptr->data);
  return int(std::to_string(geometry->topology ? geometry->topology->active_component_id :
                                                 geometry->active_control_vertex_id)
                 .size());
}

static bool rna_NurbsGeometry_has_topology_get(PointerRNA *ptr)
{
  return static_cast<NurbsGeometry *>(ptr->data)->topology != nullptr;
}

#  define NURBS_TOPOLOGY_ITERATOR(name, member, type) \
    static void name##_begin(CollectionPropertyIterator *iter, PointerRNA *ptr) \
    { \
      NurbsGeometry *geometry = static_cast<NurbsGeometry *>(ptr->data); \
      if (geometry->topology) { \
        rna_iterator_listbase_begin(iter, ptr, &geometry->topology->member, nullptr); \
      } \
      else { \
        iter->valid = false; \
      } \
    }

NURBS_TOPOLOGY_ITERATOR(rna_NurbsGeometry_vertices, vertices, NurbsTopoVertex)
NURBS_TOPOLOGY_ITERATOR(rna_NurbsGeometry_edges, edges, NurbsTopoEdge)
NURBS_TOPOLOGY_ITERATOR(rna_NurbsGeometry_faces, faces, NurbsTopoFace)
NURBS_TOPOLOGY_ITERATOR(rna_NurbsGeometry_selection_history, selection_history, NurbsSelection)

static void rna_NurbsTopologyVertex_position_get(PointerRNA *ptr, float *values)
{
  const NurbsTopoVertex *vertex = static_cast<NurbsTopoVertex *>(ptr->data);
  for (int i = 0; i < 3; i++) {
    values[i] = float(vertex->position[i]);
  }
}

static void rna_NurbsTopologyEdge_domain_get(PointerRNA *ptr, float *values)
{
  const NurbsTopoEdge *edge = static_cast<NurbsTopoEdge *>(ptr->data);
  values[0] = float(edge->domain[0]);
  values[1] = float(edge->domain[1]);
}

static int rna_NurbsCurve_knots_get_length(const PointerRNA *ptr,
                                           int length[RNA_MAX_ARRAY_DIMENSION])
{
  length[0] = static_cast<NurbsCurve *>(ptr->data)->knot_count;
  return length[0];
}

static void rna_NurbsCurve_knots_get(PointerRNA *ptr, float *values)
{
  const NurbsCurve *curve = static_cast<NurbsCurve *>(ptr->data);
  for (int i = 0; i < curve->knot_count; i++) {
    values[i] = float(curve->knots[i]);
  }
}

static void rna_NurbsCurve_control_vertices_begin(CollectionPropertyIterator *iter,
                                                  PointerRNA *ptr)
{
  NurbsCurve *curve = static_cast<NurbsCurve *>(ptr->data);
  rna_iterator_array_begin(iter,
                           ptr,
                           curve->control_vertices,
                           sizeof(NurbsControlVertex),
                           curve->control_vertex_count,
                           0,
                           nullptr);
}

}  // namespace blender

#else

namespace blender {

static void rna_def_nurbs_control_vertex(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "NurbsControlVertex", nullptr);
  RNA_def_struct_ui_text(srna, "NURBS Control Vertex", "Exact curve control vertex");

  PropertyRNA *prop = RNA_def_property(srna, "component_id", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_NurbsControlVertex_component_id_get",
                                "rna_NurbsControlVertex_component_id_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Component ID", "Stable unsigned 64-bit control vertex ID");

  prop = RNA_def_property(srna, "position", PROP_FLOAT, PROP_TRANSLATION);
  RNA_def_property_float_sdna(prop, nullptr, "position");
  RNA_def_property_array(prop, 3);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Position", "Euclidean position in object-local coordinates");

  prop = RNA_def_property(srna, "weight", PROP_FLOAT, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Weight", "Positive rational control vertex weight");

  prop = RNA_def_property(srna, "select", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", NURBS_CV_SELECT);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Selected", "Control vertex is selected");
}

static void rna_def_nurbs_curve(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "NurbsCurve", nullptr);
  RNA_def_struct_ui_text(srna, "NURBS Curve", "Exact rational B-spline curve");

  PropertyRNA *prop = RNA_def_property(srna, "component_id", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(
      prop, "rna_NurbsCurve_component_id_get", "rna_NurbsCurve_component_id_length", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Component ID", "Stable unsigned 64-bit curve ID");

  prop = RNA_def_property(srna, "degree", PROP_INT, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Degree", "Polynomial degree of the curve");

  prop = RNA_def_property(srna, "order", PROP_INT, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Order", "Curve degree plus one");

  prop = RNA_def_property(srna, "dimension", PROP_INT, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Dimension", "Dimension of the stored Euclidean control vertices");

  prop = RNA_def_property(srna, "is_rational", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "rational", 0);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Rational", "Curve uses rational control vertex weights");

  prop = RNA_def_property(srna, "domain", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 2);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Domain", "Stored curve parameter interval");

  prop = RNA_def_property(srna, "knots", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 0);
  RNA_def_property_flag(prop, PROP_DYNAMIC);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_dynamic_array_funcs(prop, "rna_NurbsCurve_knots_get_length");
  RNA_def_property_float_funcs(prop, "rna_NurbsCurve_knots_get", nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Knots", "Reduced Rhino/openNURBS knot vector");

  prop = RNA_def_property(srna, "control_vertices", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "control_vertices", "control_vertex_count");
  RNA_def_property_collection_funcs(prop,
                                    "rna_NurbsCurve_control_vertices_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_struct_type(prop, "NurbsControlVertex");
  RNA_def_property_ui_text(
      prop, "Control Vertices", "Authoritative Euclidean curve control vertices");
}

static void rna_def_nurbs_topology(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "NurbsTopologyVertex", nullptr);
  RNA_def_struct_sdna(srna, "NurbsTopoVertex");
  RNA_def_struct_ui_text(srna, "NURBS Topology Vertex", "Persistent topological vertex");
  PropertyRNA *prop = RNA_def_property(srna, "component_id", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_NurbsTopoVertex_component_id_get",
                                "rna_NurbsTopoVertex_component_id_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  prop = RNA_def_property(srna, "position", PROP_FLOAT, PROP_TRANSLATION);
  RNA_def_property_array(prop, 3);
  RNA_def_property_float_funcs(prop, "rna_NurbsTopologyVertex_position_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  prop = RNA_def_property(srna, "select", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", NURBS_TOPO_SELECT);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

  srna = RNA_def_struct(brna, "NurbsTopologyEdge", nullptr);
  RNA_def_struct_sdna(srna, "NurbsTopoEdge");
  RNA_def_struct_ui_text(srna, "NURBS Topology Edge", "Persistent topological edge");
  prop = RNA_def_property(srna, "component_id", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_NurbsTopoEdge_component_id_get",
                                "rna_NurbsTopoEdge_component_id_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  prop = RNA_def_property(srna, "support_curve_id", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_NurbsTopoEdge_support_curve_id_get",
                                "rna_NurbsTopoEdge_support_curve_id_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  prop = RNA_def_property(srna, "domain", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 2);
  RNA_def_property_float_funcs(prop, "rna_NurbsTopologyEdge_domain_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  prop = RNA_def_property(srna, "select", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", NURBS_TOPO_SELECT);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

  srna = RNA_def_struct(brna, "NurbsTopologyFace", nullptr);
  RNA_def_struct_sdna(srna, "NurbsTopoFace");
  RNA_def_struct_ui_text(srna, "NURBS Topology Face", "Persistent trimmed NURBS face");
  prop = RNA_def_property(srna, "component_id", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_NurbsTopoFace_component_id_get",
                                "rna_NurbsTopoFace_component_id_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  prop = RNA_def_property(srna, "surface_id", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(
      prop, "rna_NurbsTopoFace_surface_id_get", "rna_NurbsTopoFace_surface_id_length", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  prop = RNA_def_property(srna, "loop_count", PROP_INT, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  prop = RNA_def_property(srna, "select", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", NURBS_TOPO_SELECT);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

  srna = RNA_def_struct(brna, "NurbsSelectionHistoryItem", nullptr);
  RNA_def_struct_sdna(srna, "NurbsSelection");
  RNA_def_struct_ui_text(
      srna, "NURBS Selection History Item", "Persistent topology selection history entry");
  prop = RNA_def_property(srna, "component_id", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_NurbsSelection_component_id_get",
                                "rna_NurbsSelection_component_id_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  static const EnumPropertyItem selection_type_items[] = {
      {NURBS_TOPO_SELECT_VERTEX, "VERTEX", 0, "Vertex", "Topology vertex"},
      {NURBS_TOPO_SELECT_EDGE, "EDGE", 0, "Edge", "Topology edge"},
      {NURBS_TOPO_SELECT_FACE, "FACE", 0, "Face", "Topology face"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  RNA_def_property_enum_items(prop, selection_type_items);
  RNA_def_property_enum_sdna(prop, nullptr, "type");
  RNA_def_property_enum_default(prop, NURBS_TOPO_SELECT_VERTEX);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
}

void RNA_def_nurbs(BlenderRNA *brna)
{
  rna_def_nurbs_control_vertex(brna);
  rna_def_nurbs_curve(brna);
  rna_def_nurbs_topology(brna);

  StructRNA *srna = RNA_def_struct(brna, "NurbsGeometry", "ID");
  RNA_def_struct_ui_text(srna, "NURBS Geometry", "Exact NURBS geometry data-block");
  RNA_def_struct_ui_icon(srna, ICON_CURVE_DATA);

  PropertyRNA *prop = RNA_def_property(srna, "curves", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "curves", nullptr);
  RNA_def_property_struct_type(prop, "NurbsCurve");
  RNA_def_property_ui_text(prop, "Curves", "Exact curves owned by this geometry data-block");

  prop = RNA_def_property(srna, "has_topology", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_NurbsGeometry_has_topology_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  prop = RNA_def_property(srna, "active_component_id", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_NurbsGeometry_active_component_id_get",
                                "rna_NurbsGeometry_active_component_id_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

#  define DEF_TOPOLOGY_COLLECTION(identifier, label, description, type_name, begin_func) \
    prop = RNA_def_property(srna, identifier, PROP_COLLECTION, PROP_NONE); \
    RNA_def_property_collection_funcs(prop, \
                                      begin_func, \
                                      "rna_iterator_listbase_next", \
                                      "rna_iterator_listbase_end", \
                                      "rna_iterator_listbase_get", \
                                      nullptr, \
                                      nullptr, \
                                      nullptr, \
                                      nullptr); \
    RNA_def_property_struct_type(prop, type_name); \
    RNA_def_property_ui_text(prop, label, description)
  DEF_TOPOLOGY_COLLECTION("vertices",
                          "Vertices",
                          "Persistent topology vertices",
                          "NurbsTopologyVertex",
                          "rna_NurbsGeometry_vertices_begin");
  DEF_TOPOLOGY_COLLECTION("edges",
                          "Edges",
                          "Persistent topology edges",
                          "NurbsTopologyEdge",
                          "rna_NurbsGeometry_edges_begin");
  DEF_TOPOLOGY_COLLECTION("faces",
                          "Faces",
                          "Persistent topology faces",
                          "NurbsTopologyFace",
                          "rna_NurbsGeometry_faces_begin");
  DEF_TOPOLOGY_COLLECTION("selection_history",
                          "Selection History",
                          "Ordered topology selection history",
                          "NurbsSelectionHistoryItem",
                          "rna_NurbsGeometry_selection_history_begin");
#  undef DEF_TOPOLOGY_COLLECTION
}

}  // namespace blender

#endif
