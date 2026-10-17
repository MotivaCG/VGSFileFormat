# Playback measurements, 2026-10-17

Measurements use Release/MSVC, Qt 6.8.0, RTX 4070, full source SH and the local `morph.vgsproj` referencing `arabs.mint`. The original project and capture are unchanged.

`playback_bench` processes every requested frame and forces a framebuffer readback. Its timings include GPU completion and image transfer to the CPU. They are conservative and differ from normal playback, which overlaps worker processing with rendering. `mainwindow_tests::playbackTimingOnARealCapture` measures distinct presented frames in the actual editor after five seconds of warm-up.

The baseline is commit `f1375b5`, built separately under `editor/build/playback-baseline-build`.

## Actual editor playback

Distinct frames presented after five seconds of warm-up, measured for six seconds, with full source SH and no builds or other test programs running:

| Case | Baseline FPS | Updated FPS | Baseline worker | Updated worker |
| --- | ---: | ---: | ---: | ---: |
| `arabs.mint`, no modifiers | 13.99 | 29.60 | 64.41 ms | 11.27 ms |
| `arabs.vgs`, no modifiers | 27.25 | 29.99 | 14.58 ms | 9.37 ms |
| `morph.vgsproj`, morph-only playback, modifiers enabled, warm cache | 15.50 | 29.95 | 52.89 ms | 6.85 ms |

These captures target 30 FPS. MINT contains degree-3 SH and the VGS degree-2 SH; each before/after comparison uses exactly the same input. The real editor viewport was 944 x 552 logical pixels without modifiers and 944 x 522 with the morph tools shown, on a scaled display.

The actual qmake Release executable was also measured with the editor theme, the real project and a 1416 x 783 physical-pixel viewport on the same RTX 4070. Normal timeline playback in a cold segment presented 10.00 FPS with 83.71 ms mean worker time and zero mask hits. After the menu's complete 929-frame precalculation, it presented 30.08 FPS with 10.98 ms worker time and 181 mask hits for 181 presented frames. It retained 22.49 MiB of masks and 433.46 MiB of morph shapes/shared SH. The project's Color is export-only, so there is no RGB cache to fill in this case.

The actual qmake morph loop measured 26.83 FPS despite 6.07 ms worker time and all shapes hitting. The playback clock and repeated assignments of an unchanged transform were also repainting the old Gaussian frame before its replacement arrived. Removing those redundant repaint requests raised that run to 29.25 FPS, with 176 shape hits for 176 presented frames and 6.19 ms worker time. No SH degree, resolution, sorting or Gaussian shading was reduced. The remaining variation depends on drawing and event scheduling; cached shapes do not remove the cost of drawing them.

## Cache checks

The Cache menu follows Edit. Ctrl+B bakes the cache, Ctrl+Alt+B toggles cache retention, and Ctrl+Shift+B opens Info, whose menu action has a monochrome circled-information icon and follows Bake cache's enabled state. Bake cache is also available from the main timeline transport, after Skip to End with a 20 px gap, using a monochrome cache-cylinder and lightning icon. Both entry points share one action and its enabled state, including cache-off, capture loading and a bake in progress. Its first entry, Enable cache, is checked by default and remembered in editor preferences. Turning it off releases optional visibility, RGB, morph-shape/SH dictionary, frame/scratch and prune-score retention, and stops decoder look-ahead. The active frame, current decoder chunk and fitted morph model remain necessary working data. Bake cache is disabled while caching is off.

Bake cache processes every frame from Start through End and the morph tail on the worker thread, with full SH. It emits progress rather than decoded frames, leaves the playhead and project unchanged, dims the view and suspends viewport repainting while a cancellable progress dialog is open. The normal preview is refreshed once at completion. Memory limits and system-pressure eviction remain active; completion reports when entries could not all be retained.

Normal Play also retains evaluated frames. Because playback can skip source frames when processing is slower than real time, one cold pass need not cover every timeline frame; precalculation deliberately visits all of them.

