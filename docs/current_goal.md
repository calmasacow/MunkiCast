<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Current status

Reviewed against the implementation on **2026-11-09**. This is a pre-alpha studio;
implemented controls do not establish production readiness across every device.
The actionable backlog is [todo.md](todo.md). Dated benchmarks and earlier development
notes are preserved in [development-history.md](development-history.md).

## Implemented

- Build/Live docking, saved layouts, Encode graph, per-canvas scenes/sources/program
  views, browser docks, scene/favourites reordering, multi-selection, grouping and
  group collapse, transforms, clipboard, undo/redo and collection management.
- CPU/Vulkan compositing, nested live scenes, screen/window/game and V4L2 capture,
  image/text/browser/media/slideshow/playlist sources, audio capture/mixing/meters
  and monitoring. Availability depends on the platform and hardware.
- Native video/audio filter stacks and isolated Linux VST2 hosting, fixed-target
  source cloning, countdown text bindings and file-backed stingers. These are
  implemented subsets of the corresponding OBS/plugin workflows.
- Studio Mode with preview/program, quick transitions and manual transition control.
- Native GPU Program/Preview projectors with windowed/fullscreen monitor selection.
  Program works outside Studio Mode; renderer-owned DMA-BUF presentation continues
  independently of the UI. See [projectors](projectors.md) for timing and hardware limits.
- One H.264/AAC RTMP/RTMPS stream, MKV recording and RAM replay buffering. Concurrent
  stream/record/replay share compatible encodes; independent simultaneous encodes,
  multistreaming, automatic reconnect and background operation remain pending.
  V4L2 buffering now adds a bounded two-frame presentation delay without waiting
  inside renders; see [recording cadence](recording-cadence.md) for hardware checks
  and remaining camera/clock limits.
  Elgato fast-motion repeats remain under investigation: original-file repeats are
  confirmed, the isolated numbered-frame NVENC test passes, and buffering is now
  enabled in the live document. The 16:34 capture trace isolated buffer eligibility
  chatter despite different input already waiting. Matching-rate hysteresis removes
  all 27 application holds in the exact 90,275-frame replay; focused jitter/drift
  regressions pass. The 18:02 hardware recording now confirms zero selected-frame
  reuse across all 16,436 pictures, matching the corrected replay exactly.
  Separate output-deadline stalls (19 slots near 4:08), long-show/lip-sync coverage
  and attribution of a few near-identical moving-crop candidates remain open;
  see the recording-cadence investigation.
  The later hour-long simultaneous stream/SMB recording confirms 844 missed output
  slots. Per-destination bounded packet writers now keep file/socket writes off
  the render thread; a stalled-destination regression verifies continued encoding
  and complete delivery to the healthy destination. Periodic CPU/GPU stalls and
  long-stream acceptance remain open; ordinary runs now log deadline-stage timings.
  Current 120-to-60 capture also uses bounded integer-stride phase tolerance, and
  recording reuses full-resolution readback storage. Focused capture/preview/encoder
  checks and all 15 GPU cases pass. A final 5:23 recording has 19,386 packets and no
  missed output slots; the app exited normally before the next periodic-refresh
  window, so periodic-stall and concurrent-live acceptance remain open.
  A later operator run had a roughly 1 fps preview despite low render-time readings;
  full captured-picture hashing was enabled. A clean restart without diagnostic
  flags restored measured 60 fps preview delivery, around 2 ms preview-only rendering,
  and around 4.5–5.6 ms while recording. Separate 30-second local and media-store
  recordings each have zero missing output slots, including healthy preview delivery
  after stopping. Restart and tracing removal were combined, so the freeze's cause
  is not yet isolated. The cadence checker now uses sampling-only diagnostics.
- Encoder graph settings/presets, destination catalog, keyring-backed stream keys,
  Twitch/YouTube authorization flows, and prepared asset caching.
- Libertas/OBS WebSocket modes, native Companion module with Studio Mode toggle and
  timecode presets, and an independent native HTTP MCP server with document and UI
  control. MCP defaults to loopback port 4466; WebSockets default to port 4455.
  See [remote control](remote-control.md), [MCP coverage](native-mcp.md), and
  [client configuration](../integrations/mcp/README.md).

## Panel QA and optimization pass

The new `panel_loading_self_check` exercises actual visible scene/source rows while
switching collections, per-canvas selection, first reopen of all 13 ordinary panel
bodies, live canvas titles/sizes, countdown source choices/readouts, and browser
attachment across workspace/layout changes. The existing shell
check had passed while missing the collection-switch failure.

The panel fixes re-resolve Main's scene/source model when a collection replaces its
canvas ID and keep Sources visibility/Add controls tied to that panel's own canvas.
Comments explain the signal dependencies and the QML compiler's handling of unused
reads. The same dependency fix covers program geometry, transform outlines, dock
checkmarks, countdown choices/readouts, mixer hidden counts, and encoder choices.
Hidden source dialogs no longer rebuild their catalogs on every edit; hidden
or empty countdown docks stop their display timer; encoder graph link updates are
coalesced after a layout pass. Output status/help text now reflects existing live and
replay support instead of saying the output engine does not exist.

All 41 configured tests passed across the ordinary, opt-in GPU, and UI/integration
runs. The final panel check also passed on the rebuilt launch binary. Detailed
validation and remaining acceptance work are recorded in [todo.md](todo.md).
No live destinations or saved operator collections are needed for the panel checks.

## Launch recovery: 2026-11-09

The root/home Btrfs filesystem reached 100% usage (64 KiB available). Libertas
failed with SIGBUS in Chromium's cache thread; the separate Omcontrol desktop
plugin's Python/SQLite processes also failed with SIGBUS. The journal could not
store their dumps because the disk was full. No stored core was available to
symbolize; recovery after freeing space supports filesystem exhaustion as the
common trigger.

Recovery removed 2.32 GB of downloadable debug symbols and 74 unindexed prepared
media files (23.00 GB logical), each SHA-256 verified against a retained indexed
copy. Btrfs sharing means logical deletions do not equal physical space recovered:
after delayed Btrfs reclamation, the filesystem reported 23 GiB available. All 54 indexed media copies,
original assets, recordings, projects and databases were retained. Both Python and
the SQLite CLI passed read-only `PRAGMA quick_check` on Omcontrol's history database.
The exact `build-make` application launched with its renderer attached, restored
the saved show and displayed live capture; native MCP and window capture responded.
No new error-priority journal entries appeared during the recovery observation.
Cache lifecycle and disk-full acceptance remain in [todo.md](todo.md).

## Run and validate the application

Rebuild the directory used to launch the application:

```sh
cmake --build build-make --target libertas-ui -j8
./build-make/apps/libertas-ui/libertas-ui
ctest --test-dir build-make -R panel_loading_self_check --output-on-failure
ctest --test-dir build-make --output-on-failure
```

Restart an already-running app to use the rebuilt executable. `--check` uses
isolated project/settings fixtures and the offscreen Qt platform. Real Wayland/GPU,
portal, capture-device, OAuth and receiver acceptance remains separate from these
checks. Browser-specific commands are in [browser-docks.md](browser-docks.md).

## Next work

Prioritize reproducible UI failures and output resilience, then finish or remove the
explicit disabled menu/dock entries listed in [todo.md](todo.md). Do not treat the
historical pre-renderer TODOs as missing features, or advertise full OBS parity based
on the number of MCP tools or WebSocket requests.
