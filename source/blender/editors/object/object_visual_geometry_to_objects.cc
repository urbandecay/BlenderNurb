/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"
#include "BKE_geometry_set_instances.hh"
#include "BKE_instances.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_object.hh"
#include "BKE_pointcloud.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_collection_types.h"
#include "DNA_curves_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "ED_object.hh"
#include "ED_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"

#include "object_intern.hh"

namespace blender::ed::object {

/** Sets the transform of the object to a specific matrix. */
static void set_local_object_transform(Object &ob, const float4x4 &transform)
{
  float3 location;
  math::EulerXYZ rotation;
  float3 scale;
  math::to_loc_rot_scale_safe<true>(transform, location, rotation, scale);
  copy_v3_v3(ob.loc, location);
  copy_v3_v3(ob.rot, float3(rotation.x().radian(), rotation.y().radian(), rotation.z().radian()));
  copy_v3_v3(ob.scale, scale);
  ob.rotmode = ROT_MODE_EUL;
}

/** Objects for a #GeometrySet. */
struct ComponentObjects {
  Object *mesh_ob = nullptr;
  Object *curves_ob = nullptr;
  Object *pointcloud_ob = nullptr;
  Vector<Object *> instance_objects;

  Vector<Object *> all_objects() const
  {
    Vector<Object *> objects;
    if (mesh_ob) {
      objects.append(mesh_ob);
    }
    if (curves_ob) {
      objects.append(curves_ob);
    }
    if (pointcloud_ob) {
      objects.append(pointcloud_ob);
    }
    for (Object *instance_object : instance_objects) {
      objects.append(instance_object);
    }
    return objects;
  }
};

static void copy_materials_to_new_geometry_object(const Object &src_ob_eval,
                                                  const ID &src_data_eval,
                                                  Object &dst_ob_orig,
                                                  ID &dst_data_orig)
{
  const int materials_num = BKE_id_material_used_eval(src_data_eval);
  if (materials_num == 0) {
    return;
  }
  *BKE_id_material_len_p(&dst_data_orig) = materials_num;
  dst_ob_orig.totcol = materials_num;

  dst_ob_orig.matbits = MEM_new_array_zeroed<char>(materials_num, __func__);
  dst_ob_orig.mat = MEM_new_array_zeroed<Material *>(materials_num, __func__);
  Material ***dst_materials = BKE_id_material_array_p(&dst_data_orig);
  *dst_materials = MEM_new_array_zeroed<Material *>(materials_num, __func__);

  for (int i = 0; i < materials_num; i++) {
    const Material *material_eval = BKE_object_material_get_eval(
        src_ob_eval, src_data_eval, i + 1);
    Material *material_orig = const_cast<Material *>(DEG_get_original(material_eval));
    if (material_orig) {
      (*dst_materials)[i] = material_orig;
      id_us_plus(&material_orig->id);
    }
  }
}

/** Utility class to build objects for a #GeometrySet recursively. */
class GeometryToObjectsBuilder {
 private:
  Main &bmain_;
  Map<const ID *, Object *> new_object_by_generated_geometry_;
  int skipped_instances_ = 0;

 public:
  GeometryToObjectsBuilder(Main &bmain) : bmain_(bmain) {}

  int skipped_instances() const
  {
    return skipped_instances_;
  }

  ComponentObjects get_objects_for_geometry(const Object &src_ob_eval,
                                            const bke::GeometrySet &geometry)
  {
    const StringRefNull name = geometry.name().is_empty() ?
                                   StringRefNull(BKE_id_name(src_ob_eval.id)) :
                                   StringRefNull(geometry.name());
    ComponentObjects objects;
    if (const Mesh *mesh = geometry.get_mesh()) {
      if (mesh->verts_num > 0) {
        objects.mesh_ob = this->ensure_object_for_mesh(src_ob_eval, *mesh, name);
      }
    }
    if (const Curves *curves = geometry.get_curves()) {
      if (curves->geometry.curve_num > 0) {
        objects.curves_ob = this->ensure_object_for_curves(src_ob_eval, *curves, name);
      }
    }
    if (const PointCloud *pointcloud = geometry.get_pointcloud()) {
      if (pointcloud->totpoint > 0) {
        objects.pointcloud_ob = this->ensure_object_for_pointcloud(src_ob_eval, *pointcloud, name);
      }
    }
    if (const bke::Instances *instances = geometry.get_instances()) {
      objects.instance_objects = this->create_objects_for_instances(src_ob_eval, *instances);
    }
    return objects;
  }

