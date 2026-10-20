# VGS Editor modifiers and cleanup examples

Tool discovery (`tools/list`) is the source of truth for each tool's purpose, parameters,
limits, defaults and annotations, taken from the server's docstrings and typed signatures.
This resource explains modifier behavior and worked examples; it does not repeat the tool
catalog. Use `vgs-editor://workflow` for the capture context and cleanup procedure.

## Modifier types: what each one does

All types below can be added with `add_modifier` and configured by `edit_modifier` using
the **existing** native fields. Modifiers operate over the source timeline; seek through
several poses to evaluate them. Animation varies a modifier over time where supported.

| `type` | Intended use | Native settings and cautions |
|---|---|---|
| `crop` | The main tool for removing large unwanted regions: keep the subject inside a cylinder/box, or remove floor/rig fragments. | See "Crop volumes" below for shapes, fields and how several crops combine. Check the entire motion; tight single-pose crops can cut limbs or object parts. |
| `purge-isolated` | Remove sparse, detached reconstruction noise automatically. | `isolation.neighbour` and `medianPercent`. Removes points whose Nth-neighbor distance exceeds median distance times medianPercent/100. Default neighbour=4, medianPercent=700; use 900 for a conservative first comparison. **Higher medianPercent removes less.** Density does not prove semantic relevance; protect thin detail. |
| `erase` | Manually remove residual floaters or fragments missed by filters. | Use brush/lasso/clear tools, not a patch to `erase`. Picks are chunk-local source-record identities. Pink editing preview differs from the final removed result; always check both. |
| `remove-green` | Remove splats classified as green by their base colour, e.g. green-screen fragments. | `green.minimumSaturation`, `hueTolerance`, `colourSpace`; target hue is green (120 degrees). Can delete intentional green clothes/props. Deleting green points and correcting green spill are different operations. |
| `colour` | Adjust exposure, white balance, saturation, opacity and green spill while retaining capture colour representation. | `colour`: exposure, temperature, tint, saturation, opacity, despillMode/strength and related fields. Exposure is stops, opacity a multiplier. Despill modes `none`/`export`/`always` determine whether the viewport shows it. This corrects appearance; it does not identify floating geometry. |
| `prune-low-contribution` | Reduce splats contributing little to rendered images, including faint, tiny or hidden data. | `prune.percent` is an upper reduction share, `protectAbove` protects higher average 1080p pixel coverage. Evaluated per chunk from surrounding views. It is a visual-contribution optimization; it may preserve highly visible unwanted junk and remove valid weak detail. Validate separately from spatial cleanup. |
| `bake-antialiasing` | Prepare an antialias-trained capture, such as Gracia MINT, for renderers without the matching compensation. | `bake.distance`, `screenHeight` at 45-degree vertical FOV. Widens thin splats and compensates opacity for the target viewing conditions. Changes rendering preparation, not semantic noise selection. |
| `animate-transform` | Apply keyed position/rotation/scale offsets to the whole subject over time. | `animation.keys` in reference-offset space, using the returned native schema. Key arrays are replaced in full when patched. VGS/PGS can carry motion samples; MINT cannot preserve this motion representation. |
| `walk` | Mark the capture as walking along +Z with a speed. | `walk.speed` in metres/second, optional display units. Preview moves the floor; VGS export records walking metadata and leaves point positions in place. Does not create limb animation or clean geometry. |
| `audio` | Attach the capture's own soundtrack or an external file, with offset, volume and optional looping. | `audio.source`, `file`, `offset` in seconds, `volume` 0..1 and `loop`. Paths belong to the editor's computer. Multiple tracks mix; VGS/PGS carry synchronized AAC audio and MINT carries MP3. |
| `loop-morph` | Close a loop without a jump: frames added after End that morph the End frame back into the Start frame - or, with a negative length, the range's last frames carrying the playing capture into Start (End stands for Start and is not played). | `morph.seconds` (length of the added frames; negative: the frames before End it overlaps, at most all but Start's), `morph.leadIn` (overlapping only: also blend into the motion of as many frames before Start, when the source has them) and `morph.markers`: `{number, end, endFrame, start, startFrame}` with `end`/`start` the same point of the subject in capture-local coordinates (before the transform) on the End and Start frames. Only markers whose frames match the current End/Start are used. Rather than writing coordinates, seek Start or End, take a snapshot and use `place_morph_marker` with points on it; `get_loop_morph` reads the markers, how far off each is and a suggested point for the next one; `remove_morph_markers` removes them. Removing modifiers above it shape the two frames it morphs; those below it apply to its frames. Seek past End to inspect the added frames (overlapping, the frames before End). Every export (.vgs, .pgs, .mint, .ply sequence) writes them after the range, or in place of its last frames. |

## Importing PLY frames and sequences

`import_capture` accepts Gaussian 3DGS PLY and ordinary XYZ/RGB point clouds, binary
little/big endian or ASCII. Select one file, not a directory. A final numeric suffix
starts a sequence forward from that file: same directory, prefix, extension and digit
width, stopping at the first missing number. Thus `00000048.ply` through `00000054.ply`
are seven frames; `00000056.ply` is excluded when 55 is absent. No final digits means
one frame. Spirula's `frames/NNN/splat.ply` layout is also supported by selecting one
`splat.ply`; the numbered parent directories determine the sequence.

