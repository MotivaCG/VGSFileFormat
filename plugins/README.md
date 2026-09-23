# plugins/ — host integrations

```
vgsblender/     the native library: the decoder, and the threads that play ahead of Blender
  include/vgsblender.h    its C interface, and why it is shaped the way it is
  src/vgsblender.cpp
blender/        the Blender add-on, and the build step that packages it
  vgs/                    the add-on itself (Python)
  INSTALL.txt             how a user installs it, in English; installed beside the zip
  README.md               using it, how it was tested, known behaviour, what is left
houdini/        the Houdini plugin: an HDK object and SOP with the same player inside
  src/                    the C++ (HDK)
  package/                the Houdini package around it: menu entry, help, icon
  INSTALL.txt             how a user installs it, in English; installed beside the zip
  README.md               using it, what it writes, how it was tested, what is left
```

Built with everything else (`VGS_BUILD_PLUGINS`, on by default, native builds only):

```
cmake -S . -B build_win64 -G "Visual Studio 17 2022" -A x64
cmake --build build_win64 --config Release
```

The result is `build_win64/plugins/blender/Release/vgs-<version>.zip`, which is what a
user installs: Blender 5.3 or later, *Edit > Preferences > Get Extensions > ▾ > Install
from Disk*, or drag it onto Blender's window. `cmake --install` copies it to
`INSTALL/blender`, beside `INSTALL/decoder` and `INSTALL/encoder`; `--component blender`
installs it alone. The unpacked `vgs/` folder beside the zip is the same thing for
development.

The Houdini plugin is built when CMake finds a Houdini to build against - `VGS_HOUDINI_ROOT`,
`HFS`, or the newest in the default install folder - and skipped otherwise:

```
cmake -S . -B build_win64 -DVGS_HOUDINI_ROOT="C:/Program Files/Side Effects Software/Houdini 22.0.429"
```

Its result is `build_win64/plugins/houdini/Release/vgs-houdini-<version>-h<houdini>.zip`,
unzipped into the user's Houdini `packages` folder; `cmake --install` copies it to
`INSTALL/houdini` (`--component houdini`). `houdini/README.md` says more.

## Two licences, one line between them

The add-on is Python written against Blender's API, and the Blender Foundation's position
is that anything written against that API is GPL; it is licensed GPL-3.0-or-later and
carries the licence text. The native library holds the decoder and is not GPL: it ships
under the decoder's own terms, inside the add-on's zip as a separate file in `bin/`.

Keep that line where it is. Nothing of the format belongs in the Python: it opens a
capture, says which instants it wants and copies arrays it is handed. Everything that
knows how a capture is encoded is in the library, and the library knows nothing of
Blender beyond the attribute layout it fills.

The library compiles the decoder's source list itself rather than linking libvgsdecoder,
so that it can use the static C runtime and load on a machine without the Visual C++
redistributable. It is therefore its own artefact, and `keyleak_blender` checks it for
the signing key like the other decoder builds.

The Houdini plugin has no such line to keep: an HDK plugin is not bound by the GPL, and
it is closed like the decoder. It compiles the same player (`vgsblender.cpp`, with
`VGSBLENDER_STATIC`) into its DSO, so `keyleak_houdini` checks that too.

## How playback works

A capture becomes a point cloud of type *Gaussian Splat*. Blender draws that natively
from named attributes — `position`, `rotation`, `scale`, `radiance:base`,
`radiance:sh_0..n` — so the add-on only has to fill them on every frame change.

- The library runs **two lanes per capture**, each a `vgsdec::Capture` on its own thread:
  one takes the even chunks and one the odd. While one evaluates the frames of the chunk
  being played, the other decompresses the next, so a chunk boundary costs nothing.
  Forwards or backwards makes no difference. Two chunks are held decoded.
- Frame evaluation is itself split over cores (`Capture::setThreadCount`), which is what
  brought a frame with harmonics from 27 ms to about 6.
- The add-on tells each capture which instants playback will ask for — the next frames
  and a second and a half ahead, looping as the timeline loops — and then asks for the
  current one, which is normally ready.
- Frames arrive shaped for Blender: dead records dropped, rotations wxyz, harmonics one
  plane per coefficient. Blender quantises attributes against their range over every
  point, and a dead record with a zero scale would ruin it for all the others.
