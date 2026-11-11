# Remote control, OBS compatibility, Companion and MCP

Start or restart the newly built `build/linux/apps/munkicast-ui/munkicast-ui`.
Tools > WebSocket Server Settings now places **Mode** directly below Password.
Choose **MunkiCast** for the native Companion module, or **OBS**
for an existing Companion OBS Studio connection. Apply persists the mode and
restarts the listener, disconnecting existing clients. Default: native, port 4455.
Existing settings without a mode migrate to native mode; native also accepts the
OBS transports, preserving existing replay clients.

The native MCP server is separate: **Tools > MCP Server Settings**, default
`http://127.0.0.1:4466/mcp`, with its own token. It works with WebSockets disabled.
See [native MCP](native-mcp.md). Rebuild the directory you launch from; operators
using `./build-make/apps/munkicast-ui/munkicast-ui` must rebuild `build-make`.

The listener binds all IPv4 interfaces, requires a password, limits clients to eight,
limits incoming messages to 1 MiB, and times out unauthenticated connections after
10 seconds. Settings and global persistent data use atomic writes and owner-only
permissions. The password grants full application control and access to files the
application's user can access. Use a trusted control network or authenticated TLS
tunnel; the built-in listener does not provide TLS.

Environment overrides: `MUNKICAST_WEBSOCKET_PASSWORD`, `MUNKICAST_WEBSOCKET_PORT`,
`MUNKICAST_WEBSOCKET_MODE=munkicast|obs`. Overrides affect the running application;
Apply saves the selected values. `websocket.json` lives in the existing application
configuration directory. `websocket-data.json` beside it stores OBS global data.
Profile persistent data belongs to the current collection and is undoable.

## Wire protocol

Both modes use the OBS WebSocket v5 RPC 1 handshake and envelopes. JSON and
MessagePack work, including the default MessagePack transport used by
`obs-websocket-js` and Companion's OBS module. Supported subprotocols:
`obswebsocket.json`, `obswebsocket.msgpack`; native additionally accepts
`munkicastwebsocket.json`. Authentication uses the standard SHA-256 challenge flow
with a fresh salt/challenge per connection and constant-time proof comparison.
The server never logs the password, proof, or request payload.

Identify, Reidentify, Request, RequestResponse, Event, RequestBatch and
RequestBatchResponse are implemented. Subscription masks control event delivery.
Batches support **SerialRealtime**, 1–128 requests, one active batch per connection,
ordered responses, halt-on-failure, and asynchronous `Sleep` with `sleepMillis`
0–10000. SerialFrame/Parallel and `sleepFrames` are unavailable; use SerialRealtime.
Document command batches are atomic; OBS request batches intentionally are not.

Scene/input identifiers are the document's stable ULIDs in the OBS `*Uuid` fields.
Numeric scene item IDs remain stable across edits, reordering and rename for the
life of a listener. Reconnect/discover IDs after changing mode or restarting Studio.
OBS list indices count from bottom to top; MunkiCast tree order is translated.
Use names only when unique. Main is the default program/preview deck; native
methods expose every canvas/deck. Optional OBS canvas routing is implemented for
scene lookup/listing/creation, not every newer OBS canvas-specific request.

OBS mode exposes a numeric compatibility `obsVersion` of `32.0.0` so existing
clients' version parsing works. `platformDescription` explicitly identifies
MunkiCast. This value does not claim the application contains OBS.
`GetVersion.availableRequests` advertises implemented requests only. Requests for
unavailable features return failure status 204, not successful no-ops. Invalid
fields, missing resources, busy outputs and renderer failures return explicit
failure statuses. Partial implementations are listed below and in the inventory.

## Native mode

Native mode includes the supported OBS-shaped operations plus:

| Request | Result |
| --- | --- |
| `MunkiCastGetState` | Revision, Main deck, real stream/record/replay state, output pending state, paths and render statistics. |
| `MunkiCastGetProject` | Project manifest, complete scene documents and assets. Stream credentials are not stored in the document. |
| `MunkiCastGetCapabilities` | Every request's availability/schema plus document methods. |
| `MunkiCastGetMethods` | Exact invokable document signatures, named arguments, return types, including overloads/default-argument forms. |
| `MunkiCastGetCommandTemplates` | Every command variant serialized with its fields. Replace template IDs/values with valid project values. |
| `MunkiCastInvoke` | Invoke an advertised signature with a positional `arguments` array. Only public, typed document invokables are eligible; QObject internals/signals are excluded. |
| `MunkiCastExecuteCommand` | Parse and apply a document command through the existing validated session bus. Atomic batches, journal persistence, undo and failure handling are preserved. |
| `MunkiCastGetScreenshot` | Base64 PNG of a canvas's current program frame for agents to inspect. |

For example:

```json
{"op":6,"d":{"requestId":"rename","requestType":"MunkiCastExecuteCommand","requestData":{"command":{"cmd":"project.set-name","name":"Evening show"}}}}
{"op":6,"d":{"requestId":"undo","requestType":"MunkiCastInvoke","requestData":{"method":"undo()","arguments":[]}}}
```

This exposes the existing document features, including canvases, linked scenes,
groups, source settings, mixer buses/routing, effects, countdowns, encoding graphs,
encoder presets, collections, transitions, transforms and undo/save. Inspect method
signatures and command templates rather than guessing serialized command fields.
Runtime availability of sources/encoders still depends on devices, drivers and
renderer capabilities; remote control does not fabricate hardware support.

OBS mode isolates direct `MunkiCast*` requests. An integration needing native
operations from an OBS connection can use standard `CallVendorRequest` with
`vendorName: "munkicast"`, the native `requestType`, and `requestData`.

## Output and feedback behavior

Start/stop/toggle stream, record and replay requests wait for renderer state,
returning failure on refusal or timeout. Saving replay waits for the actual save
result. A timed-out/disconnected request must be reconciled against status before
retrying; a lost reply is not proof that a mutation failed. Streaming requires the
existing single armed, configured RTMP/RTMPS destination, H.264 video and AAC audio.
Recording/replay require connected encoders; Replay must also be enabled in
Backtrack Properties. No stream keys or credentials are synthesized.

Events include scene create/remove/rename/list, program/preview changes, input
create/remove/rename/settings/mute/volume/monitor changes, filter stack changes,
scene item creation/removal/visibility/lock/order/blend/transform changes, Studio
Mode and transition selection/duration, output state, replay saved, custom events,
and native `MunkiCastStateChanged`. Transform events use the OBS high-volume
SceneItemTransformChanged subscription bit. Native state events use Vendors (512).
The full OBS event catalog, volume meters and transition-frame timing are not
claimed; unimplemented event telemetry is a remaining compatibility gap.

## Compatibility limits and missing product features

This is a substantial compatibility implementation, **not a claim of 100% OBS
parity or a measured 90% of real-world Companion usage**. The request-by-request
inventory below is authoritative. Existing native document operations remain
available through command/method APIs even when an OBS-specific representation
has no equivalent.