 private:
  Object *ensure_object_for_mesh(const Object &src_ob_eval,
                                 const Mesh &src_mesh,
                                 const StringRefNull name)
  {
    return new_object_by_generated_geometry_.lookup_or_add_cb(&src_mesh.id, [&]() {
      Mesh *new_mesh = BKE_id_new<Mesh>(&bmain_, name.c_str());
      Object *new_ob = BKE_object_add_only_object(&bmain_, OB_MESH, name.c_str());
      new_ob->data = id_cast<ID *>(new_mesh);

      BKE_mesh_nomain_to_mesh(BKE_mesh_copy_for_eval(src_mesh), new_mesh, new_ob);
      new_mesh->attributes_for_write().remove_anonymous();
      copy_materials_to_new_geometry_object(src_ob_eval, src_mesh.id, *new_ob, new_mesh->id);
      bke::mesh_remove_invalid_attribute_strings(*new_mesh);
      multires_customdata_delete(new_mesh);
      return new_ob;
    });
  }

  Object *ensure_object_for_curves(const Object &src_ob_eval,
                                   const Curves &src_curves,
                                   const StringRefNull name)
  {
    return new_object_by_generated_geometry_.lookup_or_add_cb(&src_curves.id, [&]() {
      Curves *new_curves = BKE_id_new<Curves>(&bmain_, name.c_str());
      Object *new_ob = BKE_object_add_only_object(&bmain_, OB_CURVES, name.c_str());
      new_ob->data = id_cast<ID *>(new_curves);

      new_curves->geometry.wrap() = src_curves.geometry.wrap();
      new_curves->geometry.wrap().attributes_for_write().remove_anonymous();
      copy_materials_to_new_geometry_object(src_ob_eval, src_curves.id, *new_ob, new_curves->id);
      return new_ob;
    });
  }

  Object *ensure_object_for_pointcloud(const Object &src_ob_eval,
                                       const PointCloud &src_pointcloud,
                                       const StringRefNull name)
  {
    return new_object_by_generated_geometry_.lookup_or_add_cb(&src_pointcloud.id, [&]() {
      PointCloud *new_pointcloud = BKE_id_new<PointCloud>(&bmain_, name.c_str());
      Object *new_ob = BKE_object_add_only_object(&bmain_, OB_POINTCLOUD, name.c_str());
      new_ob->data = id_cast<ID *>(new_pointcloud);

      BKE_pointcloud_nomain_to_pointcloud(BKE_pointcloud_copy_for_eval(&src_pointcloud),
                                          new_pointcloud);
      new_pointcloud->attributes_for_write().remove_anonymous();
      copy_materials_to_new_geometry_object(
          src_ob_eval, src_pointcloud.id, *new_ob, new_pointcloud->id);
      return new_ob;
    });
  }

