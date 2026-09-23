# The Blender add-on — working notes

What the add-on is and how playback works is in [../README.md](../README.md). This file is
the rest: how to use it, how it was tested, what is known to be odd, and what is left.

## Using it

Blender 5.3 or later, Windows x64.

- **Install:** drag `vgs-<version>.zip` onto Blender, or *Edit > Preferences > Get
  Extensions > ▾ > Install from Disk*. After reinstalling, **restart Blender**: a running
  Blender keeps the old `vgsblender.dll` loaded, and the new Python can end up talking to
  the old library.
- **Import:** *File > Import > VGS (.vgs, .pgs)*, or drop one or more captures on the 3D
  viewport. Each becomes a point cloud of type Gaussian Splat. Import options: harmonics,
  start frame, loop mode, up axis (Y Up by default: +90° about X, which is how captures
  are written), fit the scene's range and frame rate to the first capture.
- **Per capture**, in *Properties > Data > VGS*: capture path, start frame, speed
  (negative plays backwards from the last instant), loop (No Loop, Loop, Ping-Pong),
  spherical harmonics, *No Harmonics While Playing* (on by default), *Fit Scene*,
  *Reload*.
- **Preferences** (*Add-ons > VGS*): frames decoded ahead per capture, decoder threads
  (0 is half the cores), how long playback waits for a late frame, and whether decoded
  splats are kept out of saved `.blend` files (on by default).

The capture is a reference to its file. Moving or renaming the file breaks it; *Reload*
after fixing the path.

## Settings that are deliberate

| Setting | Value | Why |
|---|---|---|
| Up axis on import | Y Up | Captures are Y up. Rotating the object is exact and free; converting the data would mean rotating quaternions and harmonics too. |
| Loop | Loop | Captures are short takes; holding the last frame surprised people. |
| No Harmonics While Playing | on | A third of the data to copy per frame; harmonics come back when paused, scrubbed or rendered. |
| Keep splats out of `.blend` | on | A frame is tens of megabytes and is decoded again on load. |
| Frames decoded ahead | 4 | About 240 bytes per splat per frame with harmonics. |

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
  the second. If it comes back: toggle *Spherical Harmonics* off while paused, and try
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
- **The package itself:** `blender --command extension validate vgs-<version>.zip`.
- **With the interface:** a script that queues steps with `bpy.app.timers` (import, play,
  stop) and saves `bpy.ops.screen.screenshot_area` images. It is the only way to see what
  EEVEE draws.
- **The decoder's parallel evaluation** was checked bit for bit: every array of 60
  instants, with and without harmonics, hashed with 1, 2, 8 and 16 threads, against the
  single-threaded decoder from before the change. All identical.

## Left to do

- **A repository on the website**, so users add one URL once and get updates: host the
  zips and the `index.json` that `blender --command extension server-generate` writes. A
  link ending in `?repository=<url of index.json>` can then be dragged from a browser into
  Blender to install. Could be a CMake target that leaves the folder ready to upload.
- **The library's licence in the zip**, as `bin/LICENSE.md` with the decoder's terms, so
  it is plain that the DLL is not covered by the GPL that `LICENSE.txt` gives the Python.
- **`website`** (and perhaps `copyright`) in `blender_manifest.toml`.
- **Signing `vgsblender.dll`** with a code-signing certificate, which quietens some
  antivirus products about an unsigned DLL.
- **Linux and macOS**: the library already builds as `.so`/`.dylib` names and `native.py`
  looks for them; the zip would be one per platform, with `platforms` set to match.
