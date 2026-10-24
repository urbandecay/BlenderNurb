/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <limits>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"

#include "BKE_nurbs_geometry.hh"
#include "BKE_nurbs_selection.hh"

namespace blender::bke::nurbs {

static bool is_selected(const SelectionState &selection, const ElementRef element)
{
  switch (element.domain) {
    case ElementDomain::Vertex:
      return selection.vertices.contains(element.id);
    case ElementDomain::Edge:
      return selection.edges.contains(element.id);
    case ElementDomain::Face:
      return selection.faces.contains(element.id);
  }
  return false;
}

static bool element_exists(const TopologyIndex &index, const ElementRef element)
{
  switch (element.domain) {
    case ElementDomain::Vertex:
      return index.vertex(element.id) != nullptr;
    case ElementDomain::Edge:
      return index.edge(element.id) != nullptr;
    case ElementDomain::Face:
      return index.face(element.id) != nullptr;
  }
  return false;
}

static void selection_history_prune(SelectionState &selection)
{
  selection.history.remove_if(
      [&](const ElementRef element) { return !is_selected(selection, element); });
  if (selection.active && !is_selected(selection, *selection.active)) {
    selection.active.reset();
  }
}

bool selection_validate(const SelectionState &selection,
                        FunctionRef<bool(ElementRef)> exists,
                        std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return false;
  };
  for (const int64_t id : selection.vertices) {
    if (!exists({ElementDomain::Vertex, id})) {
      return fail("NURBS selection references an unknown vertex");
    }
  }
  for (const int64_t id : selection.edges) {
    if (!exists({ElementDomain::Edge, id})) {
      return fail("NURBS selection references an unknown edge");
    }
  }
  for (const int64_t id : selection.faces) {
    if (!exists({ElementDomain::Face, id})) {
      return fail("NURBS selection references an unknown face");
    }
  }
  if (selection.active && !is_selected(selection, *selection.active)) {
    return fail("The active NURBS element is not selected in its source domain");
  }
  Set<int64_t> history_ids;
  for (const ElementRef element : selection.history) {
    if (!is_selected(selection, element) || !history_ids.add(element.id)) {
      return fail("NURBS selection history has an unselected or repeated element");
    }
  }
  return true;
}

std::optional<SelectionState> selection_flush(const NurbsGeometry &geometry,
                                              const SelectionState &selection,
                                              const SelectionMode mode,
                                              std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  if (int(mode) == 0 || (int(mode) & ~7) != 0) {
    if (r_error != nullptr) {
      *r_error = "Invalid NURBS component selection mode";
    }
    return std::nullopt;
  }
  const auto index = TopologyIndex::build(geometry, r_error);
  if (!index || !selection_validate(
                    selection,
                    [&](const ElementRef element) {
                      switch (element.domain) {
                        case ElementDomain::Vertex:
                          return index->vertex(element.id) != nullptr;
                        case ElementDomain::Edge:
                          return index->edge(element.id) != nullptr;
                        case ElementDomain::Face:
                          return index->face(element.id) != nullptr;
                      }
                      return false;
                    },
                    r_error))
  {
    return std::nullopt;
  }
  SelectionState result = selection;
  auto set_vertex = [&](const int64_t id, const bool select) {
    if (select) {
      result.vertices.add(id);
    }
    else {
      result.vertices.remove(id);
    }
  };
  auto set_edge = [&](const NurbsTopologyEdge &edge, const bool select) {
    set_vertex(edge.vertex_ids[0], select);
    set_vertex(edge.vertex_ids[1], select);
  };
  auto set_face = [&](const NurbsTopologyFace &face, const bool select) {
    for (const NurbsTopologyLoop &loop : face.loops) {
      for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
        if (select) {
          result.edges.add(use.edge_id);
        }
        else {
          result.edges.remove(use.edge_id);
        }
        set_edge(*index->edge(use.edge_id), select);
      }
    }
  };
  const bool vertex_mode = flag_is_set(mode, SelectionMode::Vertex);
  const bool edge_mode = flag_is_set(mode, SelectionMode::Edge);
  if (!vertex_mode) {
    /* Deselect first, then restore the selected closure. A shared component is selected when
     * any selected incident component requires it, independently of storage traversal order. */
    if (edge_mode) {
      for (const bool select : {false, true}) {
        for (const NurbsTopologyEdge &edge : geometry.edges) {
          if (result.edges.contains(edge.id) == select) {
            set_edge(edge, select);
          }
        }
      }
    }
    else {
      for (const bool select : {false, true}) {
        for (const NurbsTopologyFace &face : geometry.faces) {
          if (result.faces.contains(face.id) == select) {
            set_face(face, select);
          }
        }
      }
    }
  }
  if (vertex_mode) {
    for (const NurbsTopologyEdge &edge : geometry.edges) {
      if (result.vertices.contains(edge.vertex_ids[0]) &&
          result.vertices.contains(edge.vertex_ids[1]))
      {
        result.edges.add(edge.id);
      }
      else {
        result.edges.remove(edge.id);
      }
    }
  }
  if (vertex_mode || edge_mode) {
    for (const NurbsTopologyFace &face : geometry.faces) {
      bool selected = true;
      for (const NurbsTopologyLoop &loop : face.loops) {
        for (const NurbsTopologyEdgeUse &use : loop.edge_uses) {
          selected &= result.edges.contains(use.edge_id);
        }
      }
      if (selected) {
        result.faces.add(face.id);
      }
      else {
        result.faces.remove(face.id);
      }
    }
  }
  selection_history_prune(result);
  return result;
}

