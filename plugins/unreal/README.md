# plugins/unreal — the Unreal Engine plugins

Two plugins sharing one renderer:

- **VGS** plays VGS captures (4D, from `.vgs` files).
- **VGSStatic** shows static Gaussian splat characters, imported from PLY files as assets
  ([VGSStatic](#vgsstatic-static-characters-from-ply) below).

Both draw through a copy of Epic's Gaussian Splat renderer (`Engine/Plugins/GaussianSplat`,
experimental in 5.9), changed to blend splats as they were trained, with exposure, a grade
and lighting by the scene, which Epic's own lacks. **The copy is Epic's code and has to be
taken again for every engine version: [EPIC_RENDERER_COPY.md](EPIC_RENDERER_COPY.md).** It
no longer needs Epic's GaussianSplat plugin (which only 5.9 has): both plugins build against
Unreal 5.8 and 5.9 (run and tested in 5.9; 5.6 and 5.7 still to be built). Captures play,
can be lit by the scene (*Lighting* Shaded) and cast shadows, as static characters do.
Sixteen captures at
once keep their 30 fps — see *State* below.

```
VGS/                         the plugin as it goes into a project's Plugins folder
  VGS.uplugin                Win64 only; needs no other plugin
  Shaders/Private/VGSPacked.usf  evaluates a packed chunk at an instant, on the GPU
  Shaders/Private/SplatRenderer/ Epic's renderer shaders, copied (EPIC_RENDERER_COPY.md)
  Shaders/Shared/            Epic's record layout and constants, copied (C++ and HLSL)
  Resources/InverseToneCurve Epic's inverse tone curve LUTs, as data the copy builds textures from
  Source/VGSShaders/         the shader's C++ side; loads at PostConfigInit, as global shaders must
  Source/VGSSplatRenderer/   Epic's renderer module, copied and renamed (EPIC_RENDERER_COPY.md)
  Source/VGSRuntime/
    VGSRuntime.Build.cs      why it is treated as an engine module; where the bridge comes from
    Public/VGSCaptureComponent.h   the component: file, playback, splat and rendering settings
    Public/VGSCaptureActor.h       the placeable actor, with an editor sprite to select it by
    Private/VGSPlayer.*            vgsbridge.dll loaded at run time, and a player around it
    Private/VGSSplatLayout.h       plain C++: a frame into the renderer's buffer layouts
    Private/VGSSplatSource.*       render thread: uploads, the evaluation pass, the proxy
    Private/VGSCaptureComponent.cpp time, scheduling, acquiring, packing (CPU)
VGSStatic/                   the static plugin's own files (the shared ones are added when assembled)
  Source/VGSStaticRuntime/   the asset, its GPU copy, the component and actor; VGSStaticPly.h
                             (plain C++: reading a PLY, clusters, normal words)
  Source/VGSStaticEditor/    the PLY importer and its dialog, reimport, placing the asset;
                             VGSStaticNormals.h/.inl (normals for PLYs without them);
                             VGSStaticProxy.h (plain C++: the shadow proxy mesh)
CMakeLists.txt               assembles both plugins (VGS with the bridge beside it) and their zips
EPIC_RENDERER_COPY.md        what of Epic's is copied, what changed, how to update it
tools/                       the copy script, the patch of VGS's changes to the copy, the
                             LUT packer, assemble_vgsstatic.cmake
```

## Building and installing

CMake does not build the plugin; Unreal does, inside a project. CMake assembles it:

```
cmake --build build_win64 --config Release --target vgs_unreal_plugin
```

which gives `build_win64/plugins/unreal/Release/VGS/` (sources, `vgsbridge.h`,
`vgsbridge.dll` and its licence, icon) and `vgs-unreal-<version>.zip` of the same. Copy or
link that `VGS` folder into `<Project>/Plugins/`, or unzip the zip there, and open the
project: Unreal builds the module. Target `vgs_static_unreal_plugin` does the same for
VGSStatic (`VGSStatic/`, `vgsstatic-unreal-<version>.zip`). Both can be enabled in one
project, with Epic's GaussianSplat plugin or without it.
Use the assembled folder, not `plugins/unreal/VGS` from the tree — the bridge's header and
DLL are only in the assembled one. `cmake --install ... --component unreal` copies the zip
to `INSTALL/unreal`.

In a level: *Place Actors*, search *VGS Capture*, drop it, pick the `.vgs` in *File*. A
relative path is looked for under the project's `Content` folder, then the project folder.
For a packaged game the file has to be staged loose, not inside a .pak (the bridge maps the
file itself): put captures under e.g. `Content/VGS` and add that folder to *Project
Settings > Packaging > Additional Non-Asset Directories To Copy*.

## How it works

Epic's renderer draws splats from a world's scene view extension, which runs in the
post-process chain before depth of field. Besides Epic's own components it accepts
**external proxy sources**: a delegate called on the render thread once per view, which may
add passes to the graph and append render proxies (a GPU buffer in its layout, a transform,
a few settings). Niagara's splat renderer uses that hook, and so does this plugin - with
`VGSSplatRenderer`, a renamed copy of that renderer that blends splats in sRGB values, as
captures are trained, instead of in linear light (*Blend Space*; see
[EPIC_RENDERER_COPY.md](EPIC_RENDERER_COPY.md) for why and what changed). The copy draws
only its own sources; Epic's plugin, where it is enabled, draws its assets untouched.

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
- **Colour and exposure**: splats are an emissive of fixed brightness - the blended sRGB
  colour made linear, times `View.PreExposure`. A white of the capture is 1 in Unreal's
  units whatever the lights, so a scene exposed for physical light units (EV100 around 14
  under a sun) shows it nearly black. *Intensity (EV)* scales the linear colour by 2^EV
  where the splats are composited (`FVGSSplatRenderProxy::Intensity`). *Invert UE Tone
  Curve*, on by default, undoes Unreal's filmic tone curve on the splats with Epic's inverse
  LUT, so they show as captured, as in the VGS viewer. *Gamma*, *Contrast* (about mid
  grey), *Saturation* and *Tint* (a multiply, last) grade the capture's sRGB values in the same composite, before they
  are made linear; at 1 they are skipped, and otherwise cost a few operations per pixel. (Before the copy, intensity was put
  into the coefficients as they were packed - `VGSSplatLayout.h`, `gainFor`, still tested -
  which the per-splat clamp of the sRGB blend would now cut.)
- **Lighting** (*Lighting* Shaded): the capture goes into Unreal's GBuffer as an opaque
  Default Lit surface, after the base pass and before lighting
  (`PostRenderBasePassDeferred_RenderThread`), so the scene's lights, shadows falling on it,
  the skylight, Lumen and fog reach it as they reach a mesh. The splats are drawn front to
  back into a scratch target that keeps, per pixel, their coverage, colour, expected depth
  and opacity-weighted view-space normal; `VGSSplatGBuffer.usf` turns that into base colour
  (the graded colour made linear), normal, depth, *Roughness* and *Specular*, and writes
  *Self-illumination Amount* of the colour as emissive. Pixels the splats only partly cover
  are written where they cover half or more; `r.VGSSplat.Lit.Dither 1` writes them at random
  in proportion instead, for TSR to smooth, but TSR leaves it speckled along edges (seen
  2026-11-11 on a static character's trousers and shoes, with and without TSR history), so it
  is off by default; below `r.VGSSplat.Lit.MinCoverage` (2 %) never. Normals are the capture's when it has them
  (*Normals* in the VGS editor's export settings, attribute 48: `VGSPacked.usf` turns each
  into world space with its splat's rotation), otherwise each splat's shortest axis turned
  towards the camera, which is much noisier. Either way a splat's normal is noisy (a splat
  is a blob as often as a patch of surface), so the GBuffer pass averages it over *Normal
  Smoothing* (2 cm by default; `r.VGSSplat.Lit.NormalSmoothing` overrides it for all) on
  screen, among neighbours at the same depth, so a silhouette is not crossed: without it the
  capture looks crumpled and specked under coloured lights. With Substrate (its own material
  buffer, `SUBSTRATE_GBUFFER_FORMAT` 1) the surface is packed as one simple Default Lit slab
  with `PackSubstrateOut`, as the engine's `DebugProbes.usf` stamps its probes, and with
  Substrate's blendable GBuffer through `EncodeGBuffer`. Lighting costs about 0.55 ms a frame at
  1920x1080 on an RTX 4070 (one paused capture of 535k splats: 5.65 ms self-illuminated, 6.20
  ms shaded).
- **Shadows** (*Lighting* > *Cast Shadow* per actor, on by default, shaded or
  self-illuminated). How, for the whole project: Project Settings > Plugins > **VGS**
  (`UVGSSplatSettings`, written to the `r.VGSSplat.Shadow.*` console variables; VGSStatic has
  the same page, *VGSStatic*, for its own actors):
  Every light lights a Shaded actor whichever the method, and every mesh's shadow falls on
  it; the method is only about the shadows the splats cast. In the order the settings list
  them (`r.VGSSplat.Shadow.Method` 0 to 3):
  - *Splat Shadow Map (sun only)*, the default (`VGSSplatLightShadow.cpp/.usf`): the casting
    actors are drawn from the main directional light (`VGSSplat::FindMainLight`: of the
    directional lights that cast shadows, the one marked *Atmosphere Sun Light* with index 0,
    otherwise the brightest; not Unreal's own pick, which is whichever was added first) into
    a shadow map of the splats' own, fitted to them, as
    Gaussians with their real opacity: transmittance multiplied, and the nearest and farthest
    depth along the light, so no sorting. Before post-processing, the light's direct
    contribution (diffuse and GGX specular from the GBuffer, over the atmosphere's
    transmittance) is taken off every pixel behind the splats, in proportion to what they
    block, with a penumbra that widens with the distance behind them by the light's *Source
    Angle* (*Shadow Softness*). A pixel within the splats' depth is the capture itself: left
    alone unless *Self Shadowing* is on, so lit characters show no blotches. *Shadow Texel
    Size* 0.5 cm (up to 2048 texels a side). Cost on the carrito playing (283k splats, 1280x720):
    the map 0.98 ms at 0.5 cm (640x640), 0.60 ms at 1 cm, about 0.4 ms of it per splat
    (vertex work) and the rest fill; the composite 0.03 ms, whatever the screen resolution.
    Only that one light; spot, point and rect lights get no splat shadows. Where the splats'
    shadow falls on another object's shadow the pixel ends darker than the skylight alone
    leaves it (the direct light taken off was not there). With Substrate's own material
    buffer, the colour is darkened by a fixed 60 % instead.
  - *Splat Shadow Map (sun) + Unreal Shadow Maps (other lights)*: both, the second for every
    light but the main one. A shadow depth view looks along its light
    (`FProjectedShadowInfo::ModifyViewForShadow` turns it so, for billboards), so the
    casters leave out the views looking along the main light; with the sun alone the image is
    the first method's (mean difference 0.24 of 255). Level `/Game/Stress/Lit` (a red point
    light beside the sun) shows the point light's shadow added.
  - *Unreal Shadow Maps (all lights)*: the component is a primitive whose only part in
    Unreal's scene is its shadow casters, drawn as VGSStatic's are (below, with the same
    *Shadow Inset*), from the instant on the GPU. For the shadows to cast the frame on screen,
    the instant is uploaded or evaluated before the frame's shadow depths, when the renderer
    starts each view family (`FOnPrepareVGSSplatSources`, `FVGSSplatSource::Prepare_RenderThread`),
    rather than in the view's own pass; RDG keeps the buffer readable from then on
    (`UseExternalAccessMode`), since the shadow vertex factory reads it outside the graph.
    Virtual shadow maps cache nothing of it (*Shadow Cache Invalidation Behavior* Always): it
    changes every frame. Grainy and frayed, and Shaded characters shadow themselves in
    blotches. Cost on the carrito playing (288k splats, one sun, 1280x720): ShadowDepths 0.15
    to 0.57 ms with virtual shadow maps, 0.17 to 0.80 ms with cascades (9M triangles over the
    cascades).
  - *No Shadows*.

  Gracia (docs/research/gracia-20260521) casts its own shadows from one directional light
  only, with up to four cascades; nothing of it was found for spot or point lights.

  Levels `/Game/Stress/Shadow` (and `_Off`, `_Self`, `_Play`); the method can be tried in
  `-ExecCmds` with `r.VGSSplat.Shadow.Method 0/1/2/3`. EEVEE (Blender's splats) does the second
  way with a per-pixel random cutoff, which its accumulation of many samples smooths;
  Unreal's shadow maps keep no history, so it stays grainy here.

- **Normals in the details panel**: *Normals* (under *Capture*) says whether the capture has
  them (on the GPU path), or not, or that they are not used on the CPU path.

## VGSStatic: static characters from PLY

A Gaussian splat PLY (the 3DGS trainer's layout: x y z, f_dc, f_rest, opacity, scale, rot,
optionally nx ny nz; binary or ASCII) dropped in the Content Browser becomes a **VGS Static
Splat** asset; dropped in a level, an actor showing it, with the capture's controls
(*Intensity (EV)*, *Blend Space*, the grade, *Invert UE Tone Curve*, *Lighting* and the
lighting settings, *Up Axis*: Y up, Y down as COLMAP, Z up). Its details panel says how
many splats, the SH degree, and whether it has normals and where they come from.

- **Importer.** `UVGSStaticSplatFactory`, before Epic's `.ply` importer: Unreal picks the
  highest priority and shows no choice, so its dialog has *Import as Epic's Gaussian Splat*
  where Epic's plugin is enabled (5.9), which hands the file to Epic's importer. Only PLYs
  with f_dc_0 and rot_0 are taken; meshes and point clouds are left to others. A script
  imports with an `AssetImportTask` whose `options` is a `VGSStaticImportOptions`.
  Reimport repeats the import with the asset's *Import Settings*.
- **Normals**, worked out once, at import, never per frame:
  - the PLY's own when it has them (SMNForge's and the VGS editor's, from the capture's own
    cameras), as they are;
  - without them, the VGS editor's estimate with a studio rig's cameras, when the PLY names
    its rig (SMNForge writes `comment capture_rig SMNV1` or `THE4DSCANNER`, and `comment
    normals SMNForge` beside the normals it writes) or the import is told one; if that rig's
    cameras see less than half of the splats the PLY is not in its frame, and
  - otherwise (a PLY from anywhere else): the same estimate with **cameras all around**
    instead of a rig's (`gaussiannormals::surroundingViews`: 42 on a Fibonacci sphere about
    the opaque splats, whichever way is up), which decide each splat's outward side as a
    rig's would. On andreavictor (221k splats) against the file's normals (the SMNV1 rig's):
    mean |cos| 0.981 (96.6 % within 30 degrees), with the side 0.917 (94.2 %); 0.35 s.
  - *Method* Shortest axes keeps the earlier way: as PGSR takes a splat's normal (Chen et
    al. 2024), replaced where the splat is round or disagrees with its neighbours by their
    consensus, then smoothed; its side is decided on screen, facing the camera
    (`FVGSSplatRenderProxy::bNormalsFaceCamera`). Rougher: on andreavictor |cos| 0.91 (84 %).
  SMNForge and the VGS editor use the same estimate and the same cameras all around, for
  captures none of our rigs recorded and when a rig's cameras see less than half of them
  (`gaussiannormals::estimateNormals`; the editor's export settings have *Rig* None of ours).
- **Shadow proxy** (the default *Shadow Caster*). The import makes a closed mesh of where the
  splats are opaque (`VGSStaticProxy.h`, plain C++, checked by `tests/unreal_static.cpp`) and
  keeps it in the asset as a static mesh (*Shadow Proxy* > *Mesh*, in the asset's package).
  Each component attaches a hidden `UStaticMeshComponent` with it (transient, made on
  register): never drawn, not even into depth, it casts the character's shadows from every
  light as any mesh does, and with *Proxy Collision* it blocks everything (its triangles as
  collision). The splats then cast nothing themselves.
  - Made as PlayCanvas splat-transform's collision mesh starts (MIT; its GPU voxelisation and
    block clean-up, ported to the CPU on a dense grid): every 1 cm voxel adds up its splats'
    opacity·exp(-d²/2) at its point nearest each one, solid where 1 - exp(-sum) reaches 0.3;
    lone voxels go and single holes close. Then this plugin's: pieces under 0.5 % of the
    largest go (floaters), what the outside cannot reach is filled (the splats are a shell),
    surface nets on the occupancy blurred over 3x3x3 voxels, Taubin smoothing, and 2 cm
    inwards along the normals so that the lit splats are not inside their own caster.
  - On andreavictor (221k splats): 117,000 triangles in 0.3 s, reduced by Unreal to 20,000
    when the mesh is built (its LOD 0 *Reduction Settings*, about 10 s on import). gs2mesh
    (`3dgs2mesh/gs2mesh_cpp`) with only the PLY takes about 60 s: it renders about a thousand
    views of the splats twice and fuses each into a TSDF, then reduces and unwraps, for a
    textured millimetre surface; splat-transform's command line about 1.5 s.
  - Import options *Shadow Proxy* (voxel size, opacity, inset, triangles; centimetres for a
    PLY in metres), kept by the asset for reimport. The mesh is made for the default *Up Axis*
    and *Unit Scale*; a component with others turns and scales it (`ProxyToComponent`).
  - Seen: floor shadows like the splats' own shadow map; every light's (level
    `/Game/Static/Static_Shadow_Point`, the sun and an orange point light); Shaded, the
    character also shadows itself, with somewhat grainy edges where the splats lie in front of
    and behind the proxy. Collision: 14 of 15 traces through the character hit with *Proxy
    Collision*, none without.
- **Shadows** from the splats (*Shadow Caster* *Splats*, or an asset without a proxy), as
  captures' (above; Project Settings > Plugins > *VGSStatic*), by default in
  the splats' own shadow map. With *Unreal Shadow Maps* the component is a primitive whose
  only part in Unreal's scene is its shadow casters (*Lighting* > *Cast Shadow*, on by
  default; off per actor costs nothing): in shadow depth passes every splat is drawn as a
  small opaque octahedron spanning the ellipse where its opacity reaches a cutoff of its own,
  drawn at random (so that on average the shadow is as dense as the splats), read straight
  from the asset's GPU copy by `FVGSSplatShadowVertexFactory` (VGS's own, in the renderer
  copy; `Shaders/Private/SplatRenderer/VGSSplatShadowVertexFactory.ush`), with Unreal's
  default material. It supports GPU Scene, so virtual shadow maps take it as they take
  meshes, and every light that casts shadows does: directional (cascaded or virtual), spot,
  point. Shaded, the character also shadows itself (an arm on the torso); each caster is
  moved *Shadow Inset* (3 cm) inwards along its normal, or the lit surface, which lies among
  its own splats, shadows itself in blotches. Memory: one shared index buffer of 512 KB,
  nothing per asset. Cost on andreavictor (221k splats, one sun, 1280x720): ShadowDepths 0.15
  to 0.57 ms with virtual shadow maps, 0.12 to 0.20 ms with cascades (1.8M triangles in one
  cascade). Levels `/Game/Static/Static_Shadow` (and `_Off`, `_Self`, `_Close`, `_Close_Off`).