| Area | Current behavior / limitation | Completion path |
| --- | --- | --- |
| Source kinds/settings | OBS mode translates common native source kinds, media setting names, browser shutdown, image unload, audio device, text font and packed colors. Plugin-specific OBS schemas are not interchangeable. | Extend explicit translations when a matching native setting exists; add regression fixtures for that source. |
| Audio inputs | OBS input mute/volume/monitor writes apply atomically to all placements. Getters report the first placement. Track routing maps the first six Track buses. Monitor-only is unavailable. | Use native per-node routing for independent scene values. Add per-input pan/sync and monitor-only renderer paths before mapping those OBS operations. |
| Transforms | Basic position/scale/rotation/crop and native layout dimensions are exposed; setters support top-left OBS alignment and no OBS bounds fitting. Native pivots/layouts are not an exact OBS transform model. | Implement explicit OBS anchor/bounds conversion with rendered geometry golden tests. Use native layout controls meanwhile. |
| Transitions | Cut, Fade, Fade to Black and existing stingers; stinger settings map path, millisecond cut point and duration. | Add any missing native slide/transition/audio-fade features before advertising them. Scene-specific transition overrides need a document field and renderer support. |
| Screenshots | PNG/JPEG capture/save for scenes currently on program. Native captures a selected canvas. Quality and 1–4096 pixel dimension validation are supported. | Add offscreen rendering of arbitrary scenes/inputs and full source dimensions. |
| Output settings | Generic output settings expose `url` and `enabled`; setters reject other fields and active outputs. Encoding, bitrate, rescale, routing and credentials use native methods/UI. | Add explicit backend-specific OBS translations as needed. Multiple independent output controls must follow renderer support. |
| Stream service | Reads configured RTMP/RTMPS destination; updates custom server URL. Keys stay in the existing secret-store path. | Complete asynchronous remote secret-store writes before accepting `key`; do not put keys in project JSON. |
| Statistics | Render/output frame counters, FPS, render time, disk space, UI-process CPU/RSS and per-client message counters. Some output status fields are zero/false placeholders because per-output bytes/congestion/reconnection telemetry is absent. | Extend renderer IPC telemetry; do not interpret placeholder zeros as measured network health. |
| Hotkeys | Advertises available native action refs; triggers by name or mapped existing keyboard binding. No OS key injection. | Extend the explicit key ID map for additional keys; configure bindings in Studio. |
| Profiles | Lists a single `MunkiCast` compatibility profile. Collections and encoder presets are native concepts. | Decide whether a separate OBS-style global profile model is wanted before implementing profile CRUD/INI parameters. |
| Media playback | Files render, but runtime seek/pause/restart/cursor/status controls have no renderer IPC interface. | Add media transport commands and playback telemetry, then map the four media requests and Companion actions. |
| Deinterlacing | No native per-input deinterlace mode/field-order controls. | Add decoder/capture processing settings with runtime support checks. |
| Recording pause/split/chapters | No corresponding renderer operations. | Add recorder commands, acknowledgement/state, timestamp continuity and playable-file tests. |
| Virtual camera | Status reports inactive; start/stop/toggle fail explicitly. | Add a runtime-detected virtual camera destination and lifecycle telemetry. |
| Captions | No live caption injection path. | Add caption support to supported streaming transports and an acknowledged renderer control. |
| UI | Existing input filters can be opened; source properties require a loaded Sources panel. Program/Preview have [native GPU projectors](projectors.md); OBS request mapping and source/multiview projectors remain pending. Browser interaction remains unavailable. | Map native Program/Preview projectors; add missing browser interaction and source/multiview features before mapping those requests. |
| Properties buttons | Native source schema lists are queryable; OBS property-button callbacks such as browser refresh have no native command. | Add explicit native refresh/reload actions before mapping property buttons. |
| Persistent data | Global data is private and survives listener restart; profile data is collection-local, journaled and undoable. | An OBS-style independent profile model would change the profile realm mapping. |

## Install Companion module

Requires a Companion installation with the Node 22 module runtime (a compatible
Companion 3.x release or later). Source: `integrations/companion`.

```sh
npm ci --prefix integrations/companion
npm test --prefix integrations/companion
npm run package --prefix integrations/companion
```

Import `integrations/companion/munkicast-1.0.0.tgz` in Companion's module
manager. Add **MunkiCast / Studio**, configure host, port and password. The module uses Companion’s secret field/store for the password. Select
**MunkiCast** mode in Studio. Presets include scenes and output toggles;
actions/feedback/variables cover program, preview, Take, Studio Mode, mute, volume,
streaming, recording and replay. Generic request, serialized command and native
method actions expose the full advertised control surface. See the bundled HELP.

The package preserves GPL-3.0-or-later. Companion's packager requires its explicit
`--ignore-license-rules` override for this repository's license; that flag is in
the package script. The produced archive is intended for local import. Publication
in Bitfocus's upstream module catalog needs a licensing-policy decision with its
maintainers; this work does not relicense project code.

## Independent native MCP server

