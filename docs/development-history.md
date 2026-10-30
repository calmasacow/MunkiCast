<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Development history through 2026-10-24

Historical observations and measurements, retained as recorded. Statements such as
"still open", "next phase", and test counts describe their dated entry, not the current
build. See [current status](current_goal.md) and [remaining work](todo.md).

## Still-image compositing overhead, 2026-10-22

The operator's MaraTalk scene reproduced 16.503 ms/frame and 374 missed budgets
across 840 rendered frames after warmup (30.8%). Hiding only the camera-overlay PNG
in a temporary collection copy reduced this to 12.465 ms and 87 missed (9.4%).
The saved operator collection was not changed by these measurements.

Decoded stills previously used a one-entry cache returning full pixel copies, and
GPU uploads repeated every composite. Stills now share immutable decoded pixels,
with an LRU bounded to 16 entries / 128 MiB. Metadata changes replace the identity;
existing plans retain their old pixels. GPU textures follow that identity, retain
only images present in the current plan, and are bounded to 16 / 128 MiB within
the existing compositor allocation budget. CPU and GPU still use the same plan.

With the PNG visible, a 1,800-frame measured run after warmup averaged **12.369 ms**
with **72 missed budgets (3.85%)**. A final 840-frame run matching baseline length
measured **12.848 ms**, **112 missed budgets (11.76%)**. Other media was still
preloading through 13.5 seconds in that run. Results remain variable; this is a
short-run improvement, not a claim that live drops or latency are solved. The main-thread CPU profile now points at
memory copying inside GPU drawing; capture conversion also consumes substantial
CPU on separate threads. An experiment caching changing camera uploads allocated
GPU images repeatedly and regressed timing; it was reverted. No camera lifetime
or capture settings were changed. Remaining tail latency needs further work.

A real CPU/GPU check covers multiple still identities, repeated rendering, file
replacement and continued rendering of a retained old plan.
Final build is warning-free under `-Werror`; all 30/30 checks pass with GPU and
monitoring enabled (179 seconds).

## Native countdown dock and file-backed stingers, 2026-10-22

**Docks → Countdown Timers** is now functional. Create timer text or bind an
existing text source; Start/Pause/Reset, remaining/elapsed/target modes, displayed
time units, leading zeros, `%time%` formatting, end message and optional end scene
are available. State/configuration lives on the text source; workers paint changing
text without document writes per tick. Timers remain shared across placements.
Import OBS timers selects a companion config explicitly and matches unique text
source names. The operator's two bindings live in
`~/.config/obs-studio/plugin_config/obs-plugin-countdown/config.json`.
Color rules, hotkeys, stream triggers and smoothing are reported as unsupported;
raw imported timer configuration is retained. This is not full plugin equivalence.

**Controls → Add stinger…** creates/selects a file-backed transition for that deck.
Duration and cut point are editable; Edit stinger media opens normal source
properties. The renderer preloads media, overlays alpha through the shared CPU/GPU
scene plan, switches underlying scenes at cut point and routes transition audio.
Repeated transitions rewind existing decoders. Interrupted transitions start from
the currently visible underlying scene. Initial project load does not play one.
Missing/unready stingers fall back to Cut with a diagnostic. Track mattes and audio
crossfade are not supported; underlying audio switches at cut point.

OBS import/export preserves supported stingers. Actual collection reimport into a
temporary directory recovered Glitchy (400 ms cut / 1080 ms clip) and MarathonGlitch
(1500 ms cut / 3504 ms clip), selecting MarathonGlitch. Existing saved collection
is not rewritten by that check. See ADR-0068.

Full-collection GPU checks, Main 2560x1440, saved operator collection copied to a
temporary directory, measured **22.96 ms to first frame/ack for FS.W.CAM → START**
and **34.18 ms for FS.W.CAM → START.CD**, after all media had preloaded. One expected
partial-picture gap remains for the audio-only media placement. Starting every Qt
player together initially took 11-12 seconds. Workers now probe independently, then
limit inactive QMediaPlayer starts to two at once, prioritizing keep-running media;
foreground activation bypasses that queue. Silent alpha never waits for a Qt slot.
A fresh full-collection run measured Starting.MS ready at 346 ms, the background
at 4578 ms, and the cut at six seconds delivered its first frame/ack in 47.82 ms
with only the expected audio-only picture gap. A second run measured background
readiness at 2956 ms and FS.W.CAM → START.CD at 25.39 ms after six seconds.
Initial warmup remains; these assets
resolve through GVFS to an SMB share. Restricting Qt hardware selection to CUDA did
not improve startup; automatic selection remains. CPU perf sampling did not isolate
the elapsed startup wait. Worker shutdown requests all stops before joining,
preventing serial teardown delays across retained media.

Final build is warning-free under `-Werror`; all **30/30** checks pass with real
GPU and monitoring enabled (179 seconds). Checks include real countdown UI/actions,
worker text changes, stinger alpha/cut/audio, import mappings and foreground media
activation while inactive startup slots are occupied.

## Media activation and fast scene cuts, 2026-10-22

Unseen media workers were destroyed after every scene change. Opening the actual
background MP4 through a new Qt worker measured 2.04 seconds, including teardown.
OBS retains/preloads media unless close_when_inactive is enabled; the imported
background explicitly disables closing and restarting, but both flags were lost.
ADR-0067 separates activation from worker lifetime, preloads retained media, pauses
restarting clips after their first frame, and gates hidden-source audio/refresh.
Existing imported settings recover from retained OBS metadata. Explicit close-on-hide
remains available and incurs startup latency. Sources are released on deletion/edit.

Real background cuts after preload measured 10.82, 4.48 and 5.69 ms through the
renderer (small preview, actual decoded asset); first collection warmup remains.
Warning-free build, all 30/30 checks pass, including actual worker activation and
first-cut availability. This trades retained decoder memory for fast switching.

## Alpha media performance and recent render stats, 2026-10-22

Render-time stats now average the previous **500 ms** and expire when idle;
rendered/missed totals remain cumulative since reset. Media Source and playlists
expose **Use hardware decoding when available**, enabled by default and translated
from/to OBS `hw_decode`. Qt selects a usable decoder at runtime. Transparent
VP8/VP9 WebM remains on libvpx software decoding to preserve alpha, matching OBS;
GPU compositing/scaling is separate from hardware decoding. See ADR-0066.

Silent alpha no longer constructs a second Qt playback pipeline. Audio-bearing
alpha interpolates coarse player positions. Workers send immutable native-size
frames rather than CPU-upscaled copies; sealed shared-memory leases retain frame
ownership. Receiver mappings populate on the worker thread. CPU and GPU execute
the same authored-size scene plan. GPU caches resized immutable frames and reuses
bounded images/staging buffers; fully transparent pixels skip color conversion.

Final real-worker/renderer measurements, Main 2560x1440@60, vertical canvas retained,
using temporary copies of the operator's collection with capture devices active:

- One `Starting.MS` plus actual H264 background: **6.652 ms/frame**, 1,222 rendered,
  **zero missed budgets** after warmup. This is a short run, not a zero-drop guarantee.
- Three independent alpha sources plus background: **10.313 ms/frame**, 3,555
  rendered, **148 missed budgets (4.00%)**. Average meets 12-14 ms target; tail
  latency and drops under this stress remain unresolved.
- A preceding four-minute three-overlay run crossed the 210-second media loop:
  10.785 ms/frame, 14,568 rendered, 1,016 missed (6.52%). No worsening timing trend
  or unbounded memory growth observed across that loop: renderer roughly
  354-378 MiB; alpha workers roughly 178-193 MiB each. Longer leak checks remain.

Build is warning-free under `-Werror`; all **30/30** checks pass with real GPU and
monitoring enabled. Checks cover immutable frame lifetime, alpha cadence/native
size, CPU/GPU sizing/crop/cache invalidation, OBS hardware-policy roundtrip, and
500 ms UI stats expiry. Saved operator collection remains untouched.

## Nested-scene render performance, 2026-10-21

Measured before changing the renderer: a 10-second `perf` sample showed capture
conversion and GPU draw/allocation costs. Alternating a nested 4K canvas with a
vertical canvas cost 28.1 ms in the GPU benchmark versus 11.4 ms for the nested
canvas alone. One positional image pool was being rebuilt between canvas sizes.
Each canvas now retains its own GPU compositor resources; deleted canvases release
them. CPU fallback remains available.

