# Edited VGS/PGS export

Implemented in `exportcapture.cpp`, `nativeexport.cpp` and `core/src/vgsencode.cpp`.

## Container and memory

`vgs::encodeSequence` writes the existing signed encoding-0 container from a
repeatable decoded-chunk provider and a random-access sink. Measurement and writing
are separate passes. Only one source chunk, decoded frame, packed frame and encoded
chunk are retained, not the whole output. MINT's current reader still holds its source
file in memory. VGS/PGS sources stream through the decoder with a small cache.

Only `editor_authoring` compiles the writer and signer with VGS_AUTHORING.
`editor_decoder` still contains neither writer nor signing key. qmake links the
authoring sources into the editor application. No new decoder profile or format
extension is needed: existing encoding-0 readers can open both output formats.

## Time and crop

For constant-rate input and conformal scene transforms (translation, rotation,
reflection, uniform scale), the writer retains source temporal chunks and their
native frame rate. `MintLogicalSource` imports logical MINT arrays directly;
VGS input is authenticated and its pages assembled into complete arrays.

Crop membership is sampled at native frame boundaries. Outside records are removed;
repeated exits/reentries split just that row into contiguous lifetime spans. Every
attribute and residual rank table is filtered consistently. Unused SH, staged
DC/opacity and position/rotation dictionary entries are remapped and compacted.
Trimming slices temporal arrays and adjusts rotation accumulators at the new start.
Trimmed direct-position groups become trajectories with the same frame samples,
because the source's direct position interpolation uses normalized chunk time.
These converted groups interpolate linearly between retained samples.

Translation and uniform scale preserve source rotation/colour coding and temporal
interpolation. Rotation/reflection preserve position trajectories and scale indices;
orientations are sampled into typed quaternion trajectories, and SH is refitted in
shared static and temporal dictionaries. Orientation subframe interpolation follows
the new native sample quaternions rather than the source's residual weights.

Nonuniform scale/shear and variable-rate sources use the sampled fallback: each
sample is a one-interval held chunk. The current profile stores static scale indices;
general affine covariance can require animated scales. This fallback can produce
larger files and is reported explicitly, rather than being described as native coding.

For every active source Gaussian, transform its mean to world space, then test it with
`inverse(crop.transform)`. Cylinder: x?+z? <= radius? and 0 <= y <= height.
Box: abs(x) <= width/2, abs(z) <= depth/2, and the same Y interval.
The crop is fixed in world space. Its Edit state affects the preview only; export
always applies an enabled crop. Built-in crop presets only reset the crop.
Zero-opacity live Gaussians are kept. An empty frame uses one inactive sentinel to
satisfy the format's nonempty-array rules. An entirely empty result is rejected.

## Covariance and SH

For the scene affine linear part A and source covariance ? = R diag(s?) R?,
export uses A?A?. A symmetric eigendecomposition supplies positive principal scales
and a right-handed rotation, including under reflections and shear. Position is A p + t.
Decoded scales/opacity are already activated; no exp/sigmoid is applied.

The angular reference is f_out(d) = f_source(normalize(A?? d)), using the same
camera-to-centre direction and real SH basis as the viewport. A 24-by-48
Gauss-Legendre/azimuth quadrature forms a 16-by-16 coefficient transform, including DC
converted from RGB via SH0 = (RGB - 0.5)/C0. Rotations/uniform positive scales are
exact within numerical precision before quantization. General nonuniform scales or
shear warp the sphere and are projected approximately to the selected finite SH band;
the export report identifies this limitation. Lower requested SH degrees intentionally
truncate coefficients after the bake.

## Existing-format precision

The native path preserves the source's scale indices and multiplies its float32
scale LUT by the uniform scene scale. Position bases/trajectories are transformed and
requantized to the existing 21-bit-per-axis representation. Unrotated SH/DC/opacity
retain the source dictionaries (after requested colour processing and compaction).