**Tools > MCP Server Settings** controls an independent, built-in Streamable HTTP
MCP listener (`http://127.0.0.1:4466/mcp` by default), with its own access token and
`mcp.json`. It works with WebSockets disabled. It exposes native document/application
methods as typed tools plus UI inspection, actions, input and screenshots. It is
not limited by OBS/Companion requests. See [native-mcp.md](native-mcp.md) and
[the MCP client guide](../integrations/mcp/README.md). Node is only needed for the
optional stdio compatibility transport and SDK tests. The former 1.x WebSocket
MCP adapter has been replaced; migrate its configuration to native MCP.

## Validation and return checklist

Build dependency added: **msgpack-c**, discovered through pkg-config, alongside
existing libsoup-3.0. On Arch install `msgpack-c`; other distributions commonly
provide `libmsgpack-dev`. No hardware capability is assumed at compile time.

`ctest --preset linux -R replay_self_check` exercises the real GUI/renderer listener:
authentication, both modes, JSON, default OBS MessagePack client, batches, native
commands, typed invocation, undo, scene/placement/audio/filter controls, persistence,
recording/replay playable files, frame capture and the native Companion connection.
`--check --check-mcp` separately verifies the native MCP server with WebSockets off. Node integration portions run when their npm dependencies are
installed. Node schema/action tests are also registered with CTest when dependencies
are present at configure time. `ctest --preset linux` runs the repository suite.

No human intervention is required to build/package/run these local checks. Remaining
external acceptance work:

1. **Actual Companion installation and button bank:** import the archive, connect
   the existing OBS module in OBS mode, try a representative button bank; then use
   the native module in MunkiCast mode. Record Companion version, module
   version, failed request/status and intended behavior. Physical hardware/that
   installation are not present in this workspace.
2. **Real streaming destination:** provide an armed RTMP/RTMPS destination with
   valid stored credentials and a safe test endpoint. Verify start/stop, tally and
   failure recovery end to end. Automated tests deliberately use local files and
   do not start a real broadcast.
3. **Upstream catalog publication (optional):** resolve Bitfocus's module licensing
   policy with maintainers. Local package installation is already available.
4. **Remaining parity work:** use the missing-feature table and request inventory
   below as the implementation backlog. Renderer/document features listed above
   must exist before those OBS requests can honestly succeed. Anchor conversion,
   richer event telemetry and additional settings translations remain compatibility
   work; they are not blocked on user approval and are not represented as complete.

## Request inventory

115 of 147 OBS request names have implementations, including the partial mappings explicitly identified below. Native adds 8 discovery/control requests. These counts are not feature-parity percentages.

Schema/description baseline: [OBS WebSocket generated protocol](https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.json), retrieved 2026-10-28. Machine-readable copy: `apps/munkicast-ui/data/remote-protocol.json`. Runtime discovery comes from the same file.

