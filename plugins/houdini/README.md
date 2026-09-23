# The Houdini plugin — working notes

An HDK plugin for Houdini 22.0: a **VGS Capture** object and a **VGS Capture** SOP. The
same player as the Blender add-on (`plugins/vgsblender`) is compiled into it, so it plays
captures the same way; what differs is how the frames get into the host.

## Using it

Install: unzip `vgs-houdini-<version>-h22.0.zip` into the `packages` folder of the
Houdini preferences (`$HOUDINI_USER_PREF_DIR/packages`: on Windows
`Documents\houdini22.0\packages`, on Linux `~/houdini22.0/packages`; `hou.getenv(
"HOUDINI_USER_PREF_DIR")` in Houdini's Python Shell says which).
That gives `packages/vgs.json` and `packages/vgs/`, and Houdini picks it up on its next
start. The package file enables itself in Houdini 22.0 only, which is the version the DSO
was built for.

- **File > Import > VGS Capture (.vgs, .pgs)...** creates one VGS Capture object per file,
  and fits the scene's frame rate and range to the first, as Blender's import does.
- The object carries the controls, above the usual geometry object tabs. The SOP inside it
  does the work; its parameters are channel references to the object's.
- The SOP also works on its own: Tab > Import > VGS Capture in any SOP network, or in a
  SOP Create LOP. Then its own parameters drive it.
- Middle-click the SOP for the capture's metadata, duration, splat count and the instant
  shown.

The controls, and what they correspond to in Blender:

| Houdini            | Blender                | Notes                                         |
|--------------------|------------------------|-----------------------------------------------|
| Capture, Reload    | Capture path, Reload   |                                               |
| Phase, Start Frame | Phase, Start Frame     | kept in step; only the phase decides          |
| Speed              | Speed                  | negative plays backwards from the phase       |
| Loop               | Loop                   | From Capture (default) / No Loop / Loop / Ping-Pong |
| Viewport Density   | Viewport Density       | same eased field; renders always full         |
| Up Axis            | import's Up Axis       | Y Up is no turn here: Houdini is Y up         |
| Harmonics, Off While Playing | Harmonics    | both on by default                            |
| Fit Scene          | Fit Scene              | sets the frame rate exactly, fractions too    |
| Linearize Color    | —                      | Houdini wants scene-linear `Cd`               |
| Cast Shadows in Karma | —                   | off, as Bake GSplats leaves it                |
| `VGS_SLOTS`, `VGS_THREADS`, `VGS_PLAYBACK_WAIT_MS` | add-on preferences | environment variables, defaults 4 / 0 / 250 |

Blender's *strip splats on save* has no counterpart: a SOP's cooked geometry is not saved
in the .hip unless the node is locked.

## What it writes

Exactly what **Bake GSplats** writes for a `.ply`: the attributes, their sizes and type
infos were read off that node's output, not guessed.

- points only, no primitives;
- `P`; `Cd` (colour, scene linear through the session's OCIO config, as Bake GSplats'
  `ocio_transform("sRGB", "scene_linear", ...)` - checked equal to it to the bit);
  `orient` (quaternion, xyzw); `scale` (activated); `GS_Alpha` (activated opacity);
- with harmonics, `GS_SPH_R/G/B` (16 floats each: the DC term, then the coefficients,
  zeros past the capture's degree) and `restorient`;
- detail `karma:object:rendervisibility = "-shadow"` unless Cast Shadows is on;
- detail `vgs_seconds`, the capture time shown.

Houdini takes it from there: the viewport draws GSplats natively, and SOP Import turns it
into a `ParticleField3DGaussianSplat` for Solaris and Karma, harmonics included.

## Playback and renders

The same rules as Blender, with Houdini's hooks:

- **playing** is `UT_Playback::isPlaying()`. When the playbar stops, every capture is
  recooked, so the frame it stops on gets its harmonics back.
- **rendering** is any of: a ROP render under way, an object cooked for a render, or no
  interface at all (hbatch, hython, a farm). Renders get every splat and every harmonic.
- Houdini does not cook a SOP again when nothing about it changed, so a render at the
  frame the viewport shows would reuse the viewport's thinned splats. The plugin hooks
  every ROP as it is created or loaded (`addGlobalOpChangedCallback`, then
  `addRenderEventCallback`) and recooks every capture at pre-render and again at
  post-render, as the Blender add-on does at `render_init` and `render_complete`. The ROPs
  inside LOP nodes, Karma's among them, are ROPs too and are caught the same way.
- Playing, a frame not decoded within `VGS_PLAYBACK_WAIT_MS` leaves the previous one
  showing; paused, scrubbed or rendered, the SOP waits for the exact frame.

## Settings that are deliberate

- **The player is a static library built with this project's flags**, and only the SOP
  and object are compiled with the HDK's (C++20, its defines). Both use the release DLL
  runtime in every configuration, as the HDK requires.
- **The object's own parameter table.** Its controls come before the geometry object's
  parameters, which moves those; `OBJ_Geometry` caches parameter positions in a table
  shared by every geometry object, so the object overrides `getIndirect()`.
- **Phase and Start Frame are kept in step in `opChanged`**, not in parameter callbacks,
  so it holds however a field is set: by hand, from Python, from a preset. Not when either
  is animated or channel-referenced, as the SOP inside the object is.
- **One DSO per Houdini version.** `VGS_HOUDINI_ROOT` (or `HFS`) picks the Houdini to build
  against; the zip's name carries the version, and the package file enables itself for
  that version only.

## How it was tested

With Houdini 22.0.429, Apprentice licence, and the test capture `boxing_despill.vgs`:

- in hython: attribute set and types compared with Bake GSplats; colour compared with VEX
  `ocio_transform` (difference 0.0); Phase 0.5 → Start Frame 106, Start Frame 40 → Phase
  0.1887, 9999 → 212, from Python `set()` calls; the three loop modes both ways give the
  same capture frames as Blender; Up Axis bounds; about 13 ms a frame with harmonics
  (~200k splats) stepping through frames;
- in a GUI session: Viewport Density 0.25 draws 13,997 splats of 207,729; a Geometry ROP
  rendering the SOP at the frame shown writes all 207,729, and the viewport is back to
  13,997 afterwards; a `usd_rop` in /stage writes a `ParticleField3DGaussianSplat` of
  207,729 with harmonics; a Karma LOP render recooks the SOP at its start and end;
- `keyleak_houdini` checks the DSO for the signing key.

Not tested automatically, to be looked at by hand: playback frame rate and harmonics off
while playing (a GUI session started from a script would not advance the playbar), the
File > Import menu entry, the node icons and the parameter layout on the object.

## Left to do

- Playback measurements in the GUI, as done for Blender.
- Drag and drop of `.vgs` files onto the viewport (`externaldragdrop.py` would replace
  Houdini's own handler, so it needs care).
- Builds for other Houdini versions, Linux and macOS.
- Houdini writes `.hipnc` under Apprentice; nothing in the plugin depends on the licence.