std::optional<SelectionState> selection_apply_vertices(const NurbsGeometry &geometry,
                                                       const SelectionState &selection,
                                                       const Span<int64_t> hits,
                                                       const SelectionAction action,
                                                       const std::optional<int64_t> active_vertex,
                                                       std::string *r_error)
{
  const auto index = TopologyIndex::build(geometry, r_error);
  if (!index || !selection_validate(
                    selection,
                    [&](const ElementRef element) { return element_exists(*index, element); },
                    r_error))
  {
    return std::nullopt;
  }
  Set<int64_t> unique_hits;
  for (const int64_t id : hits) {
    if (index->vertex(id) == nullptr) {
      if (r_error != nullptr) {
        *r_error = "NURBS selection hit references an unknown source vertex";
      }
      return std::nullopt;
    }
    unique_hits.add(id);
  }
  if (active_vertex && index->vertex(*active_vertex) == nullptr) {
    if (r_error != nullptr) {
      *r_error = "NURBS active selection hit references an unknown source vertex";
    }
    return std::nullopt;
  }
  SelectionState result = action == SelectionAction::Set ? SelectionState{} : selection;
  switch (action) {
    case SelectionAction::Set:
    case SelectionAction::Add:
      for (const int64_t id : unique_hits) {
        result.vertices.add(id);
      }
      break;
    case SelectionAction::Subtract:
      for (const int64_t id : unique_hits) {
        result.vertices.remove(id);
      }
      break;
    case SelectionAction::Invert:
      for (const int64_t id : unique_hits) {
        if (!result.vertices.remove(id)) {
          result.vertices.add(id);
        }
      }
      break;
    case SelectionAction::Intersect:
      result.vertices.remove_if([&](const int64_t id) { return !unique_hits.contains(id); });
      break;
    default:
      if (r_error != nullptr) {
        *r_error = "Invalid NURBS vertex selection action";
      }
      return std::nullopt;
  }
  selection_history_prune(result);
  if (active_vertex && result.vertices.contains(*active_vertex)) {
    const ElementRef active{ElementDomain::Vertex, *active_vertex};
    result.history.remove_if([&](const ElementRef element) { return element == active; });
    result.history.append(active);
    result.active = active;
  }
  auto normalized = selection_flush(geometry, result, SelectionMode::Vertex, r_error);
  if (normalized && !normalized->active && !normalized->history.is_empty()) {
    /* Mesh vertex editing resolves the active element from the surviving selection history. */
    normalized->active = normalized->history.last();
  }
  return normalized;
}

std::optional<SelectionState> selection_remap(const NurbsGeometry &source,
                                              const NurbsGeometry &target,
                                              const SelectionState &selection,
                                              const Map<int64_t, int64_t> &id_map,
                                              const SelectionMode mode,
                                              std::string *r_error)
{
  const auto index = TopologyIndex::build(source, r_error);
  if (!index || !selection_validate(
                    selection,
                    [&](const ElementRef element) { return element_exists(*index, element); },
                    r_error))
  {
    return std::nullopt;
  }
  SelectionState result;
  auto remap_domain = [&](const Set<int64_t> &input, Set<int64_t> &output) {
    for (const int64_t id : input) {
      if (const int64_t *mapped = id_map.lookup_ptr(id)) {
        output.add(*mapped);
      }
    }
  };
  remap_domain(selection.vertices, result.vertices);
  remap_domain(selection.edges, result.edges);
  remap_domain(selection.faces, result.faces);
  if (selection.active) {
    if (const int64_t *mapped = id_map.lookup_ptr(selection.active->id)) {
      result.active = ElementRef{selection.active->domain, *mapped};
    }
  }
  for (const ElementRef element : selection.history) {
    if (const int64_t *mapped = id_map.lookup_ptr(element.id)) {
      const ElementRef remapped{element.domain, *mapped};
      result.history.remove_if([&](const ElementRef other) { return other == remapped; });
      result.history.append(remapped);
    }
  }
  auto normalized = selection_flush(target, result, mode, r_error);
  if (normalized && !normalized->active && !normalized->history.is_empty()) {
    normalized->active = normalized->history.last();
  }
  return normalized;
}

