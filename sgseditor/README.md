# SGS Editor

Desktop editor for **static captures**: one 3D Gaussian Splatting `.ply` (a scanned
character or prop). It makes the capture's normals, a textured mesh of it with gs2mesh,
previews it under lights, and exports it as `.ply` or `.sgs` (FORMAT.md, *Static captures*).

The SGS Editor lives beside `editor/` and `viewer/`, with its own CMake and qmake projects,
deployment script and installer. Like the viewer, it compiles the VGS editor's sources it
shares (viewport, export, normals) from `../editor`; it is supplied under the
[VGS Editor licence](../editor/LICENSE.md). Its version is set in `sgsversion.h`.

## What it does

- **Imports one PLY** (INRIA layout, SH degree 0 to 3, File > Import), or opens an SGS project
  (`.sgsproj`).
- **Normals, our method**: the PLY's own when it has some; otherwise, or on *Estimate
  normals*, the VGS editor's estimate (`editor/gaussiannormals.cpp`): each splat's side
  decided by the cameras of the rig the PLY names (`comment capture_rig SMNV1` or
  `THE4DSCANNER`), or by 256 cameras all around it. *Estimate normals* has those cameras see
  finer than an import does (1024 pixels, `kFineVisibility`): some ten seconds for 300 000
  splats, the window waiting, and a sixth fewer splats on the wrong side; the project
  remembers it and makes them so again when opened.
- **Mesh with gs2mesh** (`editor/dependencies/gs2mesh_cpp`, a submodule): a textured mesh,
  without LODs. *Generate mesh* asks for the capture's photos (an undistorted COLMAP model and
  its images) the first time, unless one is beside the PLY (the folder itself,
  `undistorted\sparse` before a distorted `sparse`, then anything below: the first model
  gs2mesh can use), and then for the resolutions, gs2mesh's own by default (200 000
  triangles, 4096 textures). Both are kept in the project and can be changed later (*Photos
  (COLMAP)* in the panel, or *Change photos…* in the mesh dialog). With photos, gs2mesh takes
  the geometry from stereo between them (FoundationStereo), the faces' relief (MoGe-2) and the
  albedo from them; without, everything comes from the splats, the albedo baked from renders of
  them all around. The first run with photos on a machine builds TensorRT's engine for its GPU
  (some twenty minutes, said under the progress bar); later runs reuse it. The mesh goes to
  `<ply name>_mesh` beside the project, or beside the PLY before the project is saved. The
  panel says when it is out of date (the PLY, the photos or the settings changed since).
  While gs2mesh works the rest of the editor waits; only its progress and Cancel stay on.
- **Sculpt**: the main mesh cleaned up by hand before its LODs are made, in mesh view (Tab
  in the view switches splats and mesh) on the main mesh only, with T4DEditor's brushes and
  controls, so both editors sculpt alike. Its tools go down the left of the window as there:
  *Mesh brushes* drops the list down (Ctrl+F1 to Ctrl+F8: *Move*, *Draw*, *Inflate*, *Pinch*,
  *Smooth*, *Relax*, *Flatten*, *Erase*), Esc goes back to navigating. On the mesh the left
  button paints and the right one the opposite (Draw in, Inflate deflates, Pinch spreads), Ctrl
  inverts and Alt smooths with any brush; off it the left button orbits and the right one pans.
  The brush is T4DEditor's: a radius in cm along the surface (or a sphere: *Reach*), a strength
  and a hardness (the share of the radius at full strength), changed with Ctrl, Shift and Alt +
  wheel, drawn as there with their values under it while those keys are held. *Erase* takes the
  surface back to how gs2mesh made it. *Relax* is this editor's: the points slid along the
  surface to even the triangles out, the shape kept; where faces are folded over (against the
  surface four rings around) they are taken to their neighbours' middle until they unfold. While
  a brush is chosen the mesh shows grey under a light that turns with the view, to read its
  shape. *Fix spikes* finds the spikes and folded faces over the whole mesh and smooths them into
  what is around; *Remove loose pieces* removes those with fewer than 2 % of the triangles. Each
  stroke or fix is undone with Ctrl+Z (Ctrl+Y redoes). Only vertices move and faces go, so the
  UVs and the albedo stay valid, and the vertices of a UV seam move together
  (`meshsculpt.cpp`). The edit is written beside the generated mesh as
  `<name>_edited.obj` (with its own `.mtl`) when the project is saved, LODs are made or the
  mesh is exported; the generated one stays, and *Revert to generated* goes back to it. A new
  *Generate mesh* replaces the edit, after asking.
