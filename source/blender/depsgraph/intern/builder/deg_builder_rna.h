/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 */

#pragma once

#include "intern/node/deg_node.hh"
#include "intern/node/deg_node_operation.hh"

namespace blender {

struct ID;
struct PointerRNA;
struct PropertyRNA;

namespace deg {

struct Depsgraph;
struct Node;
class DepsgraphBuilder;

/* For queries which gives operation node or key defines whether we are
 * interested in a result of the given property or whether we are linking some
 * dependency to that property. */
enum class RNAPointerSource {
  /* Query will return pointer to an entry operation of component which is
   * responsible for evaluation of the given property. */
  ENTRY,
  /* Query will return pointer to an exit operation of component which is
   * responsible for evaluation of the given property.
   * More precisely, it will return operation at which the property is known
   * to be evaluated. */
  EXIT,
};

/* A helper structure which wraps all fields needed to find a node inside of
 * the dependency graph. */
class RNANodeIdentifier {
 public:
  RNANodeIdentifier();

  /* Check whether this identifier is valid and usable. */
  bool is_valid() const;

  ID *id;
  NodeType type;
  const char *component_name;
  OperationCode operation_code;
  const char *operation_name;
  int operation_name_tag;
};

/* Helper class which performs optimized lookups of a node within a given
 * dependency graph which satisfies given RNA pointer or RNA path. */
class RNANodeQuery {
 public:
  RNANodeQuery(Depsgraph *depsgraph, DepsgraphBuilder *builder);
  ~RNANodeQuery();

  Node *find_node(const PointerRNA *ptr, const PropertyRNA *prop, RNAPointerSource source);

 protected:
  Depsgraph *depsgraph_;
  DepsgraphBuilder *builder_;

  /* Construct identifier of the node which corresponds given configuration
   * of RNA property. */
  RNANodeIdentifier construct_node_identifier(const PointerRNA *ptr,
                                              const PropertyRNA *prop,
                                              RNAPointerSource source);

  /* Check whether prop_identifier contains rna_path_component.
   *
   * This checks more than a sub-string:
   *
   * prop_identifier           contains(prop_identifier, "location")
   * ------------------------  -------------------------------------
   * location                  true
   * ["test_location"]         false
   * object["custom_property"].location     true
   * object["custom_property"].location.x   true
   */
  static bool contains(const char *prop_identifier, const char *rna_path_component);
};

bool rna_prop_affects_parameters_node(const PointerRNA *ptr, const PropertyRNA *prop);

}  // namespace deg
}  // namespace blender