template<typename T> static bool storage_array_is_valid(const T *data, const int size)
{
  return size >= 0 &&
         (size == 0 || (data != nullptr && uint64_t(size) * sizeof(T) <= MEM_allocN_len(data)));
}

std::optional<SelectionState> selection_from_storage(const NurbsStoredSelection *storage,
                                                     std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  auto fail = [&](const char *message) -> std::optional<SelectionState> {
    if (r_error != nullptr) {
      *r_error = message;
    }
    return std::nullopt;
  };
  SelectionState selection;
  if (storage == nullptr) {
    return selection;
  }
  if (!storage_array_is_valid(storage->elements, storage->elements_num) ||
      !storage_array_is_valid(storage->history, storage->history_num))
  {
    return fail("Invalid NURBS stored selection array size");
  }
  for (int i = 0; i < storage->elements_num; i++) {
    const NurbsSelectionElement &element = storage->elements[i];
    Set<int64_t> *domain = nullptr;
    switch (ElementDomain(element.domain)) {
      case ElementDomain::Vertex:
        domain = &selection.vertices;
        break;
      case ElementDomain::Edge:
        domain = &selection.edges;
        break;
      case ElementDomain::Face:
        domain = &selection.faces;
        break;
    }
    if (domain == nullptr || element.id <= 0 || !domain->add(element.id)) {
      return fail("Invalid or repeated NURBS stored selected element");
    }
  }
  if (storage->active.id != 0) {
    selection.active = ElementRef{ElementDomain(storage->active.domain), storage->active.id};
  }
  else if (storage->active.domain != 0) {
    return fail("Invalid NURBS stored active-element domain");
  }
  selection.history.reserve(storage->history_num);
  for (int i = 0; i < storage->history_num; i++) {
    const NurbsSelectionElement &element = storage->history[i];
    selection.history.append({ElementDomain(element.domain), element.id});
  }
  return selection;
}

NurbsStoredSelection *selection_to_storage(const SelectionState &selection, std::string *r_error)
{
  if (r_error != nullptr) {
    r_error->clear();
  }
  const int64_t elements_num = selection.vertices.size() + selection.edges.size() +
                               selection.faces.size();
  if (elements_num > std::numeric_limits<int>::max() ||
      selection.history.size() > std::numeric_limits<int>::max())
  {
    if (r_error != nullptr) {
      *r_error = "NURBS selection exceeds persistent array limits";
    }
    return nullptr;
  }
  NurbsStoredSelection *storage = MEM_new<NurbsStoredSelection>(__func__);
  storage->elements_num = int(elements_num);
  storage->history_num = int(selection.history.size());
  if (elements_num != 0) {
    storage->elements = MEM_new_array<NurbsSelectionElement>(elements_num, __func__);
    int i = 0;
    auto append_domain = [&](const Set<int64_t> &ids, const ElementDomain domain) {
      for (const int64_t id : ids) {
        storage->elements[i].id = id;
        storage->elements[i].domain = int(domain);
        i++;
      }
    };
    append_domain(selection.vertices, ElementDomain::Vertex);
    append_domain(selection.edges, ElementDomain::Edge);
    append_domain(selection.faces, ElementDomain::Face);
    std::sort(storage->elements,
              storage->elements + elements_num,
              [](const NurbsSelectionElement &a, const NurbsSelectionElement &b) {
                return a.domain == b.domain ? a.id < b.id : a.domain < b.domain;
              });
  }
  if (selection.active) {
    storage->active.id = selection.active->id;
    storage->active.domain = int(selection.active->domain);
  }
  if (!selection.history.is_empty()) {
    storage->history = MEM_new_array<NurbsSelectionElement>(selection.history.size(), __func__);
    for (const int64_t i : selection.history.index_range()) {
      storage->history[i].id = selection.history[i].id;
      storage->history[i].domain = int(selection.history[i].domain);
    }
  }
  return storage;
}

NurbsStoredSelection *selection_storage_copy(const NurbsStoredSelection *storage)
{
  if (storage == nullptr) {
    return nullptr;
  }
  NurbsStoredSelection *copy = MEM_dupalloc(storage);
  copy->elements = MEM_dupalloc(storage->elements);
  copy->history = MEM_dupalloc(storage->history);
  return copy;
}

void selection_storage_free(NurbsStoredSelection *storage)
{
  if (storage != nullptr) {
    MEM_SAFE_DELETE(storage->elements);
    MEM_SAFE_DELETE(storage->history);
    MEM_delete(storage);
  }
}

}  // namespace blender::bke::nurbs