  Vector<Object *> create_objects_for_instances(const Object &src_ob_eval,
                                                const bke::Instances &src_instances)
  {
    /* Only direct object references and identity-transformed geometry references can be
     * converted without creating collection-instancer objects. Other instance types are skipped. */
    const Span<float4x4> transforms = src_instances.transforms();
    const Span<int> handles = src_instances.reference_handles();
    const Span<bke::InstanceReference> references = src_instances.references();

    Vector<Object *> objects;
    for (const int i : IndexRange(src_instances.instances_num())) {
      if (transforms[i] != float4x4::identity()) {
        skipped_instances_++;
        continue;
      }
      const int handle = handles[i];
      if (handle < 0 || handle >= references.size()) {
        skipped_instances_++;
        continue;
      }
      const bke::InstanceReference &reference = references[handle];
      switch (reference.type()) {
        case bke::InstanceReference::Type::None: {
          break;
        }
        case bke::InstanceReference::Type::Object: {
          Object &object_eval = reference.object();
          Object *object_orig = DEG_get_original(&object_eval);
          if (ELEM(object_orig, &src_ob_eval, nullptr)) {
            skipped_instances_++;
            break;
          }
          objects.append(object_orig);
          break;
        }
        case bke::InstanceReference::Type::Collection: {
          skipped_instances_++;
          break;
        }
        case bke::InstanceReference::Type::GeometrySet: {
          const ComponentObjects component_objects = this->get_objects_for_geometry(
              src_ob_eval, reference.geometry_set());
          objects.extend(component_objects.all_objects());
          break;
        }
      }
    }

    return objects;
  }

};

static Vector<Collection *> find_collections_containing_object(Main &bmain,
                                                               Scene *scene,
                                                               Object &object)
{
  VectorSet<Collection *> collections;
  FOREACH_COLLECTION_BEGIN (&bmain, scene, Collection *, collection) {
    if (BKE_collection_has_object(collection, &object)) {
      collections.add(collection);
    }
  }
  FOREACH_COLLECTION_END;
  return collections.extract_vector();
}

static wmOperatorStatus visual_geometry_to_objects_exec(bContext *C, wmOperator *wm_op)
{
  Main &bmain = *CTX_data_main(C);
  Scene &scene = *CTX_data_scene(C);
  Depsgraph &depsgraph = *CTX_data_ensure_evaluated_depsgraph(C);
  ViewLayer &active_view_layer = *CTX_data_view_layer(C);

  Vector<Object *> selected_objects_orig;
  CTX_DATA_BEGIN (C, Object *, src_ob_orig, selected_objects) {
    selected_objects_orig.append(src_ob_orig);
  }
  CTX_DATA_END;

  /* Create all required objects and collections and add them to bmain. However, so far nothing is
   * linked to the scene or view layer. That happens below. */
  GeometryToObjectsBuilder op(bmain);
  Vector<Object *> all_new_top_level_objects;
  for (Object *src_ob_orig : selected_objects_orig) {
    Object *src_ob_eval = DEG_get_evaluated(&depsgraph, src_ob_orig);
    bke::GeometrySet geometry_eval = bke::object_get_evaluated_geometry_set(*src_ob_eval);
    const ComponentObjects new_component_objects = op.get_objects_for_geometry(*src_ob_eval,
                                                                               geometry_eval);
    const Vector<Object *> top_level_objects = new_component_objects.all_objects();
    all_new_top_level_objects.extend(top_level_objects);
    /* Find the collections that the active object is in, because we want to add the new objects
     * in the same place. */
    const Vector<Collection *> collections_to_add_to = find_collections_containing_object(
        bmain, &scene, *src_ob_orig);

    const float4x4 src_ob_world_transform = src_ob_eval->object_to_world();

    for (Object *object : top_level_objects) {
      /* Link the new objects into some collections. */
      for (Collection *collection_to_add_to : collections_to_add_to) {
        BKE_collection_object_add(&bmain, collection_to_add_to, object);
      }
      /* Transform the objects so that they align with the source object. */
      float4x4 old_transform;
      BKE_object_to_mat4(object, old_transform.ptr());
      set_local_object_transform(*object, src_ob_world_transform * old_transform);
    }
  }

  if (op.skipped_instances() > 0) {
    BKE_reportf(wm_op->reports,
                RPT_WARNING,
                "%d transformed or collection instances were skipped",
                op.skipped_instances());
  }

  /* Ensure that the #Base for objects and #LayerCollection for collections are created. */
  BKE_scene_view_layers_synced_ensure(bmain, &scene);

  /* Deselect everything so that we can select the new objects. */
  BKE_view_layer_base_deselect_all(bmain, &scene, &active_view_layer);
  /* Select the new objects. */
  for (Object *object : all_new_top_level_objects) {
    Base *base = BKE_view_layer_base_find(&active_view_layer, object);
    base->flag |= BASE_SELECTED;
  }
  /* Make one of the new objects active. */
  if (!all_new_top_level_objects.is_empty()) {
    Base *first_base = BKE_view_layer_base_find(&active_view_layer, all_new_top_level_objects[0]);
    BKE_view_layer_base_select_and_set_active(&active_view_layer, first_base);
    base_active_refresh(&bmain, &scene, &active_view_layer);
  }
  BKE_view_layer_need_resync_tag(&active_view_layer);
  DEG_id_tag_update(&scene.id, ID_RECALC_BASE_FLAGS);

  DEG_relations_tag_update(&bmain);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, &scene);
  WM_main_add_notifier(NC_SCENE | ND_LAYER, nullptr);
  WM_main_add_notifier(NC_SCENE | ND_LAYER_CONTENT, nullptr);
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, nullptr);
  return OPERATOR_FINISHED;
}

void OBJECT_OT_visual_geometry_to_objects(wmOperatorType *ot)
{
  ot->name = "Visual Geometry to Objects";
  ot->description = "Convert evaluated geometry and supported instances into editable objects";
  ot->idname = "OBJECT_OT_visual_geometry_to_objects";

  ot->exec = visual_geometry_to_objects_exec;
  ot->poll = ED_operator_object_active;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

}  // namespace blender::ed::object