Splats are followed by row, so a temporal import needs frames that hold every row in
every frame plus `birth_events.tsv`, which names each splat `(row, generation)`; its
frame column is local to the run's first file, even when import starts later. Spirula's
`frames/` and Forge's aligned `.work/vgs_<first frame>/` (kept with `-graciakeepwork`)
qualify. Selecting Forge's published frames switches to that run's aligned frames when
they exist, and otherwise fails naming the run's `.vgs`: published frames are decoded
from it and hold only the shown splats. Without a birth log, rows are trusted, with a
new record when a previously invisible row becomes visible or its scale changes; no
spatial nearest matching is done. Rows invisible throughout a chunk are omitted.
A chunk runs while each frame stays compatible with the one before (same population and
schema, not every row renewed, no `sequence.json` boundary), up to 255 frames and to the
lesser of 12 GiB and the free memory less 2 GiB for fitting it at export; incompatible frames are
chunks of their own. A saved project keeps its chunk layout. Records unchanged into the next chunk's first frame
run into that closing sample. Native export notes report the fit checked at every frame.

PLY defaults to 30 fps. Use `set_export_settings` with `{"plyFps":24}` to change it,
as in Metadata and processing. Frame indices and keyframes stay fixed; the timeline,
duration and exported timing change. `plyRowOffset` is normally 0, including subject-only
Spirula/Forge output. Only set it for PLYs with room rows prepended but omitted by the
birth log: it is the leading room vertex count. Changing it clears Erase picks as
record IDs change; Undo restores the mapping and picks. An adjacent `room.ply` is not
automatically included in the sequence.

`get_project.plySequence` records the exact files, original numeric indices, chunk
starts, optional birth-log path and local-frame origin. `save_project` persists it,
with paths relative to the project, and `captureSettings.plyFps`/`plyRowOffset`.
`open_project` reuses that frozen list instead of appending files added later.
The timeline starts at frame zero. A missing next file ends discovery normally;
an existing unreadable/corrupt PLY fails the import with its error. Frames are read
progressively and the identity index is spooled to disk. Export retains compatible
temporal chunks and uses disk spooling to avoid keeping all fitted chunks in RAM.
Native VGS/PGS export fits encoding 0 using the `vgsplyseq` algorithm, including
residual rotations and constant median higher SH per record. Source-frame preview
and PLY snapshots keep the original per-frame SH. Rank ordering is mapped back to
source record IDs when applying Erase and preview visibility caches.

## Audio sources and saved projects

Importing a MINT or VGS/PGS with embedded audio automatically creates an enabled
`audio` modifier named **Capture audio**. Read `get_project` first and reuse its ID;
adding another capture-source Audio layer plays the same track twice. Opening a saved
project preserves its layers, including disabled or removed Audio layers.

The UI's Source dropdown maps to these exact native values:

| UI source | Native fields inside `audio` |
|---|---|
| Included in capture | `"source":"capture"`; `file` is empty. Uses the source capture's embedded track, or silence if it has none. |
| External file | `"source":"file","file":"D:/Audio/music.mp3"`. MP3, AAC (.m4a), Opus and WAV are supported; the path is on the editor PC. |

Configure an external track using `edit_modifier`:

```json
{"modifier_id":"AUDIO_ID","patch":{"audio":{"source":"file","file":"D:/Audio/music.mp3","offset":0.25,"volume":0.7,"loop":false}}}
```

`offset` is seconds on the capture timeline: positive delays the track, negative starts
it earlier. Embedded audio also accounts for the capture's original start offset.
`volume` is 0..1. All enabled Audio layers mix during playback, following play, pause,
seek and playback speed. `loop` repeats this track; it is separate from the timeline loop.
Switch back to the embedded track with:

```json
{"modifier_id":"AUDIO_ID","patch":{"audio":{"source":"capture"}}}
```

Mute a layer with `{"modifier_id":"AUDIO_ID","patch":{"enabled":false}}`.
Disabling all Audio layers also silences exports. With no Audio layers at all, export
retains the source track for compatibility; removing a layer is therefore different
from muting it. With ffmpeg, VGS/PGS exports bake the mix to AAC and MINT to MP3,
including timing, volume and looping. Without it, only one unchanged track is kept;
MINT requires an MP3 starting with the capture. MINT export also writes the companion
MP3 used by GraciaStudio.

Edits change the live project; call `save_project` to persist them. `.vgsproj` stores
each layer's ID, name, enabled state, source, external path, offset, volume and loop.
External paths are saved relative to the project and resolved on reopening; embedded
audio remains in the source capture. The project references these files and does not
bundle their audio bytes. Move the project, capture and external files together while
preserving their relative paths. `open_project` restores the settings without creating
duplicate Audio layers.

## Crop volumes: removing the bulk first

Crops are the most effective way to remove large unwanted data (floor, rig parts,
background clouds): one crop acts on every frame and chunk at once, unlike Erase picks.
Use them before Purge Isolated and Erase.

