"""Run inside Blender with --factory-startup --background --python ... -- ADDON_PARENT CAPTURE RESULT."""
import json
import sys
from pathlib import Path

import bpy
import numpy as np

addon, capture, output = sys.argv[sys.argv.index('--') + 1:]
sys.path.insert(0, addon)
from vgs import native, playback

pc = bpy.data.pointclouds.new('VGS optimized player test')
pc.type = 'GAUSSIAN_SPLAT'
player = native.Player(capture, True, 2, 4)
cases = 0
try:
    for include, density in [(True, 1.), (False, 1.), (True, .25), (True, 1.)]:
        player.set_include_sh(include)
        player.set_density(density)
        for t in [0., .5, min(player.info.duration, 1.01), 0.]:
            player.schedule([t])
            frame = player.acquire(t, 30000)
            assert frame is not None
            playback.write_frame(pc, frame)
            assert len(pc.points) == frame.count
            entries = [('position', 'vector', frame.positions), ('rotation', 'value', frame.rotations),
                       ('scale', 'vector', frame.scales), ('radiance:base', 'vector', frame.radiance)]
            entries += [(f'radiance:sh_{k}', 'vector', values) for k, values in enumerate(frame.sh)]
            for name, member, values in entries:
                read = np.empty_like(values)
                pc.attributes[name].data.foreach_get(member, read)
                assert np.array_equal(read.view(np.uint32), values.view(np.uint32)), name
            assert pc.attributes.get(f'radiance:sh_{len(frame.sh)}') is None
            player.release()
            cases += 1
finally:
    player.close()
    bpy.data.pointclouds.remove(pc)
Path(output).write_text(json.dumps({'blender': bpy.app.version_string, 'frames': cases, 'passed': True}), encoding='utf-8')
print('VGS Blender attribute parity:', cases, 'frames passed')