Rotated SH uses the exact real band transform before fitting. Static RGB and temporal
RGB trajectories are clustered into dictionaries of at most 1024 entries per
coefficient. Inactive temporal samples are extended from live endpoints so unused
values do not dominate the fit. The completion report measures coefficient RMS and
maximum error over live samples; the SH fit is an approximation, and this is not
an angular RGB error bound. Quaternion samples use existing smallest-three packing.

The sampled fallback uses a 256-entry logarithmic scale LUT, two staged DC dictionaries,
and static/temporal residual SH grids. Typed numerical descriptors are used for
integer fields, half floats, lifetime runs and temporal/quaternion predictors; encoding
all those arrays as generic byte streams was another source of inefficient coding.
PGS bypasses entropy coding, not attribute quantization.

Regression: the supplied `arab.vgsproj` with a 31,302,800-byte MINT produced a
339,491,851-byte VGS in the previous frame-expanded writer. Native temporal export
with the same crop, translation, metadata and despill produces 25,348,292 bytes,
with all 30 frames authenticated/decoded and crop activity checked against source.
No general size guarantee is made for arbitrary transforms, refits or source layouts.

## Colour and metadata

Despill runs before angular conversion: copied Gracia Converter adaptive despill,
green gain, strength, linear SH green opponent/chroma suppression, and optional copied
skin recovery in source metre coordinates. The native path uses the copied converter
operator on temporal dictionaries; the fallback processes sampled frames;
preview continues to show the source's colours.

The main Metadata and processing panel supplies catalogue/title/author/project/take,
studio/copyright/software/tags, free JSON, SH degree, playback and coding. Its independent
preset scope and last settings are retained. Metadata templates use `.presetmetadata`
files, separate from Capture Tools `.preset` files. Each export gets a new authoring UUID.
Producer JSON and vendor extras are copied and authenticated. Consumer JSON is retained
under sourceMetadata in the export provenance block, alongside userMetadata and the
baked transform/crop/processing settings. Array JSON is preserved as an array.
Audio and thumbnails are omitted because edited time/framing may invalidate them;
the completion report mentions this when the source had these payloads.

## Validation and publication

Export runs on a dedicated thread from an immutable project snapshot, with cancellation
checks during processing, encoding, verification and copying. It first writes a temporary
file next to the destination. A fresh decoder authenticates and decodes every output
frame including SH. QSaveFile publishes only after verification; cancellation or any
error preserves an existing destination. Exporting over the source is refused.

Tests cover world-space cylinder/box membership, zero-opacity records, affine covariance,
reflections, SH directional invariance under rotations, despill and skin recovery,
signed PGS/VGS reexports, trim and empty frames, large dictionaries, metadata JSON arrays,
atomic cancellation/empty-crop failure, and a real MINT sample via EDITOR_TEST_CAPTURE.
Qt tests also cover independent In/Out marker dragging and Tab from the viewport and
transform fields.

Additional tests cover native direct/residual trimming, repeated crop visibility,
large residual offsets, rigid/reflected covariance, rotated SH directional colour,
and the real reported project size/activity regression via EDITOR_TEST_PROJECT.

## Neutral cloth protection in skin recovery

The supplied capture revealed a false positive in the copied skin-colour recovery:
a robe Gaussian with an apparently neutral SH colour was classified from its DC term
as skin, and received a large red correction. Position/DC/SH matching against the
source confirmed that rows were correctly associated and that processing caused the
colour shift. At one reported crease record, raw rendered RGB was approximately
(0.7080, 0.8160, 0.7279); recovery changed it to (0.9902, 0.6883, 0.6766).

Skin recovery now receives the original SH data and conservatively excludes candidates
and reference seeds which have bright neutral appearance over multiple directions.
Clipped-white extrapolations do not count as neutral evidence. The same crease then
matches despill without recovery, approximately (0.7998, 0.8006, 0.8094), while ordinary
skin-like candidates remain eligible. This is a material heuristic, not a semantic mask.
A synthetic skin/cloth case, the reported record, and full per-Gaussian position/DC/SH
parity tests protect the correction. Export provenance carries colourProcessingVersion=2.
