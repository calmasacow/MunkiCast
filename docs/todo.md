<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Todo

Implementation audit: **2026-10-29**. Open work only is unchecked. A supported API or
saved setting is not proof that its renderer, UI, or hardware workflow is finished.
Model changes need an ADR. Historical design discussion remains in Git history
(`b9bb757:docs/todo.md`), the [ADRs](adr/), and [development history](development-history.md).
See [current status](current_goal.md) for what is usable now.

## QA and reliability

- [ ] Validate the rebuilt app in the operator's normal Wayland session: cold start,
  restore a saved layout, switch collections/workspaces, open each dock/dialog,
  change canvas selection, dock/undock tabs, resize the window, and repeat at the
  normal display scale. The automated panel check covers headless loading; it does
  not certify every driver/display combination. Capture the exact panel, preceding
  action, and log if an intermittent blank remains.
- [ ] Restore reliable pointer-hover coverage for the Transform flyout. Commands are
  covered; the old synthesized-hover test was flaky. Use real hit regions and
  condition-based waits rather than event-loop pass counts.
- [ ] Extend settled scene-graph geometry checks across DPI/window sizes. Layout math
  and the new positive-size panel checks are covered; exact divider/drop-overlay
  pixel coverage is still incomplete.
- [ ] Long-running real capture/media/stream/record/replay soak, device unplug/replug,
  destination failure/recovery and disk-full checks. Existing focused tests and
  short benchmarks do not establish long-show stability.
- [ ] Profile remaining renderer/capture memory-copy cost and tail latency using a
  temporary show copy. Preserve CPU/GPU equivalence and measure before/after under
  the same workload. Earlier still-image/alpha-media optimizations are implemented;
  historical results still showed occasional missed frame budgets.
- [ ] Inspect unsaved editor state when docking/closing a panel or changing
  collections; define preserve/discard behavior before retaining every hidden UI.

## Disabled menus: implement or remove

These are explicit `Pending` entries in `apps/libertas-ui/qml/MainMenu.qml`, not
controls temporarily disabled by selection or output state. Each group needs a
working action, UI acceptance check, and documentation, or removal from the menu.

| Menu | Unfinished entries | Completion direction |
| --- | --- | --- |
| File | Show Recordings; Remux Recordings; Show Settings Folder; Show Profile Folder | Open real configured directories; add a remux job with failure reporting; profile folder depends on profiles. |
| Edit | Paste (Duplicate); Copy Filters; Paste Filters; Preview Scaling; Lock Preview | Define independent-source duplication, validated filter copy/paste, and persisted preview controls. Ordinary multi-source copy/paste already works. |
| View | Dock Toolbars; Source Icons; Open Multiview; Always On Top | Implement actual view controls and native Wayland-supported window behavior, or remove unsupported entries. |
| Docks | Lock Docks; Chat; Activity; Stream Info; Overlays | Enforce layout locking; define service-backed docks. Custom Browser Docks already works and is not blocked on these. |
| Profile | New; Duplicate; Rename; Remove; Import; Export; Profiles are not available yet | Decide whether a separate global profile model is wanted. Collections and encoder presets already exist; the OBS compatibility profile is not profile CRUD. |
| Tools | Auto-Configuration Wizard; Plugin Manager; Output Timer; Scripts; Source Profiler | Define each workflow/backend before enabling it. Output Timer is distinct from the working Countdown Timers dock. WebSocket and MCP settings are implemented. |
| Help | Join Discord Server; Restart in Safe Mode; What's New; Release Notes | Supply a real community URL, a defined safe-start mode and release content, or remove the placeholders. |

## Other incomplete controls and product surfaces

- [ ] **Outputs dock:** per-destination stream start/stop, Start All, Stop All and
  Built-in virtual camera remain unfinished. The top toolbar already runs the
  supported single live stream; row recording and replay-save actions work.
  Implement supported transport semantics before enabling bulk controls.
