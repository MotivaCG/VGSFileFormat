# The Blender add-on — working notes

What the add-on is and how playback works is in [../README.md](../README.md). This file is
the rest: how to use it, how it was tested, what is known to be odd, and what is left.

Available free of charge. The Python add-on is GPL-3.0-or-later with the native
linking permission in [vgs/LICENSE.md](vgs/LICENSE.md); the bundled closed-source
library has its own VGS Decoder licence in `bin/LICENSE.md`. Both include
warranty and liability limitations, subject to applicable law.

## Using it

Blender 5.3 or later, Windows x64.

- **Install:** drag `vgs-<version>.zip` onto Blender, or *Edit > Preferences > Get
  Extensions > ▾ > Install from Disk*. After reinstalling, **restart Blender**: a running
  Blender keeps the old `vgsblender.dll` loaded, and the new Python can end up talking to
  the old library.
- **Import:** *File > Import > VGS (.vgs, .pgs)*, or drop one or more captures on the 3D
  viewport. Each becomes a point cloud of type Gaussian Splat. Import options: harmonics,
  loop mode, up axis (Y Up by default: +90° about X, which is how captures
  are written), fit the scene's range and frame rate to the first capture.
- **Per capture**, in *Properties > Data > VGS*, under the logo (`vgs/logo.png`: a
  256-pixel copy of the project's `logo_trans.png`, cropped to the mark, transparent for
  Blender's dark interface): capture path, phase and start frame (the same setting
  twice: where in the capture the scene's first frame falls, as a fraction and as a
  capture frame counted from 0), speed
  (negative plays backwards from the phase), loop (From Capture, No Loop, Loop, Ping-Pong),
  viewport density (0.01 to 1, falling faster than the slider: 0.5 draws about a fifth
  of the splats in the viewport; renders draw them all),
  spherical harmonics, *Harmonics: Off While Playing* (on by default), *Fit Scene*,
  *Reload*.
- **Preferences** (*Add-ons > VGS*): frames decoded ahead per capture, decoder threads
  (0 is half the cores), how long playback waits for a late frame, and whether decoded
  splats are kept out of saved `.blend` files (on by default).

The capture is a reference to its file. Moving or renaming the file breaks it; *Reload*
after fixing the path.

## Scatter

*Distribution* in the capture's panel holds a note and a button, *Sample: Scatter on
Points* (also *Object > VGS Scatter on Points*). The note says what it is: a sample, one
Geometry Nodes scatter of the capture over the points, and an invitation to build one's
own distribution. It is wrapped to the panel's width, since a label does not wrap itself.

The button puts a copy of the active capture on every point of the other selected object
(mesh, point cloud or curves), for variety at a distance from a few captures - the idea of
the Houdini plugin's scatter, done the Blender way:

- **Variants** are point clouds of their own, copies of the capture's settings at phases
  spread over it and at speeds varied around a speed, deterministic in a seed. They sit in
  a collection named after the scatter, excluded from the view layer, and play like any
  capture: each has its player, so a frame costs one decode per variant, all decoding at
  once.
- A **Geometry Nodes modifier** on the points object, *VGS Scatter*, instances them:
  Collection Info (separate children, children keep their transforms - the capture's
  up-axis turn) into Instance on Points with Pick Instance, then Scale Instances.
- **Everything is set on the modifier**: *Variants*, *Phase Spread*, *Speed*, *Speed
  Variation*, *Seed*, *Random Z Rotation*, *Inherit Object Scale*, *Follow Normal*, *Show
  Original Geometry*. The first five shape the
  variants, which are point clouds rather than nodes, so a `depsgraph_update_post` handler
  compares them - and the capture's own settings and transform - with what the variants
  were last built from, and rebuilds them when they differ: changing the count adds or
  removes point clouds, changing the capture's loop or density passes it on.
- **Which variant**: the point's `vgs_variant` attribute when it has one, random otherwise.
- **Turn**: the point's `rotation` (quaternion) when it has one; otherwise a random angle
  about the vertical axis up to *Random Z Rotation* either way, 360 degrees by default,
  different for every copy; 0 leaves them unturned.
- **Follow Normal** (off by default) stands each copy along the surface's normal instead
  of straight up. The random turn comes first, about the copy's own vertical axis, and
  Align Rotation to Vector then takes Z to the normal by the shortest rotation, which keeps
  that turn: on a slope a copy still spins on itself. The normal is the point's `normal`
  attribute if it has one, the mesh's vertex normal otherwise; points from Distribute
  Points on Faces carry none, so store its Normal output as `normal`. Checked on an ico
  sphere: every copy's up within 0.00 degrees of its normal, every copy facing its own way.
- **Scale**: the point's `scale` (vector) when it has one. The points object's own scale is
  not passed on: instances live in that object's space, so a Scale Instances node undoes it
  with the inverse of the object's scale (Self Object -> Object Info), about each copy's
  own position and in the object's space - exact for a non-uniform scale and a turned
  copy, while the copies' places still follow the scaled surface. *Inherit Object Scale*
  brings it back. The object's rotation is passed on.
- **The object's own geometry** is joined to the copies, so the plane stays visible;
  *Show Original Geometry* turns it off.
- **No greyed-out inputs.** Blender greys an input the nodes do not reach, and *Phase
  Spread*, *Speed* and *Speed Variation* are read by the add-on, not the nodes; each copy
  stores them as instance attributes (`vgs_phase_spread`, `vgs_speed`,
  `vgs_speed_variation`), which makes them count as used and records what a copy was made
  with.