**Shapes.** All sizes are in metres; Y is vertical. A crop's pivot is at its **base**:
local Y runs from 0 (bottom) to `height` (top), so `crop.position[1]` is the bottom,
not the centre. With the subject placed on the floor (feet or base at Y=0, centred on
X=0, Z=0, by eye), put the crop's base at [0,0,0]: everything below Y=0 is then cut as noise.
Never place the base from the lowest point or `bounds` minimum; stray noise sits there.

- `cylinder`: elliptic, `radius` along local X, `radiusZ` along local Z, plus `height`.
  Usually the best `keep` volume around a person, since it matches a standing or
  turning figure and the rig's inward-facing layout.
- `box`: `width` (X), `depth` (Z), `height` (Y). Best for flat or rectangular regions:
  a thin `remove` box for a floor slab, or a box around a rig fragment or prop table.

`crop.position`, `rotation` (degrees), `scale` and `shear` place the volume in world
space. Crops do not follow `set_capture_transform`; move them separately if the capture
is transformed.

**Combining crops (`mode`).** Stack order between crops does not matter. A point is kept
when it is inside at least one `keep` crop (or there are no `keep` crops) and inside no
`remove` crop:

- several `keep` crops add together (union), e.g. a cylinder for the body plus a box
  for a prop held at arm's length;
- `remove` crops subtract, and win wherever they overlap a `keep` crop, e.g. a keep
  cylinder around the subject plus a thin remove box for the floor under the feet;
- with only `remove` crops, everything outside them is kept.

**Order with other modifiers.** Processing order is fixed, not the stack order:
Prune Low Contribution and Erase picks, then crops and Remove green, then Purge Isolated
on what remains. Purge Isolated therefore computes its median distance only on points
that crops keep, so cropping the bulk away first makes its threshold more meaningful.
Stack order only matters between Remove green and Colour (a Remove green below a Colour
modifier tests the corrected colour).

**Typical setup.** First place the subject with `set_capture_transform`: centred on
X=0, Z=0 by eye (top and front views) and with its feet or base on the floor grid at Y=0
by eye in the front view; noise below the subject makes the lowest point unreliable. Then add one
`keep` cylinder with its base at [0,0,0] covering the full motion with a margin of
0.1-0.2 m. Its base cuts everything below Y=0. A new crop initially fits the capture;
read its values with `get_project`, then patch only what changes, e.g.:

```json
{"modifier_id":"ID","patch":{"crop":{"shape":"cylinder","mode":"keep","position":[0,0,0],"radius":0.6,"radiusZ":0.6,"height":2.0}}}
```

Crop animation keys can be patched when they already exist; this MCP has no dedicated
tool to create crop keys, so prefer a static crop large enough for the whole motion.

## Example: inspect, clean and validate

1. Call `list_editor_sessions`; explicitly select the intended window if needed.
2. Call `pause_playback`, `get_editor_state` and `get_project`. Record the user's range,
   existing modifiers and selected modifier; do not overwrite their work with a preset.
3. Use `set_render_mode(mode="points", point_size=2)`, `set_view` and
   `get_viewport_image(max_width=1024)`. Follow the survey baseline in the workflow:
   front, side, top and perspective at first/middle/last frames and both sides of chunk boundaries.
   Place the subject with `set_capture_transform`: centred on X=0, Z=0 and feet/base on the
   floor grid at Y=0, judged by eye in the top and front views, not from the lowest point.
   Then add a `keep` cylinder with its base at [0,0,0]; it also cuts everything below Y=0.
   Add `remove` boxes for remaining rig fragments, checked across the full motion.
4. If noise is isolated and sparse, add a `purge-isolated` modifier and use its returned ID:

   ```json
   {"modifier_id":"ID_FROM_ADD_MODIFIER","patch":{"isolation":{"neighbour":4,"medianPercent":900}}}
   ```

   This raises the threshold from the default 700 to 900 for a conservative comparison.
   Compare enabled/disabled images. Tune from the observed result; this filter is not
   mandatory when the unwanted data is dense or touches the subject.
5. For a remaining detached fragment, add/select an enabled `erase`, call `set_erase_editing(true)`,
   then get an image. Use `erase_brush` or `erase_lasso` on the observed fragment, avoiding
   the subject in depth. Use the image's `view_id`; get a new image after each stroke.
6. Turn Erase editing off and inspect in splats. Check fine details and full motion;
   undo or soften an edit when it damages the subject. Repeat separately in other chunks.
7. Save a `.vgsproj` if requested. For delivery, start an export to a new requested path,
   poll its job until terminal, and report success only for `done`, including relevant notes.

## MCP resources and prompt

- `vgs-editor://workflow`: app purpose, rig context, cleanup sequence and operating rules.
- `vgs-editor://tools`: modifier behavior and cleanup examples; no editor changes.
- `clean_noise` prompt: the workflow plus the caller's `detail` objective, defaulting to
  preserving the subject and fine details. Requesting the prompt performs no cleanup;
  the assistant still needs to inspect images and execute tools.