- [ ] **Transitions:** Slide is explicitly unimplemented. Native Cut, Fade, Fade to
  Black and file-backed stingers exist. Track mattes, underlying-audio crossfade
  and scene-specific transition overrides need model/render support.
- [ ] **Source preview/interaction:** source settings preview is not a full live
  preview for every source type. Add browser-source interaction/reload and projector
  surfaces if retained in the intended feature set.
- [ ] **Countdown parity:** color rules, hotkeys, stream triggers and smoothing remain
  unsupported. The Source selector in Countdown Settings is intentionally read-only:
  binding/unbinding belongs to the dock, so it is not a dead control.
- [ ] **Clone parity:** fixed source targets work; current/previous-scene selection,
  scene/canvas targets, lifecycle controls and buffer/downscale options remain open.
- [ ] **Extension SDK:** manifest/contracts, negotiated worker API, permissions,
  restart supervision, Wasm/Slang integration, author tools and plugin management.
  Native filters/VST2 and built-in source workers do not constitute a public SDK.
  See [ADR-0062](adr/0062-extension-contracts-and-obs-porting.md).

## Editing and workspace follow-ups

- [ ] Media/slideshow/playlist dimensions and playback defaults: finish metadata-based
  sizing before removing the manual Speed/Output width/Output height fields requested
  for simplification. Preserve existing projects and deliberate playback overrides.
- [ ] Arbitrary rotation/skew rendering remains limited; group transforms and nested
  live rendering are implemented. Keep explicit unsupported-operation diagnostics.
- [ ] Rubber-band multi-selection, drag edge auto-scroll, and opening collapsed groups
  while hovering a dragged item. Ordinary multi-select, copy/paste and group collapse
  are already implemented.
- [ ] Mixed-value Properties editing for multiple selected nodes (currently deliberately
  blank), plus multi-node batch/undo semantics.
- [ ] Consistent scroll indicators for panel lists that scroll only with the wheel.
  Some dialogs already use ScrollView; the old blanket claim of no scrollbars was wrong.
- [ ] Reorder secondary canvases while Main stays first; move scenes between canvases;
  optionally open all of one canvas's panels together.
- [ ] Precomp retargeting controls and a warning when deleting a scene used elsewhere.
  Review cross-canvas precomp paste and off-canvas paste warnings against rendered bounds.
- [ ] Direct rename in the source manager and an explicit policy for deleting on-air
  sources. Sources are genuinely live now; lack of an output engine is no longer a reason
  to defer the policy.
- [ ] Decide whether Fit to Canvas should follow later canvas size changes or retain
  its authored box. Current behavior keeps the saved box.
- [ ] Mixer icon/styling follow-up and any remaining source-settings refinements need
  concrete operator acceptance criteria. Live meters, per-strip routing and monitor
  device selection already exist. Solo remains deliberately excluded (ADR-0031).
- [ ] Floating top-level panels, popup output settings and clone canvases with source
  substitution remain deferred design choices, not missing parts of ordinary docking.
- [ ] Asset commands and compound editing beyond the existing atomic Batch operation.

## Outputs and remote control

- [ ] Multiple independent concurrent encodes/destinations, automatic reconnect,
  custom stream delay, and operation after the UI closes.
- [ ] Native SRT/WHIP transport, virtual camera, recording pause/split/chapters and
  live captions. Do not advertise these just because destination fields or compatibility
  status requests exist.
- [ ] Runtime media seek/pause/restart/status IPC, deinterlacing, and richer per-output
  congestion/reconnect telemetry. Replace placeholder status values with measurements.
- [ ] Complete OBS transform/bounds translation, audio monitor-only/per-input sync,
  asynchronous remote stream-key writes, arbitrary-scene screenshots, property-button
  actions and unsupported batch modes. The request-by-request inventory and completion
  paths live in [remote-control.md](remote-control.md); full OBS parity is not achieved.
