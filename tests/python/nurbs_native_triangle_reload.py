# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Continue the pyramid UI fixture in a fresh Blender process, including its Mesh reference."""

from pathlib import Path

import bpy


def main():
    source = Path(bpy.data.filepath)
    assert source.stem.startswith('triangle-') and source.stem.endswith('-reload'), source
    obj = bpy.context.view_layer.objects.active
    assert obj is not None
    if obj.type != 'MESH':
        assert obj.data.is_native_nurbs and len(obj.data.splines) == 0
    if obj.mode != 'EDIT':
        bpy.ops.object.mode_set(mode='EDIT')
    assert bpy.ops.transform.translate(value=(0.125, 0, 0)) == {'FINISHED'}
    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.context.view_layer.update()
    result = source.with_name(source.stem.removesuffix('-reload') + '-fresh-move.blend')
    assert bpy.ops.wm.save_as_mainfile(filepath=str(result), check_existing=False) == {'FINISHED'}
    print('TRIANGLE_FRESH_MOVE_PASS', source.stem, bpy.app.build_date, bpy.app.build_time)


if __name__ == '__main__':
    main()