- **Static, so:** uploaded once per asset and shared by every actor showing it; the splats
  are ordered along a Morton curve and cut into clusters of 64 that the renderer culls
  against each view (Epic's cluster record, one level of detail); no tick work beyond
  noticing a setting changed from Blueprint; *GPU Layout* Quantized keeps a third of the
  memory (Epic's compressed layout, packed once).
- **Shared code.** The renderer copy, its shaders, the shared headers, the LUTs and
  `VGSSplatLayout.h` are the VGS plugin's, copied when VGSStatic is assembled and renamed
  (`VGSSplat` to `VGSStaticSplat`, `/Plugin/VGS/` to `/Plugin/VGSStatic/`, the shaders
  module) by `tools/assemble_vgsstatic.cmake`, which fails if a name that would collide is
  left: edit them in `VGS/`, never in the assembled copy. The normals estimate is the VGS
  editor's (`editor/gaussiannormals.cpp`, `normalrigs.cpp`, nanoflann), copied in as
  `.inl` files and built with the engine's Eigen; it ships as source with the plugin.
- **Test.** `tests/unreal_static.cpp` (ctest `plugin_unreal_static`): synthetic PLYs, binary
  and ASCII, every value read and packed as the renderer reads it, header comments, clusters
  covering every splat, normal words within 0.0007 rad, shortest-axis normals on a sphere
  with round and turned splats; given a `.ply` it also times reading, building and every
  estimate (each rig, cameras all around, shortest axes) against the file's normals. In the host project, `/Game/Static/Static_Self`, `Static_Neon`,
  `Static_Neon_File`, `Static_Neon_Axes` and `Static_Rim` show `andreavictor`.

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

From Epic's renderer, which the copy keeps (`VGSSplatRenderPipeline.cpp`,
`VGSSplatViewExtension.cpp`):

- Self-illuminated (the default), no lighting and no shadows received (they are cast); no fog,
  Lumen or ray tracing. Splats are composited over translucency, occluded only by opaque
  depth.
- Shaded: lit as an opaque surface, so faint splats, hair and edges are cut where they
  cover half a pixel rather than blended, and its shadows are splat-shaped (see *Known
  limits*). The
  studio's light is already in its colours, so it shows lit twice. Needs the deferred
  renderer (with or without Substrate), hardware rasterization and vertex shaders that read buffers
  (`r.VGSSplat.*` defaults on desktop GPUs); elsewhere a Shaded capture is drawn
  self-illuminated. No velocity and no stencil are written, so a
  moving performer can ghost under TSR. With *Evaluate On* CPU the capture's normals are
  not used (the shortest axis is). The VGS editor's normals of `maria_simbo_carrito`
  (THE4DSCANNER rig) have scattered splats tilted the wrong way, which show as dark specks
  under a strong light; the shading pass turns those clearly facing away from the camera
  (`VGSShadedNormal`), the editor now corrects the worst before writing them, and *Normal
  Smoothing* takes care of most of the rest. Forward shading is not lit (it has no GBuffer):
  a Shaded capture stays self-illuminated there.
- Within the capture splats blend in sRGB values (*Blend Space* sRGB, the default), but the
  capture is composited over the scene in linear light, as any translucent layer: its edges
  against the background are blended as Unreal blends, not as the viewer would.
- Splats go through Unreal's filmic tone curve unless *Invert UE Tone Curve* (on by default)
  undoes it; without it skin turns warmer, darks crush and contrast rises.
- Splats are composited before TSR, at the scene's screen percentage, with no velocity of
  their own: below 100 % they are upscaled, and a moving performer is softened by TSR's
  history. `r.ScreenPercentage 100` keeps them sharp when it can be afforded.
- Velocity is written as zero (camera motion only), so a moving performer can ghost under
  TSR or motion blur. Depth writes are off unless `r.VGSSplat.Depth` is set (the copy has its
  own `r.VGSSplat.*` console variables; Epic's `r.GaussianSplat.*` no longer affect captures).
- Objects are sorted against each other by the centre of their bounds; two captures that
  interpenetrate will show order errors where they overlap.
- The renderer's headers are in `Internal/`, so the modules that use them set
  `bTreatAsEngineModule`. Epic's plugin is experimental in 5.9 and may change between engine
  versions, and the copy with it (EPIC_RENDERER_COPY.md).

- Shadows (captures' and static characters'): the default *Splat Shadow Map* is for the
  main directional light only, and is not seen by translucency, fog or Lumen; *Unreal Shadow
  Maps* (alone, or for the other lights) reach every light but are grainy, and a character
  shadowing another shows dappled where the splats leave gaps.
- A static character's shadow proxy cannot leave out its own splats: Shaded, it always
  shadows itself (Unreal has no per-receiver exclusion), with grainy edges. Thin parts
  thinner than twice the inset (2 cm) cast from a surface turned inside out. Its sizes assume
  a PLY in metres.
- Static characters lit by the scene: thin, faint splats where a shoe meets the floor (the
  scan's contact smear) are written opaque with sideways normals and show as a dark wedge at
  the toe (andreavictor), whichever normals; not addressed yet.

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
