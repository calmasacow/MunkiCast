<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0062 - Extension contracts and OBS source porting

- **Status:** proposed; investigation complete, SDK not implemented
- **Date:** 2026-10-20
- **Constraints:** ADR-0001 (no libobs), ADR-0004 (no in-process native plugins)

## Goal and boundary

Make MunkiCast extensible across sources, filters, transitions, audio, encoders,
outputs, services, automation and interface tools. Make OBS plugin **source ports**
practical through small documented host bindings, without loading OBS binaries,
linking libobs, copying its implementation, or recreating its runtime. Independently
licensed algorithm/vendor-library reuse requires a per-project license review.
OBS implementation remains a read-only behavior reference, including utilities.

An unchanged OBS plugin is not a supported artifact. Shared pointers, synchronous
render callbacks and QWidget docks cannot cross our process boundary unchanged.
Porting effort depends on those dependencies, not plugin size. No evidence yet
supports promising trivial ports for most plugins.

## Evidence and existing foundation

Inspected MunkiCast at `4cfea11` and local OBS at
`f1c0a116441bdb7a1de7896f810009ea487b8508`:

- `libs/lipc/include/lipc/channel.hpp`: bounded SOCK_SEQPACKET messages and file
  descriptor transfer; useful transport, not a public extension protocol.
- `libs/lcomp/include/lcomp/preview.hpp`: versioned preview messages and memfd
  frames; not a negotiated plugin SDK or general timestamped media contract.
- `apps/munkicast-render/source_worker.cpp`: actual child process source transport,
  timeout and cleanup. Its fixed refresh delay and failure exit are not a finished
  plugin scheduler or restart supervisor.
- `libs/lcomp/src/composite.cpp`: shared scene traversal; enabled effects still
  unsupported. Reference data in the model does not establish a working clone.
- `apps/munkicast-ui/document.cpp`: built-in source/property registration remains
  application code. `schema/` contains no tracked effect manifests; CMake has no
  Wasm runtime integration. Slang effects and WIT are architectural intentions.
- OBS `libobs/obs-source.h`, `obs-encoder.h`, `obs-output.h`, `obs-service.h` and
  `frontend/api/obs-frontend-api.h`: source/filter callbacks, encoded/raw output,
  service constraints, frontend events and direct QWidget dock insertion.

Representative plugin source reads used downloaded master snapshots:
[Source Clone](https://github.com/exeldro/obs-source-clone) (`src` implementation
saved locally as `munkicast-obs-source-clone.c`) and
[Countdown](https://github.com/ashmanix/obs-plugin-countdown) (`countdown-widget.cpp`,
`timer-persistence.cpp`, README). Remote HEADs observed on this date were
`e5ca9dd25229fdf1f1b8c6fa83762c0581c18c92` and
`c0bbb194803e64f5d31291926fba9bc3c72c69a0`, respectively; the earlier downloaded
snapshots were not hash-verified against these commits. This is an API inventory,
not a completed port or exhaustive compatibility audit.

## Porting map

| Extension | Observed dependency / required behavior | Native contract and effort |
| --- | --- | --- |
| Source Clone | Weak source/canvas references, showing/active counts, filtered or raw render, frame buffering, audio callbacks | Shared source handles, explicit pre/post-filter taps, retention leases and audio routing. Significant host glue; never duplicate capture sessions. |
| Countdown | Frontend events, hotkeys, text updates, JSON persistence, Qt dock, websocket vendor events | Timer/action/event API and declarative panel. Timer logic may transfer; dock and bindings need replacement. |
| Image/color/key filters | Render callbacks and graphics effects | Shader manifest plus typed parameters, color/alpha contract and CPU implementation where promised. Rewrite graphics glue. |
| Audio filters | Mutable sample callbacks | Timestamped block processor with bounded queues, format negotiation and latency reporting. DSP logic potentially reusable. |
| Transitions | Two scene inputs, clock/progress, optional media/audio | Host schedules input frames and completion; effect/worker produces transition. Stingers require decoded media and audio timing. |
| Capture/generated/browser sources | Async frames/audio, device lifecycle, settings | Worker source contract, capabilities and negotiated formats. Vendor/library code may be reusable; lifecycle glue changes. |
| Encoders | Raw video/audio or GPU textures, packet/extradata callbacks | Worker media inputs and timestamped encoded packets. Explicit device negotiation; high effort for shared-context encoders. |
| Outputs/recording/virtual cameras | Raw media or encoded packets, start/stop, congestion | Independent consumers with bounded queues and flush/error/status contracts. Multiple destinations reuse one encode. |
| Services | Endpoint/auth, supported codecs/resolutions/bitrates | Credential broker and declarative constraints; validate before starting. No plugin access to unrelated keys. |
| Docks/tools/scripts/integrations | QWidget pointers, frontend events, save hooks, websocket | Host-rendered declarative panels plus actions/events; custom native windows remain separate processes. Existing Qt docks require UI adaptation. |

These categories cover extension surfaces, not a promise to reproduce every OBS
internal hook. Python/Lua support can later be worker adapters to the same contract;
no embedded unrestricted scripting runtime is required for the first SDK.

## Proposed contracts

