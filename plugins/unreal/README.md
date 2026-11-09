# plugins/unreal — the Unreal Engine plugin

Plays VGS captures in Unreal Engine 5.9 through Epic's own Gaussian Splat renderer
(`Engine/Plugins/GaussianSplat`, experimental in 5.9). Phase 1: playback, nothing of
lighting or shadows. **Not yet compiled or run in Unreal** — see *State* below.

```
VGS/                         the plugin as it goes into a project's Plugins folder
  VGS.uplugin                depends on Epic's GaussianSplat plugin, Win64 only
  Source/VGSRuntime/
    VGSRuntime.Build.cs      why it is treated as an engine module; where the bridge comes from
    Public/VGSCaptureComponent.h   the component: file, playback, splat and rendering settings
    Public/VGSCaptureActor.h       the placeable actor, with an editor sprite to select it by
    Private/VGSPlayer.*            vgsbridge.dll loaded at run time, and a player around it
    Private/VGSSplatLayout.h       plain C++: a frame into the renderer's buffer layouts
    Private/VGSSplatSource.*       render thread: upload, and the proxy handed to the renderer
    Private/VGSCaptureComponent.cpp time, scheduling, packing
CMakeLists.txt               assembles the plugin with the bridge beside it, and its zip
```

## Building and installing

CMake does not build the plugin; Unreal does, inside a project. CMake assembles it:

```
cmake --build build_win64 --config Release --target vgs_unreal_plugin
```

which gives `build_win64/plugins/unreal/Release/VGS/` (sources, `vgsbridge.h`,
`vgsbridge.dll` and its licence, icon) and `vgs-unreal-<version>.zip` of the same. Copy or
link that `VGS` folder into `<Project>/Plugins/`, or unzip the zip there, and open the
project: Unreal builds the module. The plugin enables Epic's GaussianSplat plugin itself.
Use the assembled folder, not `plugins/unreal/VGS` from the tree — the bridge's header and
DLL are only in the assembled one. `cmake --install ... --component unreal` copies the zip
to `INSTALL/unreal`.

In a level: *Place Actors*, search *VGS Capture*, drop it, pick the `.vgs` in *File*. A
relative path is looked for under the project's `Content` folder, then the project folder.
For a packaged game the file has to be staged loose, not inside a .pak (the bridge maps the
file itself): put captures under e.g. `Content/VGS` and add that folder to *Project
Settings > Packaging > Additional Non-Asset Directories To Copy*.

## How it works

Epic's renderer draws splats from a world's `FGaussianSplatViewExtension`, a scene view
extension that runs in the post-process chain before depth of field. Besides its own
components it accepts **external proxy sources**: a delegate called on the render thread
once per view, which may add passes to the graph and append `FGaussianSplatRenderProxy`
objects (a GPU buffer in its layout, a transform, a few settings). Niagara's splat
renderer uses that hook (`NiagaraRendererSplats.cpp`), and so does this plugin; nothing of
Epic's plugin is copied or modified.

- **Decoding** is vgsbridge's, as in Blender and Houdini: two lanes per capture decode
  ahead of the times the component schedules. The plugin loads `vgsbridge.dll` through its
  C interface and holds nothing of the format.
- **Time** runs with the world's (pause and time dilation apply), from *Phase*, at *Play
  Rate*, looped as the add-on loops, and is **snapped to the capture's frames**. The game's
  frame times are not the capture's, and the bridge decodes ahead exactly the instants it
  was told about; snapped, what was scheduled is what gets asked for. The schedule is the
  next 1.5 s in the direction of play, one step per capture frame or per tick, whichever is
  longer.
- **Packing** happens on the game thread when the instant changes, split over cores with
  `ParallelFor`, into one of the renderer's two layouts (`VGSSplatLayout.h` describes them).
  The render thread uploads it on the first view of the next frame (RDG, buffers in whole
  MiB so the pool reuses them although the splat count varies) and keeps it for the frames
  and views that show the same instant.
- **Axes**: nothing is converted per splat. The renderer applies the proxy's LocalToWorld
  to positions and to the covariance it builds, and its WorldToLocal to the view direction
  for the harmonics, so the capture's space stays the local space and the turn from the
  capture's axes and metres to Unreal's is one matrix in front of the component transform.
  The harmonics are therefore evaluated in the space they were fitted in.
