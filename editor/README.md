# VGS Editor - phase 1

VGS Editor is proprietary commercial software, **not freeware**. Ownership
and licensing rights in the original software remain with **Víctor
M. Feliz**. **ScanMeNow** and **The4DScanner** receive only the internal-use
permission in [LICENSE.md](LICENSE.md), with no sale, redistribution, sublicensing
or ownership rights. Other uses require a separate written licence and the
agreed payment conditions.
The free VGS Decoder licence does not grant permission to use this editor.

Qt 6 desktop editor for animated `.vgs`, `.pgs` and `.mint` captures.
The viewport draws **opaque points** with depth testing. Point size is fixed in
pixels and adjustable; Gaussian opacity and individual scale do not affect the
preview. Temporal activity determines which records are displayed. The scene
scale transforms the entire capture without changing point size.

## Controls

- `Ctrl+O`: open a capture. Drag and drop or a command-line path also work.
- `Ctrl+Shift+O`: open a `.vgsproj` project.
- `Ctrl+S` / `Ctrl+Shift+S`: save project / save as. Project commands are in File; there is no Project toolbar.
- `Ctrl+Z`: undo, `Ctrl+Y` or `Ctrl+Shift+Z`: redo (Edit menu). Undone are edits to the project:
  the capture transform, adding, removing, reordering, renaming or toggling modifiers, their
  parameters and keys, the Start/End range, presets loaded and the export settings. A drag, or
  typing in one field without pausing, is one step. The camera, the playhead, display options
  and Global/Local are not edits, and opening a capture or project starts the history afresh.
- Space: play or pause. Arrow keys: previous or next frame.
- Drag: orbit; right/middle drag or Shift+drag: pan; wheel: zoom. Double-click: the pivot glides to
  the point under the cursor (what is seen there: through faint splats to the first that covers
  it), keeping the angle and distance, as the web viewer does; any navigation cancels the glide.
- `Numpad decimal` / `Numpad Del` / `F`: Focus visible. `Ctrl+E`: export the edited capture. `Ctrl+Shift+E`: export the viewport as PNG.
- `G` / `R` / `S`: activate Move / Rotate / Scale. Repeat the active mode's key to switch Global/Local. `Esc`: leave all transform modes, and what `Tab` entered (crop or Eraser editing).
- `Shift+G` or **View > Grid**: toggle the floor grid. **View > Axes** toggles the coloured
  X/Y/Z axes and **View > Origin and front marker** a faint square round the origin and a grey chevron on
  the floor just past a metre along +Z,
  pointing the way the capture faces; each on its own (grid and marker on, axes off at first).

The timeline uses Gracia Converter's range control: drag the upper Start marker, lower End marker, or white playhead. Above it, the clock toggle comes first, followed by Start/Frame/End on the left; transport buttons are centred and Loop/Speed sit on the right. The frame field reads **Frame X of Y**, with a zero-based index and the full capture frame count. The clock switches all three fields to seconds; typed values snap to the nearest valid frame. Start and End are inclusive and define the export range. Current time / full duration appears below decode statistics in the viewport. The unit preference is remembered without changing the project range.
The single **Transform** group provides Position, Rotation (XYZ Euler degrees)
and Scale on three compact rows, plus reset. X/Y/Z labels use the red, green and
blue modifier colours. Numeric values remain fully editable without stepper
buttons; keyboard and wheel stepping still work. The scene matrix is
`T * Rz * Ry * Rx * H * S`, where H preserves shear from global scaling.
View-dependent colour uses spherical harmonics automatically,
with no UI toggle. Older projects with SH disabled still open with SH enabled.

Each transform group has a checkable G/R/S button before X; only one mode is active.
Clicking an active button switches that mode off; repeating its keyboard shortcut
keeps the mode active and alternates Global/Local reference space.
The gizmo appears at the current target's origin or cylinder base. Each XYZ row
has a **Global/Local** toggle directly after its Move/Rotate/Scale button; Global
is the default. The space button has no checked/green state and is available only
while that transform mode is active. Repeating G/R/S toggles
the position/orientation/scale reference space. Numeric fields use the chosen
space as well. Global gizmos align with world axes; local gizmos follow the target.
Global nonuniform scaling preserves its full affine result, including shear.
Local rotations are composed with the current orientation and displayed as XYZ
Euler angles. The centre handle moves in the
camera plane in Move mode and scales uniformly in Scale mode. Gizmos retain a
constant screen size and stay visible over the point cloud.

Navigation remains available in every mode: dragging outside the gizmo orbits,
right/middle drag or Shift+drag pans, and the wheel zooms. Transformations only
change while dragging an actual gizmo handle. `Esc` also cancels an unfinished
gizmo drag; releasing the mouse commits it. Numeric XYZ values update during
manipulation, and scene transforms leave source Gaussian attributes intact.

