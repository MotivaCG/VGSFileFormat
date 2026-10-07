# Compact animated export

The previous animated fallback evaluated and repacked the full capture at every
frame. It repeated Gaussian records and colour/SH/opacity data instead of retaining
the original temporal dictionaries. Adding a few scene transform keys does not
justify that multiplication. Varying animated export is now blocked before opening
an output file. Tests verify that an existing destination remains byte-identical.

## Existing representation

Encoding 0 and MINT format 6 have static per-Gaussian scale indices, a shared scale
LUT, and at most four position residual terms. Rotation and SH have temporal data,
but an arbitrary global transform cannot always be baked exactly into those fixed
limits. Keeping the full record set once per frame is a fallback, not a requirement
of animated content. A legacy-compatible bake can alter temporal position/rotation
data and fit SH dictionaries; varying scale needs additional representation or
duplicate lifetimes. Quality and size must be measured before making it automatic.

## Compact VGS/PGS option

Use a new authenticated encoding profile, preserving the native Gaussian arrays
and dictionaries, and store the global reference/offset curve once. Encoding 1 is
already reserved for the temporal experiment; a production scene-transform profile
must use a distinct identifier. Updated decoders expose correct world geometry,
and GPU viewers apply the affine matrix to means and covariance while evaluating
SH in the corresponding local camera direction. Positions, rotations, covariance
and directional colour must all agree with the editor; adding ignored JSON
metadata is insufficient.

Cropping/colour filtering/isolation still evaluate transformed world means at each
source frame. A per-record activity mask can retain visibility gaps without
duplicating all attributes for every lifetime span. For 200,000 records and 30
frames, a raw one-bit visibility mask costs 750,000 bytes; a small transform curve
costs orders of magnitude less than another full capture. Dictionary compaction
still removes rows/entries unused over the exported range.

Updated readers must continue accepting ordinary encoding-0 captures. Old readers
must reject the new profile rather than silently ignoring its animation. Native,
WASM and the PlayCanvas renderer/sorter all need coverage before release.

## MINT compatibility

Current MINT readers have no supported global transform curve or temporal scale
attribute. A compatible MINT output therefore needs a measured native bake, or
must report unsupported animation explicitly. No transform is to be hidden in
unknown auxiliary metadata and presented as a compatible animated MINT capture.

The pending decision is whether updating VGS/PGS readers/webviewer is acceptable,
or all outputs must remain compatible with the currently deployed readers.

## Deferred shared translation delta

At the user's request, the codec/rank extension is deferred; no encoding or reader
changes are currently applied. A pure translation can be represented by one shared
position trajectory and a coefficient/index per Gaussian. The reported MINT has
214,706 residual-position records, all at rank four, plus eight direct-position
records. Appending the common delta would require a fifth term for most records.
A future implementation must address that limit, decoder/viewer compatibility,
MINT's four-term constraint, quantization accuracy and measured size before release.