- **Paint the texture**: T4DEditor's texture brushes on the main mesh's albedo, behind
  *Texture brushes* (Alt+F1 to Alt+F6): *Paint* (the colour in the panel), *Erase paint* (back
  to the albedo gs2mesh made), *Clone* (Alt+click its source, then paint), *Blur*, *Lighten*,
  *Darken*; the right button paints the opposite (Paint and Erase paint, Lighten and Darken).
  Only the faces turned to the view are painted, as far as the brush reaches in space, with the
  same radius, strength (its opacity) and hardness. Each stroke is undone as a brush's is; after
  it the gutters around the UV islands are filled from their edges (8 texels), so the seams do
  not bleed old colours. The painted albedo is written as `<name>_edited_albedo.png`, named by
  the edit's material, so the LODs made after (which share it) and the `.sgs` carry it; the
  generated one stays.
- **LODs**, once the mesh looks right: *Generate LODs* (40 000 and 3 000 triangles by
  default) reduces it keeping its texture layout, so every LOD shares its albedo and gets a
  normal map of its own baked from the dense surface (`gs2m_run_lods`; `gs2mesh_cli
  --lods-from main.obj main_dense.ply`). New LODs replace those made before. Of an edited mesh,
  the LODs are made from the edit: the dense surface is first moved as the mesh was
  (`<name>_edited_dense.ply`, each vertex by the edit around it), and gs2mesh bakes the edited
  mesh's own normal map again from it too (`gs2m_run_lods_ex`, `GS2M_LODS_REBAKE_MAIN`). The
  panel says when the LODs are of the mesh before its last edits. In mesh view a button beside
  the view cube goes through the main mesh and its LODs.