Unchanged normal-blend precomps can omit their isolation surface, provided opacity,
geometry, filters and child blends make that equivalent. CPU and GPU share this
plan optimization. Neutral color correction skips processing. GPU sampling avoids
wide division where exact identity/32-bit arithmetic suffices. Alpha-only masks
avoid redundant color conversions. Decoded mask/LUT images retain GPU uploads
(maximum eight / 128 MiB, counted against target budget); file replacement creates
a new decoded identity and invalidates the cached upload.

Real `FS.W.CAM`, Main 2560x1440 plus 1080x1920 vertical, using actual capture devices:
**9.417 ms/frame over 1,800 rendered frames after warmup**, 27 missed frames
(1.48% of rendered + missed). This meets the requested 12-14 ms render target but
is not a zero-drop claim or a 4K/live-encoding benchmark. Prior intermediate runs
were 13.2-13.8 ms; mask upload caching delivered the largest final reduction.

Reproduce without touching the operator's collection: copy it to a temporary
folder, close the production app to release devices, then run
`LIBERTAS_RENDER_BACKEND=gpu LIBERTAS_BENCH_PROJECT=/tmp/copy LIBERTAS_BENCH_SCENE=FS.W.CAM build-make/tests/unit/test_preview`.
`LIBERTAS_BENCHMARK=1` additionally logs per-canvas cumulative render stats and GPU
preparation/fence-wait samples. Full warning-free build and all 30 tests pass,
including real monitoring, GPU/CPU equivalence, mask replacement, alpha animation,
alpha audio, nested preview playback and the non-neutral eight-filter stress case.

## Transparent WebM and WebP media, 2026-10-21

WebM with `alpha_mode=1` uses FFmpeg's libvpx VP8/VP9 decoder so block-side alpha
survives conversion to RGBA and source-worker IPC. Frame presentation follows the
existing media player's position; its audio path remains active. Decoder errors
clear the picture and report a warning. Decoder pixel allocation is bounded.
Static WebP media uses QImageReader; animated WebP uses QMovie, including speed,
looping and clear-at-end handling. Static files do not restart in a busy loop.
The operator confirmed `Starting.ms` transparency works in the comparison build.
Generated fixtures exercise partial alpha, animation, looping and audio transport.

## Nested live scene refresh, 2026-10-21

`FS.W.CAM` froze because the UI's live-source detection visited groups and direct
sources but never followed precomp scene references. A program containing only
nested scenes therefore stopped its refresh timer after the first frame.
Live-source detection now follows visible nested scenes with a visited-scene set
for cycle protection. A real UI/renderer self-check runs an alternating slideshow
through two nested scenes and verifies changing delivered pixels and generations.

## Scene picker and unfinished plugin equivalents, 2026-10-21

Switching Add Sources to Scene now queries scenes directly from the selected
category. The category-change handler previously depended on a derived QML binding
that could still hold the preceding category, yielding an empty source query.
The UI self-check now switches categories and checks actual scene tiles.

Source Clone now has a native fixed-source workflow (2026-10-24): **Add Source →
Source Clone**, then choose an input in its settings. `source.clone` follows a
stable source ID, shares the existing capture/worker, supports independent source
and placement filters, optional audio with independent routing, and bypass of
input video filters. Audio starts disabled; enabling it routes existing placements
to Program. Settings use ordinary save/load and undo commands. Missing inputs and
cycles produce no picture; settings reject cycles and invalid targets. Chains are
bounded to 64. OBS collection import/export maps fixed source targets and options.

This is native source cloning, not full OBS plugin equivalence: current/previous
scene selection, scene/canvas targets, active-clone lifecycle controls and video
buffer/downscale options are not implemented. Imports report unsupported target
modes and buffering. The compositor still rejects the separate `ReferenceData`
node concept. Countdown gained a native dock on 2026-10-22 (see above), also with
explicit compatibility limits. Reference repos include
`/home/calmasacow/Development/obs-source-clone` and `/tmp/libertas-countdown-reference`.

Nested-scene failure was subsequently identified as frozen playback in
`FS.W.CAM`; see the live-refresh fix above. Saved Marathon collection remains
untouched.

## Separate audio and video filter panes, 2026-10-21

Filter editing now follows source capabilities: audio-only sources have one audio
pane and no preview; silent visual sources have one video pane; combined sources
have separate audio and video panes beside a shared preview/settings area. Each
pane has its own domain-limited Add menu, Remove, Up, and Down controls. Selecting
an entry in either pane selects its controls. Reordering skips the other domain
without changing that domain's positions or order in the saved stack.

Warning-free `-Werror` build; all 30 tests pass. Real UI checks cover both Add
menus, cross-pane selection, and domain-local reordering. An initial monitoring
assertion failed with the production app running; the full rerun passed after
closing it.

## Filter editor interaction and source preview, 2026-10-21

The editor follows the supplied OBS references: persistent assigned-filter list
on the left, selected-source preview above settings, sliders with numeric inputs,
color swatches/pickers, file browsing, defaults, and conditional crop/scroll/noise
suppression controls. Key ranges display in OBS's 0-1000 units. Resolution and
expander presets edit the existing native parameters. Audio-only sources offer
the ten audio filters and do not show a video preview.

Dropdowns now stack above their containing dialog. Outside clicks no longer close
the filter editor; Close and Escape do. Deferred document refresh avoids rebuilding
controls during their edit callbacks. Sliders have an explicit hit area and commit
once on release, preserving one Undo step per drag.

Preview uses a transient renderer-only canvas referencing existing source IDs.
No saved canvas or output is created, capture workers remain shared, and its deck
is excluded from audio routing. Closing the editor removes the transient view.
Renderer animation freshness combines all canvases so a static source preview
cannot stop an animated Program canvas. Checks exercise actual dropdown clicks,
every visual filter selection, slider drag/Undo, source preview delivery, audio
filter availability, excluded preview audio, and multi-canvas animation.
Warning-free `-Werror` build; all 30 tests pass (29 renderer/library tests plus
the expanded UI check). The UI timeout is 150 seconds to accommodate added
interaction coverage. Captured editor screenshot inspected for control alignment.

## Remaining stock filters and isolated VST2 hosting, 2026-10-21

The Filters editor now covers all 14 Linux stock video filter types and 10 audio
types, including Linux VST2. Right-click a source in Sources, Program, or the
source manager and choose Filters; mixer channels expose the same editor.
Changes apply immediately, persist, and support Undo. File-based filters have
a file picker; VST parameters come from the selected plugin.

New native CPU/Vulkan stages implement LUT, scroll, HDR tone mapping, SDR white
level, render delay and async video delay. Async delay also delays audio.
Scaling exposes point, bilinear, bicubic, Lanczos and area sampling, including
aspect-ratio sizing. Keys expose opacity/contrast/brightness/gamma; masks expose
tint and stretch. OBS import/export preserves these controls, independent scroll
width/height limits, and VST parameter snapshots. No libOBS dependency.

VST2 loads mono/stereo Linux `.so` plugins in disposable helper processes. Bounded
queues keep plugin IPC off the mixer thread. Crashes/timeouts mute the affected
path; disabling the filter restores audio. Generic parameter controls and program
selection are available; plugin-native windows are not. Processing adds at least
10 ms latency without automatic compensation. External vendor plugins remain
untested; the test plugin exercises gain, saved-state restoration and deliberate
crash through both the worker and real renderer monitoring.

Limits: native effects are not bit-identical OBS shaders/DSP. HDR controls map
into the current SDR output pipeline, not end-to-end HDR output. LUTs accept
2-64 entries per axis with finite 0-1 output, stored at 8-bit precision. Video
delay history is capped at 256 MiB and 1,200 frames per backend; warmup and
exhaustion report gaps rather than silently ignoring delay. Large resolutions
therefore support shorter delays. Unsupported imported filters remain visible.

The 4K/eight-filter stress check initially exceeded the 512 MiB compositor
allocation budget. Reusing equal-size intermediate images fixes that; measured
6.09 ms/frame including preview readback on this RTX 2070 (five-frame smoke
measurement, not a production latency guarantee). A temporary Marathon import
produced 40 scenes and 61 sources without changing the user's saved collection.
The imported contrast value 3.73 is now accepted; one existing mask file remains
missing and is reported for repair. Final warning-free `-Werror` build and 30/30
tests pass with GPU and monitoring enabled. Checks include all sampling modes,
invalid-LUT recovery, import/export settings, real context-menu editing, delayed
frames, nested gaps, and VST mute/crash/disable recovery through renderer audio.

