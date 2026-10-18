/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"

#include "GEO_foreach_geometry.hh"
#include "GEO_subdivide_curves.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_subdivide_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Curve"_ustr)
      .supported_type(GeometryComponent::Type::Curve)
      .description("Curves to subdivide");
  b.add_output<decl::Geometry>("Curve"_ustr).propagate_all_geometry().align_with_previous();
  b.add_input<decl::Int>("Cuts"_ustr)
      .default_value(1)
      .min(0)
      .max(1000)
      .evaluated_geometry_field()
      .description("The number of control points to create on the segment following each point");
}

static Curves *subdivide_curves(const Curves &src_curves_id,
                                Field<int> &cuts_field,
                                const bke::AttributeFilter &attribute_filter)
{
  const bke::CurvesGeometry &src_curves = src_curves_id.geometry.wrap();

  const bke::CurvesFieldContext field_context{src_curves_id, AttrDomain::Point};
  fn::FieldEvaluator evaluator{field_context, src_curves.points_num()};
  evaluator.add(cuts_field);
  evaluator.evaluate();
  const VArray<int> cuts = evaluator.get_evaluated<int>(0);

  if (cuts.is_single() && cuts.get_internal_single() < 1) {
    return nullptr;
  }

  bke::CurvesGeometry dst_curves = geometry::subdivide_curves(
      src_curves, src_curves.curves_range(), cuts, attribute_filter);

  Curves *dst_curves_id = bke::curves_new_nomain(std::move(dst_curves));
  bke::curves_copy_parameters(src_curves_id, *dst_curves_id);
  return dst_curves_id;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curve"_ustr);
  Field<int> cuts_field = params.extract_input<Field<int>>("Cuts"_ustr);

  GeometryComponentEditData::remember_deformed_positions_if_necessary(geometry_set);
  const NodeAttributeFilter &attribute_filter = params.get_attribute_filter("Curve"_ustr);

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (geometry_set.has_curves()) {
      const Curves &src_curves_id = *geometry_set.get_curves();
      Curves *dst_curves_id = subdivide_curves(src_curves_id, cuts_field, attribute_filter);
      if (dst_curves_id) {
        geometry_set.replace_curves(dst_curves_id);
      }
    }
  });
  params.set_output("Curve"_ustr, geometry_set);
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSubdivideCurve"_ustr, GEO_NODE_SUBDIVIDE_CURVE);
  ntype.ui_name = "Subdivide Curve";
  ntype.ui_description = "Dividing each curve segment into a specified number of pieces";
  ntype.enum_name_legacy = "SUBDIVIDE_CURVE";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_curve_subdivide_cc