## View cube and keyboard views

The top-right view cube has clickable faces for Front, Back, Left, Right, Top
and Bottom; the same views are in **View > Standard views** and on the numeric
keypad. Standard views default to orthographic; orbiting with the
mouse or numeric keypad returns immediately to free perspective. Pan and zoom
preserve a fixed orthographic view. Free orthographic views are intentionally
unsupported. The capture coordinate system remains Y-up.
Selected-view highlights clear when orbiting. Hover highlights are temporary and
clear immediately when the pointer leaves the cube.

## Reusable editor presets

The Tools panel offers **Save preset…** (Ctrl+Shift+P), a **Load preset…** dropdown
and a folder button (Ctrl+Alt+P). Give the preset a name; saving over an existing
name asks before replacement within that scope. Selecting an entry restores capture and crop
position, orientation, scale/shear, cylinder dimensions and crop state, reference
spaces and playback settings. Viewport camera and display controls are excluded.

Built-in presets ship in a `presets` folder beside the program (the build copies
`editor/presets` there) and are listed with the user's, read-only: the folder button opens
only the user's folder, and saving under a built-in preset's name writes the user's own copy,
which then takes its place in the list while the built-in file stays as it is.
The loaded capture path and its
current time/range and metadata remain unchanged, so presets work across captures.

Capture Tools presets use `.preset` files with format `vgs-editor-preset`.
Metadata presets use `.presetmetadata` files with format `vgs-editor-metadata-preset`.
Capture Tools uses schema version 6 and metadata uses version 4; both record
`scope` and `appliesTo`. They may have identical
display names; saving/replacing a metadata preset never overwrites a Capture Tools
preset. Each dropdown accepts only its own extension, format and scope; invalid or
renamed files are excluded.

Old JSON presets are converted automatically into separate typed files. Mixed
version-1 templates produce one file of each applicable type; scoped version-2
templates produce only their own type. Original JSON files are retained as backups
and are not shown in the dropdowns. Existing native presets take precedence over
legacy data, so later edits are preserved.

Loading a Capture Tools preset keeps the current viewport camera, including its
target, orientation, zoom and projection. Loading a metadata preset keeps the
current Title and Catalogue ID, including edits not yet applied. Title appears
before Catalogue ID in the metadata panel.

Presets are separate, atomically saved JSON files in Qt's AppData directory:
on Windows, normally `%APPDATA%/THE4DSCANNER/VGS Editor/presets`. The folder button
opens that location in the system file manager; its button is grey while capture controls are disabled.
The dropdown refreshes when returning to the application after external changes.
Saving a reusable preset does not replace or save the current `.vgsproj` project.
Automated previews and preset tests use temporary folders.

With focus in the viewport, the numeric keypad follows Blender's navigation keys:

| Key | Action |
|---|---|
| Numpad 1 / Ctrl+1 | Front / Back |
| Numpad 3 / Ctrl+3 | Right / Left |
| Numpad 7 / Ctrl+7 | Top / Bottom |
| Numpad 9 | Opposite view |
| Numpad 5 | Toggle orthographic/perspective while in a fixed view |
| Numpad 2/4/6/8 | Orbit in 15-degree steps, returning to perspective |
| Ctrl+Numpad 2/4/6/8 | Pan |
| Shift+Numpad 4/6 | Roll in 15-degree steps, returning to perspective |
| Numpad +/- | Zoom |
| Numpad decimal / Numpad Del / F | Focus visible capture and ghost points |
| Numpad 0 | Reset the user perspective |

