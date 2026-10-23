/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Isolated source-contract trial, not a Blender geometry adapter or operator. */

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include <BRepGraph.hxx>
#include <BRepGraph_Compact.hxx>
#include <BRepGraph_Copy.hxx>
#include <BRepGraph_EditorView.hxx>
#include <BRepGraph_ShapesView.hxx>
#include <BRepGraph_TopoView.hxx>
#include <BRepGraph_UIDsView.hxx>
#include <BRep_Builder.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>

static void require(const bool condition, const char *message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static occ::handle<Geom_BSplineCurve> make_arc(const std::array<double, 3> &weights,
                                               const bool check_rational = false)
{
  NCollection_Array1<gp_Pnt> poles(1, 3);
  poles(1) = gp_Pnt(1, 0, 0);
  poles(2) = gp_Pnt(1, 1, 0);
  poles(3) = gp_Pnt(0, 1, 0);
  NCollection_Array1<double> source_weights(1, 3), knots(1, 2);
  NCollection_Array1<int> multiplicities(1, 2);
  for (int i = 1; i <= 3; i++) {
    source_weights(i) = weights[i - 1];
  }
  knots(1) = 3;
  knots(2) = 9;
  multiplicities(1) = multiplicities(2) = 3;
  return new Geom_BSplineCurve(
      poles, source_weights, knots, multiplicities, 2, false, check_rational);
}

static void curve_contracts()
{
  const std::array<double, 3> weights{1, std::sqrt(0.5), 1};
  const auto arc = make_arc(weights);
  require(arc->FirstParameter() == 3 && arc->LastParameter() == 9, "Changed arc domain");
  require(arc->Degree() == 2 && arc->NbPoles() == 3, "Changed arc representation");
  for (int i = 1; i <= 3; i++) {
    require(arc->Weight(i) == weights[i - 1], "Changed rational coefficient");
  }
  double maximum_radius_error = 0;
  for (int i = 0; i <= 128; i++) {
    const gp_Pnt p = arc->Value(3 + 6 * (double(i) / 128));
    maximum_radius_error = std::max(maximum_radius_error, std::abs(std::hypot(p.X(), p.Y()) - 1));
    require(p.Z() == 0, "Arc left its source plane");
  }
  require(maximum_radius_error < 1e-14, "Arc evaluation exceeded sampled radius threshold");
  std::cout << "arc: exact stored weights/domain; sampled max radius error "
            << maximum_radius_error << '\n';

  const double base = 4e-308;
  const double adjacent = std::nextafter(base, 1.0);
  const std::array<double, 3> near_equal{base, adjacent, base};
  const auto preserved = make_arc(near_equal, false);
  const auto default_checked = make_arc(near_equal, true);
  require(preserved->Weight(2) == adjacent, "Explicit rational import lost adjacent weight");
  std::cout
      << "near-equal tiny weights: CheckRational=false preserves source; default retains weight="
      << (default_checked->Weight(2) == adjacent) << '\n';

  for (const double scale : {1e-320, -1.0, 0.0}) {
    bool rejected = false;
    try {
      const auto unsupported = make_arc({scale, scale * std::sqrt(0.5), scale});
      (void)unsupported;
    }
    catch (const Standard_Failure &) {
      rejected = true;
    }
    require(rejected, "Pinned constructor restriction changed; inspect trial expectations");
    std::cout << "weight scale " << scale << ": rejected explicitly\n";
  }
}

static TopoDS_Edge make_edge(const occ::handle<Geom_BSplineCurve> &curve,
                             const TopoDS_Vertex &start,
                             const TopoDS_Vertex &end)
{
  BRep_Builder builder;
  TopoDS_Edge edge;
  builder.MakeEdge(edge, curve, 0.0);
  builder.Range(edge, 3.0, 9.0);
  builder.Add(edge, start.Oriented(TopAbs_FORWARD));
  builder.Add(edge, end.Oriented(TopAbs_REVERSED));
  builder.UpdateVertex(start, 3.0, edge, 0.0);
  builder.UpdateVertex(end, 9.0, edge, 0.0);
  return edge;
}

static void surface_import_contract()
{
  NCollection_Array2<gp_Pnt> poles(1, 2, 1, 2);
  NCollection_Array2<double> weights(1, 2, 1, 2);
  for (int u = 1; u <= 2; u++) {
    for (int v = 1; v <= 2; v++) {
      poles(u, v) = gp_Pnt(u - 1, v - 1, (u - 1) * (v - 1));
      weights(u, v) = 1;
    }
  }
  weights(2, 2) = std::nextafter(1.0, 2.0);
  NCollection_Array1<double> knots(1, 2);
  NCollection_Array1<int> multiplicities(1, 2);
  knots(1) = 3;
  knots(2) = 9;
  multiplicities(1) = multiplicities(2) = 2;
  const occ::handle<Geom_BSplineSurface> surface = new Geom_BSplineSurface(
      poles, weights, knots, knots, multiplicities, multiplicities, 1, 1);
  require(surface->Weight(2, 2) == 1, "Pinned surface import behavior changed; inspect trial");
  std::cout << "surface: constructor replaced adjacent-to-one weight with unit weight; "
               "coefficient-preserving general import is unsupported by this constructor\n";
}

static void graph_contracts(const bool shared_start)
{
  BRep_Builder builder;
  const auto arc = make_arc({1, std::sqrt(0.5), 1});
  TopoDS_Vertex a, b, c, d;
  builder.MakeVertex(a, gp_Pnt(1, 0, 0), 0);
  builder.MakeVertex(b, gp_Pnt(0, 1, 0), 0);
  if (shared_start) {
    c = a;
  }
  else {
    builder.MakeVertex(c, gp_Pnt(1, 0, 0), 0);
  }
  builder.MakeVertex(d, gp_Pnt(0, 1, 0), 0);
  const TopoDS_Edge first = make_edge(arc, a, b);
  const TopoDS_Edge second = make_edge(arc, c, d);
  TopoDS_Compound compound;
  builder.MakeCompound(compound);
  builder.Add(compound, first);
  builder.Add(compound, second);

  BRepGraph graph;
  BRepGraph::ShapesView::Options options;
  options.TrackAddedNodes = true;
  const auto result = graph.Shapes().Add(compound, options);
  require(result.IsOk(), "Open-wire graph ingestion failed");
  require(graph.Topo().Edges().NbActive() == 2, "Independent edge identities collapsed");
  const unsigned int expected_vertices = shared_start ? 3 : 4;
  require(graph.Topo().Vertices().NbActive() == expected_vertices,
          "Declared vertex identity was replaced by coordinate welding");
  const auto edge_node = graph.Shapes().FindNode(first);
  require(edge_node.IsValid() && result.AddedNodes.IsBound(first), "Missing edge provenance");
  const auto uid = graph.UIDs().Of(edge_node);
  const auto compact = BRepGraph_Compact::Perform(graph);
  (void)compact;
  const auto resolved = graph.UIDs().NodeIdFrom(uid);
  require(resolved.IsValid(), "Persistent edge UID lost during compaction");
  BRepGraph copy;
  require(BRepGraph_Copy::Perform(graph, copy, BRepGraph_Copy::GeomPolicy::Copy),
          "Graph deep copy failed");
  require(copy.Topo().Vertices().NbActive() == expected_vertices, "Copy changed internal sharing");
  const auto copied_curve = occ::down_cast<Geom_BSplineCurve>(
      copy.Topo().Edges().Curve3D(BRepGraph_EdgeId::Start()));
  require(!copied_curve.IsNull() && copied_curve.get() != arc.get(),
          "Copy shares writable source");
  copied_curve->SetPole(1, gp_Pnt(2, 0, 0));
  require(arc->Pole(1).X() == 1, "Mutating deep copy changed original curve");
  std::cout << "open wires: shared start=" << shared_start << ", vertices=" << expected_vertices
            << "; provenance, compact UID and deep-copy isolation passed\n";
  if (shared_start) {
    const auto duplicated = BRepGraph_Copy::CopyNode(graph, graph, resolved);
    require(duplicated.IsValid() && duplicated != resolved, "Self-copy did not create a new edge");
    require(graph.Topo().Edges().NbActive() == 3, "Self-copy changed unrelated edge identity");
    const unsigned int created_vertices = graph.Topo().Vertices().NbActive() - expected_vertices;
    std::cout << "single-edge self-copy with an external incident edge: created vertices="
              << created_vertices << "; Blender disconnected duplicate requires two\n";
  }
  else {
    const auto second_before = graph.Shapes().FindNode(second);
    require(second_before.IsValid(), "Second input edge lost its construction provenance");
    const auto second_uid = graph.UIDs().Of(second_before);
    graph.Editor().Gen().RemoveNode(resolved);
    const auto removed = BRepGraph_Compact::Perform(graph);
    const auto second_after = graph.UIDs().NodeIdFrom(second_uid);
    require(second_after.IsValid() && second_after.Index != second_before.Index,
            "Fixture did not preserve persistent edge identity through an actual index change");
    require(graph.Topo().Edges().NbActive() == 1, "Compaction lost the remaining wire edge");
    std::cout << "edge removal/compaction: surviving UID resolved from index "
              << second_before.Index << " to " << second_after.Index
              << "; unmapped active definitions " << removed.NbUnmappedActiveDefs
              << "; remaining vertices " << graph.Topo().Vertices().NbActive() << '\n';

    BRepGraph keep_vertices;
    require(keep_vertices.Shapes().Add(compound).IsOk(), "Keep-vertices fixture ingestion failed");
    require(keep_vertices.Shapes().Add(a).IsOk() && keep_vertices.Shapes().Add(b).IsOk(),
            "Explicit vertex-root ingestion failed");
    const auto retained_uid = keep_vertices.UIDs().Of(keep_vertices.Shapes().FindNode(a));
    keep_vertices.Editor().Gen().RemoveNode(keep_vertices.Shapes().FindNode(first));
    const auto kept = BRepGraph_Compact::Perform(keep_vertices);
    require(keep_vertices.Topo().Vertices().NbActive() == 4 &&
                keep_vertices.UIDs().Has(retained_uid),
            "Explicit roots did not preserve vertices after edge removal");
    std::cout
        << "edge removal with explicit vertex roots: retained four vertices; unmapped active "
           "definitions "
        << kept.NbUnmappedActiveDefs << '\n';
  }
}

void shared_boundary_trial();

int main()
{
  try {
    curve_contracts();
    surface_import_contract();
    graph_contracts(false);
    graph_contracts(true);
    shared_boundary_trial();
    return 0;
  }
  catch (const Standard_Failure &error) {
    std::cerr << "OCCT failure: " << error.what() << '\n';
  }
  catch (const std::exception &error) {
    std::cerr << "Trial failure: " << error.what() << '\n';
  }
  return 1;
}
