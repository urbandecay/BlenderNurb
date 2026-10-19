# SPDX-FileCopyrightText: 2020-2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import unittest
import bpy
import sys
import pathlib


class MotionPathTestObject(unittest.TestCase):

    anim_object: bpy.types.Object

    def setUp(self) -> None:
        super().setUp()
        bpy.ops.wm.read_homefile(use_factory_startup=True)
        self.anim_object = bpy.data.objects.new("anim_object", bpy.data.meshes.new("anim_mesh"))
        bpy.context.scene.collection.objects.link(self.anim_object)
        bpy.context.view_layer.objects.active = self.anim_object
        self.anim_object.select_set(True)

    def test_cache_range(self):
        """Testing if the motion path creates the correct range."""
        self.anim_object.keyframe_insert("location", frame=0)
        self.anim_object.keyframe_insert("location", frame=10)

        bpy.ops.object.paths_calculate(range='SCENE')
        self.assertNotEqual(self.anim_object.motion_path, None)
        motion_path = self.anim_object.motion_path
        self.assertEqual(motion_path.frame_start, bpy.context.scene.frame_start)
        # The motion path frame_end is exclusive while the scene frame_end is inclusive.
        self.assertEqual(motion_path.frame_end, bpy.context.scene.frame_end + 1)

        bpy.ops.object.paths_calculate(range='KEYS_ALL')
        self.assertEqual(motion_path.frame_start, 0)
        self.assertEqual(motion_path.frame_end, 11)

        bpy.ops.object.paths_calculate(range='KEYS_SELECTED')
        # Both keys are selected.
        self.assertEqual(motion_path.frame_start, 0)
        self.assertEqual(motion_path.frame_end, 11)

        self.anim_object.animation_visualization.motion_path.frame_start = 3
        # frame_end is inclusive.
        self.anim_object.animation_visualization.motion_path.frame_end = 6
        bpy.ops.object.paths_calculate(range='MANUAL')
        self.assertEqual(motion_path.frame_start, 3)
        self.assertEqual(motion_path.frame_end, 7)

    def test_motion_path_world_space(self):
        """Motion paths report the object's world-space movement."""
        self.anim_object.location = (1, 0, 1)
        self.anim_object.keyframe_insert("location", frame=1)
        self.anim_object.location = (0, 0, 0)
        self.anim_object.keyframe_insert("location", frame=10)

        bpy.ops.object.paths_calculate(range='SCENE')
        motion_path = self.anim_object.motion_path
        self.assertAlmostEqual(motion_path.points[0].co[0], 1, 3)
        self.assertAlmostEqual(motion_path.points[0].co[1], 0, 3)
        self.assertAlmostEqual(motion_path.points[0].co[2], 1, 3)

        self.assertAlmostEqual(motion_path.points[9].co[0], 0, 3)
        self.assertAlmostEqual(motion_path.points[9].co[1], 0, 3)
        self.assertAlmostEqual(motion_path.points[9].co[2], 0, 3)


def main():
    global args
    import argparse

    if '--' in sys.argv:
        argv = [sys.argv[0]] + sys.argv[sys.argv.index('--') + 1:]
    else:
        argv = sys.argv

    parser = argparse.ArgumentParser()
    parser.add_argument('--testdir', required=True, type=pathlib.Path)
    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()