- **Exposure**: the renderer draws splats as an emissive of fixed brightness - DC + 0.5 as
  sRGB, made linear, times `View.PreExposure` - exactly as it draws Epic's own splat
  assets. A white of the capture is 1 in Unreal's units whatever the lights, so a scene
  exposed for physical light units (EV100 around 14 under a sun) shows it nearly black,
  and the renderer has no intensity control. *Intensity (EV)* is ours: k = 2^EV applied
  to the coefficients as they are packed (each one past DC times k^(1/2.4), DC also
  offset), which makes the linear colour exactly k times brighter above sRGB's toe. In
  the quantized layout it costs nothing: the bytes stay, the scales and biases carry it.

## Tests

`tests/unreal_layout.cpp` (ctest `plugin_unreal_layout`, always runs) compiles
`VGSSplatLayout.h` without Unreal and reads its buffers back with a transcription of the
renderer's shader code:

- the float layout returns every value bit for bit;
- the quantized layout returns every value within half a quantization step;
- *Intensity*: 27k random colours at gains from 1/4 to 30000 come out k times brighter in
  linear light through the shader's colour conversion (worst 5.5e-7 relative), and both
  layouts carry the gain;
- the axes: 2000 random splats, converted one by one as Epic's importer converts an SPZ
  file, against ours (capture space plus the matrix) — same centres, covariances equal to
  1e-15, same view direction for the harmonics.

Given a capture it also packs real frames through vgsbridge. 2026-11-08, `boxingfull.vgs`
(about 200k splats, SH degree 2), one core:

| layout | per frame | packing | worst error |
|---|---|---|---|
| float | 29 MB | 2.7 ms | none |
| quantized | 10 MB | 21 ms | colour 0.008, opacity 0.002, SH 0.002, position 0.7 µm, scale 0.4 %, rotation 0.09 rad |

```
build_win64/tests/Release/vgs_unreal_layout.exe D:/.../boxingfull.vgs
```

The rotation error is the quantized layout's own: it stores x, y, z in 11:11:10 bits and
rebuilds w, which is coarse when w is near 0. That is why *GPU Layout* defaults to Float:
29 MB a frame at 30 fps is under 1 GB/s, well within a desktop's PCIe; Quantized is there
for several captures at once or a tighter budget.

## State, and what to check first in Unreal

Written against the UE 5.9 sources at `E:\UE_INSTALL\5.9SRC`, whose engine was not built,
so the Unreal side has **not been compiled**. Every engine call was checked against the
5.9 headers, but expect a round of compile fixes. Then, in this order:

1. The module loads and logs `Loaded ...vgsbridge.dll`; a capture opens (*Status* in the
   details panel, `LogVGS`).
2. It draws, the right way up and the right size (Y Up: the capture's +Z becomes Unreal's
   -X, as Epic turns SPZ files; rotate the actor to face it).
3. Colours match the editor or Blender (the renderer treats DC + 0.5 as sRGB, as every 3DGS
   viewer does).
4. Playback is smooth at 30 fps and at the game's own rate; `stat unit` with one and four
   captures.
5. *Spherical Harmonics* off and on, *Density*, *GPU Layout* switched while playing.
   *Intensity (EV)* against a lit scene with auto exposure: near the scene's EV100 the
   capture should look as it does at EV100 0.
6. Editor: moving the actor, undo, changing the file, *Play In Editor* on and off, PIE.

## Known limits

From the renderer, not from this plugin (`GaussianSplatRenderPipeline.cpp`,
`GaussianSplatViewExtension.cpp` in Epic's plugin):

- No lighting and no shadows, cast or received; no fog, Lumen or ray tracing. Splats are
  composited over translucency, occluded only by opaque depth.
- Velocity is written as zero (camera motion only), so a moving performer can ghost under
  TSR or motion blur. Depth writes are off unless `r.GaussianSplat.Depth` is set.
- Objects are sorted against each other by the centre of their bounds; two captures that
  interpenetrate will show order errors where they overlap.
- Epic's headers are all in `Internal/`, so `VGSRuntime` sets `bTreatAsEngineModule`. The
  plugin is experimental in 5.9 and its interface may change between engine versions.

From this plugin, for later phases:

- No Sequencer track yet; `SetPlaybackTime` from Blueprint or a level sequence event is
  the way for now.
- Win64 only (vgsbridge is built for it).
- Packing runs on the game thread; moving it to a task would take it off the frame.
- The plugin's own licence is still to be written (the decoder's is included beside the
  DLL).