| Request | Status | Limitation / missing feature |
| --- | --- | --- |
| `GetCanvasList` | Implemented |  |
| `GetPersistentData` | Implemented |  |
| `SetPersistentData` | Implemented |  |
| `GetSceneCollectionList` | Implemented |  |
| `SetCurrentSceneCollection` | Implemented |  |
| `CreateSceneCollection` | Implemented |  |
| `GetProfileList` | Partial | Single compatibility profile, MunkiCast. |
| `SetCurrentProfile` | Unavailable | No OBS-style global profile/INI model. Native collections and encoder presets are available. |
| `CreateProfile` | Unavailable | No OBS-style global profile/INI model. Native collections and encoder presets are available. |
| `RemoveProfile` | Unavailable | No OBS-style global profile/INI model. Native collections and encoder presets are available. |
| `GetProfileParameter` | Unavailable | No OBS-style global profile/INI model. Native collections and encoder presets are available. |
| `SetProfileParameter` | Unavailable | No OBS-style global profile/INI model. Native collections and encoder presets are available. |
| `GetVideoSettings` | Implemented |  |
| `SetVideoSettings` | Implemented |  |
| `GetStreamServiceSettings` | Partial | Configured RTMP destination only; stored keys are never returned. |
| `SetStreamServiceSettings` | Partial | rtmp_custom server URL only; remote key writes are unavailable. |
| `GetRecordDirectory` | Implemented |  |
| `SetRecordDirectory` | Implemented |  |
| `GetSourceFilterKindList` | Implemented |  |
| `GetSourceFilterList` | Implemented |  |
| `GetSourceFilterDefaultSettings` | Implemented |  |
| `CreateSourceFilter` | Implemented |  |
| `RemoveSourceFilter` | Implemented |  |
| `SetSourceFilterName` | Implemented |  |
| `GetSourceFilter` | Implemented |  |
| `SetSourceFilterIndex` | Implemented |  |
| `SetSourceFilterSettings` | Implemented |  |
| `SetSourceFilterEnabled` | Implemented |  |
| `GetVersion` | Implemented |  |
| `GetStats` | Partial | UI-process CPU/RSS; frame statistics from renderer; some output/network metrics unavailable. Session counters are per client. |
| `BroadcastCustomEvent` | Implemented |  |
| `CallVendorRequest` | Partial | munkicast native request tunnel only; OBS plugins are not loaded. |
| `GetHotkeyList` | Implemented |  |
| `TriggerHotkeyByName` | Implemented |  |
| `TriggerHotkeyByKeySequence` | Implemented |  |
| `Sleep` | Partial | SerialRealtime batches only; sleepMillis 0–10000, no frame-based sleep. |
| `GetInputList` | Implemented |  |
| `GetInputKindList` | Implemented |  |
| `GetSpecialInputs` | Implemented |  |
| `CreateInput` | Implemented |  |
| `RemoveInput` | Implemented |  |
| `SetInputName` | Implemented |  |
| `GetInputDefaultSettings` | Implemented |  |
| `GetInputSettings` | Implemented |  |
| `SetInputSettings` | Implemented |  |
| `GetInputMute` | Partial | First placement is reported; writes apply to all placements. |
| `SetInputMute` | Implemented |  |
| `ToggleInputMute` | Implemented |  |
| `GetInputVolume` | Partial | First placement is reported; writes apply to all placements. |
| `SetInputVolume` | Implemented |  |
| `GetInputAudioBalance` | Unavailable | No independent per-input pan control; native bus pan would affect other inputs. |
| `SetInputAudioBalance` | Unavailable | Add per-input pan and renderer routing, then map to OBS balance 0–1. |
| `GetInputAudioSyncOffset` | Unavailable | No per-input audio sync-offset field or renderer control. |
| `SetInputAudioSyncOffset` | Unavailable | Add signed per-input audio delay and renderer support. |
| `GetInputAudioMonitorType` | Implemented |  |
| `SetInputAudioMonitorType` | Partial | NONE and MONITOR_AND_OUTPUT only; no monitor-only path. |
| `GetInputAudioTracks` | Partial | First six existing Track buses; no fixed six-track grid in native model. |
| `SetInputAudioTracks` | Partial | Routes all placements to existing Track buses; cannot create missing tracks. |
| `GetInputDeinterlaceMode` | Unavailable | No native per-input deinterlace/field-order control; add decoder/capture processing and runtime checks. |
| `SetInputDeinterlaceMode` | Unavailable | No native per-input deinterlace/field-order control; add decoder/capture processing and runtime checks. |
| `GetInputDeinterlaceFieldOrder` | Unavailable | No native per-input deinterlace/field-order control; add decoder/capture processing and runtime checks. |
| `SetInputDeinterlaceFieldOrder` | Unavailable | No native per-input deinterlace/field-order control; add decoder/capture processing and runtime checks. |
| `GetInputPropertiesListPropertyItems` | Implemented |  |
| `PressInputPropertiesButton` | Unavailable | No native source property-button command, including browser refresh/reload. |
| `GetMediaInputStatus` | Unavailable | No renderer media transport/cursor/status IPC. Add acknowledged playback controls and telemetry. |
| `SetMediaInputCursor` | Unavailable | No renderer media transport/cursor/status IPC. Add acknowledged playback controls and telemetry. |
| `OffsetMediaInputCursor` | Unavailable | No renderer media transport/cursor/status IPC. Add acknowledged playback controls and telemetry. |
| `TriggerMediaInputAction` | Unavailable | No renderer media transport/cursor/status IPC. Add acknowledged playback controls and telemetry. |
| `GetVirtualCamStatus` | Partial | Reports inactive; no native virtual camera destination exists. |
| `ToggleVirtualCam` | Unavailable | No OBS-compatible mapping implemented. Native MunkiCast controls remain available through MunkiCastGetMethods/MunkiCastInvoke; see the parity report. |
| `StartVirtualCam` | Unavailable | No OBS-compatible mapping implemented. Native MunkiCast controls remain available through MunkiCastGetMethods/MunkiCastInvoke; see the parity report. |
| `StopVirtualCam` | Unavailable | No OBS-compatible mapping implemented. Native MunkiCast controls remain available through MunkiCastGetMethods/MunkiCastInvoke; see the parity report. |
| `GetReplayBufferStatus` | Implemented |  |
| `ToggleReplayBuffer` | Implemented |  |
| `StartReplayBuffer` | Implemented |  |
| `StopReplayBuffer` | Implemented |  |
| `SaveReplayBuffer` | Implemented |  |
| `GetLastReplayBufferReplay` | Implemented |  |
| `GetOutputList` | Implemented |  |
| `GetOutputStatus` | Implemented |  |
| `ToggleOutput` | Implemented |  |
| `StartOutput` | Implemented |  |
| `StopOutput` | Implemented |  |
| `GetOutputSettings` | Implemented |  |
| `SetOutputSettings` | Partial | url and enabled only, outputs stopped. Other settings require native methods. |
| `GetRecordStatus` | Implemented |  |
| `ToggleRecord` | Implemented |  |
| `StartRecord` | Implemented |  |
| `StopRecord` | Implemented |  |
| `ToggleRecordPause` | Unavailable | Recording pause/resume, file split, and chapters are not implemented by the renderer. Add those operations before exposing requests. |
| `PauseRecord` | Unavailable | Recording pause/resume, file split, and chapters are not implemented by the renderer. Add those operations before exposing requests. |
| `ResumeRecord` | Unavailable | Recording pause/resume, file split, and chapters are not implemented by the renderer. Add those operations before exposing requests. |
| `SplitRecordFile` | Unavailable | Recording pause/resume, file split, and chapters are not implemented by the renderer. Add those operations before exposing requests. |
| `CreateRecordChapter` | Unavailable | Recording pause/resume, file split, and chapters are not implemented by the renderer. Add those operations before exposing requests. |
| `GetSceneItemList` | Implemented |  |
| `GetGroupSceneItemList` | Implemented |  |
| `GetSceneItemId` | Implemented |  |
| `GetSceneItemSource` | Implemented |  |
| `CreateSceneItem` | Implemented |  |
| `RemoveSceneItem` | Implemented |  |
| `DuplicateSceneItem` | Implemented |  |
| `GetSceneItemTransform` | Partial | Native layout/pivots are not a complete OBS transform model; basic fields are exposed. |
| `SetSceneItemTransform` | Partial | Basic position/scale/rotation/crop. Only top-left OBS alignment and no OBS bounds fitting; native pivots/layout are not identical. |
| `GetSceneItemEnabled` | Implemented |  |
| `SetSceneItemEnabled` | Implemented |  |
| `GetSceneItemLocked` | Implemented |  |
| `SetSceneItemLocked` | Implemented |  |
| `GetSceneItemIndex` | Implemented |  |
| `SetSceneItemIndex` | Implemented |  |
| `GetSceneItemBlendMode` | Implemented |  |
| `SetSceneItemBlendMode` | Implemented |  |
| `GetSceneList` | Implemented |  |
| `GetGroupList` | Implemented |  |
| `GetCurrentProgramScene` | Implemented |  |
| `SetCurrentProgramScene` | Implemented |  |
| `GetCurrentPreviewScene` | Implemented |  |
| `SetCurrentPreviewScene` | Implemented |  |
| `CreateScene` | Implemented |  |
| `RemoveScene` | Implemented |  |
| `SetSceneName` | Implemented |  |
| `GetSceneSceneTransitionOverride` | Unavailable | No per-scene transition override in the document. |
| `SetSceneSceneTransitionOverride` | Unavailable | Add scene-specific transition metadata and renderer semantics first. |
| `GetSourceActive` | Implemented |  |
| `GetSourceScreenshot` | Partial | Captures scenes currently on program; arbitrary offscreen sources are unavailable. PNG/JPEG, dimensions 1–4096. |
| `SaveSourceScreenshot` | Partial | Captures scenes currently on program; arbitrary offscreen sources are unavailable. PNG/JPEG, dimensions 1–4096. |
| `GetStreamStatus` | Implemented |  |
| `ToggleStream` | Implemented |  |
| `StartStream` | Implemented |  |
| `StopStream` | Implemented |  |
| `SendStreamCaption` | Unavailable | No renderer/stream caption injection path. |
| `GetTransitionKindList` | Implemented |  |
| `GetSceneTransitionList` | Implemented |  |
| `GetCurrentSceneTransition` | Implemented |  |
| `SetCurrentSceneTransition` | Implemented |  |
| `SetCurrentSceneTransitionDuration` | Implemented |  |
| `SetCurrentSceneTransitionSettings` | Partial | Stinger path, transition_point milliseconds, duration and tp_type=0; other transition settings unavailable. |
| `GetCurrentSceneTransitionCursor` | Implemented |  |
| `TriggerStudioModeTransition` | Implemented |  |
| `SetTBarPosition` | Implemented |  |
| `GetStudioModeEnabled` | Implemented |  |
| `SetStudioModeEnabled` | Implemented |  |
| `OpenInputPropertiesDialog` | Partial | Requires a workspace with a loaded Sources panel. |
| `OpenInputFiltersDialog` | Implemented |  |
| `OpenInputInteractDialog` | Unavailable | No native browser interaction window/API. |
| `GetMonitorList` | Implemented |  |
| `OpenVideoMixProjector` | Unavailable | Native Program/Preview GPU projectors exist; OBS request/geometry and multiview mapping remain pending. |
| `OpenSourceProjector` | Unavailable | No native source projector windows/API. |
| `MunkiCastGetState` | Implemented |  |
| `MunkiCastGetProject` | Implemented |  |
| `MunkiCastGetMethods` | Implemented |  |
| `MunkiCastGetCapabilities` | Implemented |  |
| `MunkiCastInvoke` | Implemented |  |
| `MunkiCastExecuteCommand` | Implemented |  |
| `MunkiCastGetScreenshot` | Implemented |  |
| `MunkiCastGetCommandTemplates` | Implemented |  |

## Prior remote-control baseline

- `cmake --preset linux` and `cmake --build --preset linux -j 6`: passed;
  `MUNKICAST_WERROR=ON` in the configured build.
- `ctest --preset linux`: 38 passed, 1 GPU composite test skipped, 0 failures.
- Expanded remote/Companion/MCP checks: passed after the final changes.
- Companion archive: built; relocated package registered through the actual module
  framework IPC entrypoint; actions, feedback, variables and live native connection tested.
- The former MCP 1.x adapter tests have been superseded by the independent native
  MCP checks described in [native-mcp.md](native-mcp.md). Native MCP uses a separate
  HTTP port/token and works with WebSockets disabled. The 2.0.0 package is optional
  stdio compatibility for that server.
- WebSocket settings screenshot rendered and inspected:
  `build/remote/screenshots/websocket-settings.png`.

Ready-to-import archives: `build/remote/munkicast-1.0.1.tgz` and
`build/remote/munkicast-mcp-2.0.0.tgz`. Generated archives/build products are
ignored by git; package sources and lockfiles remain in `integrations/`.
