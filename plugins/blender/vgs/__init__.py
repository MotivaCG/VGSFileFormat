# SPDX-License-Identifier: GPL-3.0-or-later

"""VGS (4DGS fileformat): import and play 4D Gaussian splat captures (.vgs, .pgs).

A capture becomes a point cloud of type Gaussian Splat. Its settings live on the point
cloud, and on every frame change the add-on writes the capture's splats at that instant
into the point cloud's attributes, which Blender draws natively.

    native.py      the decoder library, through ctypes
    playback.py    which frame each capture shows, and writing it into Blender
    properties.py  per-capture settings and add-on preferences
    operators.py   import, reload, fit the scene to a capture
    interface.py   the panel and the menu entry
"""

from . import interface, operators, playback, properties

_modules = (properties, operators, interface, playback)


def register():
    for module in _modules:
        module.register()


def unregister():
    for module in reversed(_modules):
        module.unregister()
