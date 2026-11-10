# plugins/unreal — the Unreal Engine plugin

Plays VGS captures in Unreal Engine 5.9 through Epic's own Gaussian Splat renderer
(`Engine/Plugins/GaussianSplat`, experimental in 5.9). Phase 1: playback, nothing of
lighting or shadows. Draws and plays; sixteen captures at once keep their 30 fps — see
*State* below.

```
VGS/                         the plugin as it goes into a project's Plugins folder
  VGS.uplugin                depends on Epic's GaussianSplat plugin, Win64 only
  Shaders/Private/VGSPacked.usf  evaluates a packed chunk at an instant, on the GPU
  Source/VGSShaders/         the shader's C++ side; loads at PostConfigInit, as global shaders must
  Source/VGSRuntime/
    VGSRuntime.Build.cs      why it is treated as an engine module; where the bridge comes from
    Public/VGSCaptureComponent.h   the component: file, playback, splat and rendering settings
    Public/VGSCaptureActor.h       the placeable actor, with an editor sprite to select it by
    Private/VGSPlayer.*            vgsbridge.dll loaded at run time, and a player around it
    Private/VGSSplatLayout.h       plain C++: a frame into the renderer's buffer layouts
    Private/VGSSplatSource.*       render thread: uploads, the evaluation pass, the proxy
    Private/VGSCaptureComponent.cpp time, scheduling, acquiring, packing (CPU)
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
  ahead of the times the component schedules, on Unreal's task workers (the bridge's
  `vgsb_set_parallel_for`; `vgs.DecodeOnTaskGraph 0` gives them threads of their own). The
  plugin loads `vgsbridge.dll` through its C interface.
- **Evaluating** an instant - every splat's position, rotation, scale, colour and harmonics
  at that time - happens on the GPU by default (*Evaluate On*). The bridge plays the capture
  *packed* (`vgsb_open_packed`): its lanes decompress each chunk once and lay it out as the
  VGS editor's packed viewport and the web viewer read it, and per frame the CPU only
  evaluates the chunk's shared tables at the instant (the basis, under a millisecond). The
  render thread uploads a chunk once (about 34 MB for 290k splats with SH 2, once a second)
  and the basis every frame, and `VGSPacked.usf` writes the renderer's float records
  straight into the buffer it draws from. Records not alive at the instant, or thinned out
  by *Density*, get no size. This is the one place the plugin knows the format: how to read
  a packed chunk, as the web viewer's shader does; decompressing and authenticating stay in
  the bridge.
- **Time** runs with the world's (pause and time dilation apply), from *Phase*, at *Play
  Rate*, looped as the add-on loops, and is **snapped to the capture's frames**. The game's
  frame times are not the capture's, and the bridge decodes ahead exactly the instants it
  was told about; snapped, what was scheduled is what gets asked for. The schedule is the
  next 1.5 s in the direction of play, one step per capture frame or per tick, whichever is
  longer.
- **On the CPU** (*Evaluate On* CPU) the bridge evaluates every frame, about 20 ms of a core
  each, and the game thread packs it into one of the renderer's two layouts
  (`VGSSplatLayout.h` describes them), split over cores with `ParallelFor`. The render
  thread uploads it on the first view of the next frame (RDG, buffers in whole MiB so the
  pool reuses them although the splat count varies). A capture the machine cannot keep up
  with shows the latest frame decoded (`vgsb_acquire_between`) rather than waiting for the
  exact one, so it plays at a lower rate instead of stalling.
- **Waiting**: playing, a tick waits for a late frame or chunk at most *Max Wait Ms*, and all
  captures together at most `vgs.WaitBudgetMs` (10) per game frame.
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

## Scaling

Measured 2026-11-10 on a Ryzen 9 7900X with copies of `maria_simbo_carrito.vgs` (288k
splats, SH degree 2, 30 fps) side by side, in a rendered `-game` window at 1280x720.
Frames shown of 150 due in 5 s, per capture:

| captures | CPU, first version | CPU, now | GPU |
|---|---|---|---|
| 1 | 150, ~245 fps | 150, ~245 fps | 150, ~200 fps |
| 2 | 75-130, ~87 fps | 150, ~200 fps | 150, ~258 fps |
| 4 | 30-70, ~60 fps | ~100, ~41 fps | 150, ~147 fps |
| 8 | 3-8, ~52 fps | ~45, ~17 fps | 150, ~100 fps |
| 16 | | | 150, 60-80 fps |

The bridge alone, flat out on the whole machine, evaluates about 130-150 frames a second
of this capture in all, so on the CPU four captures is the ceiling before Unreal takes its
share. On the GPU the CPU's part is about 2 ms a frame, and what limits is the renderer.
`vgs.EvaluateOn 0/1` switches every capture for comparisons like this one.

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

`tests/unreal_packed.cpp` (ctest `plugin_unreal_packed`, when CMake's `VGS_TEST_CAPTURE`
names a capture) plays the same 20 instants evaluated by the bridge and packed, the packed
ones evaluated by `tests/unreal_packed_reference.h`, a line-for-line C++ twin of
`VGSPacked.usf`. Every live splat agrees: positions 3e-7 of the capture's size, rotations
1e-7, colour 4e-7, scale, opacity and harmonics exactly. A change to the shader is a
change to the twin. On 2026-11-10 the shader's output looked as the CPU's in Unreal.

The rotation error is the quantized layout's own: it stores x, y, z in 11:11:10 bits and
rebuilds w, which is coarse when w is near 0. That is why *GPU Layout* (CPU evaluation
only) defaults to Float:
29 MB a frame at 30 fps is under 1 GB/s, well within a desktop's PCIe; Quantized is there
for several captures at once or a tighter budget.

## State, and what to check first in Unreal

2026-11-09: built against the installed UE 5.9.0 (CL 60351404, `E:\UE_INSTALL\UE_5.9`),
whose GaussianSplat plugin is identical to the 5.9 sources the plugin was written from,
with no errors or warnings, inside a host project (below). It draws the right way up, the
right size and colour. 2026-11-10: playback fixed (it starved: the bridge's own threads
fought the engine's), GPU evaluation added and measured (*Scaling*). The host project has
`/Game/Stress/Stress{1,2,4,8,16}` (copies side by side, each at its own phase) and
`/Game/Stress/Compare` (one capture paused on a frame).

The host project is `D:\Trabajos\THE4DSCANNER\UEVGSProject`, outside this tree: a blank C++
project whose `Plugins\VGS` is a junction to `build_win64\plugins\unreal\Release\VGS`. Its
`Build.bat` reassembles the plugin with CMake and builds the editor target; `OpenEditor.bat`
opens it. Remove the junction with `rmdir`, never with a recursive delete, which would
follow it into the build tree.

What to check, in this order:

1. The module loads and logs `Loaded ...vgsbridge.dll`; a capture opens (*Status* in the
   details panel, `LogVGS`, which also logs a summary of playback every 5 s). Done.
2. It draws, the right way up and the right size (Y Up: the capture's +Z becomes Unreal's
   -X, as Epic turns SPZ files; rotate the actor to face it). Done.
3. Colours match the editor or Blender (the renderer treats DC + 0.5 as sRGB, as every 3DGS
   viewer does). Done by eye.
4. Playback is smooth at 30 fps in the editor's viewport and in PIE, not only in `-game`;
   `stat VGS`, `stat unit`.
5. *Spherical Harmonics* off and on, *Density*, *Evaluate On*, *GPU Layout* switched while
   playing; GPU and CPU the same picture for the same frame.
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
- On the CPU, packing runs on the game thread; moving it to a task would take it off the
  frame. On the GPU there is nothing to pack.
- On the GPU every record of the chunk is sorted and culled by the renderer, the dead ones
  too (with no size); a compaction pass would save the renderer those.
- The plugin's own licence is still to be written (the decoder's is included beside the
  DLL). `VGSPacked.usf` ships as source, as Unreal compiles shaders from source, so how a
  packed chunk is read is readable in every install, as it is in the web viewer.