## First working native filters and editor, 2026-10-21

Source, preview and mixer context menus open one shared Filters editor: add,
rename, edit, enable/disable, move and remove are undoable document commands.
CPU and Vulkan execute color correction, crop/pad, scaling, color/chroma/luma
keys, sharpening and image mask/blend. Source geometry uses the shared filtered
size for transform handles. GPU processing stays on device. OBS translation
preserves stacks and reports unsupported entries instead of silently bypassing.

Audio supports gain, polarity inversion, three-band EQ, gate, compressor with
source-ID sidechain, limiter, expander, upward compressor and RNNoise/Speex.
Source processing precedes routing; group/scene processing folds child sums per
destination; bus processing precedes its fader. Meters use processed audio.
Independent native implementations are not claimed bit-identical to OBS.

Warning-free build, 30/30 tests with GPU and monitoring enabled. Checks exercise
real source-menu/editor actions, rendered pixels on both backends, import/export,
save/reopen, and source/scene audio enable-disable through renderer monitoring.
Remaining: LUT, scroll/delay, HDR/SDR controls, isolated Linux VST2 hosting, broader
parameter fidelity and performance review. No libOBS dependency.

## Shared filter stack foundation, 2026-10-21

ADR-0065 records the required stock video/audio inventory, shared ownership,
CPU/GPU processing contract and editor scope. Sources and Scenes now own ordered
effect stacks alongside existing node effects and bus inserts. Named filters,
enabled state and parameters persist through schema version 3; SetEffects edits
any owner with Undo and journal serialization. Scene duplication gives owned
filters fresh IDs and remaps their node masks. Version 2 fixtures remain frozen;
version 3 has a separate golden fixture and migration checks.

Unknown active shared filters report video gaps and unavailable audio rather than
silently bypassing processing. Media workers remain alive during filter refusal,
so disabling a filter restores audio without restarting capture. OBS export warns
when shared filters cannot be converted. Real renderer monitoring checks cover
source/scene enable-disable recovery. Warning-free build and all 29 tests pass,
with GPU and monitoring checks enabled.

This is persistence infrastructure, not completed filter processing. Next: native
filter catalog and CPU/Vulkan video stages, audio DSP and isolated Linux VST2
hosting, OBS translation, and source context-menu editor with add/edit/reorder/
toggle/delete. Existing imported OBS records still do not apply filters.

## Scene-only audio import and mixer deletion, 2026-10-21

Scene imports skip OBS global Desktop/Aux audio devices and no longer create
track or per-source mixer buses. Scene audio sources route directly to Program,
preserving placement gain/mute; monitor-only sources remain muted. OBS monitoring,
balance/sync and track settings are reported rather than rebuilding OBS's mixer.
The original JSON remains available for reference. A temporary Marathon import
produces 40 scenes, 61 sources and only the native Program bus.

Mixer bus context menus now offer Delete channel. Program is protected; deleting
another bus removes incoming bus sends, with missing-route warnings for source
routes still naming it. Undo restores the bus and sends. Real UI right-click/
delete/undo and import checks pass; warning-free `-Werror` build and all 29 tests
passed with GPU and monitoring checks enabled. Existing collections are not
silently migrated; reimport or delete unwanted buses manually.

## Text worker startup, 2026-10-21

Text workers eagerly constructed QMediaPlayer and QVideoSink, triggering CUDA
device probes in every process. A 16-worker startup probe took about 21 seconds,
exceeding the five-second handshake deadline and leaving sources blank until an
edit recreated their worker. Both media objects now initialize only for media
and playlist sources; the same probe completes in 309-375 ms. Failed workers retry
after three seconds, with stop-aware backoff. Real-worker checks cover simultaneous
text glyph output and recovery after killing a worker without changing settings.
Warning-free `-Werror` build and all 29 tests passed, GPU/monitoring checks enabled.

Next: implement the OBS filter suite and
source context-menu filter editing (add, edit, reorder, enable/disable, remove),
including Linux equivalents where available and no libOBS dependency. Filter
processing and editing are not currently implemented; retained OBS records alone
do not apply filters.

## Text font selector, 2026-10-21

Text source settings now list installed font families in the existing dropdown
control. The stored `font` setting and `Sans` default remain compatible with
existing sources and OBS imports. GUI checks click an installed font, save it and
reopen the dialog to verify the selection persists. Warning-free `-Werror` build;
all 29 tests passed with GPU and monitoring checks enabled.

## OBS groups and linked canvas tabs, 2026-10-21

ADR-0064 imports OBS groups as native group nodes inside their scenes, with saved
local bounds, transforms, cropping and fresh placement IDs. Underlying sources
remain shared; actual nested scenes remain scene references. CPU/GPU rendering and
UI transform handles use the saved bounds. Ungroup refuses bounded groups rather
than silently dropping their clipping; OBS export still reports groups converted
to nested scenes. Recursive expansion is capped before any project is written.

The obsolete `streamelements_transition_audio_wrapper_source` type is skipped at
both global and scene placements, with a report warning. Other unknown types keep
their placeholders. A temporary Marathon import produces 40 scenes, 17 native
group placements covering all eight group names, and no obsolete wrapper sources.
Existing collections are untouched: import again to receive these corrections.

Selecting a Scenes or Sources canvas tab activates its counterpart in the other
panel. Closed panels remain closed; when both panels share one tab group, the
clicked tab wins. Checks cover actual tab clicks, GUI OBS import and child
transforms, persistence, shared source identity, CPU/GPU rendering and import
limits. Validation: warning-free `-Werror` build and all 29 tests passed with GPU
and monitoring checks enabled.

## Combined video capture audio, 2026-10-21

ADR-0063 adds an Audio input selector to Video Capture creation/settings: None,
Default (matching capture device), and discovered recording inputs. New sources
default to matching the card through physical USB/PCI identity; missing or ambiguous
matches remain unavailable, never fall back to the system microphone. Explicit
inputs are verified before delivering samples and cannot move to another device.

The same source owns video and audio, shares its name and mixer controls, and
retains capture while off-scene unless deactivation is enabled. None removes its
mixer strip. New attached audio routes to Program; enabling audio on older sources
routes unassigned placements in the same undo step. Old sources without the setting
remain video-only, preserving separate audio inputs. Existing projects are not
automatically converted; remove redundant standalone input after enabling attached
audio to avoid doubling. OBS export reports attached audio as a conversion gap.

Validation includes real-renderer private audio sinks for capture, switching, None,
missing/default-unmatched inputs, hidden placements, deletion and legacy settings;
UI selector/mixer/undo checks; and read-only Elgato physical-device matching. Full
suite passes 29/29 with GPU and monitoring checks enabled, warning-free build.
Independent hardware clock drift and automatic video reconnect remain limitations.

## OBS canvas membership, 2026-10-20

ADR-0061 preserves extra OBS canvas identities and scene membership, using native
canvases instead of flattening/renaming their scenes. Consistent saved custom scene
sizes supply extra canvas geometry; missing geometry explicitly warns and uses the
supplied Main dimensions on a separate canvas. Conflicting or oversized geometry
is refused before writing. FPS inherits the operator-supplied rate with a report.
Each extra canvas gets its own deck, initialized to its first scene with a warning
because OBS canvas metadata does not save active scenes here.

A temporary Marathon reimport creates Main at 2560x1440, Aitum Vertical at 1080x1920
and Vertical at 2560x1440 with an explicit missing-geometry warning. The operator's
existing collection remains unchanged. Import checks save/reopen and render both
canvas paths, preserve source identity and verify independent Program selection.
Extra-canvas OBS export still uses the previously reported custom-size-scene
approximation and is separate follow-up work. Validation: warning-free `-Werror`
build and all 29 tests passed with GPU and monitoring checks enabled.

## Audio monitoring output, 2026-10-20

ADR-0060 adds an Audio Mixer footer selector for None, System default and discovered
PulseAudio/PipeWire output sinks. None is the default, including projects without
a saved output. Refresh updates the list; unavailable saved outputs remain visible.
The selection is undoable and persists in project settings. Named sinks do not fall
back or move to another output automatically. Changing output recreates only monitor
playback; None releases it without clearing source/bus monitor flags or changing the
program mix. Failed opens retry at a bounded rate.

