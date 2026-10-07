"""Run in isolated hython preferences with the rebuilt VGS package enabled."""
import json
import sys
from pathlib import Path

import hou

capture, output = sys.argv[1:]
package = hou.getenv('VGS_HOUDINI')
assert package and Path(package).is_dir(), 'VGS package is not configured'
assert hou.nodeType(hou.sopNodeTypeCategory(), 'vgs_capture') is not None
geo = hou.node('/obj').createNode('geo', 'vgs_optimized_test', run_init_scripts=False)
sop = geo.createNode('vgs_capture')
sop.parm('file').set(capture)
hou.setFps(30)
rows = []
try:
    for use_sh in [1, 0, 1]:
        sop.parm('usesh').set(use_sh)
        for frame in [1, 16, 32, 1]:
            hou.setFrame(frame)
            sop.cook(force=True)
            assert not sop.errors(), sop.errors()
            points = sop.geometry()
            count = points.intrinsicValue('pointcount')
            assert count > 0
            for name in ['P', 'orient', 'scale', 'Cd', 'GS_Alpha']:
                assert points.findPointAttrib(name) is not None, name
            assert (points.findPointAttrib('GS_SPH_R') is not None) == bool(use_sh)
            rows.append({'frame': frame, 'requestedSh': use_sh, 'points': count})
finally:
    geo.destroy()
Path(output).write_text(json.dumps({'houdini': hou.applicationVersionString(), 'package': package,
    'preferences': hou.getenv('HOUDINI_USER_PREF_DIR'), 'dsos': hou.hscript('dsoinfo')[0],
    'cooks': rows, 'passed': True}, indent=2), encoding='utf-8')
print('VGS Houdini smoke:', len(rows), 'cooks passed')
