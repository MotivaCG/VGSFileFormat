# VGSSplatRenderer: a copy of Epic's Gaussian Splat renderer

**This is Epic's code, copied from Unreal Engine 5.9.0 (CL 60351404) and changed in a few
places. When the plugin moves to another engine version, the copy has to be taken again
from that version and the changes applied again.** The steps are at the end.

What is copied:

| from Epic's `Engine/Plugins/GaussianSplat` | to `plugins/unreal/VGS` |
|---|---|
| `Source/GaussianSplatSceneViewExtension/` (the renderer module) | `Source/VGSSplatRenderer/` |
| the 14 shaders it compiles, `Shaders/Private/GaussianSplat*.usf/.ush` and `Shaders/Public/SphericalGaussianCommon.ush` (not Niagara's) | `Shaders/Private/SplatRenderer/` |

Every copied file starts with a line saying so. Everything VGS changed is marked
`VGS CHANGE` in the code, and the whole set of changes is
[`tools/vgs_renderer_changes.patch`](tools/vgs_renderer_changes.patch).

## Why a copy

Epic's renderer blends splats in **linear light**: each splat's colour is made linear before
blending (`CalculateColor` in `GaussianSplatShading.usf`). Captures are trained blending in
**sRGB values**, as almost all 3D Gaussian splatting training does, and the VGS viewer draws
them that way. The difference hardly shows on opaque surfaces but is large for faint
splats: a 20 % splat over black gives about 48 % of its colour in linear instead of 20 %. On
`maria_simbo_carrito.vgs` that is a purple haze through and around dark hair, and a
coloured fringe on edges, which the viewer does not show. It was confirmed on 2026-11-10 by
rendering the same frame on the CPU both ways (same kernel and clamp as the viewer) before
touching Unreal: only the linear blend showed it.

The blend is inside Epic's renderer, so there was no way to change it from outside: the
plugin already handed Epic its own splat buffers through `RegisterExternalProxySource`, and
all of that still works the same. The choices were a renderer of our own or a copy of
Epic's with the blend changed. The copy was chosen for being small to change, at the price
of keeping it in step with the engine.

The copied files keep Epic's copyright line and are under the Unreal Engine EULA, not under
any VGS licence. Check what the EULA allows before shipping the plugin's source to people
who are not Unreal Engine licensees.

## What was renamed, and what was not

Both renderers are loaded at once - Epic's plugin stays enabled, for its own splat assets,
for the shared headers and for the LUT assets - so everything that would collide was renamed
by [`tools/copy_epic_splat_renderer.py`](tools/copy_epic_splat_renderer.py):

- the module, `GaussianSplatSceneViewExtension` to `VGSSplatRenderer`, and its API macro;
- every class, struct, namespace and file named `GaussianSplat*` to `VGSSplat*`
  (`FVGSSplatRenderProxy`, `FVGSSplatViewExtension`, `UVGSSplatSubsystem`,
  `VGSSplat::RegisterExternalProxySource`, ...);
- global shader types, whose names must be unique in the process, including the three
  that do not say GaussianSplat (`FCullClustersCS`, `FInverseToneCurvePostBlendPS`,
  `FHardwareRasterizeStochasticDepthPS` become `FVGSSplat...`);
- shader paths, `/Plugin/GaussianSplat/Private/` to `/Plugin/VGS/Private/SplatRenderer/`
  (mapped by the `VGSShaders` module);
- console variables, `r.GaussianSplat.*` to `r.VGSSplat.*`, so the copy has its own
  (`r.VGSSplat.Depth`, `r.VGSSplat.Falloff`, ...), separate from Epic's;
- stats, the CSV category, the GPU stat, the log category (`LogVGSSplat`).

Still Epic's, used as they are: the GaussianSplat runtime module's types
(`EGaussianSplatCoordinateSpace`, `FGaussianSplatCluster`, `UGaussianSplatComponent` and the
rest, `GaussianSplat::IsEnabled`), the shared shader headers under
`/Plugin/GaussianSplat/Shared/` (`GaussianSplatShared.h`, `GaussianSplatDefinitions.h`), and
the inverse tone curve LUTs at `/GaussianSplat/LUTs/`. So the plugin still depends on Epic's
GaussianSplat plugin being enabled.

Not copied: `GaussianSplatSceneViewExtensionRenderer.h/.cpp`, which register Epic's
components with the renderer. Epic's splat assets are drawn by Epic's renderer and never by
this copy; its component code is still there, unused (nothing registers a component with it),
to keep the copy close to Epic's.

## What was changed

All marked `VGS CHANGE`; `tools/vgs_renderer_changes.patch` is the exact diff.

1. **Shading** (`VGSSplatShading.usf`, `CalculateColor`). With `BlendInSRGB` (new parameter,
   from the proxy), the colour stays the sRGB value, clamped to [0, 1] as the viewer clamps
   it, instead of Epic's sRGB to linear to working colour space. With it off, Epic's code,
   unchanged.
2. **Rasterization** (`VGSSplatHardwareRasterize.usf`, `VGSSplatTileRasterize.usf`). No
   `View.PreExposure` when writing splat colour: the target is the scratch texture, and
   exposure is applied after the colour is made linear.
3. **Composite** (`VGSSplatInverseToneCurvePostBlend.usf`, Epic's post-blend pass). With
   `BlendInSRGB`: unpremultiply the scratch to the splat layer's sRGB colour, make it linear,
   then as Epic: the inverse tone curve if asked for, the working colour space; then the new
   `Intensity`, pre-exposure, and premultiply again. Without it, Epic's composite, with
   `Intensity` and with pre-exposure applied here rather than in rasterization. In both, an
   optional grade (`Grade`: gamma, contrast about 0.5, saturation; then `Tint` multiplies)
   on the unpremultiplied sRGB values before they are made linear, faded out below 5 %
   opacity; skipped entirely when both are (1, 1, 1).
4. **Pipeline** (`VGSSplatRenderPipeline.cpp`). Splats always go through the scratch texture
   when the platform supports it (Epic uses it only for the inverse tone curve, expected
   depth or downsampling), since the composite is what makes the blend linear. New shader
   parameters `BlendInSRGB` (shading, composite), `Intensity`, `Grade`, `Tint` and `GradeActive`
   (composite).
5. **Proxy** (`VGSSplatRenderPipeline.h`, `FVGSSplatRenderProxy`). New `Intensity` (a scale
   on the linear colour; VGS's *Intensity (EV)* is 2^EV), `bBlendInSRGB`, `Grade` and `Tint`;
   for lit captures `bLit`, `SelfIllumination`, `Roughness`, `Specular`, `NormalSmoothing`
   and `NormalBuffer`.
6. **Module** (`VGSSplatRendererModule.cpp`). No registration with Epic's
   `FGaussianSplatRendererRegistry`.
7. **Lit captures** (*Lighting* Shaded). The view extension also overrides
   `PostRenderBasePassDeferred_RenderThread` and draws there, into the GBuffer, the proxies
   with `bLit` (when `VGSSplat::SupportsLit`: deferred, hardware rasterization);
   the post-process pass draws the rest, so each proxy is drawn in one pass only.
   `VGSSplat::RenderLit` sorts front to back and forces the expected-depth path; the shading
   pass (`VGSSplatShading.usf`, `VGSShadedNormal`) writes each splat's view-space normal,
   from the proxy's `NormalBuffer` (the capture's normals) or its shortest axis turned to the
   camera; the vertex and pixel shaders (`VGS_LIT` permutation of
   `VGSSplatHardwareRasterize.usf`) accumulate it, opacity-weighted, beside the depth
   moments; and a new full-screen pass, `FVGSSplatGBufferPS` in `VGSSplatGBuffer.usf`, writes
   SceneColor (emissive), GBuffer A-C through Unreal's `EncodeGBuffer` and depth, after a
   bilateral filter of the normal over the proxy's `NormalSmoothing`. With Substrate's own
   material buffer it writes instead the buffer's first three slices and its top layer, one
   simple slab packed with `PackSubstrateOut` (`SUBSTRATE_FASTPATH`: without it a global
   shader packs the complex encoding, and those tiles go unlit in a scene with no complex
   material), with the buffer's UAV from `View.SubstrateViewData.SceneData`, as the engine's
   `DebugProbes.usf`. Console variables `r.VGSSplat.Lit.Dither`, `r.VGSSplat.Lit.MinCoverage`
   and `r.VGSSplat.Lit.NormalSmoothing`. The grade
   moved into `VGSSplatGrade.ush`, shared by the composite and the GBuffer pass.
   `VGSSplatGrade.ush` and `VGSSplatGBuffer.usf` are VGS's own files in the copy's folder:
   copying again removes them, and the patch adds them back.
8. **Build rules** (`VGSSplatRenderer.Build.cs`). `bTreatAsEngineModule`, since a project
   plugin needs it to see the GaussianSplat runtime module's `Internal/` headers, and a
   dependency on `VGSShaders`, which maps `/Plugin/VGS`.

Outside the copy, in the VGS plugin's own code: `VGS.uplugin` lists `VGSSplatRenderer`
(Runtime, PostConfigInit, Win64); `VGSRuntime` registers its splat sources with
`VGSSplat::RegisterExternalProxySource` instead of Epic's; and the capture component has
*Blend Space* (sRGB, the default, or Linear, for a capture trained in linear light) and
*Gamma*, *Contrast*, *Saturation* and *Tint* (the proxy's `Grade` and `Tint`), *Lighting*,
*Self-illumination Amount*, *Roughness*, *Specular* and *Normal Smoothing*, and applies *Intensity (EV)*
through the proxy instead of in the splat data, where the per-splat
clamp would cut it.

One consequence to know: without scratch support (an RGBA16F render target that can be
blended; every desktop GPU has it) the colours would be composited as sRGB values taken as
linear, and pre-exposure would be missing. Win64 is the only platform the plugin builds for.

## Moving to another engine version

1. See what Epic changed: diff `Engine/Plugins/GaussianSplat/Source/GaussianSplatSceneViewExtension`
   and `Engine/Plugins/GaussianSplat/Shaders` between the old engine and the new one. Epic's
   plugin is experimental in 5.9; anything may move.
2. Copy again from the new engine. This replaces `Source/VGSSplatRenderer` and
   `Shaders/Private/SplatRenderer` whole:

   ```
   python plugins/unreal/tools/copy_epic_splat_renderer.py <new engine root>
   ```

   If it fails, or the result does not compile, the lists in the script are what to update:
   shaders added or removed (`SHADERS`), Epic types the copy should keep using (`KEEP`),
   and shader types whose names do not say GaussianSplat (`SHADER_TYPES`; two global shader
   types of one name stop the engine at start-up).
3. Apply the VGS changes, from `plugins/unreal`:

   ```
   cd plugins/unreal
   patch -p1 < tools/vgs_renderer_changes.patch
   ```

   Rejected hunks are where Epic changed the same code; redo those changes by hand, from the
   list above and the `VGS CHANGE` comments in the patch. Then regenerate the patch against a
   fresh copy (copy into a scratch folder with the script's second argument, and
   `diff -ruN` the two), so it applies cleanly next time.
4. Update the engine version in this file, and check that `VGSRuntime` still compiles against
   the proxy (`FVGSSplatRenderProxy`) and `VGSSplat::RegisterExternalProxySource`.
5. Look at it: the same frame with *Blend Space* sRGB and Linear (Linear should look like
   Epic's own renderer), *Invert UE Tone Curve* on and off, *Intensity (EV)*, the grade
   (`set VGSCaptureComponent Saturation 0` in `-ExecCmds` sets it on every capture; a colour
   has commas, which `-ExecCmds` splits on, so put its `set` in a file under the engine's
   `Engine/Binaries` and run `exec <file>`). The host project
   has `/Game/Stress/Close` and `/Game/Stress/Close_Linear` for this, and
   `vgs.Debug.After 8 HighResShot 1920x1080` in `-ExecCmds` takes the screenshot once
   things have loaded. For lighting, `/Game/Stress/Lit` (a capture with normals, Shaded, a
   red point light on its left), `Lit_Self` (the same, self-illuminated) and `Lit_Axis`
   (no normals, Shaded); `ShowFlag.VisualizeBuffer 1,r.BufferVisualizationTarget
   WorldNormal` shows the normals it wrote. `/Game/Stress/Color_Neon`, `Color_Rim` and
   `Color_RGB` are the same capture in the dark under two or three coloured point lights.
   Substrate without changing the project: `-ini:Engine:[/Script/Engine.RendererSettings]:r.Substrate=1`
   on the command line (the first start compiles every shader again), and
   `r.Substrate.ViewMode 4` shows its tile classification: every tile the capture covers
   should be simple (green).

The copy was taken and checked this way on 2026-11-10, and the patch regenerated the same
way after the lighting changes: copying again and applying it reproduced the plugin's files
exactly.