The UI check clicks System default and None in a project containing only colors,
checks save/undo/redo and rejects unknown identities. An opt-in real-renderer audio
check (`LIBERTAS_TEST_MONITOR=1`) sends a media tone through two private null sinks,
verifies output switching and None, and rejects a missing output. It never routes
its test tone to the operator's speakers. Validation: warning-free `-Werror` build
and all 29 tests passed with GPU and monitoring checks enabled.

## Seven blend modes, 2026-10-20

ADR-0059 carries Normal, Add, Subtract, Screen, Multiply, Lighten and Darken through
one shared draw plan to CPU and GPU execution. Properties offers these seven modes;
Add retains the existing `linear-dodge` serialized name. OBS import/export maps all
seven. Non-Normal groups and explicitly isolated groups flatten before blending,
with opacity applied once. Blended sources retain canvas transform handles.

Native alpha uses source-over coverage in scene-linear RGB. OBS fixed-function
blending can differ for transparent layers, so conversion reports state this limit.
Unimplemented blend/effect combinations still report gaps. Real scene tests cover
all modes, alpha, opacity, transformed/nested groups, and OBS round trips. Validation:
warning-free `-Werror` build and all 29 tests passed with GPU checks enabled. The UI
check clicks each Properties option and verifies actual renderer pixels and handles.

## Plugin support investigation, 2026-10-20

The operator moved this investigation immediately after monitoring output, which
is implemented and pushed. Proposed ADR-0062 inventories actual worker transport,
OBS extension interfaces, Source Clone and Countdown dependencies. It recommends
native versioned contracts across control/UI/media, retaining no libobs and no
in-process native plugins. Countdown and clone/filter implementations should prove
the contracts before publishing an experimental SDK. Existing Qt docks and graphics
callbacks need adaptation; unchanged OBS binaries are not supported. Wasm, shader
manifests, plugin discovery and permission enforcement remain future implementation,
not existing capabilities. The ADR includes delivery gates and the full extension
surface: sources, filters, transitions, audio, encoders, outputs, services and tools.
The source/import fidelity backlog below remains pending where not marked complete.

## OBS collection fidelity backlog, 2026-10-19

The operator approved the following scope after reviewing the Marathon import. Its
40 OBS scenes and eight groups exist as 48 native scene files, with scene item
references retained. The remaining work concerns rendering, behavior and import
translation, rather than merely creating the collection. Original JSON and the
import report remain alongside the imported project.

1. **Native source cloning.** Use https://github.com/exeldro/obs-source-clone as a
   behavior reference; expose a native equivalent and translate OBS parameters.
   Review target source/scene/canvas, audio, active lifetime, buffering and filter
   inclusion. Fixed-source cloning and import/export landed on 2026-10-24 (see
   above); scene/canvas and buffer modes remain pending. At the initial audit,
   six imported clones produced no picture/audio. Two target
   `Camera.SRC`, which is absent from the original collection: report unresolved
   targets and support explicit remapping rather than guessing a replacement.
2. **Stock filters.** Make OBS-equivalent filters available natively and translate
   their settings. This collection has six enabled image masks, 13 enabled color
   corrections and one enabled crop filter. Preserve disabled filters too,
   including luma key. Validate source, nested-scene and group filter behavior.
3. **Canvas mapping.** Reuse existing native vertical canvas support. Map OBS
   canvas identities, geometry and scene membership correctly; 14 Aitum scenes
   and one other vertical scene currently flatten onto Main. Do not infer missing
   canvas geometry silently.
4. **Blending.** Implement native blend modes in both render backends and import
   their settings. Screen and Multiply are used here. Normal remains standard
   alpha compositing, without an extra color blend operation. Preserve opacity,
   group isolation and color-space semantics.
5. **Media fidelity.** Resolve actual media dimensions before calculating imported
   placement; preserve aspect ratio and restart/keep-running behavior. Several
   sources explicitly disable restart on activation. Existing playback support
   does not make these imported settings equivalent.
6. **Text/group fidelity.** Audit existing support before adding features. Translate
   supported settings and fill actual gaps in text outline, shadow, wrapping and
   measurement. Repair missing bounds for `Live.Chat.Text.GRP`; groups already
   import as nested scenes.
7. **Stinger transitions.** Required: import and run the two file-backed stingers,
   with timing and audio behavior. The operator no longer uses StreamElements;
   ignore its four obsolete transition wrappers with a clear import report.
8. **Browser/audio behavior.** Preserve browser lifetime/settings where supported,
   review required OBS browser APIs, and prevent duplicate nested/shared audio.
   Add a monitoring-output selector in the audio panel. **None is the default**;
   offer system default and available sinks, including the operator's WaveSink
   system output and custom OBS monitoring sink when discovered. Never enable
   monitoring or select a sink by guessed name during import.

9. **Countdown timers.** Replicate the workflow of
   https://github.com/ashmanix/obs-plugin-countdown using native timers driving text
   sources. Inspect plugin configuration and controls before choosing the model;
   preserve multiple timer bindings and supported formatting/start/stop/reset behavior
   during import where saved plugin configuration is available. Existing countdown
   text alone is not evidence that timer automation was imported. The local plugin
   config is `~/.config/obs-studio/plugin_config/obs-plugin-countdown/config.json`,
   outside scene collection JSON. It contains two 15-minute timers bound to
   `START.CD.TXT` and `BRB.CD.TXT`; importing their automation needs that companion
   configuration. Review elapsed/target-time modes, formatting, end actions, color
   rules and hotkeys against the plugin before claiming full equivalence.

Capture rates must follow runtime device capabilities; a 60 fps canvas does not
require capture at 60 fps. Read-only V4L2 enumeration of the attached Elgato 4K X
reports 2560x1440 at 120 and 144 fps in NV12, YU12 and MJPEG, with no exact 119 or
165 fps mode at that resolution. YUYV, BGR3 and P010 advertise at most 60 fps at
1440p. These are advertised capabilities, not new streaming validation. Leave the
operator's saved capture settings unchanged. Review imported device identity and
timeouts separately from missing scene features; all 31 checked image/media/mask
file references exist.

For implementation, inspect existing model/render capabilities first, write an ADR
before model changes, drive real rendering/UI in regression checks, and commit and
push each successful section after a warning-free build and green suite. Keep CPU
fallback and GPU behavior aligned. Blend implementation status is recorded above; remaining sections are pending.

## Startup scene alignment, 2026-10-19

Initial scene selection now follows each canvas's saved Program scene. The saved show had
Screen in Program while startup selected the first scene, Elgato, so the viewport and Sources
panel disagreed. Opening a collection also restores selection from Program. Valid selections
during normal editing, including right-click selection, retain their existing behavior.

The headless check boots a real saved project with second-scene Program and first-scene Preview,
checks both canvases, opens it in the real GUI, and verifies scene highlighting, visible Sources,
Favourites and green pixels returned by the renderer. Alignment does not change the saved show.
Validation: warning-free -Werror build and all 29 tests passed with GPU checks enabled.

## Scene collection GUI, 2026-10-19

ADR-0058 enables every Scene Collection menu entry: New, Duplicate, Rename, Remove,
Import, Export, Project Problems and switching. New collections use the studio output defaults.
Current work is saved before switching, including initially unsaved sessions in managed storage.
An atomic catalog records names, paths and the last-open collection; --project overrides it.
Rename uses the document command and supports undo. Removal requires confirmation and uses
native Trash. Existing export paths are refused. Native copies preserve companion files and
absolute external media references; locks and journals are not copied.

Desktop portal choosers support OBS JSON/native directory import and both native/OBS export.
OBS import asks for canvas dimensions and rational FPS; conversion reports remain visible and
on disk. OBS export maps shared sources, scenes, nested groups and supported placements, with
explicit reports for unsupported effects, opacity, routing, device identities and profile settings.
Unsupported imported OBS plugin records can be restored for OBS to load. OBS profiles and
Libertas outputs are separate; credentials are never read. Editable sessions reject newer schemas.