Without using Bake cache, the qmake Release editor was left playing the real project for 95 seconds, then measured for six seconds. It presented 180 frames at 29.99 FPS, with 180 visibility hits, 11.61 ms worker time, 22.44 MiB of masks and 433.46 MiB of morph data. This reproduces the gradual warm-up across loops rather than a cache that is cleared every frame. It does not make the first cold pass real time.

Cache > Info lists each layer, cached frames/entries, MiB and its processing policy. Visibility memory is attributed once per processing stage; earlier layers point to the layer sharing their result. Color explicitly identifies export-only despill, GPU adjustments and superseded despill settings. It also shows the separate preview and RGB budgets and system memory reserve, with worker-thread snapshots even while paused. Invalidated morph shapes are released immediately, without waiting for playback to reach the morph again. The dialog is nonmodal and refreshes twice a second.

Amber calculation markers remain visible across short gaps (650 ms after the last miss) with a gentle pulse instead of alternating with the retained state every frame. Complete caches use the editor's green interaction color (#41b018); any partially cached layer uses a half-filled green circle, including visibility and despill RGB. Coverage describes the timeline rather than just the displayed frame. Layers without their own bake, including direct GPU Color and results shared with a later layer, use #0e0f10: darker than the row background, without pure black. Info uses the same coverage and ownership data as these markers. Regression coverage now requires ordinary Play to fill and reuse Purge, RGB and morph results over multiple loops, without the precalculation menu.

A fresh 100-second normal-Play run with the Info build (no precalculation) retained 898/899 ordinary timeline masks and all 30/30 morph frames, with zero cache reductions and a constant dependency revision. Ten-second snapshots showed mask coverage increasing 91, 198, 310, 400, 502, 609, 698, 790, 869, 898; morph coverage increased from 28 to 30. Overall presentation averaged 15.69 FPS including cold calculation, and the final interval presented 255 frames, with 197 mask hits. At that point the dialog reported 22.47 MiB of masks, 433.46 MiB of morph data and 242.5 MiB of reusable buffers (698.4 MiB combined). This specifically confirms gradual retention during Play, while the remaining skipped frame prevents claiming full timeline coverage. The first pass with Purge remains CPU-bound.

Modifier rows show dark gray filled dots for direct GPU work and results shared with a later layer, hollow dots for results not retained, amber pulses for actual cache misses being computed, green for complete caches and half-filled green for partial coverage. Coverage is shown even outside a modifier's frame range. Crop, Remove green and ordinary Color run in shaders; Purge/Prune/Erase share the worker's visibility masks. Color's RGB cache applies to Despill Always, including skin recovery.

Invalidation now follows processing dependencies rather than one project revision. Ordinary timeline masks exclude Morph settings. Source masks depend on cleaning above the morph and only on colour processing when a Remove green consumes it. Final morph masks depend on its geometry and filters below. Morph fitting/shapes depend on its source masks, despill/skin, transforms, range and own parameters, so a downstream Purge does not discard source masks or refit the graph. Modifier names, selection and unrelated export settings do not change these identities. Keys are prepared once per edit and copied by reference into frame requests.

The project's normal timeline has Crop, Remove green and Purge Isolated before Color and Loop morph. A repeated 60-frame MINT segment produced 60 visibility hits and zero additional isolation evaluations. The first pass needed 60 evaluations. Visibility retention for those frames was about 1.3 MiB.

Across repeated 30-frame morph passes, all 30 shapes hit the cache and there were zero additional isolation evaluations. Retention stabilized at 433.5 MiB for shapes plus shared endpoint SH dictionaries, and 129.2 MiB for reusable buffers. For a normal morph during playback, endpoint SH is uploaded once and mixed on the GPU using each cached shape's source/partner indices and the current fraction. Color is mixed each frame; complete rendered frames and full per-frame SH arrays are not cached across the timeline. Paused frames and exports materialize their SH normally. Overlapping morphs retain the CPU SH path.