- Copies are **instances**: no splat is copied per copy. EEVEE and Cycles both draw
  instanced Gaussian splat point clouds; checked before building this.
- Running the button again on the same points replaces the scatter; *Object > Remove VGS
  Scatter* takes it off, variants and all.

Tested in the background: with the modifier's inputs changed and the object tagged, as the
interface does, Variants 8 -> 5 left five variants at phases 0, 0.2 ... 0.8; Speed 1.5 with
0.2 variation gave speeds 1.28 to 1.77; the capture's loop set to ping-pong reached every
variant; the turn at 180 degrees gave 20 copies 20 different angles, at 0 one; a points
object scaled 2 had copies at scale 1, and at 2 with Inherit Object Scale. With the
7-second boxing capture, 8 variants and 20 points: 165 ms a frame paused, harmonics on.

## Settings that are deliberate

| Setting | Value | Why |
|---|---|---|
| Up axis on import | Y Up | Captures are Y up. Rotating the object is exact and free; converting the data would mean rotating quaternions and harmonics too. |
| Loop | From Capture | The capture carries its author's playback mode (once, loop, ping-pong; loop unless set). The other entries override it. |
| Harmonics: Off While Playing | on | A third of the data to copy per frame; harmonics come back when paused, scrubbed or rendered. |
| Keep splats out of `.blend` | on | A frame is tens of megabytes and is decoded again on load. |
| Frames decoded ahead | 4 | About 240 bytes per splat per frame with harmonics. |
| Viewport Density | 1 | Full quality unless asked; worth lowering with several captures (see ../README.md). |

Renaming a property of `VGSCaptureSettings` orphans what older `.blend` files stored under
the old name: `loop` became `loop_mode` when it grew a third option, and captures
imported before that open with the default. Add a new name rather than change the type of
an existing one.

## Known behaviour

- **EEVEE draws nothing for a moment the first time it sees a new set of attributes** —
  the first frame without harmonics, the first with them. It is compiling the material's
  shader; it caches it, and the next time is instant. Cycles is unaffected.
- **EEVEE, paused, blotchy colours (reported once, not reproduced).** On a fresh
  factory-startup scene, EEVEE's Rendered view of a paused frame with harmonics matches
  Blender's own PLY importer on the same frame exactly (exported with `vgsexport`), so the
  attributes are right. The report came from the user's own session after reinstalling
  without a restart; the stale DLL is the first suspect, the startup file's EEVEE settings
  the second. If it comes back: untick *Harmonics: Enabled* while paused, and try
  *File > New > General* with Blender freshly started.
- EEVEE quantises every harmonic coefficient to 8 bits against one range shared by all
  splats and coefficients. A capture with a few extreme coefficients would lose precision
  everywhere in EEVEE and look fine in Cycles. Not seen with the test captures; worth
  knowing if colours ever look posterised only in EEVEE.

## How it was tested

Test capture: `D:\Trabajos\ScanMeNow\SMNWebviewer\Playcanvas\dist\data\boxing_despill.vgs`
(7.07 s, 7 chunks, about 240k splats, harmonics of degree 2, 30 fps). The smaller
`build_tests/tests/roundtrip.vgs` and `.pgs` exist once the tests have run with a `.mint`.

- **In the background:** `blender --background --factory-startup --python script.py`, with
  the unpacked add-on on `sys.path` (`build_win64/plugins/blender/Release`) and
  `import vgs; vgs.register()`. Playback is simulated with `scene.frame_set` paced at
  30 fps and `playback._playing` patched to return True. Keep the frames inside the
  scene's range: past `frame_end` the look-ahead wraps to the start of the range and
  mispredicts, which looks like stalls at every chunk boundary and is not a bug.
- **As installed:** with `BLENDER_USER_RESOURCES` pointing at an empty folder,
  `blender --command extension install-file -r user_default --enable vgs-<version>.zip`,
  then a script that imports through the real package (`bl_ext.user_default.vgs`).
- **Renders draw every splat:** at viewport density 0.25, a still render and a two-frame
  animation render from the command line (`bpy.ops.render.render` in the background) both
  drew all of them, and the viewport went back to 0.25 afterwards.
- **The package itself:** `blender --command extension validate vgs-<version>.zip`.
- **With the interface:** a script that queues steps with `bpy.app.timers` (import, play,
  stop) and saves `bpy.ops.screen.screenshot_area` images. It is the only way to see what
  EEVEE draws.
- **The decoder's parallel evaluation** was checked bit for bit: every array of 60
  instants, with and without harmonics, hashed with 1, 2, 8 and 16 threads, against the
  single-threaded decoder from before the change. All identical. The WebAssembly build was
  checked the same way under node (built with `-DVGS_WASM_NODE=ON`): `positionsAt` and
  `setTime` from the previous commit's module and the new one, 133 instants, identical.

## Left to do

- **A repository on the website**, so users add one URL once and get updates: host the
  zips and the `index.json` that `blender --command extension server-generate` writes. A
  link ending in `?repository=<url of index.json>` can then be dragged from a browser into
  Blender to install. Could be a CMake target that leaves the folder ready to upload.
- **`website`** (and perhaps `copyright`) in `blender_manifest.toml`.
- **Signing `vgsblender.dll`** with a code-signing certificate, which quietens some
  antivirus products about an unsigned DLL.
- **Linux and macOS**: the library already builds as `.so`/`.dylib` names and `native.py`
  looks for them; the zip would be one per platform, with `platforms` set to match.
