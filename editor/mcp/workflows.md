# Working with VGS Editor

## Purpose and capture context

VGS Editor is a desktop editor for cleaning and preparing animated volumetric captures
represented as 3D Gaussian splats/points (.vgs, .pgs and .mint). Its primary workflow is
to remove floaters and other reconstruction junk while preserving the captured subject
and its motion, then save a reversible project or export a cleaned capture.

The subject is usually one or more people, but it can also be objects, animals, or people
together with objects. Do not assume a human shape: identify from the images what the
subject is before cleaning, and treat everything that belongs to it as content to keep.

The typical source is a multi-camera capture rig surrounding a central performance area,
with the physical cameras pointing inward at the subject. The intended result is the
whole subject: for people, including clothing, hair, fingers and accessories; for objects,
including thin parts, handles, edges and anything attached to them; and any intentional
props. Typical
cleanup targets include floating or isolated splats, detached noisy clouds, reconstruction
artifacts and unwanted floor, background or rig fragments. Treat this as capture context,
not proof that any particular point is unwanted. Use this subject-focused context to
interpret cleanup requests, while preserving scene content the user wants to keep.

When asked to "clean the noise" or "remove the junk", inspect the actual capture and use
the workflow below. Preserve the full silhouette and motion throughout the requested
range: hands, feet, hair, protruding object parts and props can legitimately be far from
the main mass. Do not simply
keep the largest or most central cluster, or apply a tight crop around a single pose.
Pointcloud mode is often clearer than Gaussian splats for visual checks of isolated
points, floating clouds and spatial artifacts: opaque point centres reveal their separation
from the subject without splat blending. The survey step below gives the views, frames
and image sizes to use. Then validate in Gaussian splats as well, checking final appearance and silhouette;
points alone cannot show the full opacity, size or view-dependent appearance of Gaussians.

