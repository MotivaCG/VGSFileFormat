# SGS Editor over MCP

The SGS Editor turns one static 3D Gaussian Splatting capture (a `.ply`) into an asset: normals
for its splats, a textured mesh made with gs2mesh (and its LODs), lights to preview it, and an
export as `.sgs` or `.ply`. These tools drive the open editor; paths are on the editor's computer.

## Usual sequence

1. `get_state`; `open_file` a `.ply` (or a `.sgsproj`).
2. `estimate_normals` when `normals_estimated` is false.
3. `set_photos` with the capture's folder when it has an undistorted COLMAP model and its
   images (better geometry and albedo); leave it empty otherwise.
4. `set_mesh_settings` if the defaults do not fit (200000 triangles, LODs 40000 and 3000,
   4096 textures).
5. `generate_mesh`. A capture without photos takes a few minutes, with photos up to about ten
   (the first run with photos on a machine also builds TensorRT's engine, some twenty minutes).
   Much longer suggests a hang: read `get_gs2mesh_log` and `cancel_gs2mesh`.
6. Look at the result: `set_view(show="mesh", gray=true)` for the shape, `gray=false` for the
   texture, several cameras around the subject (`yaw` 0, 90, 180, 270), then `screenshot`.
7. `generate_lods`, and look at each level (`set_view(level=...)`).
8. `save_project`, then `export` a `.sgs` with the mesh.

## What to look for in the pictures

Shape: spikes, folds, webbing between body parts that should be apart, holes, blobs where the
splats show nothing. Texture: seams, smeared or doubled detail, background colours on the
subject, dark baked shadows. LODs: the silhouette kept, no stretched texture.