Validation: warning-free -Werror build and 29/29 tests with GPU checks enabled. Real menu/dialog
checks cover rename, duplicate, new, both import/export formats, cancellation, native companion
files, failed switching, no-overwrite, Trash and catalog persistence. Exported cropped, flipped,
pivoted and rotated scenes reimport with identical rendered pixels. OBS itself and interactive
portal browsing have not been exercised for the new export workflow.

## Capture format and timeout controls, 2026-10-19

ADR-0057 exposes device-enumerated video formats, Use buffering, Autoreset on
timeout, and Frames until timeout. Defaults are Auto, buffering on, autoreset off,
and five frame periods; timeout integers must be 2–120. Auto retains the measured
format preference; capture logs now report the negotiated fourcc. Explicit format
selection must match the requested mode. MJPEG decoding uses installed FFmpeg;
raw conversion includes YV12 and SDR P010 when the device reports them. HDR remains
unsupported. OBS import preserves these four settings.

Buffering uses the bounded V4L2 queue. Disabling it drains older queued frames
before conversion. Timeout uses negotiated FPS and monotonic time; autoreset
stops, requeues and restarts the stream. Physical unplug/reconnect is separate and
unsupported. Hardware checks produced frames in all six Elgato formats at
1080p30, with buffering both enabled and disabled, and retained 1440p60 capture
and off-scene lifetime coverage. A physical signal-loss timeout reset has not
been exercised on this hardware.
Validation: warning-free `-Werror` build, 29/29 suite and both hardware-enabled
video/preview tests passed. Real UI clicks save both checkboxes and the integer
timeout. Source checkbox rows now have explicit click-area dimensions.
Shared checkbox layout anchors text six pixels after its square, avoiding overlap
from the former Row layout. UI checks assert this spacing for all three capture flags.

## Video capture stays ready, 2026-10-19

ADR-0056 starts configured V4L2 capture devices when the project loads, even if
their scenes have never been shown. Devices keep streaming off scene and publish
the latest frame, so switching back reuses capture without reopening the device.
The source settings expose "Deactivate when not showing", disabled by default.
Hidden capture updates do not trigger composites. Removing a source, changing its
capture mode or exiting the renderer releases the device. Changing only the
deactivation setting preserves a visible session.

The real-renderer hardware check exercises startup without an active scene,
repeated scene switches, opt-in deactivation, and hidden-source removal. It checks
device descriptors and requires a complete first composite on returning.
Validation: warning-free `-Werror` build, 29/29 suite, and both hardware-enabled
video/preview tests passed with the attached Elgato at 2560x1440@60.

## Scene clicks activate Program, 2026-10-19

ADR-0055 makes left-clicking a scene in Scenes or Favourites activate Program
immediately on that scene's canvas. Preview and Take are not required. Right-click
still selects for context actions; internal selection APIs remain selection-only.
Repeated clicks on the active scene add no undo entry.

Secondary canvases acquire an undoable, journaled deck on first activation. Canvas
removal includes its decks in the existing atomic removal command. The UI self-check
clicks real scene rows, verifies rendered colors, checks activation undo and verifies
that a secondary-canvas favourite leaves Main's Program unchanged. Command tests
cover deck lifecycle undo/redo and journal serialization.

## Preview cadence correction, 2026-10-19

ADR-0054 coalesces busy preview ticks into one refresh sent on acknowledgment,
without a growing queue or another full timer wait. Qt uses precise timer ticks.
Decoded pixels transfer into QImage ownership, removing one full-image copy while
keeping previously published images immutable. Unchanged-source skipping remains.

Live screen-capture profiling at a 2280x1282 preview found GPU work mostly 4-9 ms
and request round trips reaching 66-95 ms. CPU samples were dominated by copying.
Before/after samples measured average preview round trips of 12.31/11.07 ms;
these are sequential live observations, not a controlled benchmark or latency guarantee.
The operator reports noticeably smoother motion. A post-change panel sample showed
7.15 ms average rendering and zero overruns over 908 renders since reset.
Temporary stage probes were removed. The UI check stresses busy refreshes, preserves
an old image across a new rendered frame, and verifies stopping leaves no backlog.

## Render-lag accounting correction, 2026-10-19

ADR-0053 replaces UI busy-request ticks with renderer-measured composite overruns.
Preview protocol v6 carries cumulative per-canvas missed budgets alongside render
time and completed frames. Each composite contributes `max(0, ceil(duration / period) - 1)`;
the period uses that canvas's rational FPS. Reset baselines all three counters.
Audio work, transport, delayed acknowledgments, UI dispatch and unchanged capture
frames no longer masquerade as render lag. This measures composite overruns;
it does not claim to measure end-to-end preview or output delivery deadlines.

## Output groups and defaults, 2026-10-19

ADR-0052 makes Output Manager one named group per encode, with destinations nested
under that group's settings. The Outputs dock still lists individual destinations.
Adding a destination reuses the encode; removing a destination keeps its encode;
removing a group removes both in one undo step. The manager remains movable and resizable.

Global encoder presets support save, explicit replacement, apply, and delete. They
store encoder settings and rescale dimensions in `encoder-presets.json` under the
application config directory, excluding destination identities, URLs and credentials.
Applying a preset leaves canvas geometry and destinations intact. Invalid libraries
are refused without overwriting their contents. Existing projects retain their settings.

New UI sessions include Main canvas, Main Output encode and one disabled destination.
Adding a canvas offers horizontal 1920x1080 or vertical 1080x1920 at 60 fps, with its own
encode and destination. Both default to x264 H.264, 6000 kbps CBR, 2-second keyframes,
medium CPU preset, Main profile, Film tune, and custom buffer disabled (6000 retained).
Audio defaults to AAC 160 kbps, track 1, VOD track 0. Built-in presets downscale larger
canvases to the matching 1080 resolution without upscaling smaller canvases.
Encoding/output integration remains separate work; these controls configure intended output.
The warning-free `-Werror` build and all 29 tests pass with `LIBERTAS_TEST_GPU=1`.
Real UI checks cover shared destinations, preset controls and persistence, option validation,
new-session defaults, 4K downscaling, and canvas choices refreshed after adding a canvas.

## Source expansion and performance pass, 2026-10-19

The GPU, render statistics and Elgato work below is the validated baseline for this pass.
The warning-free `-Werror` build and all 26 tests passed again before the baseline commit.
Next: measure and reduce render costs, finish Linux source backends and their transform menus,
then import OBS scene collections. Commit and push each validated section. macOS-only sources
are excluded at the operator's request; use Linux equivalents where available.

### OBS importer and source coverage

ADR-0048 adds `libertas import-obs collection.json new-directory width height fps-num fps-den`.
The destination must not exist. Original JSON and a durable conversion report accompany the native
project. UUID references, source sharing, scene order, visibility/locks, quarter turns, flips, crops,
bounds and audio buses are converted. OBS groups become nested scenes with saved tight dimensions.
Name-only main-scene selection respects OBS's main-canvas UUID, so an Aitum scene with the same
name cannot replace program. Extra-canvas scenes are imported on the supplied canvas with warnings.

The local `Untitled.json` converted into `~/Shows/OBS-Imported-20261019.libertas`, using the OBS
profile's 2560x1440 at 60/1 fps. It contains two scenes and three sources; reopening and the project
inspector report no problems. Original OBS files and the existing demo projects remain intact.
Tests create/reopen real projects, render imported colors/images/groups, check cropped/flipped and
centered rotated placement, drive the actual CLI, and reject malformed references, cycles, duplicate
keys, excessive nesting and existing destinations. The warning-free `-Werror` build and expanded
suite pass 29/29 with GPU checks enabled.

Import requires operator review: capture identities, text layout, media output size, filters,
non-quarter rotations, crop-to-bounds and extra canvases may differ. Source records retain their
original settings. Transitions use Cut; hotkeys and output configuration are not converted.
Repeated audio placements can sum, including global audio in nested scenes; review routes.
The first importer is a CLI command, not an in-app import dialog.

Linux source coverage now includes audio input/output, color, image, text, slideshow, media,
playlist (VLC equivalent), browser, screen, window/game, V4L2 video, scene and transformed group.
Blackmagic has an isolated optional adapter and explicit unavailable-device behavior; real hardware
acquisition remains unverified. Only progressive SDR modes are accepted. Capture uses Hyprland's
native protocols; a portable portal fallback and cross-device GPU capture remain separate work.
Encoding/output integration remains separate from preview rendering and the audio mixer.

### Measured capture optimization

