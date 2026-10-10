# VGS Viewer: plan

Not started. A separate program that only opens and plays `.vgs` and `.pgs` captures,
looking and behaving like the editor's viewport and timeline, with nothing that edits,
saves or re-encodes. This note records what was decided and the tasks, in order.

## Decisions

- **Its own executable, `VGSViewer.exe`, not a mode of the editor.** The editor links the
  encoder and the signing code (`vgsencode`, `vgssign`), which carry the private key. The
  viewer links the decoder only, which carries the public key, so the key protection
  stays as it is. A `keyleak_viewer` test checks every build.
- **Shared `.cpp` files, not a library.** Both projects compile the same sources, listed
  once in `editor/vgsui.pri` (qmake) and in a CMake variable (`VGS_UI_SOURCES`). Same
  look by construction: `EditorTheme::install()`, the same widgets, the same icons.
- **Licence.** The viewer can go out under the VGS Decoder licence (free,
  redistributable), unlike the editor (internal use).
- **Errors.** A capture whose signature or digests fail - not authentic, or damaged -
  shows only **"This file cannot be opened."**, the same text for every case, so it gives
  no hint of which check failed. A chunk that fails its digest during playback stops
  playback with the same message rather than showing broken frames.

## What the viewer has

- **Open:** `.vgs` and `.pgs` only - dialog, drag and drop, recent files, double-click
  (file association from its installer).
- **Playback:** the editor's transport bar and timeline - play/pause, previous/next frame,
  first/last, loop, speed, scrub - without the modifier tracks and without the Start/End
  fields (trimming is editing; the range is the whole file).
- **View:** the same navigation (orbit, pan, zoom, double-click focus), the ViewCube with
  the dark/light **background** button, and Grid, Axes and Origin and front marker in the
  View menu.
- **Always** Gaussian rendering and the file's full SH degree (falling back on its own
  only when the GPU cannot hold all coefficients, as the editor does).
- **From the file:** its audio, in sync (`startTick`); its suggested initial camera
  (provenance `view`); its play mode (once, loop, ping-pong); a read-only information
  panel with the metadata (title, author, project, take, studio, copyright, duration,
  frames, size).

## What it does not have

Projects, modifiers, transform or crop editing, presets, export of any kind (VGS, PGS,
MINT, PLY, PNG), tasks, autosave, undo, MINT sources, ffmpeg. In the viewport: no
statistics text (points, decode time…), no Gaussian/points switch, no SH selector, no
ghost.

## Tasks

1. **Shared sources** - no behaviour change in the editor.
   - Create `editor/vgsui.pri` and `VGS_UI_SOURCES` with what both use: `editortheme`,
     `displayscaling`, `viewport`, `viewcube`, `rangeslider`, `audiopreview`, and the
     capture decoding. `VGSEditor.pro` and the editor's CMake target include them.
   - Split `CaptureWorker`: a decode-only path for `.vgs`/`.pgs` (shared), and the
     modifier, prune, erase and despill processing (editor only). Today it includes
     `exportcapture.h` for `despillFrame`, which pulls in the encoder: that dependency must
     move to the editor-only side.
   - Check nothing from `exportcapture`, `nativeexport`, `vgsencode`, `vgssign`,
     `mintfile` or `pruning` is in the shared list.
   - Editor tests (editor, export, main window) still pass.
2. **`TransportBar`** - extract from `MainWindow`.
   - Move the transport row (first/previous/play/next/last, Start/Time/End, Loop, Speed)
     with the `RangeSlider` and the playback clock (loop, speed, frame stepping, Space)
     into a widget of its own that emits `timeChanged`, `playingChanged`…
   - An option hides/locks Start and End for the viewer.
   - The editor uses it with everything on; its modifier tracks stay below it, still in
     step with the slider's zoom and pan. Main window tests still pass.
3. **`Viewport` options** for the viewer: hide the statistics text; force Gaussian and
   full SH; no ghost. Defaults keep the editor exactly as it is.
4. **The viewer** - `editor/viewer/` with `VGSViewer.pro`, a CMake target, `main.cpp`
   and a small main window: viewport, ViewCube with the background button, transport
   bar, View menu, Open/recent files, drag and drop, the error dialog.
5. **What the file carries:** audio through `AudioPreview`, the initial camera, the play
   mode, the information panel.
6. **Safety:** `keyleak_viewer` in `tests/CMakeLists.txt` (`vgs_keyleak --absent` on the
   viewer's executable), as `keyleak_bridge` and `keyleak_houdini` do.
7. **Distribution:** a deploy script (windeployqt, MSVC runtime, licence; no ffmpeg, no
   presets) and its own Inno Setup installer associating `.vgs` and `.pgs` (asked, as the
   editor's installer does), with its own AppId and the version from its own header.
