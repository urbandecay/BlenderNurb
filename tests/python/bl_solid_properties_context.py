# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run with blender --background --factory-startup --python tests/python/bl_solid_properties_context.py."""

import bpy
import unittest
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts/startup/bl_ui"))
import properties_data_mesh


class SolidPropertiesContextTest(unittest.TestCase):
    def setUp(self):
        for obj in list(bpy.data.objects):
            if obj.mode != 'OBJECT':
                bpy.ops.object.mode_set(mode='OBJECT')
                break
        for obj in list(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)

        self.properties_space = next(
            area.spaces.active for area in bpy.context.screen.areas if area.type == 'PROPERTIES')

    def test_solid_context_is_only_available_for_analytic_meshes(self):
        # The dynamic tab list is refreshed by Properties editor redraws, which do not run in
        # background mode. Check the stable RNA registration and the panel's analytic-mesh poll.
        context_property = self.properties_space.bl_rna.properties["context"]
        self.assertIn("SOLID", {item.identifier for item in context_property.enum_items_static})
        self.assertTrue(self.properties_space.show_properties_solid)
        self.assertEqual(properties_data_mesh.DATA_PT_solid_model.bl_context, 'solid')

        bpy.ops.solid.sketch_add(type='RECTANGLE', size=1)
        solid_mesh = bpy.context.object.data
        self.assertTrue(solid_mesh.is_solid_model)
        self.assertTrue(properties_data_mesh.DATA_PT_solid_model.poll(SimpleNamespace(
            mesh=solid_mesh, engine='BLENDER_EEVEE')))

        if bpy.context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        for obj in list(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        bpy.ops.mesh.primitive_cube_add(size=2)
        regular_mesh = bpy.context.object.data
        self.assertFalse(regular_mesh.is_solid_model)
        self.assertFalse(properties_data_mesh.DATA_PT_solid_model.poll(SimpleNamespace(
            mesh=regular_mesh, engine='BLENDER_EEVEE')))


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SolidPropertiesContextTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
