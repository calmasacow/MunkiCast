<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Current status

Reviewed against the implementation on **2026-10-29**. This is a pre-alpha studio;
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
- One H.264/AAC RTMP/RTMPS stream, MKV recording and RAM replay buffering. Concurrent
  stream/record/replay share compatible encodes; independent simultaneous encodes,
  multistreaming, automatic reconnect and background operation remain pending.
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