The worker previously solved the graph separately for every marker, even when the frame shape came from cache. Marker placement now shares one solution, and hidden playback markers are refreshed when pausing. Reusable morph output avoids repeated large allocations and an extra SH copy. The GPU SH path removes the largest remaining per-frame CPU mix and transfer without reducing the SH degree.

Purge Isolated masks now also cover source frames above an overlapping morph and final frames with Purge below the morph. Separate cache contexts prevent a source mask from being reused for a generated frame at the same time.

## Memory limits

Preview cache retention is capped at 4 GiB in total: 3.5 GiB of morph shapes and shared SH dictionaries, 256 MiB of visibility masks and 256 MiB of reusable frame/scratch buffers. Shapes larger than their budget are evaluated without being retained. Retained buffers are reused only when no queued signal, viewport, sorter or external caller owns them.

Despill/skin has a separate 6 GiB limit. It retains the final float RGB (12 bytes per source splat and frame, plus small entry metadata), shared by both effects, with least-recently-used eviction. It does not retain corrected SH per frame. Cache hits still transform SH in place on the CPU, but skip DC processing, skin classification and skin recovery. Keys distinguish exact source time, chunk, source SH degree and all despill/skin parameters; camera and ordinary exposure changes preserve the entries. Only playback fills this cache; paused frames may reuse it.

Both cache groups yield to the same system-memory guard. On Windows it checks `GlobalMemoryStatusEx` and the low-memory resource notification before retention, at decode boundaries and every 500 ms while idle. Physical headroom is the larger of 2 GiB and 10% of installed RAM; commit headroom is the larger of 1 GiB and 5% of the commit limit. Falling below either reserve stops retention and evicts old colours, morph shapes/dictionaries, masks and reusable buffers as needed; prune scores and decoder chunks can also be released. Retention resumes only after recovering an extra 512 MiB above both reserves. Query failure disables growth. This is a cache admission/eviction policy, not an absolute guarantee against another application's sudden allocation or the memory required to decode and render the active frame. The 4 + 6 GiB limits are ceilings, not preallocations.

