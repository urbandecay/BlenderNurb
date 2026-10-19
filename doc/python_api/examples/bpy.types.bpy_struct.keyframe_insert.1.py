"""
Nested properties can be keyed from the ID data-block that owns them.
"""

import bpy
from bpy.props import FloatProperty, PointerProperty


class MyPropGroup(bpy.types.PropertyGroup):
    nested: FloatProperty(name="Nested", default=0.0)


bpy.utils.register_class(MyPropGroup)
bpy.types.Scene.my_prop = PointerProperty(type=MyPropGroup, name="MyProp")

scene = bpy.context.scene
scene.my_prop.nested = 10
scene.keyframe_insert(
    data_path="my_prop.nested",
    frame=1,
    group="Nested Group",
)
