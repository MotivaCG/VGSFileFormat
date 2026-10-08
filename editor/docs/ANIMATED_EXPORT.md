# Animated export: motion samples

An animated transform is exported as the capture's native temporal blocks, unchanged
in size, plus the motion of the whole capture stored as samples beside them. Readers
apply that motion; MINT, which has nowhere to put it, is refused.

## Why not bake it

The first fallback evaluated and repacked the full capture at every frame, repeating
every Gaussian record and dictionary per frame. Baking into the native arrays instead
does not fit the format: every residual-position splat measured already uses all four
position terms (214,706 of 214,714 records in the reported capture, 100% in five
captures checked), so a shared translation cannot be added as a term, and the weights
of a splat do not sum to a constant, so moving the shared trajectories moves each
splat by a different amount. Rotation is harder still: static SH dictionaries and
per-splat bases would have to become temporal. Exact baking means a block per frame.

## What is stored

`motion_samples` (attribute 12) is a shared, base-layer attribute: one sample at each
of a chunk's sample points, eight f64 each - translation, rotation quaternion xyzw and
uniform scale. Between samples translation and scale are linear and rotation is
spherical, with the fraction every other attribute uses. FORMAT.md has the details.

Its policy is never optional. A reader that does not know attribute 12 refuses the file
("invalid VGS policy"), which is what an old reader must do: playing a moving capture
in place would be wrong without saying so. Measured with the 2.0.0 decoder: it refuses
a moving export and opens a static one from the same build. Captures without motion
are written exactly as before and stay readable by every reader.

`FrameDecoder` applies the motion, so every reader of frames - the C++ decoder, the
C API, WebAssembly, the Blender and Houdini plugins, the editor - gets positions,
rotations and scales already moved and spherical harmonics already rotated. A renderer
that draws the stored attributes itself (the PlayCanvas viewer, `Output::Packed`)
applies `motionAt()` as a model matrix; the viewer does it by putting the motion on a
child entity, so sorting, culling and the view direction of the harmonics happen in the
capture's own space.

## How the editor exports it

The transform at the first exported frame is baked into the data as for a static
export. At every native sample the world transform is the editor's transform at that
frame over whatever motion a .vgs source already had; what is stored is that, with the
baked transform taken back out. It must be a rotation with uniform scale - non-uniform
animated scale, shear or a mirror stops the export with a message rather than being
approximated. Crops, colour filters and Purge Isolated evaluate world positions at each
sample with that transform, exactly as the preview does.

On the reported capture (30 frames, 214,714 records) the moving export is 2.2 KB larger
than the static one.

## Tests

`export_tests` compares the decoded moving export with the editor's per-frame reference
bake (positions, covariance, DC and SH), checks the chunk boxes hold every live splat,
that `positionsAt` agrees with `setTime`, that the size matches a static export, that
re-exporting a moving .vgs composes the motion once, and that MINT is refused.
`tests/motion.cpp` checks the SH rotation against evaluation at the rotated direction,
the interpolation and the stored form. The Blender plugin, Houdini plugin and the
PlayCanvas viewer were checked against a static export of the same take: each moving
frame is that frame moved rigidly, by the animated angle.
