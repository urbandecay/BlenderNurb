# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Full-application loading of older NURBS files, including their UI versioning."""

from pathlib import Path
import unittest

import bpy


class NurbsLegacyCompatibilityTest(unittest.TestCase):
    def check_file(self, relative_path, name, point_count_u, point_count_v, first_point):
        path = Path(__file__).resolve().parent.parent / "files" / relative_path
        self.assertTrue(path.is_file(), str(path))
        self.assertEqual(bpy.ops.wm.open_mainfile(
            filepath=str(path), load_ui=False, use_scripts=False), {'FINISHED'})
        self.assertEqual(len(bpy.data.curves), 1)
        curve = bpy.data.curves[name]
        self.assertEqual(len(curve.splines), 1)
        spline = curve.splines[0]
        self.assertEqual(spline.type, 'NURBS')
        self.assertEqual(spline.point_count_u, point_count_u)
        self.assertEqual(spline.point_count_v, point_count_v)
        self.assertEqual(spline.order_u, 4)
        self.assertEqual(len(spline.points), point_count_u * point_count_v)
        self.assertEqual(tuple(spline.points[0].co), first_point)

    def test_older_surface_file(self):
        self.check_file(
            "render/mesh/texture_space_nurbs.blend", "Surface", 8, 2,
            (0.0, -0.8160001635551453, 0.30000001192092896, 1.0))

    def test_older_curve_file(self):
        self.check_file("alembic/single-curve.blend", "CurveData", 4, 1, (-1.0, -1.0, 0.0, 1.0))


if __name__ == "__main__":
    unittest.main(argv=[__file__])