`perf` identified swscale's horizontal scaler as the dominant capture cost. At 2560x1440,
an isolated 300-frame conversion measured NV12-to-BGRA at 7.80 ms, planar YUV420-to-BGRA
at 1.19 ms, and BGR24-to-BGRA at 0.80 ms. Prefer YUV420, retaining the same USB bandwidth
as NV12 and preserving NV12 as a fallback. The real Elgato negotiated YU12 and sustained
59.39 fps with immutable leased frames; live renderer cycle samples over ten seconds fell
from 25.6 billion to 10.9 billion. These include capture and rendering and are not latency.

The repeatable `LIBERTAS_TEST_GPU=1 LIBERTAS_BENCHMARK=1` GPU test measures 1440p host upload,
4K composite and 640-edge preview readback: 7.32 ms/frame while the live show is also running.
A guarded 32-bit shader division experiment measured 7.18 ms, insufficient evidence to keep
extra branching; exact existing shader arithmetic remains. Larger gains need profiling the
host upload path separately from the GPU-only screen path.

### Native visual sources

ADR-0045 adds the isolated `libertas-source` worker. Text uses Qt font shaping and rasterization;
slideshows decode an ordered list of images; media and playlists use Qt's FFmpeg backend;
browser pages use sandboxed Qt WebEngine. The worker publishes straight RGBA through the existing
preview transport, and both compositors consume the same source snapshots. Explicit output
dimensions drive editor handles and placement. Text and playlist fields support multiple lines.

Worker checks assert real glyph pixels and transparency, changing slides, decoded moving video
and local HTML colours. CPU and GPU renderer-process checks verify text placement. Browser
rendering currently uses software frames and a host upload; profile before adding GPU sharing.
Sources restart when reactivated. Browser and media audio routing is not yet implemented.
Qt Multimedia and Qt WebEngine development packages are additional build requirements.

### Window and game capture

Window and Game Capture use `ext_foreign_toplevel_list_v1` plus
`ext_foreign_toplevel_image_capture_source_manager_v1`. The selector displays title and app ID
while storing the exact window identity. Closing and reopening an application requires selecting
its new identity. Dimensions come from the compositor and cursor preference is honored. A
stopped window session clears the old frame and retries at one-second intervals, including
size renegotiation. On this Hyprland session, both CPU and GPU process checks captured a real
2536x1390 window and verified shared snapshots across direct/nested placements. Game Capture
uses this same native window path; it is not an injected game graphics hook.

### Audio input/output and media PCM

ADR-0046 adds native PulseAudio/PipeWire capture and protocol v5 meter/PCM messages. Real Elgato
audio acquisition passed a 48 kHz stereo PCM check. A real worker/renderer check decodes a tone
and observes both source and program-bus levels. Media audio now reaches the renderer mixer;
the earlier visual-worker note describes that section before audio integration.
Source meters are pre-fader; bus meters reflect gain, mute and pan. Source/bus sends and local
Monitor controls operate on PCM. Browser audio uses a private temporary sound-server sink,
with a real Web Audio oscillator check verifying PCM reaches the parent. Encoder output remains
separate work. Blackmagic's optional native adapter runs in the source worker and uses vendor
API declarations with their original license; the absent-driver path is tested, but there is no
Blackmagic driver or hardware here to validate acquisition. Its explicit mode must be supported;
automatic format detection and HDR are not implemented.
The expanded suite passes 28/28; the additional real Elgato PCM and browser oscillator checks
also pass. The build remains warning-free with `-Werror`.

## Video capture update, 2026-10-18

### Group placement

ADR-0047 makes transformed groups use the shared scene plan on CPU and GPU. Groups expose
canvas-sized transform handles; child selection and drag coordinates follow ancestor groups.
Identity groups retain their existing pass-through behavior. Transformed contents clip to the
owning canvas; automatic tight bounds are deferred. Ungrouping a transformed group is refused
until its transform is reset, so placement cannot silently change. Pixel, GPU parity and real
UI interaction checks pass; the warning-free build and full suite pass 28/28.

Source cropping now trims sampled pixels on both CPU and GPU, rather than squeezing the whole
picture into cropped geometry. A red/green/blue strip check retains only green, and GPU parity
checks cover cropped captures with every quarter turn and flip. Full suite remains 28/28.

ADR-0044 adds `source.v4l2` capture through native V4L2 streaming and libswscale BGRA
conversion. Add **Video Capture**, select a device, resolution and frame rate. Stable
device paths exclude metadata nodes. Explicit modes fail instead of silently downgrading.
CPU and GPU consume the same leased snapshot through the shared scene walk; duplicate
placements cannot observe different capture frames within a render commit.

The attached Elgato 4K X negotiated **2560x1440 at 60/1 fps**. The real capture-thread check
measured **59.39 fps over 181 frames**, including BGRA conversion, while retaining an older
frame and verifying it remained immutable. Real renderer-process checks passed with both
CPU and RTX 2070 GPU backends, including direct and nested placements. These measurements
describe acquisition, not end-to-end latency or sustained 4K canvas throughput.
The build is warning-free with `LIBERTAS_WERROR=ON`; all 26 CTest entries pass with
`LIBERTAS_TEST_GPU=1`, including the headless UI check.

This slice supports raw SDR NV12, YUYV, YUV420 and RGB/BGR24. Audio, compressed-only devices,
HDR, automatic reconnect and USB zero-copy remain unimplemented. libswscale development
files are now a build dependency. Hardware checks run sequentially because capture is exclusive:

```sh
LIBERTAS_TEST_VIDEO=/dev/video2 build-make/tests/unit/test_video
LIBERTAS_RENDER_BACKEND=cpu LIBERTAS_TEST_VIDEO=/dev/video2 build-make/tests/unit/test_preview
LIBERTAS_RENDER_BACKEND=gpu LIBERTAS_TEST_VIDEO=/dev/video2 build-make/tests/unit/test_preview
```

## GPU render path update, 2026-10-18

ADR-0042 and its implementation were developed from `main` at `57ce881` and are included
in the validated baseline described above. The earlier clean-tree statement below describes
the CPU handoff. `AGENTS.md` was already untracked before the GPU work.

- `lcomp::plan_canvas` is the one scene walk. CPU and Vulkan execute its resolved operations;
  transparent precomps, source availability, gaps, placement and ordering are shared.
- `lrhi::GpuCompositor` composites at canvas size in scene-linear RGBA16F. A separate GPU pass
  produces sRGB8 at the requested preview size; only that reduced buffer is read back.
- Capture publishes leased dmabuf slots. A render commit holds one snapshot per connector
  across repeated placements, nested scenes and canvases. Capture threads do no Vulkan work.
- Preview protocol v2 carries a maximum preview edge. QML reports its physical display size;
  refresh-only commits reuse the document and unchanged frames receive only an Ack. The UI
  retains its last image on that Ack. This also fixes the old freshness skip: timer ticks used
  to resend the document and mark it changed every time.
- Offscreen runs use CPU. Normal runs try Vulkan, log failure, and fall back to CPU. Set
  `LIBERTAS_RENDER_BACKEND=cpu` or `gpu` to force a backend; `gpu` fails instead of silently
  falling back. `LIBERTAS_RENDER_DEVICE=226:128` selects by DRM device identity (`226:129` is
  Intel here). Cross-device capture is explicitly refused until validated.

### Evidence and checks

The original suite passed 24/24 before changes. The expanded suite passes 25/25 when hardware
checks are enabled with `LIBERTAS_TEST_GPU=1`; without that variable the GPU test is skipped.
The real process check also runs with a required GPU. Hardware parity checks pass on both RTX
2070 and Intel UHD 630, including rotations, flips, opacity, nested scenes and reduced preview.
CPU retains its sRGB8 intermediate rounding; GPU retains half-float linear intermediates. The
tested scenes allow at most two byte values of channel error, not a universal bound for an
arbitrary number of translucent layers.

Real captures on HDMI-A-1 and HDMI-A-2 pass pixel comparison and Vulkan synchronization
validation. Repeated rendering of one captured frame, including a 640-edge preview readback,
measured **1.30 ms at 2560x1440** and **1.87 ms at 3840x2160**, averaged over 60 frames.
The final 4K run with producer-fence waiting and synchronization validation measured 1.67 ms.
These are composite/readback timings, **not sustained capture throughput**. A separate real
process check keeps capture running and verifies direct and repeated nested placements read
identical snapshots. An unavailable DRM device also passes the CPU fallback process check.