World coordinates are in metres and Y is vertical (up). Crop dimensions, transform
positions and camera targets all use these units. The convention is that the subject
stands centred on X=0, Z=0 with its floor contact (feet, or an object's base) on the floor
at Y=0 (the viewport's floor grid).
`bounds` includes any noise, so its minimum Y is not the floor and its extent is only a
rough guide; place and size by eye from images.

The capture rig's physical cameras are distinct from the editor's inspection camera.
Do not infer calibrated camera positions, rig dimensions or subject labels from this
description. These tools expose the reconstructed capture,
not an automatic semantic segmentation or the original camera calibration. Inspect the
loaded data and available project settings before choosing cleanup regions or thresholds.

## Operating the live editor

You operate the user's live editor, not a separate reconstructed scene. Start with
list_editor_sessions. If more than one is open, select the intended session explicitly.
When the user refers to a recent or the last file, read list_recent_files (most recent
first) and open that path.
Read get_editor_state before changing anything. Pause playback for inspection and edits.
Tool results report actual execution; never claim an edit or visual check without doing it.
Use tool discovery for each tool's purpose, schema, defaults and effects. Read
`vgs-editor://tools` for modifier meanings and cleanup examples. In
particular, brush/lasso select projected point centres through depth, so inspect from
another view before selecting a region overlapping the subject.

An imported MINT or VGS/PGS with embedded audio already has a Capture audio layer.
Preserve it during cleanup; inspect `get_project` before adding another Audio layer.
`vgs-editor://tools` documents the capture/external source selector, audio patches,
muting, and how `save_project` preserves the settings and file references in `.vgsproj`.

## Inspecting and removing noise

Work from global to local: crop and filters act on the whole timeline at once, while
Erase picks must be repeated chunk by chunk. Use Erase only for what the global tools miss.

### Fast path (saves calls, images and tokens)

- One measurement first: after pause, call measure_timeline. Its `extent` gives the
  visible subject's box over the whole take, the X/Z centre of the whole group and its
  horizontal radius; `removed_percent` gives the baseline. Use that centre for X/Z
  placement and that radius (plus 0.1-0.2 m) for the keep cylinder, then confirm with a
  couple of top-view images, instead of measuring extents image by image over frames.
- Frame views yourself: set_view(preset, focus=false) then set_camera({"patch":
  {"target": [x, y, z], "distance": d}}). focus=true frames everything visible,
  floaters included, so on a raw capture the subject comes out tiny. Distance about
  3-4 frames a person or group, 0.8-0.9 the feet, 0.5 a hand-sized region.
- Look with inspect_capture. One call returns the view as shown, a 3x3 survey of the
  frame (the five sides plus high and low diagonals, auto-framed on the subject, the
  user's camera untouched) and, with include_motion, top and front views across the take
  with the crop's wire. It replaces a dozen separate images. Use include_motion for the
  first survey and the final check (it decodes frames, so it takes seconds), and
  show_removed=true to validate: what the modifiers remove is drawn in red from every
  side, so a cut limb or prop shows at once. Use get_viewport_image only for close-ups,
  measurements in one view and strokes.
- Image sizes: 768 for single overview images and 1024 (the default) for close-ups. Prefer
  moving the camera closer over asking for larger images.
- Orthographic images carry `scale`: convert pixel offsets with pixels_per_metre
  around image_centre_world. In the top view image up is -Z.
- Edits return a compact state; read only the fields you need. Call get_editor_state or
  get_project only when you need chunk boundaries or modifier settings.
- A newly imported capture already has a Crop fitted to everything: edit that one.
- If the crop and Purge Isolated remove almost nothing (removed_percent near 0), the
  capture is already clean: inspect for local residues and stop, rather than iterating.
- Purge Isolated does not remove dense sprays attached to the subject (sweat, motion
  trails off a glove or hand). Go straight to Erase for those, from a view where nothing
  of the subject lies behind the region in depth (often the top view between subjects).
- For Erase, zoom in until individual points are visible (distance about 0.5) before
  drawing; from far away sparse residue looks denser than it is and strokes miss it.
- Group captures (several people or objects): centre and crop the whole group over
  the full motion, never one member or one frame.

1. Read the capture range, modifier stack, frame count and chunk boundaries. Preserve
   the source. Use reversible modifiers and individual undo steps.
2. Survey first. Switch to points with point_size 2 and focus the capture. Pointcloud
   centres show separation from the subject more clearly than blended splats.
   A sufficient baseline is front, one side, top and perspective views at the first,
   middle and last frame of the range: take it with inspect_capture(include_motion=true)
   rather than single images, and seek to both sides of chunk boundaries where needed.
   Use max_width 768 for survey images and the 1024 default for close-ups and painting;
   images are costly. sample_points provides supplementary world
   coordinates and counts, not a semantic classification of noise.
3. Distinguish floaters and junk from legitimate thin or detached-looking parts of the
   subject: hair, fingers, accessories, cables, handles, thin object edges.
   If the intent is ambiguous, ask about that region rather than deleting it broadly.
4. Place the subject before cropping, with set_capture_transform (crops do not follow
   the transform, so place first). Measure in orthographic named views: their images
   return `scale` (pixels_per_metre, the world point at the image centre and the world
   directions of image right/up), so an offset in pixels converts directly to metres.
   - X/Z: in the top view, framed on the subject (set_view with focus=false, then
     set_camera target near the subject and distance about 3), measure the subject's
     centre against the image centre. In the top view the front marker is at the bottom
     of the image, i.e. image down is +Z; use image_right/up_world rather than guessing.
   - Y: go straight to a close-up of the floor contact: front view, set_camera target
     [x, 0.1, z] at the feet or base and distance about 0.8 (several hundred px/m), plus
     a side view. A full-body view is too coarse to judge centimetres. Move the subject so
     its feet or base rest on the floor grid line. Judge by eye: do NOT use the lowest
     point, `bounds` minimum or sample_points, because stray noise below the subject would
     set the wrong height. A thin layer of leftover floor splats right under the soles
     shows where the real floor was: put that level at Y=0. The crop removes everything
     below; residue lying exactly at floor level, touching the soles, may remain.
   - Check a few frames; for people, feet lift during motion, so align to frames where
     the feet are planted. Re-take the images after moving to confirm.
5. Remove the bulk with crops, the most effective tool for large unwanted regions. A newly
   imported capture usually already has a Crop fitted to everything, noise included: edit
   that one (get_editor_state lists its id) rather than adding another. Crops
   act on every frame and chunk at once. Typically a `keep` cylinder with its base at
   Y=0, centred on X=0, Z=0, around the full motion of the subject. Because the crop's
   base is at the floor, it also removes everything below Y=0, which is noise once the
   subject is placed. A box can fit an object or a group better than a cylinder. Keep crops add together, remove crops subtract and win overlaps;
   stack order between crops does not matter. Size it by eye from images and check the
   full motion, not a single pose, so limbs or object parts are never cut. Details and an example
   patch: "Crop volumes" in `vgs-editor://tools`. To check the fit, select the Crop and
   set_crop_editing(true): images then show its wire volume and mark what it removes in
   red, so a cut limb or prop is visible directly. Any edit turns it off; turn it off
   yourself before measuring or validating.
6. For sparse detached noise, add Purge Isolated. Its default is neighbour=4,
   medianPercent=700; set medianPercent=900 for a conservative first comparison.
   HIGHER medianPercent removes LESS. Compare enabled/disabled snapshots at the same
   frame and camera, and adjust only after inspecting the result.
   Measure every global step with sample_points(limit=1): its visible_points before and
   after shows how much each crop or filter removed in this frame, cheaply. After the
   global steps, measure_timeline gives the same counts at frames spread over the whole
   range, for a total that one frame cannot show.
7. For local residual noise, add/select an enabled Erase modifier and enable Erase editing
   BEFORE taking a snapshot. erase_brush/erase_lasso use normalised image coordinates
   with the top-left of the full viewport image as origin. Brush radius is a fraction of
   the shorter image side. Use the image's view_id; take a new image after each change.
   Image coordinates you estimate are approximate: keep a clear margin from the subject,
   prefer a few generous strokes on clearly detached noise over precise strokes near it,
   and check picked_records and the pink preview after each stroke. If picked_records is
   unexpectedly large or pink appears on the subject, undo immediately.
   Pink points are picks, not the final removed result. Turn Erase editing off to validate.
8. Eraser identity is (source chunk, source record). Picks persist across frames within
   that chunk and do NOT transfer to another chunk. Never reuse a stroke's view_id after
   a seek, camera move, resize or modifier change. Reinspect and select each relevant chunk.
   With many chunks, clean the ones with visible residual noise first and tell the user
   which chunks you cleaned and which you did not inspect.
9. Validate in splats as well as points, from multiple views and frames. Check the subject
   silhouette and fine detail. Undo or soften the last edit if legitimate geometry was lost.
   Do not promise a visually lossless cleanup based on point counts or one image.
   Stop when the survey views show no remaining detached noise, or earlier if further
   edits would risk the subject; report what remains rather than over-cleaning.
10. Restore a useful view and describe the changes and frames/views actually checked.
   Save/export only to the requested destination; overwriting another file is explicit.

## Colour and appearance checks

Only after the geometry is clean, and only when the user asks or a problem is visible:

- Green spill (green cast from the screen on light clothes, skin, hair and edges, often
  strongest low down near the floor): measure it with sample_points(limit=2000) colours,
  e.g. the share of light points whose green exceeds the mean of red and blue, and
  where they are; look at close-ups in splats with set_display_options(background=
  "light"). If it is visible or the share of tinted light points is more than a few
  percent, compare a `colour` modifier with despillMode "always" from the same camera,
  checking skin keeps its tone and intentional green items survive.
- A patch that looks see-through or tinted only from some angles is usually
  view-dependent colour (spherical harmonics), not opacity: compare
  set_display_options(sh_degree=0) with "all" from that camera. If it disappears at 0,
  raising opacity will not fix it; report it. Opacity above 1 (`colour.opacity`) only
  helps faint, hazy areas that look thin from every angle, and thickens edges and haze;
  measure its effect before keeping it.

## Loop morph: closing a loop without a jump

When asked for a Loop morph (a seamless loop, closing the loop, joining the end to the
start), the result depends on the markers: numbered pairs that say which point of the
subject on the End frame is which point on the Start frame. Place them with care:

1. add_modifier type "loop-morph" (or select the existing one) and set `morph.seconds` with
   edit_modifier if asked; get_loop_morph reads its state at any time. For a cycle (a walk,
   a run) the capture should keep moving through the seam: a negative `morph.seconds` makes
   the morph overlap the frames before End, with End on the pose that matches Start (one
   cycle later), and `morph.leadIn` true also blends into the frames before Start.
2. Look before placing: seek the Start frame and the End frame and take images of each from
   several angles (front, both sides, back, and closer views of the face and hands), so the
   same features are found on both. Compare the two poses; note what moved and what changed.
3. Place markers above all on the key places, each on the same feature in both frames:
   - eyes (inner and outer corners), mouth corners, nose tip, chin, ears;
   - hands (wrist, knuckles, fingertips) and feet (heel, toe);
   - joints that bend between the frames (elbows, knees, shoulders);
   - sharp changes of colour or texture: clothing edges, collars, cuffs, belts, seams,
     pattern corners, logos, where skin meets cloth.
   Spread them over the whole subject and every side of it, more where it moves or changes.
   A point on a plain, even area cannot be matched exactly: prefer corners and contrasts.
4. Per marker: seek one frame (Start or End), take an image, place_morph_marker on the
   feature; seek the other frame, take an image from a comparable view, find the same
   feature and place the marker with the same number (place_morph_marker's `number`).
   With three pairs or more, get_loop_morph's `suggestion` predicts where the pair goes on
   the frame shown: check it in the image and correct it rather than trusting it.
5. Check the result: get_loop_morph flags pairs far from where the others put them
   (`flagged`, `off_cm`) - usually two different points; look again and fix or remove them
   (remove_morph_markers). Then seek the morph's frames (`morph_frames`: after End, or
   before it when overlapping) and look at them from several angles, the face closely: limbs should
   swing, not cut through, and features should not double or smear. Add markers where they do.

## Other operations

- get_project returns the native JSON schema. Modifier patches merge into existing fields;
  use the spellings returned by get_editor_state (colour, isolation, prune, green, etc.).
- Seeking is constrained to the current Start/End range, plus an active Loop morph's frames after End (overlapping, they are inside it). Expand it with set_frame_range
  if inspection outside it is required, then restore the user's range afterwards.
- Export is asynchronous: start_export returns a job_id. Poll get_export_status until
  done/failed/cancelled; cancellation is cooperative and can leave a partial output.
- A saved project preserves reversible edits. Export bakes enabled modifiers into a
  new capture. Export never makes the current project saved.
- A busy editor must finish decoding before images or brush selections are trusted.
  The adapter waits for processing; a timeout is not successful completion.
- Only one export runs at a time in the editor, including exports the user starts from
  its menus. If start_export reports one is running, wait and retry; do not cancel it.