The keypad works with Num Lock on or off. Number keys used in numeric fields keep
editing their values. Navigation mappings follow the
[Blender navigation manual](https://docs.blender.org/manual/id/3.6/editors/3dview/navigate/navigation.html);
Numpad 0 resets perspective because this editor has no separate scene camera.

## Modifier stack

The stack runs top to bottom. A Remove green above a Color modifier tests the capture's own
colours, whatever Color then does to them (despill shown in the viewport included); one below
it tests the colours Color leaves. The viewport and every export agree on this.

The add list runs in the order a capture is usually worked on, with separators between the
groups, and F1 to F10 add each kind directly (the key is in its name): clean it — Crop (F1),
Remove green points (F2), Purge Isolated (F3), Eraser (F4); its look — Color (F5); its motion —
Animate transform (F6), Walk (F7); its sound — Audio (F8); and delivery — Prune low
contribution (F9), Bake anti-aliasing (F10).

Modifiers form a flat list below all playback/timeline controls. Each row has a
name, an eye toggle and a full-duration coloured bar without text. The eye switches
between eye/eye-off icons; inactive modifiers and a disabled panel use grey icons.
Type is
identified by colour and tooltips rather than a separate visible column. There are
no container layers or fixed modifier-count limits. Rows have the same 30 px
minimum height, including the initial crop. A fresh capture starts with an enabled
crop automatically fitted to its bounds. The panel reserves enough height for
four rows. Each group has its family of hues: reds darkening through cleaning (Crop
#EF3450, Remove green points #D83A60, Purge Isolated #A3214F, Eraser #781638), grey for the
look (Color #A0A6AD), blue and cyan for motion (Animate transform #3B82F6, Walk #20C9D2), yellow
for sound (Audio #F4C542) and greens for delivery (Prune low contribution #70C45A, Bake
anti-aliasing #328D62). Crop wire volumes keep the section-heading red.

Add Cylinder, Box, Remove green points, Animate transform, Purge Isolated, Prune low
contribution or the others from
the type selector. Select a row to
edit that modifier's properties in Tools. The context menu provides add, rename,
duplicate, move, remove and temporary enable/disable; buttons provide the same
editing operations. Disabling/removing a modifier immediately updates the list and
preview. Removing the final modifier leaves the source unfiltered.

All enabled crops combine by **union** across the full source timeline: a Gaussian
is retained if any crop contains its transformed world-space centre. With no active
crop, all centres are retained. Colour-removal modifiers then exclude matching
source RGB values. Order does not alter these two operations in this version.

Remove green defaults to minimum HSV saturation **50%** and maximum circular hue
distance **45 degrees** from pure green (**120 degrees**). By default, clamped
source DC RGB is converted from sRGB to linear RGB before HSV classification.
The Linear RGB checkbox changes this per modifier. Older presets without a colour
space field retain sRGB classification. Classification is independent of
camera/SH appearance and runs before despill. Neutral
or black RGB has undefined hue and is not matched. Preview and both export paths
share these rules; source records remain immutable.

**Animate transform** stores keyframes as local offsets from the capture reference
pose. Select it and use **Set key**, or edit the current-frame XYZ fields/gizmo to
automatically create/update a key. Position/scale interpolate linearly; rotations
use shortest-path quaternion slerp. Values hold before the first and after the last
key. Multiple animations compose in modifier order without changing the reference.
The key table edits frame, position, rotation and scale; each XYZ cell accepts three
comma-separated values. **Remove key** deletes the selected key. White diamonds
appear on the modifier's timeline bar and clicking one seeks to its frame. Disabled
animations retain keys and leave the reference pose visible; table edits remain
available, while the gizmo is disabled until the modifier is enabled again.

**Prune low contribution** removes the splats that add least to the image. Each chunk is
rendered on the CPU from 24 views around the capture (a ring from afar, a ring from close,
four from above and four from below) at four instants, drawn as the viewport draws splats,
and every splat is credited with its blending weight over the pixels it touches: what those
images would lose without it. Hidden, faint and tiny splats score almost nothing. Up to
**Remove up to** (default **15%**) of each chunk's splats are removed, lowest first, but never
one scoring above **Protect above** (default **0.25 px**: its area at full weight in a 1080p
view, averaged over the views it is scored from), so a capture where every splat counts loses
nothing; splats alive at none of the four instants are not measured and never removed. The
decision is per chunk and per splat, for the splat's whole life in it, so the preview shows
exactly what the export writes and native temporal blocks are kept. The panel says what happened to
the chunk on screen: the share removed, or, in amber, that Protect above kept it below the
share asked and how many splats it kept. The export summary gives the share removed over the
whole capture and in how many chunks the protection limited it. On Gracia captures 15%
measured about 14-15% smaller files with renders differing by 50 dB or more on average
(at least 40 dB in the worst view); scoring adds about a second per chunk to the export.

**Eraser** removes splats picked by hand. While it is selected (and enabled) with **Edit** on —
the default; **Tab** toggles it — the viewport picks:
a left drag paints with the **Brush** (its radius in pixels; Ctrl + wheel changes it) or draws a **Lasso**, and on release
the visible splats inside are added to the picks of the chunk on screen; with **Ctrl** they
replace them and with **Alt** they are subtracted. The right button orbits and the middle one pans meanwhile. Picks show in pink
while Edit is on; with it off the viewport shows the result, the picks removed, with the usual
navigation, as it does when the modifier is not selected and in every export. A splat is
itself only within one chunk of the source, so each chunk keeps its own picks, as record indices
within it, stored in the project as runs; the panel counts them for the chunk on screen and for
all, and clears either. Each stroke is one undo step.

**Color** corrects the whole capture: **Opacity** first, every splat's opacity times a factor
(at most opaque), the training-time opacity boost of SuperSplat and Spirula applied to a finished
capture; then **Exposure** in stops, white balance (**Temperature**, positive warmer; **Tint**,
positive greener) and **Saturation** about Rec.709 luminance. The colour changes are one 3x3
matrix on the colour, base and view-dependent alike, so the viewport shows them as they are set
(Color modifiers compose in stack order) and exports keep the capture's own representation:
the native export applies the matrix to the SH codebooks and the base-colour trajectories,
rebuilds the 256-entry base-colour table from the new values' quantiles, and scales the opacity
trajectories; the sampled export applies them to each splat. **Apply despill** removes
green-screen spill: **None**, **Only on export**, or **Always**, which shows it in the viewport too
(the export's own despill on each decoded frame). Its settings start as THE4DSCANNER's and are
disabled, labels included, with None. Despill lives only here now: Metadata and processing no
longer has it, and a project, preset or task that despilled there reads with a Color modifier
named Despill, only on export, with its settings.

**Audio** plays a sound track with the timeline: the capture's own (a .vgs can carry one)
or a file (MP3, AAC .m4a, Opus or WAV), with an **Offset** that slides it along the capture's
timeline to line it up, a **Volume** (100% as recorded) and **Loop**, which repeats the track
from its start whenever it ends. Every enabled Audio modifier plays, mixed, so a voice, music
and effects can each have their own level. The editor plays them at the timeline's speed,
keeps them in step through seeks and loops, and a track is silent before it starts and, unless
looped, after it ends; disabling a modifier mutes it.

Exports to VGS/PGS make the soundtrack with ffmpeg (`tools/ffmpeg.exe` beside the program, or
`VGS_FFMPEG`): every enabled Audio modifier's track - or, with none, the source's own - placed
where it plays, at its volume, mixed and cut to exactly the exported range as AAC (.m4a,
160 kb/s, which every browser and iOS play), so nothing outside the range is stored and the
track starts with it (`startTick` 0). Without ffmpeg the last Audio modifier's file travels as
delivered, or a .vgs source's own track whole, with `startTick` saying where the range starts
on it, and a note says so. MINT cannot hold audio. The build copies ffmpeg beside the editor
from the CMake cache path `VGS_FFMPEG`; it is not part of the repository.

Exports to VGS/PGS take the viewport, as it is when the export starts (or when an export task
is made), as the capture's thumbnail: the centre square, 256 px, JPEG, without the grid, gizmo,
crop wires, ghost or help text. They also write the editor's view (target, camera position and the
45 degree vertical field of view, in the exported coordinates) as `view` in the second
metadata block; the web viewer opens a capture from it when its scene defines no camera.

**Purge Isolated** uses the exact Nth-neighbour distance, excluding self, and the
global upper median of those distances among points surviving crops/colour filters.
A point is removed when `d_N > median(d_N) * percentage / 100`. Defaults follow
SMNForge's Nth-neighbour rule: **N=4**, **700%** (7 times median). Lower percentages
remove more. Each frame is evaluated independently; small populations with at most
N points and a zero median are preserved. Preview processing runs in the decode
worker, so camera navigation does not rebuild the KD-tree. VGS/PGS/MINT export uses
the same rule, retaining temporal blocks when the capture transform is constant.
An animated transform (moving, rotating, uniform scale) exports to VGS/PGS as the
native temporal blocks plus the capture's motion stored as samples, so the file is
the size of a static export. Readers apply the motion; readers built before it refuse
the file rather than play it in place. Non-uniform animated scale or shear is refused
with a message, and so is MINT output, which cannot store motion. A moving .vgs keeps
its motion when edited again. See docs/ANIMATED_EXPORT.md.

**Walk** previews a capture walking on a treadmill: the capture stays where it is and
the floor slides back under it at the set speed (m/s), with a quarter-metre grid added
and its edge faded out. At the right speed a planted foot stays on the grid, which is
how to set it. The axes stay fixed; time is counted from the start of the export range. It is
not baked: VGS/PGS exports keep the capture in place and write `motionType = walking`
and `movingSpeed` to the header, for players to carry it along +Z (rotate the capture to
choose the direction). Several active Walk modifiers add up. MINT has no such field and
reports that the walk is not stored.

**Anti-aliased splats.** Gracia trains its captures with anti-aliasing, which leaves needle
splats far thinner than a pixel; drawn at full opacity they show as solid coloured lines.
The Gaussian view draws a `.mint` with the matching opacity compensation, and a `.vgs` when
its header carries the anti-aliasing render hint. Exports write that hint for anything
converted from a `.mint` and keep whatever a `.vgs`/`.pgs` source says.

Projects and editor presets preserve the flat stack, activation and selected row.
Older projects/presets migrate their one crop to one crop modifier without changing
its world-space placement or activation. Metadata presets still affect metadata only.

## Crop volumes

**Mode** chooses what a crop does: **Keep inside** preserves what is inside it, **Remove
inside** deletes it. A point survives when it is inside some Keep crop (or there is no
Keep crop) and inside no Remove crop, so Remove wins where they overlap. The volume is
drawn green for Keep and red for Remove. **While editing** only affects the view while
the crop is edited: what the crops would delete is shown in red, or hidden. Neither the
normal view nor the export depends on it.

The Move gizmo also has a square between each pair of axes, coloured by the axis it is
normal to, for moving in that plane.

Choose **Cylinder** or **Box** in the Shape dropdown. Both retain their own horizontal
dimensions and share the same height and transform. **Edit** (C) creates a volume automatically fitted to the capture and activates
editing. Its pivot is the centre of its base, with local Y running from 0 to its
height; rotation and scaling keep that base fixed. The crop is independent in
world space: moving the capture does not move it. While
editing, it is drawn as a wireframe, crop clipping is paused while colour filters remain active, and the
shared XYZ fields and G/R/S gizmos affect the cylinder instead of the capture.
Radius/height configure Cylinder; width/height/depth configure Box. Local XYZ
scale can also produce an elliptical cylinder. **Fit capture** (Ctrl+F) refits the volume to capture bounds, resets
its transformation, enters Move mode and returns keyboard focus to the viewport.
A crop's **Animation** can be Static or Animated at any time, each keeping its own pose. Switching
to Animated adds no key: the crop stays where it is until it is edited or **Set key** is pressed,
which adds the first key at the current frame.
Press S afterwards to select Scale; pressing S again switches Global/Local.

The two buttons next to Edit reset the cylinder's position, orientation, scale
and shear in world coordinates, place its base at (0, 0, 0), and enter editing
without changing the capture transform:

- **T4DS Preset** (Ctrl+Alt+1): height 2.5 m, radius 1.5 m (Box width/depth 3 m).
- **SMN Preset** (Ctrl+Alt+2): height 2.5 m, radius 1 m (Box width/depth 2 m).

`Tab` (or `C`) toggles crop Edit; its button and tooltip track the shortcut.
Turn editing off to hide the cylinder and clip points whose centres lie outside
the union of enabled crops. **Disable crop** (Ctrl+Shift+C) disables the selected modifier while retaining its settings. The preview clips on the GPU without
changing decoded records, so camera/crop changes do not require frame decoding or
point-buffer uploads. Membership is evaluated against transformed Gaussian centres
inside the fixed world-space volume.
Modifier definitions, metadata/processing, reference spaces and fixed camera views are saved in version-7
projects. Earlier projects and presets still open; their capture-local crops migrate
to world space without changing their existing placement. Version-2 cylinders migrate from centre
to base pivots without changing their volume. Editing activation is temporary; reopening applies
the saved crop preview. Export permanently excludes outside centres in each sampled frame.

## Metadata and processing before export

**Metadata & processing…** (Ctrl+M) is a main Tools button, independent of the
export command. It follows the converter's metadata fields and adds tags, software
information and extra JSON. Configure VGS/PGS, SH degree, playback and despill
(strength, green gain, view chroma and skin recovery). Apply remembers the last
configuration in QSettings and stores it in the current project. The panel uses
its own metadata/processing preset scope, sharing the same folder as the editor.
Loading a metadata preset does not modify capture or crop transforms.

These options are applied during **File > Export capture…** (Ctrl+E); preview
colours continue to use source data. Export writes signed encoding-0 VGS (compressed)
or PGS (plain), with baked positions, full affine covariance, SH colour and the world
crop, for the inclusive Start/End range. It runs off the UI thread with progress/cancel,
checks every output frame with the decoder and atomically publishes the verified file.

A `.ply` destination writes the range as a sequence of 3D Gaussian Splatting `.ply` files
instead, one per frame beside it, numbered by the source's own frame with at least four
digits: `take.ply` over a range starting at frame 30 gives `take0030.ply`, `take0031.ply`…
Each is what **Export current frame as PLY** writes for that frame (transform and keyframes,
modifiers, colour processing, SH degree); a `.ply` holds no audio, thumbnail or metadata.

For constant-rate sources, translation, rotation, reflection and uniform scale use
native temporal chunks. Crop removes unused rows, splits rows only on visibility
reentry, and compacts unused dictionaries instead of duplicating the capture for
every frame. Translation/uniform scale preserve the original rotation and colour
coding. Rotations resample orientations at native frames and refit rotated SH
trajectories, reporting coefficient RMS/maximum error. Both output formats remain
compatible with the existing encoding-0 decoder.

Nonuniform scale/shear and variable-rate MINT sources currently use the sampled
fallback and can produce larger files. Its SH warp uses a finite-band approximation.
The `.vgs` extension does not guarantee a size reduction for arbitrary transformed
attributes; the native path avoids the earlier 30-fold duplication of static data.
Source audio/thumbnail are omitted because they may no longer match the edited clip.
See [EXPORT_DESIGN.md](docs/EXPORT_DESIGN.md) for the implementation and limits.

Projects atomically save a relative capture reference, transform, camera, time,
playback range and display settings. Keep the capture with the project when moving
it to another computer. This phase supports one capture per project. Selection,
manual deletion, attribute editing, rigging, audio and Gaussian
splat rendering are future work.

## Estimate export size

**Export > Estimate export size…** (Ctrl+Alt+S) says what a .vgs export of the Start/End range
would weigh with every modifier and setting as they are, without writing it: four one-second
windows spread over the range are exported for real to a temporary folder and their chunks
extrapolated, and the header, metadata, thumbnail and audio are measured. A range of four
seconds or less is exported whole and the figure is exact. It gives the size, the average data
rate and that of the heaviest chunk sampled, and whether the capture streams without waiting at
10, 25 and 50 Mbit/s (its peak within 80% of the link).

## Export tasks

**Export > Export task…** asks for an output exactly as Export capture does (`.vgs`, `.pgs`,
`.mint` or a `.ply` sequence), but writes a `.vgstask` beside it instead of exporting (`result.vgs` gets
`result.vgs.vgstask`, so the same project queued to `.vgs` and `.pgs` keeps both): a copy of the project as
it is at that moment (capture, transform, modifiers, export settings and metadata), the
output, and how many frames it exports. Later edits to the project do not change the task.
Paths are stored absolute and relative to the task, so a folder of tasks moved together with
its captures still runs.

**Export > Process tasks…** opens one or more `.vgstask` files (or takes them dropped on the
window) and exports them one after another. Every task is checked before anything runs: a
missing capture, an output folder that cannot be written or a combination the export refuses
(an animated transform to MINT) is skipped with the reason. One bar follows the task being
exported, the other the whole queue by frames. Existing outputs are overwritten. A task that
fails is reported and the queue goes on; Cancel task moves on to the next, Cancel all stops,
and neither leaves a half-written file. The computer is kept from sleeping while it runs, and
a `vgstasks-<date>.log` beside the first task records what each task did.

The same from a command line, without opening the window:

```
VGSEditor.exe --process-tasks a.vgstask b.vgstask
```

It prints progress to stderr and returns 1 when any task failed.

## Appearance and resources

Application text and numeric formatting use English. Open/save dialogs use the
native operating-system UI and follow its language. The Fusion theme adapts
Motiva Layama's surfaces, with a subtly lighter, more neutral variation: window RGB (28, 29, 30), raised panels (36, 37, 38),
subtle borders and flat, recessed controls. The viewport independently keeps its
original dark neutral background, RGB (19, 19, 19). The editor keeps red headings
RGB (240, 60, 90), green interaction accents and its modifier colours.
The theme is local in `editortheme.h/.cpp`; no Layama checkout is needed at runtime.
Small spinner/combo arrow PNGs are copied from Layama into `assets/theme` and
embedded in the editor resources.
Combo boxes use a larger local SVG chevron for a clear dropdown indicator;
their popups highlight the hovered item with a brighter grey. Ghost and timeline
unit toggles share the theme's neutral checked-button styling.
The application icon uses the copied Gracia artwork in `assets/gracia`.
Resources are embedded in both qmake and CMake builds. The ICO is also embedded
in the Windows executable. No converter checkout is required at runtime.

**View > Compact controls on smaller screens** starts enabled. On Full HD it
reduces fixed controls, icons, spacing and margins to 87.5%, keeping fonts at
their readable native size. A 4K display at 150% retains the normal layout.
Qt always uses native Windows DPI for rendering, repaint regions, mouse
coordinates and tooltip placement; the editor sets no scale environment overrides.
Moving between monitors adapts control geometry using public widget APIs.
The preference belongs to the application and is excluded from capture presets.
Restart applies a preference change; disabling it restores normal widget geometry.

## Autosave and recovery

Every two minutes, while there are unsaved changes, the editor keeps a copy of the project in
its local data folder (`autosave/`, one file per running editor). Saving, starting a new
project, opening another one or closing without saving removes it. A copy still there when
the editor starts without a file to open is from a session that ended unexpectedly: the editor
offers it, and recovering opens it as unsaved changes to the project it came from.

## Recent files and preferences

On Windows, QSettings stores preferences under
`HKEY_CURRENT_USER\Software\THE4DSCANNER\VGS Editor`.
The File menu offers **Open recent**, listing up to ten successfully
opened captures/projects or saved projects, most recent first. Entries persist
across restarts and are deduplicated; **Clear recent** clears the list while
preserving dialog folders. Missing files produce a message when selected.

Below the cube, the view-cube group holds a full-width **Background**
button that flips the viewport between Dark (default) and Light. It only changes
the viewport background, grid contrast and overlay text; the rest of the
interface is unchanged and the choice is never saved in projects, presets or
settings. The full-width Point size field follows and stays enabled before
opening a capture. Point size starts at **5 px**; projects and user settings
retain overrides. The floor grid (`Shift+G`), the axes and the front marker are
toggled separately from **View**, and saved with the project. The top-left viewport statistics remain unobstructed.
The small **Ghost comparison** toggle below Point size starts off. Turning it
on freezes the currently visible points in world space as a white reference with
a 15%-opacity interior by default and a soft, brighter outline. The unlabelled,
full-width slider below the ghost icon adjusts opacity and is disabled without
a frozen ghost. Opacity is composited
once per pixel, so overlapping points do not turn opaque. The centred button uses
separate Ghost/Ghost off icons, with the off state in grey.
Time, transforms and modifier edits do not alter the snapshot; camera
navigation still works. Turning it off or opening another capture removes it.
The ghost is a transient preview overlay and is not saved/exported as capture data.
Ghost state, opacity, background, Point size and Grid and axes are excluded from reusable
presets; loading older presets ignores their preview settings as well. Capture
Tools preset version 6 no longer stores viewport camera or display fields.
Focus visible uses the points surviving current crop/colour/isolation filters,
plus the active ghost, rather than the whole source bounding box or crop gizmo.
Native dialogs remember separate capture, project and image locations. Display
settings (point size, grid), playback speed/loop, window geometry and dock
layout are also remembered. New captures/projects inherit display and playback
defaults; saved projects retain their own settings. Smoke tests use an isolated
temporary settings store and do not modify the user's registry.

## Build in Qt Creator

Open **`editor/VGSEditor.pro`** with Qt 6.8 MSVC 2022 x64. Sources and headers are
resolved from `$$PWD`, including shadow builds. After adding or changing sources,
use **Build > Run qmake** if Creator retains an earlier Makefile.
Both qmake Debug and Release builds have been checked.

CMake builds the same sources and adds automated tests:

```powershell
cmake -S . -B build/cmake-msvc -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/Qt6.8.1/6.8.0/msvc2022_64
cmake --build build/cmake-msvc --config Release --parallel 8
$env:PATH = 'C:\Qt\Qt6.8.1\6.8.0\msvc2022_64\bin;' + $env:PATH
ctest --test-dir build/cmake-msvc -C Release --output-on-failure
```

Requires OpenGL 3.3. CMake's `VGS_ROOT` overrides the VGSFileFormat source directory;
its default is the parent folder. qmake compiles the same sources directly, so
Debug and Release use their respective runtimes without prebuilt `.lib` files.
Use `windeployqt` to run outside Qt Creator.

## Version, deploy and installer

The version is set once, in `licensemanagement.h` (`VERSION_NAME`, e.g. `"v1.0.0"`). The
window title and About show it, the exe's version information carries it (qmake and CMake
read the header), and the installer reads it too.

`deploy.ps1 -BuildDir <qmake Release build folder>` makes `<build>\deploy`: the editor,
windeployqt's Qt libraries and plugins with the MSVC runtime, the built-in presets, ffmpeg
and the licence. In Qt Creator it is a Custom Process Step under Deploy (Command
`powershell.exe`, Arguments `-NoProfile -ExecutionPolicy Bypass -File "<editor>\deploy.ps1"
-BuildDir "%{buildDir}"`, Working directory the editor folder).

`installer/VGSEditor_InnoSetup.iss` (Inno Setup 6) packages that folder into
`installer/Output/VGSEditorInstaller_<version>.exe`: Program Files install, Start menu and
optional desktop shortcut, `.vgsproj` opening in the editor, the licence shown before
installing. Updates replace the built-in presets; the user's own stay in their AppData.

## Data and rendering

- `CaptureWorker` owns the decoder on a worker thread. VGS uses its official API
  and authenticates the container. MINT reuses the converter's format-6 reader in
  `dependencies/mint`, preserving its reconstruction and interpolation rules.
- `RenderFrame::records` retains **all** decoded records: position, XYZW rotation,
  XYZ scale, RGB and original opacity. `active` retains lifetime information;
  `chunkIndex` identifies the chunk. Row identities are local to each chunk.
  SH coefficients are reconstructed with each displayed frame; all original
  coefficients remain in the source capture.
- `RenderFrame::points` is a compact 28-byte preview: position, RGB and source row
  index. It contains no opacity or scale. Preview generation never alters the
  underlying Gaussian attributes.
- `Viewport` receives frames and scene transforms without knowing file formats.
  Camera and transform changes update uniforms without decoding or reuploading
  point buffers. A new frame is uploaded once.
- Seeking keeps one decode request in flight and coalesces later requests into the
  latest requested time. The UI displays the latest completed frame while waiting.
  Failed opens preserve the previous capture. New projects release decoder caches.

## Qt and libigl

The architecture uses **Qt for UI and the OpenGL context, with libigl reserved for
future geometry processing**. No unused libigl dependency is introduced yet.
The available checkout is `D:/Dependencias/libigl/libigl_git`.

| Approach | Benefits | Costs |
|---|---|---|
| QOpenGLWidget + custom renderer | Native timeline and docks; direct control of GPU data; scene transform gizmos | Implement Gaussian selection and the future splat renderer |
| libigl Viewer | Existing mesh interaction and overlays | GLFW-based viewer needs context/event adaptation for Qt; Gaussian splats still need custom rendering |
| Qt + libigl algorithms | Reuse weights, skinning and deformation without coupling capture formats to the viewer | Define rest geometry, skeleton and temporal correspondences for captured Gaussians |

The local libigl checkout includes `lbs_matrix`, `dqs`, `bbw` and `deform_skeleton`.
These functions do not require its Viewer. Automatic weights require suitable
geometry and constraints; importing libigl does not automatically rig a 4D capture.
Future Gaussian deformation must also transform orientations/covariances.

This phase draws a single `GL_POINTS` batch without CPU depth sorting or alpha
blending. Drawing and transfer costs scale with active point count; the preview
buffer uses 28 MB per million points. Complete attributes stay in CPU memory.
Degree-3 SH adds up to 180 bytes per record, CPU reconstruction and GPU evaluation.
It is always enabled when coefficients are available. Captures without higher-order
SH naturally retain their base colour; oversized SH buffers fall back to base
colour with a status message if they exceed the GPU's texture-buffer limit.

Switching Qt for the Viewer alone does not remove these costs: both send buffers
to OpenGL. Phase 2 adds projected covariances, sorting and overdraw. Solve skinning
weights outside the render loop and consider GPU deformation for large captures.
The HUD reports CPU decoding and upload time; **it does not measure GPU duration
or guarantee a frame rate**. Use Release builds to assess performance.

MINT holds the entire file in RAM, following the original reader. VGS reads file
ranges and applies a 512 MiB cache policy; an individual chunk may exceed that
budget. A candidate capture may temporarily coexist with the previous capture.

References: [QOpenGLWidget](https://doc.qt.io/qt-6/qopenglwidget.html),
[libigl tutorial and Viewer](https://libigl.github.io/tutorial/).

## Verification

Tests cover persisted recent-file history and dialog locations, project round
trips, relocatable paths, invalid data, transform order
and opaque points of equal size despite differing source opacity and scale.
Transport icons are light grey. Buttons explain their operation and keyboard
shortcut in tooltips; Ctrl+Home/Ctrl+End jump to playback range boundaries,
Alt+Home resets the current target, G toggles the grid and L toggles looping.

Transform and reference-space buttons use the supplied PNGs in `assets/icons`,
embedded as Qt resources. Checked transform modes retain the original white
icons; inactive modes are grey and unavailable controls are darker grey. The
reference-space button swaps between Global and Local icons to show its current
selection. Icon variants are cached at runtime without modifying the source PNGs.

Gizmo tests exercise global/local translation/rotation/scaling, fixed-base scaling, mode toggles, drag
cancellation, view-cube clicks, numpad views, orthographic exit, independent crop
editing and GPU crop visibility. Source records are preserved.
To exercise decoding with a real capture:

```powershell
$env:EDITOR_TEST_CAPTURE = 'D:\path\capture.mint' # or .vgs / .pgs
ctest --test-dir build/cmake-msvc -C Release --output-on-failure
```

`VGSEditor.exe capture.mint --smoke-test build/preview.png` opens a capture, seeks
to a middle frame with SH, transforms it, round-trips a project, saves viewport
and UI images, creates an empty project and reopens the saved project. It exits
with code 0 on success.

Batch export uses the same writer without opening the editor window:

```powershell
VGSEditor.exe capture.vgsproj --export-capture capture-edited.vgs
```

The output extension selects VGS/PGS/MINT; the project supplies crop, transforms,
metadata and processing. Files are verified and atomically saved as in the UI.

Export also supports **MINT format 6** through the native save dialog or a `.mint`
batch-export destination. It rebuilds the raw padded arrays from the same edited
temporal blocks, including crops, colour filters, despill and baked transforms.
Capture metadata, audio, thumbnails and playback hints are omitted. Lower SH
degrees are zero-padded to MINT's fifteen-coefficient layout. MINT is uncompressed;
VGS usually stays smaller. Every frame is decoded again before atomic saving.

In orthographic views the background's coloured axes are hidden. Perspective axes
share the exact Y=0 grid origin. The coincident grey X/Z segments are omitted in
perspective and restored in orthographic views; equal-depth drawing covers grid
crossings without moving X/Z upwards. Modifier data uses texture buffers, avoiding a fixed uniform
array limit. A CPU preview fallback handles hardware texture-buffer limits.