- **Harmonics are off while the timeline plays** and come back when it stops, when a
  frame is set by hand and when rendering (per capture: *Harmonics: Off While Playing*, on
  by default). When a frame arrives without them the `radiance:sh_*` attributes are
  removed, so what plays is base colour, never stale harmonics from another frame.
- Each capture has a phase, a speed and a loop mode (From Capture, the playback mode the capture carries, or No Loop, Loop, Ping-Pong to override it). The
  phase is where in the capture the scene's first frame falls, shown twice in the panel:
  as a fraction (*Phase*) and in the capture's frames (*Start Frame*). Only the fraction is
  stored and the frame is computed from it, so the two cannot disagree. A negative speed
  plays backwards from there; decoding ahead follows the
  instants playback will ask for, so backwards and ping-pong are as smooth as forwards.
- **Viewport Density** (per capture, 0.01 to 1) draws a fraction of the splats in the
  viewport, paused or playing; renders always draw all of them. The slider is mapped onto
  [cbrt(0.01), 1] and cubed, so it runs 1 to 0.01 like the fraction but falls faster (0.5
  draws about a fifth): the measurements below show the gain arriving from a quarter
  down, and a linear slider left all of it in the last stretch. The library picks which
  by a hash of each record's index, stable within a chunk, and raises the opacity of the
  ones it keeps to 1 - (1 - opacity)^(1 / density) so a thinned capture stays solid.
  Thinning is one more test in a loop that already runs, so it never costs more than it
  saves. Renders are recognised from `render_init` to `render_complete`/`render_cancel`
  rather than by `bpy.app.is_job_running`, which is false for a command-line render and
  would have let the render pipeline's own frame change thin the frame again.
- Several captures play side by side, each with its own lanes. The add-on schedules all
  of them before waiting for any, so they decode at once.
- By default the decoded splats are kept out of saved `.blend` files; the capture is a
  reference to its file and is decoded again on load.

Measured in Blender 5.3 with `boxing_despill.vgs` (about 240k splats, harmonics of degree
2, 30 fps), paced at 30 fps, 24-thread desktop — time spent in a frame change:

| captures | median | 99th percentile | frames over 33 ms |
|---|---|---|---|
| 1 | 3.7 ms | 6.5 ms | 1 of 426 |
| 2 | 8.4 ms | 12.1 ms | 1 of 426 |
| 4 | 17.1 ms | 29.0 ms | 2 of 426 |

With the interface, EEVEE's Rendered view, harmonics off while playing, 1600×1000 window,
playing as fast as it can (120 fps cap) — what *Viewport Density* buys:

Densities here are the fraction drawn, not the slider (slider 0.74 is 0.50, 0.53 is 0.25).

| captures | density | fps | frame | add-on's share | splats drawn |
|---|---|---|---|---|---|
| 1 | 1.00 | 99 | 9.8 ms | 2.3 ms | 235k |
| 1 | 0.50 | 116 | 8.4 ms | 1.6 ms | 120k |
| 1 | 0.25 | 120 (cap) | 8.3 ms | 0.9 ms | 61k |
| 4 | 1.00 | 29 | 32.8 ms | 10.2 ms | 767k |
| 4 | 0.50 | 35 | 28.5 ms | 7.3 ms | 396k |
| 4 | 0.25 | 58 | 16.9 ms | 4.2 ms | 206k |

One capture does not need it on this machine. With several it matters, but half the
splats is not half the time: the opacity the kept splats are given makes each one's
footprint on screen larger, so the pixels drawn fall more slowly than the splats (a
likely explanation, not a measured one). At 0.5 the difference is hard to see; at 0.25 a
capture looks brushier but stays solid.

Paused, a frame with harmonics takes about 10 ms per capture. The viewport's own drawing
— uploading and sorting the splats on the GPU — comes on top and is not in these
numbers.

## One thing that cost a factor of two hundred

`numpy.ctypeslib.as_array` over a `float *` exports its buffer as `'<f'`. Blender's
`foreach_set` only takes its memcpy path for `'f'`, and copies element by element
otherwise: 19 ms per attribute instead of 0.09. `native.py` reinterprets the view with
`.view(numpy.float32)`, which costs nothing and fixes it. Keep it.
