/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "SOL_geometry.hh"
#include <span>

namespace blender::solid {
enum class BooleanOperation { Union, Subtract, Intersect };

/** Empty is an intentional set-operation result. Unsuccessful results never contain a model.
 * NumericalUncertainty is used only by algorithms which identify that condition explicitly;
 * legacy helper failures are reported as Failed rather than guessed from error messages. */
enum class BooleanStatus {
  Success,
  Empty,
  Unsupported,
  InvalidInput,
  NumericalUncertainty,
  Failed
};
enum class BooleanPath { None, EmptyOperand, Planar, CurvedPlanar };
enum class BooleanStage {
  InputValidation,
  OperandValidation,
  Preparation,
  CurvedPlanarSolve,
  PartitionClassification,
  Reconstruction,
  OutputValidation,
};
struct BooleanTiming {
  BooleanStage stage;
  double milliseconds;
};
struct BooleanDiagnostics {
  BooleanPath path = BooleanPath::None;
  size_t first_face_count = 0, second_face_count = 0, result_face_count = 0;
  /** At most one record per stage. Omitted stages were not entered, not measured as zero. */
  std::vector<BooleanTiming> timings;
};
struct BooleanResult {
  BooleanStatus status = BooleanStatus::Failed;
  std::optional<Model> model;
  std::string message;
  BooleanDiagnostics diagnostics;
  bool succeeded() const
  {
    return model.has_value() &&
           (status == BooleanStatus::Success || status == BooleanStatus::Empty);
  }
};
const char *boolean_status_name(BooleanStatus status);
const char *boolean_path_name(BooleanPath path);
const char *boolean_stage_name(BooleanStage stage);

/** Native operation contract. Inputs are immutable. Validate operand closure before solving and
 * validate all output boundaries before returning Success/Empty. This establishes topology and
 * representation checks; it is not a certificate of set membership for arbitrary geometry. */
BooleanResult boolean_bodies_result(const Model &source,
                                    std::span<const Id> first,
                                    std::span<const Id> second,
                                    BooleanOperation operation);
struct BooleanFaceSplit {
  Model model;
  std::vector<Id> negative, positive;
};
/** Assemble independently trimmed native faces with shared vertex/edge topology.
 * Tolerance is an explicit caller policy; zero requires identical positions.
 * Closed mode rejects missing, duplicate, or inconsistently oriented uses. */
std::optional<Model> boolean_sew_faces(std::span<const Model> pieces,
                                       double tolerance,
                                       bool require_closed = true,
                                       std::string *error = nullptr);
/** Create bounded analytic charts for full-wrap cylindrical walls. Shared cap
 * edges are split in the same topology transaction. Source stays immutable. */
std::optional<Model> boolean_partition_periodic_faces(const Model &source,
                                                      std::string *error = nullptr);
std::optional<BooleanFaceSplit> boolean_split_face_by_plane(const Model &source,
                                                            Id face,
                                                            const Surface &plane,
                                                            std::string *error = nullptr);

/** Split a bounded planar face with native line/conic boundaries by a plane.
 * Produces trimmed faces sharing section edges; never fits or samples curves.
 * The returned model contains only these face pieces, not a closed solid. */
std::optional<BooleanFaceSplit> boolean_split_planar_face(const Model &source,
                                                          Id face,
                                                          const Surface &plane,
                                                          std::string *error = nullptr);
/** Intersect, split, classify and rebuild native boundaries. Inputs are complete closed
 * bodies identified by source faces. The source is immutable, including on failure. */
std::optional<Model> boolean_bodies(const Model &source,
                                    std::span<const Id> first,
                                    std::span<const Id> second,
                                    BooleanOperation operation,
                                    std::string *error = nullptr);
}  // namespace blender::solid
