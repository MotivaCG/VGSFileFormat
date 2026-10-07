"""Compare two built player libraries through the actual Blender ctypes bridge.

Usage: python tests/player_compare.py OLD_DLL NEW_DLL capture.vgs result.json
Both libraries must use the current public ABI. Run timings without other benchmarks.
"""
import importlib.util
import json
import sys
import time
from pathlib import Path

import numpy as np

def bridge(path, name):
    source = Path(__file__).resolve().parents[1] / 'plugins/blender/vgs/native.py'
    spec = importlib.util.spec_from_file_location(name, source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    # os.path.join in library() preserves an absolute last path component.
    module._library_name = lambda: str(Path(path).resolve())
    module.library()
    return module

def snapshot(frame):
    return [a.view(np.uint32).copy() for a in
            [frame.positions, frame.rotations, frame.scales, frame.radiance, *frame.sh]]

def compare(left, right):
    if len(left) != len(right) or any(not np.array_equal(a, b) for a, b in zip(left, right)):
        raise AssertionError('frame bytes differ')

def main():
    old, new, source, output = sys.argv[1:]
    modules = [bridge(old, 'before'), bridge(new, 'after')]
    players = [m.Player(source, include_sh=True, slots=2, threads=4) for m in modules]
    result = {'source': source, 'framesCompared': 0, 'timings': [], 'method':
              'C ABI schedule+acquire on warm first chunk; evaluation+packing+thread wakeup, no host attribute writes or render. Three ABBA rounds, 4 decoder threads.'}
    try:
        duration = players[0].info.duration
        assert duration == players[1].info.duration
        times = sorted(set([0.0, duration * .5, max(0, duration - .001)] +
                           [min(duration, k + a) for k in range(int(duration)) for a in [0., .5, .999]]))
        times += list(reversed(times[:3]))
        for include, density in [(True, 1.), (False, 1.), (True, .25), (True, .01), (False, .5), (True, 1.)]:
            for p in players:
                p.set_include_sh(include)
                p.set_density(density)
            for seconds in times:
                frames = []
                for p in players:
                    p.schedule([seconds])
                    frame = p.acquire(seconds, 30000)
                    if frame is None: raise TimeoutError('frame timeout')
                    frames.append((frame.count, frame.seconds, snapshot(frame)))
                    p.release()
                assert frames[0][:2] == frames[1][:2]
                compare(frames[0][2], frames[1][2])
                if not include: assert len(frames[1][2]) == 4
                result['framesCompared'] += 1
            # Published frame remains valid across scheduling/configuration changes
            # until release, even while another slot is refilled.
            for p in players:
                p.schedule([0.])
                frame = p.acquire(0., 30000)
                assert frame is not None
                saved = snapshot(frame)
                p.schedule([min(.5, duration)])
                p.set_density(.7)
                time.sleep(.02)
                compare(saved, snapshot(frame))
                p.release()
        count = max(2, min(24, int(min(duration, 1.) * players[0].info.frame_rate) - 1))
        sequence = [(i + .1) / players[0].info.frame_rate for i in range(count)]
        for include in [False, True]:
            for p in players:
                p.set_density(1.)
                p.set_include_sh(include)
            def run(which, measured):
                p = players[which]
                values = []
                for seconds in sequence:
                    start = time.perf_counter()
                    p.schedule([seconds])
                    frame = p.acquire(seconds, 30000)
                    elapsed = (time.perf_counter() - start) * 1000
                    assert frame is not None
                    p.release()
                    if measured: values.append(elapsed)
                return values
            run(0, False); run(1, False)
            for round_ in range(3):
                for which in ([0, 1, 1, 0] if round_ % 2 == 0 else [1, 0, 0, 1]):
                    result['timings'].append({'sh': include, 'which': which, 'round': round_, 'ms': run(which, True)})
    finally:
        for p in players: p.close()
    Path(output).write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({'file': output, 'framesCompared': result['framesCompared']}))

if __name__ == '__main__':
    main()