```sh
cmake --build build-make -j 8
LIBERTAS_TEST_GPU=1 ctest --test-dir build-make --output-on-failure
LIBERTAS_RENDER_BACKEND=gpu LIBERTAS_TEST_CAPTURE=HDMI-A-2 build-make/tests/unit/test_preview
LIBERTAS_TEST_GPU=1 LIBERTAS_TEST_CAPTURE=HDMI-A-2 build-make/tests/unit/test_gpu_composite
LIBERTAS_TEST_GPU=1 LIBERTAS_RENDER_DEVICE=226:129 build-make/tests/unit/test_gpu_composite
```

### What follows this slice

Encode/output integration is still absent. The full-size linear target stays in the render
process, but an encoder handoff API is not exposed yet. ADR-0019's converted host-memory
transfer to a secondary encoder still needs integration and measurement. Cross-device capture
is rejected rather than assumed to work. A timed-out GPU fence terminates the render process
without recycling potentially live resources; automatic device-loss recovery still belongs to
the future supervisor/output work. The decoded-image upload path currently uploads each drawn
image per composite; profile a real image-heavy show before adding a texture cache.

## Previous CPU handoff

Written 2026-10-18, at the end of a session that got a real screen drawing in the Program panel
and then went looking for why it is slow. Read this before picking the work up elsewhere.

## The state of the tree right now

`main` is at `8731e1c`, pushed, **and the working tree is clean**. Everything this session
produced is committed: the benchmark instrumentation has been stripped, the build is
warning-free under `-Werror`, and the suite is 24/24.

The app was launched after the final commit to confirm capture still opens and the composite
idles at 0% CPU on a static screen.

## What landed and is pushed

Four commits put a live screen on the canvas, in this order:

- `2c31475` — ADR-0041, a screen source stores the connector it captures (`output`, e.g.
  `DP-1`). `libs/lcapture` became a real library holding the measured `wlcap` backend.
- `9253c37` — `composite_canvas` gained a `FrameSource` overload, so the scene walk can draw a
  frame it is handed without `lcomp` owning a device.
- `77cb1de` — the Vulkan device moved from `tools/spike-core` to `libs/lrhi`, where ADR-0005
  always said the GPU lives. Moved, not rewritten, so ADR-0011's measurements still describe it.
- `8accff2` — the two halves connected. Verified on this machine: RTX 2070, Hyprland, a real
  desktop in the Program panel.
- `6c54750` — removed six self-check assertions that were measuring event-loop passes rather
  than layout, and split the capture budget into a first-frame wait and a later-frame wait.
- `ee69359` — the blit was doing four million integer divisions a frame. See "The problem that
  was open" below.
- `cb03a51` — this document, with the CPU cost table and the GPU brief.
- `8731e1c` — the four operator-reported fixes, the screen dropdown, the live preview timer and
  the performance work. The section below is its shape.

## What landed in the last commit (`8731e1c`)

All committed. Listed because the commit is large and this is the shape of it.

**Four operator-reported fixes:**

1. **The transform menu works on any source with a rectangle.** `placed_map` in
   `apps/libertas-ui/document.cpp` was listing drawable types by name, so only a colour or an
   image could be selected on the canvas. It now asks whether the node has a natural size. A
   screen's natural size is its monitor's resolution, read from the compositor and cached for
   half a second (`screens_now()`).
2. **Shift is inverted.** Aspect is locked by default; Shift frees it. A source is a picture
   with a shape of its own, and distorting it should be the gesture you ask for. Alt is
   unchanged. The self-check asserts both directions.
3. **Edge snapping, 12 screen pixels**, to canvas edges and centre lines. Screen pixels rather
   than canvas units so the feel does not change with zoom. Works with the aspect locked: the
   other axis is recomputed from the ratio rather than distorting.
4. **A screen dropdown.** `source.screen`'s `output` field is now a `choice` filled at runtime
   from `libertas::capture::list_screens()`. The UI asks the compositor directly — listing
   outputs is a registry read, not a capture, so it needs no GPU and no session.

**Two bugs found by the self-check while making those**, both mine, both fixed: Alt was
recentring per-axis so the rectangle drifted, and a snapped edge under Alt moved without its
opposite number (11px off centre).

**The settings-dialog preview for a screen** now shows the chosen monitor's connector,
resolution and refresh instead of "no pipeline attached". Not live pixels: that would open a
second capture session for a source that may be cancelled.

**The preview republishes on a timer while a live source is on the canvas.** It previously
published only on `Document::changed`, which is why capture produced one frame and then a still
picture. The rate is the canvas's own `fps`, not the monitor's. `Document::hasLiveSource()`
decides, and states the live types rather than the still ones so an unknown type from a later
build defaults to still.

**Six performance changes**, each one measured:

| Change | Why |
| --- | --- |
| Capture moved to its own thread, waiting unbounded | ADR-0013 Consequence 4: a bounded wait must never be used to sample. The old code waited 120 ms per tick and returned nothing — an 8 fps ceiling regardless of the screen |
| `CapturedPicture` borrows rather than owns | Two 33 MB copies per frame at 4K, 2.6 ms each, against 1.6 ms for the scaling they fed |
| BGRA→RGBA folded into the sampler (`swap_rb`) | A separate pass over the whole buffer measured 5.1 ms a frame at 4K, converting pixels that were about to be scaled away |
| `srgb_to_linear` replaced by an exact 256-entry table | `std::pow` per channel per pixel measured 110 ms on a 1080p frame. Exact, not approximate — the table is filled from the same function |
| Decoded images cached on path + mtime + size | A still was re-decoded every composite. Fine when composites only happened on edit; not at 60 fps |
| `sample_axis` precomputed per axis instead of per pixel | Two `idiv` instructions were 68% of the blit. 35 ms composite to 5.3 ms. See below |
| Composites skipped when no new frame and no document change | A 30 Hz screen on a 60 fps canvas has nothing new half the time |

## The problem that was open, and its answer

**It was integer division, and `perf` named it in one run.**

The composite took ~35 ms, of which the blit was 33 ms, while a faithful standalone
reproduction of the same blit ran in 1.16 ms. Six hypotheses had been advanced and eliminated
before a profiler was available. The profile settled it immediately:

```
93.75%  libertas-render  [.] lcomp::blit(...)
```

and annotating that symbol:

```
36.87 :  2f680:  idiv   %rbx
31.09 :  2f648:  idiv   %r9
 7.70 :  2f6b3:  movzbl 0x3(%r9),%eax
```

**Two `idiv` instructions, 68% of all samples.** `sample_axis` does a 64-bit integer division,
and the blit called it once per axis per pixel: at 1920x1080 that is 4.1 million divisions a
frame. 64-bit `idiv` is 20-40 cycles on this core and does not pipeline, so it dominated
everything else in the loop.

Why the standalone benchmark did not show it: the benchmark's inner loop was simple enough that
the compiler hoisted the `y` division out of it. The real blit could not, because the branch
choosing between the straight path and the rotate/flip cases kept both divisions inside.
**That is the lesson worth keeping — the benchmark was not wrong about the code, it was wrong
about the loop the compiler would build.**

### The fix

`axis_map()` in `libs/lcomp/src/composite.cpp` precomputes the source index for every
destination row and column, once per blit, using `sample_axis` itself so the arithmetic is
identical and the picture does not change. A 4K-to-1080p blit now does about 3000 divisions
instead of 4.1 million.

Built only for the straight path. A quarter turn reads the other axis of the source for each
destination axis, so its four cases would each need their own pair; they are left on the
per-pixel path until a profile says otherwise.

### What it bought, measured

| | before | after |
| --- | --- | --- |
| composite (walk) | 35.1 ms | 5.3 ms |
| of which blit | 33.0 ms | 4.9 ms |
| of which image source | 2.1 ms | 0.4 ms |
| render process CPU | 78% | 53% |

The image source got faster too, because it goes through the same blit.

### What is left, and whether it matters

A re-profile after the fix shows no single hotspot: the time is spread across the pixel loop
(the heaviest instruction is a 12% load of the source alpha byte), which is what a memory-bound
blit is supposed to look like. There is no second `idiv`-shaped mistake hiding behind the first.