1. **Registry and lifecycle.** Namespaced type IDs, manifest/schema versions,
   negotiated protocol/features, create/configure/activate/deactivate/destroy,
   async request IDs, cancellation and structured errors. Reject incompatible
   majors before execution. Generation-tagged opaque handles expire on deletion
   or project switch; reject dependency cycles. Never expose C++/Qt pointers.
2. **Document and settings.** Typed defaults, ranges, enums, dynamic choices,
   visibility rules and explicit configuration acknowledgement. All edits pass
   through validated host commands, undo and project revision checks. Preserve
   unavailable plugin settings without executing them. Migrations are versioned,
   transactional and retain the original document on failure.
3. **Control and UI.** Ordered scene/output lifecycle events, subscriptions,
   hotkey/action registration and host-rendered controls/panels. Snapshot plus
   revision permits reconnect after missed events. Host owns GUI thread and
   undo; callbacks never synchronously wait across processes. Countdown needs
   monotonic duration timers, wall-clock date timers and explicit resume policy.
4. **Media.** Separate control messages from bounded shared-memory media rings.
   Negotiate dimensions, pixel/sample formats, stride/planes, color space, alpha,
   sample rate/channel layout, timestamps/time base and discontinuities. Explicit
   buffer leases and release acknowledgements prevent reuse while consumers read.
   No unbounded queue or render/audio-thread wait on plugin RPC. Report queue,
   processing and total added latency separately.
5. **GPU.** Begin with correct CPU transport; benchmark before claiming zero copy.
   Later negotiate dma-buf formats/modifiers, device identity and acquire/release
   fences. RTX/Intel cross-device import may fail: use bounded CPU transfer or
   reject unsupported mode visibly. Never assume another worker shares a Vulkan
   device or graphics context. Shader filters consume host-owned render inputs.
6. **Audio and outputs.** Source identity owns capture, separately from scene
   visibility. Clone taps must declare pre/post-filter audio and routing to avoid
   duplicate mixing. Monitor routing remains a host function (None by default).
   Encoded fan-out preserves PTS/DTS, codec headers, keyframes and stream changes;
   one congested destination cannot stall others. Recording requires explicit
   drain/finalization outcomes rather than silent packet loss.
7. **Failures and permissions.** Per-worker resource/queue limits, heartbeat,
   bounded restart with backoff and visible disable state. Define fallback per
   role: missing source, filter bypass, transition cut or output failure, with
   audio discontinuity handling. Process separation alone is not a sandbox:
   enforce filesystem/network/device permissions before claiming confinement.
   Shader validation does not guarantee GPU memory or execution safety; device
   loss and hostile shaders remain separate recovery/security concerns.
8. **Packaging and author tools.** Manifest declares publisher/type IDs, version,
   compatible API/features, entry point, platform and requested capabilities.
   Validate paths and sizes before extraction, retain previous version for
   rollback, do not hot-upgrade an active output. Secrets stay outside collection
   exports. Publish protocol bindings, property helpers, examples and a host
   conformance runner. Stable contracts precede broad compatibility promises.

Wasm component-model logic, shader effects and native workers retain ADR-0004's
three tiers. Select a Wasm runtime and Slang integration only after small measured
prototypes; neither is currently a product dependency. Native C/C++ bindings can
wrap our wire protocol inside a worker, reducing port boilerplate without adding
an in-process ABI. Do not ship an `obs_*` symbol emulation layer initially.

ADR-0004's blanket licensing conclusion needs qualification: process boundaries
do not automatically resolve derivative-work obligations. Its shader memory-safety
claim is also not a guarantee. These corrections apply to extension planning.

## Delivery order and acceptance gates

1. **Prove control contracts with Countdown.** Implement text updates, actions,
   hotkeys, persistence and declarative panel through the same public-facing
   boundary. Import its separate plugin configuration explicitly; collection JSON
   alone does not contain timer state. Check real timer output, scene changes,
   save/reopen, missing targets, project switches and worker crash recovery.
2. **Prove media ownership with native cloning and stock filters.** Translate
   clone settings (`clone_type`, `canvas`, `clone`, `audio`, `active_clone`,
   `buffer_frame`, `no_filters`) with explicit unsupported-value reports. Do not
   guess absent targets such as Camera.SRC. Check real CPU/GPU pixels, pre/post
   filtering, audio deduplication, cycles and unchanged capture lifetime.
3. **Publish experimental SDK after those two consumers work.** Add discovery,
   manifests, permission enforcement, protocol negotiation, sample worker and
   conformance checks. Test malformed IPC, stale handles, crash/hang, unavailable
   plugin reopen and settings migration. Freeze v1 only after independent ports.
4. **Extend to transitions, audio DSP, encoders, outputs and services.** Exercise
   stingers and one real implementation per contract. Measure baseline versus
   plugin frame time, p95/p99 latency, CPU/GPU use and render drops under load.
   Check two GPUs, no usable GPU, audio underruns and slow destinations. Set
   measured acceptance budgets before declaring a tier production-ready.

Current importer/filter/countdown backlog remains required. This investigation
changes its integration direction; it does not claim those features are done.
No SDK code or new dependencies are introduced by this ADR. Runtime prototypes,
public API details and security enforcement remain implementation work.
