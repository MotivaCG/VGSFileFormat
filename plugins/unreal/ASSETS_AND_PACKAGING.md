# PGS, VGS, packaging and multiple machines

## Static PGS and PLY

Import converts the source into a `UPGSSplat` asset. The asset serializes the actual
renderer records, spherical harmonics, normals and clusters. An optional collision mesh is an
embedded `UStaticMesh` object referenced by the asset. A PGS-authored mesh is kept when present
and Make Collision Proxy is enabled; otherwise the existing generated-proxy path applies.

The actor's `Splat` property references that UE asset. The original `.pgs` or `.ply` is only
an editor reimport source; neither rendering nor loading the saved asset reads it. Source
import metadata is editor-only. Import does not retain the compressed PGS as the runtime
representation; asset disk size and runtime memory can therefore exceed the source PGS size.

Reference the asset from a level or another cooked asset so UE includes it in the cook.
An asset used only through a dynamically assembled path needs an explicit cook rule.
The cooked content can be placed in `.pak`/IoStore in the normal UE content workflow.
The static runtime reads UE data; PGS authentication/decoding is an editor import operation.

The PGS proxy's geometry is used for its hidden collision role. Embedded display
materials/textures are not imported by this implementation. It is not a textured-mesh
substitute for displaying the capture.

## Animated VGS

`UVGSCaptureComponent.File` currently names an external `.vgs`. The bridge maps that file
directly and reads/decompresses chunks during playback. It is not an imported UE data asset,
and copying just the map does not distribute the capture.

Use a project-relative path, e.g. `VGS/boxing.vgs` under `Content`. Add `Content/VGS` through
Project Settings > Packaging > Additional Non-Asset Directories To Copy. It must be staged
as a loose file (NonUFS), since the present bridge does not read captures inside `.pak` or
IoStore. Its decoder DLL is already a runtime dependency of the VGS module.

## Multiple machines and future Pixotope integration

Distribute the same build, plugin/shader version and content to every node. Static PGS/PLY
assets then load from each node's UE content. Animated VGS additionally needs its loose
capture at the same relative location on every node. A local copy per node avoids putting
playback bandwidth and availability on a shared network path.

Data distribution does not synchronize time. Static assets have no independent playback
clock. Animated VGS normally follows each world's local time; starting Play on several
machines does not guarantee matching frames. `SetPlaybackTime` is the existing entry point
for an external common clock. There is no dedicated Pixotope, timecode, genlock or cluster
synchronization integration in this delivery, and no Pixotope compatibility claim.

A future Pixotope integration must verify its exact UE version and renderer configuration,
plugin compilation, shader permutations/cook and deployment. Multiple rendering machines
also need the same synchronized scene transforms, camera and lighting, in addition to the
VGS playback time. The Unity review's performance proposals have not been ported here.