The Windows signals are documented in [MEMORYSTATUSEX](https://learn.microsoft.com/windows/win32/api/sysinfoapi/ns-sysinfoapi-memorystatusex) and [CreateMemoryResourceNotification](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-creatememoryresourcenotification). The chosen reserves and hysteresis are application policy, not fixed Windows thresholds.

With despill Always and skin recovery enabled in memory for the real project, six timeline frames (220,079 records each, full degree-3 SH) retained 15.11 MiB of corrected RGB. The second pass had six colour hits, six visibility hits and no additional despill/skin or isolation evaluations. Mean worker time fell from 381.41 ms cold to 20.71 ms warm. Decoded records/SH hashes were identical between passes. Peak resident memory was 444.17 MiB in that short run. This measures a six-frame sample, not the complete timeline or actual editor presentation FPS.

The MINT preview now maps the source read-only, so unused chunks do not require a full private RAM copy. Export's writable reader keeps its existing behavior. File mappings, active decoded data, GPU/driver allocations and inspection-owned snapshots are outside the preview retention budget.

For the project's 872 MiB MINT, four morph passes stabilized around 1.11 GiB resident RAM, with a measured peak of about 1.14 GiB and about 1.71 GiB of private committed memory. Normal cached timeline segments stayed around 0.5 GiB resident. These are measurements for this capture, not limits on total process memory.

## Reproducing

From the repository root, with the Qt runtime on PATH:

```powershell
cmake --build editor/build/cmake-msvc --config Release --target playback_bench
$project = 'D:/Trabajos/ScanMeNow/SMNWebviewer/Playcanvas/content/data/morph.vgsproj'
& editor/build/cmake-msvc/Release/playback_bench.exe $project 30 morph 4
& editor/build/cmake-msvc/Release/playback_bench.exe $project 60 0 2

$env:EDITOR_PLAYBACK_CAPTURE = $project
$env:EDITOR_PLAYBACK_KEEP_MODIFIERS = '1'
& editor/build/cmake-msvc/Release/mainwindow_tests.exe playbackTimingOnARealCapture -o editor/build/morph-gui.txt,txt
```

The benchmark prints mean, median, p95, maximum, cache hits, isolation evaluation counts, retained bytes and Windows process memory. `PLAYBACK_GPU_TIMING=1` also measures GPU preparation and drawing; the queries synchronize the GPU and should be used only for diagnosis. `PLAYBACK_VERIFY=1` prints hashes of the decoded records/SH and framebuffer. `PLAYBACK_IMAGE` optionally saves the last image. `PLAYBACK_CPU_SH=1` selects paused CPU evaluation for a reference image.

`PLAYBACK_DESPILL_ALWAYS=1` enables preview despill and skin recovery for this benchmark process only, leaving the original project unchanged. `PLAYBACK_COLOUR_CACHE_BYTES=0` disables RGB retention for a comparison. The benchmark reports colour hits, despill evaluations and RGB cache bytes separately.

The editor itself accepts `--playback-report output.json capture-or-project` to measure presentation in the exact executable being tested, with isolated preferences. `VGS_PLAYBACK_PRECALCULATE=1` runs the same menu action before playback; `VGS_PLAYBACK_MODE=morph` measures the morph-only loop instead of the normal timeline. The report includes executable path, Release/Debug, GPU, viewport pixels, worker time and cache hits. `VGS_PLAYBACK_IMAGE` saves the window for a visual check. This mode does not save or modify the original project.

## Other fixes and validation

The in-source qmake Release later crashed on startup with heap corruption (0xc0000374), while the isolated Release ran correctly. Its generated Makefile did not yet list modifierpanel.h as an inspection.cpp dependency. inspection.obj therefore retained the previous CacheLayerInfo layout; the link map identified that object's QArrayDataPointer<CacheLayerInfo> destructor in the failing startup stack. Regenerating qmake's dependency graph and rebuilding inspection.cpp fixed the affected executable directly. VGSEditor.pro now explicitly includes the source directory in DEPENDPATH, and the regenerated inspection target includes modifierpanel.h. Validation used three launches with the saved settings, each closing normally with exit 0, followed by an actual morph.vgsproj playback-report run exiting 0. The broad --smoke-test also opens and draws successfully but still exits 2 later in its Crop-edit checks; it is not a passing validation of that separate UI sequence.

The Release build passes all five suites: editor (37), viewport (27), export (60), automation (22) and mainwindow (10): 156 passed, zero failed, 13 optional integration checks skipped. Coverage includes full-precision RGB/SH equivalence with and without skin recovery, colour eviction by use, dependency invalidation across morph boundaries, precalculation without publishing frames, normal Play filling the cache, cache disabling, per-layer markers, cache shrinkage during idle memory pressure, commit pressure and hysteresis. Pressure is simulated through an injected memory probe, without exhausting the machine. The standalone viewer also builds and passes its authoring-key exclusion check. The real-capture playback measurement above runs its optional integration check separately.

The ghost test counted the origin marker's 0.14 grey (about 36/255) as ghost ink. It now disables that independent decoration while retaining the original threshold. The undo test now commits numeric typing with Enter, matching disabled keyboard tracking; focus is cleared before MainWindow members are destroyed.

Current-frame PLY export now evaluates normal and overlapping morphs. Regression checks compare it byte-for-byte with the corresponding PLY sequence frame, including export-only despill. Cache eviction, immutable published snapshots, read-only MINT decoding, reused morph output and GPU SH index/mix equivalence for degrees 1/2/3 have regression coverage.

Gaussian projection and SH evaluation run once per splat using OpenGL 3.3 transform feedback, then the four quad vertices expand the prepared result. Full SH is preserved. Comparison against the baseline on twelve real MINT frames produced identical decoded record/SH hashes; the saved final RGB image differed by at most one level in a channel. A separate comparison of the real project morph at frame 12/30 produced identical RGB images for CPU and GPU SH mixing.

The visibility mask and despill/skin RGB caches are separate; corrected SH is still transformed per frame. Timeline cache coverage indicators and the original overlapping-morph extremity jitter investigation remain separate work.