53% CPU for 5.3 ms of work per composite is the composite running at the canvas's 60 fps while
the captured screen only produces 30. The freshness skip added this session should cover that
and does not appear to be firing as often as it should — **that is the next thing to look at,
and it is cheap**: `Screens::anything_new()` in `apps/libertas-render/main.cpp` and the
`document_changed` flag in `libs/lcomp/src/preview.cpp`.

### Method note

The six earlier hypotheses (memory copying, `std::pow`, image decode, cold GPU memory,
optimisation flags, the wrong `blit`) were all reasonable and all wrong. A profiler answered in
one run what reasoning had not in several hours. Two practical notes for next time:

- `pgrep -f libertas-render` matches the shell wrappers as well as the process. Use
  `pgrep -f 'build-make/apps/libertas-render'`, and check the pid is stable before recording.
- `timeout 15 perf record ...` SIGKILLs perf before it writes its data, giving "no samples".
  Use `perf record ... -- sleep 10`, which lets perf terminate normally.

## What the CPU path costs, measured 2026-10-18

Run on this machine: RTX 2070, Hyprland, capturing HDMI-A-2 (3840x2160 @ 30 Hz), one screen
source plus one image source in the program scene.

| | 1080p canvas (4K downscaled) | 4K canvas (1:1) |
| --- | --- | --- |
| alloc + clear | 0.78 ms | 3.3 ms |
| blit | 4.9 ms | 18.4 ms |
| total walk | 5.3 ms | 21.4 ms |
| render CPU | 53% | 53% |
| sustained | comfortable at 30 | **~10 fps** |

A 1080p preview of a 4K screen is fine: 5.3 ms a composite leaves plenty of room at 30 Hz, and
the CPU figure is the composite running faster than the source produces rather than the blit
struggling.

Compositing 4K one-to-one is **~10 fps**, and that is the honest ceiling of this path. The blit
is 18.4 ms for 8.3 million pixels, which is about 2.2 ns a pixel -- memory-bound, no single hot
instruction left (the post-fix profile is 46% blit with the time spread across the loop rather
than concentrated). It is not a bug to be found; it is what a single-threaded scalar copy of
33 MB per frame costs.

The operator's position on this, recorded so it is not relitigated: **4K on an integrated GPU or
a CPU fallback at 10 fps is acceptable.** This path exists for machines without a usable device
and for the headless self-check. It is not the path a show runs on.

### What would make the CPU path faster, if it ever needs to be

Not done, and not currently justified. Listed so the options are known rather than rediscovered:

- **Thread the blit by destination rows.** It is embarrassingly parallel and there are 16 cores.
  Probably 4-6x on this machine.
- **SIMD the inner copy.** The straight path moves four bytes with a channel swap; that is a
  shuffle, and at 8.3 million pixels it would matter.
- **Composite at preview resolution, not canvas resolution.** The Program panel is a few hundred
  pixels tall. Compositing 4K to show it in a 400px box is most of the cost, and the encode path
  will want the full-size composite anyway -- so this is really "the preview should ask for the
  size it will draw".

The third is the one that would help a real show, and it belongs with the GPU work rather than
before it.

## The next phase: the GPU path

**This is the work to start next.** The CPU path is now as good as it needs to be for what it is
(see the section above), and the GPU path was always the intended one -- the operator's words:
*the GPU route is being built regardless; CPU was never the goal except as the fallback for a
machine without a GPU.*

### Where to start reading

- `docs/adr/0005` — six process roles. `libertas-render` owns the GPU. This is the ADR that
  settles where the device lives, and it has said so since the start.
- `docs/adr/0006` — the composite-to-encode handoff: scene-linear RGBA16F on a `VkDevice`.
- `docs/adr/0040` — why the preview is a separate process, and its last consequence, which is
  the constraint this phase has to respect.
- `docs/adr/0011`, `0019` — what was measured about capture and dual-GPU encode. The code those
  numbers describe is now in `libs/lrhi` and `libs/lcapture`, moved rather than rewritten.

### What is already built and working

- `libs/lrhi` owns a Vulkan device: instance and device creation, dmabuf export and import,
  readback to host memory, and a BGRA→NV12 compute pass hooked in from the encode spike.
- `libs/lcapture` captures a named screen into a ring of client-allocated dmabufs, on its own
  thread, blocking until the compositor delivers. Verified: 3840x2160 AR24 on an RTX 2070.
- `libertas-render` already opens the device, lazily, on the first screen source.
- `libs/lcomp` walks the scene and composites on the CPU, which is the fallback and the thing
  the headless self-check drives.

### The constraint that shapes everything

ADR-0040's last consequence, verbatim:

> The software picture must not grow into a second compositor that disagrees with the GPU one.
> When `lrhi` exists, the scene walk is the part worth sharing. A blend mode, a rotation or a
> capture implemented only here would be the disagreement.

`lrhi` now exists. So the first real decision is **not** "how do we draw on the GPU" but **"what
do the two backends share"**. The likely shape: the walk produces a list of draw operations --
source, rect, opacity, quarter turn, flips -- and the CPU and GPU backends each consume that
list. Two walks that happen to agree today will not agree in six months.

### The decisions ADR-0042 has to make

1. **The shared walk.** What the walk emits, and what a backend is. This constrains everything
   after it, so it comes first.
2. **Where the captured frame stops being a dmabuf.** Today it is read back to host memory for
   the CPU walk. On the GPU path it should stay on the device from capture through composite to
   encode, and the readback should happen only for the preview, at preview size.
3. **Preview size versus canvas size.** The Program panel is a few hundred pixels tall and the
   composite is currently full canvas resolution. The encode path wants full size; the preview
   does not. Deciding this well is most of the CPU path's remaining cost too.
4. **What happens with no usable device.** The CPU path stays, because the self-check runs
   offscreen and some machines have no GPU. Both paths must stay correct, which is the argument
   for one walk.
5. **The dual-GPU case.** This machine has an Intel UHD 630 alongside the RTX 2070 — exactly
   ADR-0019's subject. `vk_create_exportable` already carries a `host_memory` flag for the
   system-RAM round trip a second GPU needs. None of the current capture path is tested across
   two devices.

### A suggested order

1. Write ADR-0042 for the shared walk. Code after, per the repo rule.
2. Keep the frame on the device: capture → GPU composite → readback at preview size only.
3. Then the encode half, which has measured code waiting in `tools/spike-core`.

### Before writing any code, read this

The repo rules that bit hardest this session:

- **Model changes need an ADR before code.** ADR-0042 is next.
- **Non-trivial behaviour gets a self-check assertion that drives the real thing**, not the
  function behind it. The GPU path cannot be driven by the offscreen check, which is exactly why
  the CPU path has to stay correct and checked.
- **Builds are warning-free under `-Werror` and the suite is green before a commit.**
- **Measure before changing.** Six reasoned hypotheses about the composite were all wrong this
  session; one `perf` run found the answer. `perf` is installed now.

## Things deliberately left undone, with reasons

- **`showCursor` and `pauseWhenHidden` are stored and ignored.** The cursor is always captured;
  nothing stops capturing when a source is off screen.
- **Nothing checks panel geometry in the scene graph.** Six assertions did and were removed in
  `6c54750` — they measured event-loop passes, not layout. `test_layout`, `test_dropzone` and
  `test_layout_serialize` cover the engine without a window.
- **The first-frame and later-frame capture budgets (3 s and 120 ms) are guesses from one
  machine.** The slow case is the untested one. Note that the later-frame budget is now moot on
  the threaded path, which waits unbounded; the first-frame wait still matters.
- **No portal backend in `lcapture`.** `linux-portal/` is still an empty directory; the portal
  code remains in `tools/spike-core`. It stays the fallback (ADR-0011) and is what would fill
  `output` in from the picker's `mapping_id`.
- **What happens when a named connector is gone** is undecided (ADR-0041). It is a running-show
  failure and belongs with a supervisor that does not exist yet.
- **"Fit to screen" bakes a layout box at the canvas size it was used on.** Changing the canvas
  afterwards leaves the node drawing at the old size, because `fit: natural` then resolves
  against the stored box rather than the source. Hit while benchmarking: a canvas changed to 4K
  kept compositing its source at 1920x1080, which looked like a bug in the canvas resize and was
  not. Whether a fit should re-resolve when the canvas changes is a real question and nobody has
  answered it.
