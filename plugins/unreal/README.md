# plugins/unreal — the Unreal Engine plugins

Two plugins sharing one renderer:

- **VGS** plays VGS captures (4D, from `.vgs` files).
- **SGS** shows static Gaussian splat characters, imported from PLY files as assets
  ([SGS](#sgs-static-characters-from-ply) below).

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
SGS/                   the static plugin's own files (the shared ones are added when assembled)
  Source/SGSRuntime/   the asset, its GPU copy, the component and actor; SGSPly.h
                             (plain C++: reading a PLY, clusters, normal words)
  Source/SGSEditor/    the PLY importer and its dialog, reimport, placing the asset;
                             SGSNormals.h/.inl (normals for PLYs without them);
                             SGSProxy.h (plain C++: the shadow proxy mesh)
CMakeLists.txt               assembles both plugins (VGS with the bridge beside it) and their zips
EPIC_RENDERER_COPY.md        what of Epic's is copied, what changed, how to update it
tools/                       the copy script, the patch of VGS's changes to the copy, the
                             LUT packer, assemble_sgs.cmake
```

## Dependent controls

Dependent controls stay visible but grey out when they do not apply, retaining their values:

- Shaded material controls (self-illumination amount, roughness, specular and normal smoothing)
  require Lighting **Shaded**. Intensity (EV) needs self-illumination, either as the lighting mode
  or as a nonzero amount in Shaded.
- Invert UE Tone Curve applies to what shows the capture's own colour: **Self-illumination**, or the
  Self-illumination Amount part of **Shaded** (never the lit colour), so that Shaded at an amount of 1
  looks the same as Self-illumination. Its strength needs the switch on;
  Invert Default Expand Gamut also needs nonzero strength.
- A capture trained with anti-aliasing (the *anti-aliased splats* render hint, every one
  converted from Gracia `.mint`; `vgsb_render_hints`) is drawn with Mip-Splatting's opacity
  compensation: the dilation that widens a splat to a pixel lowers its opacity by the square root
  of its footprint's determinant before over after (`VGSSplatShading.usf`, `ProjectVGSSplat`), as
  the VGS editor and PlayCanvas do. Without it the needle splats along silhouettes show as solid
  coloured lines (a red edge down a white robe).
- A capture's GPU Layout applies to **CPU** evaluation, including the `vgs.EvaluateOn` override.
- Shadow Inset requires dynamic shadow casting and is inactive for project **No Shadows**.
  It remains available for the native
  fallback of Splat Shadow Map; the own map's bias is the project's Shadow Bias.
- Proxy Collision needs an asset with a proxy mesh, independently of shadow casting
  and whether shadows are enabled. Selecting Proxy Mesh without a mesh still falls back to splats.
- Import/reimport normal neighbours and radius apply only to **Shortest axes (PGSR)** with
  normal estimation enabled. The proxy-building controls require Make Collision Proxy.

These are editor dependencies; changing modes does not erase saved values or alter Blueprint access.

## Building and installing

CMake does not build the plugin; Unreal does, inside a project. CMake assembles it:

```
cmake --build build_win64 --config Release --target vgs_unreal_plugin
```

which gives `build_win64/plugins/unreal/Release/VGS/` (sources, `vgsbridge.h`,
`vgsbridge.dll` and its licence, icon) and `vgs-unreal-<version>.zip` of the same. Copy or
link that `VGS` folder into `<Project>/Plugins/`, or unzip the zip there, and open the
project: Unreal builds the module. Target `sgs_unreal_plugin` does the same for
SGS (`SGS/`, `sgs-unreal-<version>.zip`). Both can be enabled in one
project, with Epic's GaussianSplat plugin or without it.
Use the assembled folder, not `plugins/unreal/VGS` from the tree — the bridge's header and
DLL are only in the assembled one. `cmake --install ... --component unreal` copies the zip
to `INSTALL/unreal`.

In a level: *Place Actors*, search *VGS*, drop it, pick the `.vgs` in *File*. A
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
  (the graded colour made linear), normal, composited expected depth, *Roughness* and *Specular*, and writes
  *Self-illumination Amount* as an HDR lerp between the complete UE-lit result and the
  original colour. 0 uses only the lit path; 1 bypasses GBuffer lighting and uses the unlit
  splat path (also without the extra VGS motion-history buffer). Values strictly between
  0 and 1 draw the splats once and blend after opaque lighting, before translucency.
  Only mixed views allocate an RGBA16F reference (8 bytes/pixel) and run the blend pass.
  The existing R32F receiver mask also stores the winning depth: no separate depth snapshot
  or depth copy. The depth comparison rejects surfaces replaced by another renderer or
  single-layer water. Intensity scales the original-colour branch only. Pixels the splats only partly cover
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
  (`UVGSSplatSettings`, written to the `r.VGSSplat.Shadow.*` console variables; SGS has
  the same page, *SGS*, for its own actors):
  Every light lights a Shaded actor whichever the method, and every mesh's shadow falls on
  it; the method is only about the shadows the splats cast. In the order the settings list
  them (`r.VGSSplat.Shadow.Method` 0 to 3):
  - *Splat Shadow Map (sun only)*, the default (`VGSSplatLightShadow.cpp/.usf`):
    splats are rasterized once from the sun into 64 light-space depth bins. Optical depth
    is added atomically, then scanned into cumulative transmittance. The sun's Light Function
    samples only up to the receiver's depth, before UE evaluates that light; no SceneColor
    subtraction, reconstructed BRDF or fixed Substrate darkening. Its indirect and emissive
    contributions are preserved by that integration. One extra nearest-depth tile prevents
    interpolation from shadowing a point ahead of the first actual caster.
    *Self Shadowing* enables reception on Shaded captures, including self-shadow and shadows
    between captures of that renderer. Off excludes their visible pixels, using a mask written
    with the GBuffer's depth test; ordinary UE receivers between casters still receive shadows.
    The grid is shared by the family's views; VGS and SGS share one Light Function with
    independent grids, without modifying the light component's saved material. A user Light
    Function is kept and uses native shadow casters as fallback. The preferred light is still
    the atmosphere sun at index 0, otherwise the brightest directional light casting shadows.
    Texels target 0.5 cm, up to 1024 per spatial side. At 512 spatial texels the bins, atlas and
    nearest buffer require about 101 MiB, plus the screen receiver mask; the old timing figures
    for the single-column map do not apply. The 64 depth bins approximate positions within a
    bin, and splats use their conditional centre along each light ray, not a full 3D volume.
    Self-illuminated captures remain composited later and do not receive this lighting.
  - *Splat Shadow Map (sun) + Unreal Shadow Maps (other lights)*: both, the second for every
    light but the main one. A shadow depth view looks along its light
    (`FProjectedShadowInfo::ModifyViewForShadow` turns it so, for billboards), so the
    casters leave out the views looking along the main light; with the sun alone the image is
    the first method's (mean difference 0.24 of 255). Level `/Game/Stress/Lit` (a red point
    light beside the sun) shows the point light's shadow added.
  - *Unreal Shadow Maps (all lights)*: the component is a primitive whose only part in
    Unreal's scene is its shadow casters, drawn as SGS's are (below, with the same
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

## SGS: static characters from PLY or SGS

SGS actors start in **Shaded** and respond to scene lights. Shadows always come from splats,
using the project's Shadow Method. The approximate mesh is collision-only: it cannot cast
self-shadows or shadows onto other objects. No Shadows disables splat casting while
Proxy Collision remains independent. The Mesh Proxy / Shadow Caster override has been removed.

SGS asset thumbnails render the actual capture through the SGS renderer in a reusable preview
scene, framed by its bounds, without scene lighting, shadow casting or the actor billboard.

The plugin and reflected types are named SGS directly; no redirects from VGSStatic are
provided. Reimport source SGS/PLY files and place new actors after updating older test projects.

A Gaussian splat PLY (the 3DGS trainer's layout: x y z, f_dc, f_rest, opacity, scale, rot,
optionally nx ny nz; binary or ASCII) dropped in the Content Browser becomes a **SGS
Splat** asset; dropped in a level, an actor showing it, with the capture's controls
(*Intensity (EV)*, *Blend Space*, the grade, *Invert UE Tone Curve*, *Lighting* and the
lighting settings, *Up Axis*: Y up, Y down as COLMAP, Z up). Its details panel says how
many splats, the SH degree, and whether it has normals and where they come from.

An **SGS static capture (`.sgs`) imports the same way**: drag it into the Content Browser,
then drag the resulting SGS asset into the level. Import authenticates and
decodes its one frame once, preserving SH and supplied normals. The closed decoder ships
with SGS's editor module; importing SGS does not require enabling the animated VGS plugin.
Its splats are saved as UE records, clusters and normal data, not as an external file reference.
The original SGS is needed for reimport only. This can make the UE asset larger than the
compressed source SGS.
See [assets, packaging and multiple machines](ASSETS_AND_PACKAGING.md) for the difference
between static assets and animated VGS files, and the current limits for future Pixotope integration.

If the SGS carries its authored GLB proxy, Make Collision Proxy imports that mesh's geometry
for shadows/collision, preserving its shape and triangle count instead of voxelizing it again.
The generated-proxy settings grey out in this case. This supports the SGS Editor's single
identity-node triangle mesh; other GLB layouts fail explicitly instead of silently using the
wrong geometry. The embedded display textures/material are not imported: this collision proxy is hidden,
and the visible image still comes from the splats. Without an embedded mesh, the existing
generated-proxy path remains available. Supplied normals are kept unless Replace File Normals
is enabled.

Both placement paths use the same updated renderer as before. Place Actors > VGS
uses the new green project logo; SGS uses the red SGS logo, including its
editor billboard and asset/class icons. The proposals from the Unity review are not included.

- **Importer.** `USGSSplatFactory`, before Epic's `.ply` importer: Unreal picks the
  highest priority and shows no choice, so its dialog has *Import as Epic's Gaussian Splat*
  where Epic's plugin is enabled (5.9), which hands the PLY file to Epic's importer. SGS is
  handled by SGS, without offering Epic's PLY importer. Only PLYs
  with f_dc_0 and rot_0 are taken; meshes and point clouds are left to others. A script
  imports with an `AssetImportTask` whose `options` is a `SGSImportOptions`.
  Reimport repeats the import with the asset's *Import Settings*.
- **Normals**, worked out once, at import, never per frame:
  - the PLY's own when it has them (SMNForge's and the VGS editor's, from the capture's own
    cameras), as they are;
  - without them, the VGS editor's estimate with **cameras all around** the splats
    (`gaussiannormals::surroundingViews`: 256 on a Fibonacci sphere about the opaque splats,
    whichever way is up), which decide each splat's outward side for a PLY from any rig or
    none; *Method* Normal sees as the editor's Normal (`kSurroundingVisibility`, about 1.3 s for
    300 000 splats), Fine as its Fine (`kFineVisibility`, about 3 s). Against gs2mesh's dense
    mesh of two captures (2026-11-17) the side is wrong on 0.25 % / 0.15 % of the splats with
    Normal and 0.19 % / 0.10 % with Fine; a studio rig's own cameras, which the import used to
    take when the PLY named its rig (`comment capture_rig`), put it wrong on 0.9-3.8 %.
  - *Method* Shortest axes keeps the earlier way: as PGSR takes a splat's normal (Chen et
    al. 2024), replaced where the splat is round or disagrees with its neighbours by their
    consensus, then smoothed; its side is decided on screen, facing the camera
    (`FVGSSplatRenderProxy::bNormalsFaceCamera`). Rougher: on andreavictor |cos| 0.91 (84 %).
  The VGS and SGS editors use the same estimate and the same cameras all around (the VGS
  editor's export settings: *Normals* Yes, made as Fine, or No).
- **Collision proxy**. The optional embedded mesh or generated occupancy mesh is retained
  for **Proxy Collision** (BlockAll). Its component is excluded from the main pass, depth
  pass and all shadow passes. Mesh-based shadows have been removed because the approximation
  does not coincide with the splat receiver and produces severe patchy self-shadowing.
- **Shadows** always use the splats, like VGS. **Splat Shadow Map** is recommended for the sun;
  **Mixed** adds native casters for other lights. Native maps remain an approximation and may
  show grain. **Cast Shadow** controls casting independently of the lighting mix.
- **Static, so:** uploaded once per asset and shared by every actor showing it; the splats
  are ordered along a Morton curve and cut into clusters of 64 that the renderer culls
  against each view (Epic's cluster record, one level of detail); no tick work beyond
  noticing a setting changed from Blueprint; *GPU Layout* Quantized keeps a third of the
  memory (Epic's compressed layout, packed once).
- **Shared code.** The renderer copy, its shaders, the shared headers, the LUTs and
  `VGSSplatLayout.h` are the VGS plugin's, copied when SGS is assembled and renamed
  (`VGSSplat` to `SGSSplat`, `/Plugin/VGS/` to `/Plugin/SGS/`, the shaders
  module) by `tools/assemble_sgs.cmake`, which fails if a name that would collide is
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
  self-illuminated. Shaded writes camera and actor motion to UE's velocity buffer; GPU
  playback also supplies splat-centre deformation within a chunk. A new chunk or unavailable
  record correspondence uses actor motion only; CPU deformation has no stable record IDs.
  New/disappearing layers and wide rotating splats remain approximations, and stencil is not
  written. With *Evaluate On* CPU the capture's normals are
  not used (the shortest axis is). The VGS editor's normals of `maria_simbo_carrito`
  (THE4DSCANNER rig) have scattered splats tilted the wrong way, which show as dark specks
  under a strong light. Oriented normals keep their side as the camera moves. Correct incorrectly
  oriented source normals in the editor; *Normal Smoothing* can reduce local noise.
  Forward shading is not lit (it has no GBuffer):
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
- The depth-resolved Light Function uses the conventional per-light path, as its material
  needs world and screen position. Support for fog, translucency, Lumen surface cache and
  ray-traced shadows is not established by the deferred receiver tests. Existing user Light
  Functions use native casters instead; that fallback has the native approximation's grain.
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

The self-illumination blend preserves the unlit path's existing behavior: its original-colour
reference is not fog-lit, and at exactly 1 the silhouette uses unlit alpha compositing rather
than Shaded's dithered opaque coverage. Interior colours interpolate in HDR; silhouette
coverage can differ at that endpoint. Shadow casting remains independent of the mix.

Self-shadow receiver depth uses the same front-to-back opacity/transmittance weights as
visible splat colour. Do not select the nearest individual splat above alpha 0.5: a hidden
opaque layer can win over many visible faint front splats, causing circular self-shadow acne.
The removed R32F minimum-depth target was also an unnecessary raster attachment. The weighted
surface remains a single-layer approximation for transparent or widely separated layers.