- [ ] External acceptance with the installed Companion module and actual AI clients:
  reconnect/restart, Studio toggle, timecode buttons, source creation/transforms and
  menu interaction. Native MCP is independent of WebSockets and covers implemented
  app operations, not unavailable product features or external portal windows.

## Capture and platform follow-ups

- [ ] Production portal fallback for screen capture; the spike backend is not equivalent
  to a completed product backend. Window/game capture has its own implemented path.
- [ ] Screen `pauseWhenHidden` behavior and source/connector recovery policy. `showCursor`
  is read by the screen capture path now; it is no longer an ignored setting.
- [ ] Remove or bound the unused single-shot `wlcap_capture_frame` wait before adding
  callers. Preserve damage-driven capture: cancellation/re-request must not fabricate
  frame cadence on a still desktop. Review the portal spike's zero-frame diagnostic.
- [ ] Validate startup/timeout behavior on slower and different GPUs, dual-GPU paths,
  missing connectors and capture-card signal changes. V4L2 reconnect after unplug remains
  unavailable; current-mode and match-signal selection are implemented.
- [ ] Register system-wide shortcuts through the portal/session boundary. Application
  shortcuts already work; OS-global scope is not implemented by listing it in the UI.
- [ ] Expand hardware probe codec checks and an optional isolated encode-test with a
  hard timeout. Hardware claims are not actual encoding results.
- [ ] Explicit sync needs a measured use case before adding `linux_drm_syncobj_v1`.
  The historical submission-stall hypothesis did not justify implementing it.

## Completed since the old backlog

- [x] Capture and CPU/Vulkan program rendering; independent canvases, links and groups.
- [x] Native source workers, media preload/activation, alpha WebM/WebP, live nested scenes.
- [x] Audio capture/mixing/meters/monitoring and native video/audio filter editing.
- [x] Multi-source selection/copy/paste, collapsible groups and scene/favourite ordering.
- [x] Encode graph/settings/presets, RTMP/RTMPS streaming, recording and Replay/Backtrack.
- [x] Collection GUI import/export/management, menu bar, browser docks and caching UI.
- [x] Studio Mode, quick/manual transitions, stingers, countdown bindings and source clones.
- [x] Libertas/OBS WebSocket modes, Companion toggle/timecode presets, independent native MCP.
- [x] Collection-switch panel rebinding, per-canvas Sources state, retained QML refresh
  dependencies, hidden-view refresh reduction and coalesced encoder graph updates.

## Validation record

The 2026-10-29 panel regression initially reproduced blank scene/source rows on
collection switches while the prior `shell_self_check` passed. After the fixes:

- `cmake --build build-make -j8` and the final `libertas-ui` target rebuild passed.
- 32 ordinary tests passed; the GPU test skipped by default, then passed explicitly
  with `LIBERTAS_TEST_GPU=1` (2.46 seconds).
- All eight UI/integration tests passed (180.92 seconds), including the new panel
  regression, full shell, Studio Mode, reorder, replay, Companion and native MCP.
  That is **41 distinct tests passed**, with no remaining skipped test in this run.
- The final executable's `--check --check-panels` passed again after correcting the
  test fixture to use the registered `source.color` type.
- No QML reference/type/binding-loop/assignment errors appeared in the final UI log.
  `git diff --check` and local documentation link checks passed. All 34 explicit
  disabled menu entries are accounted for in the table above.

Logs for this local run are `/tmp/libertas-panel-qa-build.log`,
`/tmp/libertas-panel-qa-unit.log`, `/tmp/libertas-panel-qa-gpu.log`,
`/tmp/libertas-panel-qa-ui.log`, and `/tmp/libertas-panel-qa-final-panels.log`.
Hardware capture, provider authorization, external clients and long-show acceptance
remain the unchecked work above. No FPS improvement is claimed from the UI changes;
they remove redundant work and fix observed stale/empty contents.
