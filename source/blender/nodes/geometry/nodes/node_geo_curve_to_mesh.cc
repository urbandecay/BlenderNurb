/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"

#include "BKE_curve_to_mesh.hh"

#include "GEO_foreach_geometry.hh"
#include "GEO_randomize.hh"

#include "DNA_mesh_types.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_to_mesh_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curve"_ustr)
      .supported_type(GeometryComponent::Type::Curve)
      .description("Curve to convert to a mesh using the given profile");
  b.add_input<decl::Geometry>("Profile Curve"_ustr)
      .only_realized_data()
      .supported_type(GeometryComponent::Type::Curve)
      .description("Curves that are swept along the main curve");
  b.add_input<decl::Float>("Scale"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .evaluated_geometry_field({0})
      .description("Scale of the profile at each point");
  b.add_input<decl::Bool>("Fill Caps"_ustr)
      .description(
          "If the profile spline is cyclic, fill the ends of the generated mesh with N-gons");
  b.add_output<decl::Geometry>("Mesh"_ustr).propagate_all_geometry();
}

static Mesh *curve_to_mesh(const bke::CurvesGeometry &curves,
                           const GeometrySet &profile_set,
                           const fn::FieldContext &context,
                           const Field<float> &scale_field,
                           const bool fill_caps,
                           const AttributeFilter &attribute_filter)
{
  Mesh *mesh;
  if (profile_set.has_curves()) {
    const Curves *profile_curves = profile_set.get_curves();

    FieldEvaluator evaluator{context, curves.points_num()};
    evaluator.add(scale_field);
    evaluator.evaluate();

    const VArray<float> profile_scales = evaluator.get_evaluated<float>(0);
    mesh = bke::curve_to_mesh_sweep(
        curves, profile_curves->geometry.wrap(), profile_scales, fill_caps, attribute_filter);
  }
  else {
    mesh = bke::curve_to_wire_mesh(curves, attribute_filter);
  }
  geometry::debug_randomize_mesh_order(mesh);
  return mesh;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet curve_set = params.extract_input<GeometrySet>("Curve"_ustr);
  GeometrySet profile_set = params.extract_input<GeometrySet>("Profile Curve"_ustr);
  const Field<float> scale_field = params.extract_input<Field<float>>("Scale"_ustr);
  const bool fill_caps = params.extract_input<bool>("Fill Caps"_ustr);

  bke::GeometryComponentEditData::remember_deformed_positions_if_necessary(curve_set);
  const AttributeFilter &attribute_filter = params.get_attribute_filter("Mesh"_ustr);

  geometry::foreach_real_geometry(curve_set, [&](GeometrySet &geometry_set) {
    if (geometry_set.has_curves()) {
      const Curves &curves = *geometry_set.get_curves();

      const bke::CurvesFieldContext context{curves, bke::AttrDomain::Point};
      Mesh *mesh = curve_to_mesh(
          curves.geometry.wrap(), profile_set, context, scale_field, fill_caps, attribute_filter);
      if (mesh != nullptr) {
        mesh->mat = MEM_dupalloc(curves.mat);
        mesh->totcol = curves.totcol;
      }
      geometry_set.replace_mesh(mesh);
    }
    geometry_set.keep_only({GeometryComponent::Type::Mesh,
                            GeometryComponent::Type::Instance,
                            GeometryComponent::Type::Edit});
  });

  params.set_output("Mesh"_ustr, std::move(curve_set));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCurveToMesh"_ustr, GEO_NODE_CURVE_TO_MESH);
  ntype.ui_name = "Curve to Mesh";
  ntype.ui_description =
      "Convert curves into a mesh, optionally with a custom profile shape defined by curves";
  ntype.enum_name_legacy = "CURVE_TO_MESH";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_curve_to_mesh_cc