- **Lights**: point and directional, added, deleted, edited, turned off and on (H); the
  gizmo moves a point light (G) and turns a directional one (R). Each splat is lit by its
  normal, Lambert in linear light, plus *Self-illumination* of its colour glowing on its own
  (10 %, as Unreal's default). With no light on, the splats show as captured. A new capture
  comes with a magenta and a blue rim light, both off. *Lights > Presets* replaces the lights
  with a ready-made lighting (neon rims, red and cyan, sodium and teal night, three-point, daylight sun, sunset, soft
  studio, horror, campfire); *Lights > All lights* turns them all on or off. A light's colour
  is taken as picked (sRGB) and lights with its linear value, as in Unreal. *Show: Mesh*
  draws the mesh under the same lights.
- **Turning lights**: each light can turn about the scene's vertical axis (x = 0, z = 0), once
  every so many seconds, counter-clockwise or clockwise seen from above (*Turn every*), and
  every light together on top of that (*All turn every*). *Play* (Space) runs the clock,
  *Start* (Home) takes the lights back to their saved poses. Selecting or editing a light pauses
  it: what is edited is the light where it is seen, kept as its pose at time 0, so playing on
  goes on from there; a new speed does not move a light either. The project keeps the poses
  at time 0 and the speeds, not the clock.
- **Light icons**: drawn where each light is, except where the splats hide them (the selected
  one stays, faint); *View > Light icons* (L) shows or hides them all.
- **Export** (File > Export, by the extension chosen): **.ply**, the splats with their normals
  in `nx, ny, nz`; or **.sgs**: the splats with their normals, a thumbnail of the view and, when asked,
  the mesh (its lowest LOD by default, textures at 2048) as one `.glb` in the proxy mesh
  extra, with its albedo, normal map and ambient occlusion (`occlusionTexture`). The file is signed as the editor's exports are, read back and checked before it
  replaces the destination. The viewer and the VGS editor open `.sgs` files; `vgsdec`
  hands the mesh over with `Capture::proxyMesh()`.

## gs2mesh

The editor loads `gs2mesh.dll` when it first needs it, so it runs without it, and only
mesh generation needs an NVIDIA GPU (CUDA). Its normal maps are tangent space in MikkTSpace,
OpenGL convention (Y+), the basis Blender, Unreal, 3ds Max and glTF viewers use; the `.glb` of an
`.sgs` export carries those tangents (`TANGENT`), computed with gs2mesh's own MikkTSpace
(`meshtangents.cpp`). Every mesh and LOD also gets an ambient occlusion map, `<name>_ao.png`
(named by its `.mtl` as `map_Ka`): the albedo keeps the occlusion the photos saw, the map is
for renderers that light the mesh themselves. It looks in a `gs2mesh` folder beside the exe,
then beside the exe, then in the submodule's install for builds run where they were made.
Build that install once:

```powershell
git submodule update --init editor/dependencies/gs2mesh_cpp
cd editor/dependencies/gs2mesh_cpp
# its binary SDKs (OpenCV, ONNX Runtime, TensorRT, cuDNN, CUDA runtime) are not versioned:
# copy or link them into dependencies/win64 as its dependencies/README.md lists
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target gs2mesh gs2mesh_cli
cmake --install build --config Release      # -> INSTALL_WIN64/bin
```

The ONNX models gs2mesh uses with photos (FoundationStereo for stereo between the photos,
MoGe-2 for the faces' relief) are not in the repository. The editor takes them from
`gs2mesh/models` beside its exe, where `deploy.ps1` puts them; a build run where it was made
takes them from where CMake's `SGS_STEREO_MODEL` and `SGS_MOGE_MODEL` (qmake: the same
variables) point, by default the FoundationStereo export in `3dgs2mesh` and SMNForge's
`onnx/depth`. The mesh dialog shows them and can point elsewhere; the machine remembers that.

## MCP

**Edit > Enable MCP** (off by default, remembered) lets an AI assistant on the same computer drive
the open editor through an MCP server: open a capture, estimate normals, set the photos and the
mesh settings, run gs2mesh for the mesh and its LODs and follow it, choose the view and take a
picture of it, save the project and export. The editor only opens a private bridge on 127.0.0.1
with a token per session (`sgsautomation.cpp`, the requests in `sgswindowautomation.cpp`); the MCP
server is `mcp/server.py`, set up as `mcp/README.md` says. It is built like the VGS editor's.

## Build with CMake

```powershell
cmake -S . -B build/cmake-msvc -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/Qt6.8.1/6.8.0/msvc2022_64
cmake --build build/cmake-msvc --config Release
```

`sgseditor_tests` checks the PLY reader, the normals, the project, the COLMAP finder, the
OBJ to glTF conversion, the `.sgs` export read back, the lights, gizmo and mesh as the
viewport draws them, and sculpting (seams, undo, spikes, loose pieces, the edit written back,
the dense surface moved alike, the tools in the window). Run it with Qt's `bin` on `PATH`.
`SGS_TEST_PLY=<capture.ply>` adds a real capture; with `SGS_TEST_GS2MESH=1` too, gs2mesh runs
on it from the window (a few minutes) and the `.sgs` with its mesh is checked; with
`SGS_TEST_LODS=<main.obj gs2mesh made of it>`, a copy of that mesh is edited and gs2mesh makes
its LODs from the edit. `SGS_TEST_MESH=<mesh.obj>` times sculpting a real mesh.
`SGS_TEST_SHOTS=<folder>` saves pictures of the window, the lights, the mesh and sculpting.

`SGSEditor.exe --self-test <file.json>` writes whether gs2mesh loads and exits, without a
window: for a deployment or an installation.

## Build with Qt Creator

Open `SGSEditor.pro` with the MSVC Qt 6.8 kit. The usual Release build directory is
`build/Desktop_Qt_6_8_0_MSVC2022_64bit-Release`.

## Deploy and create the installer

`deploy.ps1` makes `<build>/deploy`: `SGSEditor.exe`, Qt's libraries and plugins, the MSVC
runtime, the licence, and gs2mesh with every library it loads in `deploy/gs2mesh` (OpenCV,
ONNX Runtime with its CUDA and TensorRT providers, cuDNN, CUDA) and the two ONNX models in
`deploy/gs2mesh/models` (`-StereoModel`, `-MogeModel`): about 6.5 GB. Run it in the
compiler environment, as Qt Creator does.

```powershell
.\deploy.ps1 -BuildDir build/Desktop_Qt_6_8_0_MSVC2022_64bit-Release
.\deploy.ps1 -BuildDir build/cmake-msvc -Configuration Release
```

For Qt Creator's Custom Process Step under Deploy, use `powershell.exe` and:

```text
-NoProfile -ExecutionPolicy Bypass -File "<sgseditor>\deploy.ps1" -BuildDir "%{buildDir}"
```

Compile `installer/SGSEditor_InnoSetup.iss` with Inno Setup 6 after deployment. It takes the
qmake Release deploy folder by default; for a CMake build pass
`/DMyAppPathToInclude=<sgseditor>\build\cmake-msvc\deploy`. It writes
`installer/Output/SGSEditorInstaller_<version>.exe` with its parts (`-1.bin`, …) beside it:
gs2mesh's libraries make it larger than one file may be. It offers to open `.sgsproj`
files and adds the editor to *Open with* for `.ply`.
