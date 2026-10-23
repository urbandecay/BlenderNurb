/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include <BRepGraph.hxx>
#include <BRepGraph_Copy.hxx>
#include <BRepGraph_ShapesView.hxx>
#include <BRepGraph_TopoView.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>

static void require(const bool condition, const char *message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static occ::handle<Geom_BSplineCurve> model_curve(const gp_Pnt &start,
                                                  const gp_Pnt &end,
                                                  const bool arc)
{
  const int count = arc ? 3 : 2;
  NCollection_Array1<gp_Pnt> poles(1, count);
  NCollection_Array1<double> weights(1, count), knots(1, 2);
  NCollection_Array1<int> multiplicities(1, 2);
  poles(1) = start;
  poles(count) = end;
  weights.Init(1);
  if (arc) {
    poles(2) = gp_Pnt(start.X(), end.Y(), start.Z());
    weights(2) = std::sqrt(0.5);
  }
  knots(1) = 3;
  knots(2) = 9;
  multiplicities.Init(count);
  return new Geom_BSplineCurve(poles, weights, knots, multiplicities, count - 1, false, false);
}

static occ::handle<Geom2d_BSplineCurve> parameter_curve(const gp_Pnt2d &start,
                                                        const gp_Pnt2d &end,
                                                        const bool projective)
{
  NCollection_Array1<gp_Pnt2d> poles(1, 2);
  NCollection_Array1<double> weights(1, 2), knots(1, 2);
  NCollection_Array1<int> multiplicities(1, 2);
  poles(1) = start;
  poles(2) = end;
  weights(1) = projective ? 2 : 1;
  weights(2) = 1;
  knots(1) = 3;
  knots(2) = 9;
  multiplicities.Init(2);
  return new Geom2d_BSplineCurve(poles, weights, knots, multiplicities, 1, false);
}

static occ::handle<Geom_BSplineSurface> support(const bool cone)
{
  NCollection_Array2<gp_Pnt> poles(1, 3, 1, 2);
  NCollection_Array2<double> weights(1, 3, 1, 2);
  const std::array<double, 3> x{1, 1, 0}, y{0, 1, 1};
  const std::array<double, 3> w = cone ? std::array<double, 3>{1, 2 * std::sqrt(0.5), 4} :
                                         std::array<double, 3>{1, std::sqrt(0.5), 1};
  for (int u = 1; u <= 3; u++) {
    for (int v = 1; v <= 2; v++) {
      const double radius = cone ? v : 1;
      poles(u, v) = gp_Pnt(radius * x[u - 1], radius * y[u - 1], cone ? 3 * (v - 1) : 2 * v - 4);
      weights(u, v) = w[u - 1];
    }
  }
  NCollection_Array1<double> knots_u(1, 2), knots_v(1, 2);
  NCollection_Array1<int> mult_u(1, 2), mult_v(1, 2);
  knots_u(1) = cone ? 7 : 2;
  knots_u(2) = cone ? 13 : 5;
  knots_v(1) = cone ? 10 : -2;
  knots_v(2) = cone ? 20 : 0;
  mult_u.Init(3);
  mult_v.Init(2);
  return new Geom_BSplineSurface(poles, weights, knots_u, knots_v, mult_u, mult_v, 2, 1);
}

static double audit_boundaries(BRepGraph &graph)
{
  require(graph.Topo().Faces().NbActive() == 2, "Shared-face count changed");
  require(graph.Topo().Edges().NbActive() == 7, "Shared edge became independent copies");
  require(graph.Topo().Vertices().NbActive() == 6, "Shared endpoint identity changed");
  require(graph.Topo().CoEdges().NbActive() == 8, "Per-face edge use was lost");
  int shared_edges = 0;
  double maximum_error = 0;
  for (uint32_t i = 0; i < graph.Topo().Edges().Nb(); i++) {
    const BRepGraph_EdgeId edge(i);
    shared_edges += graph.Topo().Edges().NbFaces(edge) == 2;
    const auto curve = graph.Topo().Edges().Curve3D(edge);
    require(!curve.IsNull(), "Missing canonical edge geometry");
    for (const BRepGraph_CoEdgeId use : graph.Topo().Edges().CoEdges(edge)) {
      const auto pcurve = graph.Topo().CoEdges().Curve2D(use);
      const auto surface = graph.Topo().Faces().Surface(graph.Topo().CoEdges().Face(use));
      require(!pcurve.IsNull() && !surface.IsNull(), "Missing per-face boundary representation");
      for (int sample = 0; sample <= 64; sample++) {
        const double parameter = 3 + 6 * (double(sample) / 64);
        const gp_Pnt2d uv = pcurve->Value(parameter);
        maximum_error = std::max(maximum_error,
                                 curve->Value(parameter).Distance(surface->Value(uv.X(), uv.Y())));
      }
    }
  }
  require(shared_edges == 1, "Expected one canonical edge shared by both faces");
  require(maximum_error < 1e-12, "Boundary parameter correspondence exceeded sampled threshold");
  return maximum_error;
}

void shared_boundary_trial()
{
  BRep_Builder builder;
  const std::array<gp_Pnt, 6> points{gp_Pnt(1, 0, -2),
                                     gp_Pnt(0, 1, -2),
                                     gp_Pnt(0, 1, 0),
                                     gp_Pnt(1, 0, 0),
                                     gp_Pnt(0, 2, 3),
                                     gp_Pnt(2, 0, 3)};
  std::array<TopoDS_Vertex, 6> vertices;
  for (int i = 0; i < 6; i++) {
    builder.MakeVertex(vertices[i], points[i], 0);
  }
  const std::array<std::array<int, 2>, 7> ends{
      {{0, 1}, {1, 2}, {3, 2}, {3, 0}, {2, 4}, {5, 4}, {5, 3}}};
  std::array<TopoDS_Edge, 7> edges;
  for (int i = 0; i < 7; i++) {
    const int a = ends[i][0], b = ends[i][1];
    builder.MakeEdge(edges[i], model_curve(points[a], points[b], i == 0 || i == 2 || i == 5), 0);
    builder.Range(edges[i], 3, 9);
    builder.Add(edges[i], vertices[a].Oriented(TopAbs_FORWARD));
    builder.Add(edges[i], vertices[b].Oriented(TopAbs_REVERSED));
    builder.UpdateVertex(vertices[a], 3, edges[i], 0);
    builder.UpdateVertex(vertices[b], 9, edges[i], 0);
  }
  std::array<TopoDS_Face, 2> faces;
  for (int face = 0; face < 2; face++) {
    const bool cone = face == 1;
    builder.MakeFace(faces[face], support(cone), 0);
    const double u0 = cone ? 7 : 2, u1 = cone ? 13 : 5;
    const double v0 = cone ? 10 : -2, v1 = cone ? 20 : 0;
    const std::array<gp_Pnt2d, 4> uv{
        gp_Pnt2d(u0, v0), gp_Pnt2d(u1, v0), gp_Pnt2d(u1, v1), gp_Pnt2d(u0, v1)};
    const std::array<int, 4> face_edges = cone ? std::array<int, 4>{2, 4, 5, 6} :
                                                 std::array<int, 4>{0, 1, 2, 3};
    TopoDS_Wire wire;
    builder.MakeWire(wire);
    for (int side = 0; side < 4; side++) {
      TopoDS_Edge &edge = edges[face_edges[side]];
      const int a = side == 2 ? 3 : side;
      const int b = side == 2 ? 2 : (side + 1) % 4;
      builder.UpdateEdge(
          edge, parameter_curve(uv[a], uv[b], cone && side % 2 == 0), faces[face], 0);
      builder.Range(edge, faces[face], 3, 9);
      builder.Add(wire, edge.Oriented(side == 2 ? TopAbs_REVERSED : TopAbs_FORWARD));
    }
    builder.Add(faces[face], wire);
  }
  TopoDS_Shell shell;
  builder.MakeShell(shell);
  builder.Add(shell, faces[0]);
  builder.Add(shell, faces[1]);
  BRepGraph graph;
  const auto result = graph.Shapes().Add(shell);
  require(result.IsOk(), "Curved shared-face graph ingestion failed");
  const double original_error = audit_boundaries(graph);
  BRepGraph copy;
  require(BRepGraph_Copy::Perform(graph, copy), "Shared-face deep copy failed");
  const double copied_error = audit_boundaries(copy);
  std::cout << "curved shared faces: 6 vertices, 7 edges, 8 uses; sampled max boundary error "
            << original_error << ", copied " << copied_error << "; ingestion status "
            << int(result.Status) << "; builder edge tolerance " << BRep_Tool::Tolerance(edges[2])
            << '\n';
}
