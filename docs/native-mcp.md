# Native MCP: application control and coverage

The built-in server is `apps/libertas-ui/mcpserver.cpp`. It listens independently
of the WebSocket server and calls the real application objects on the UI thread.
It exposes Streamable HTTP JSON-RPC at `/mcp`, with its own loopback port, Bearer
token, lifecycle and `mcp.json`. **Tools > MCP Server Settings** is a dedicated
panel; it never opens WebSocket settings. No bridge process is required for HTTP
clients. The optional Node package supports stdio-only clients through this native
MCP endpoint.

See [client setup](../integrations/mcp/README.md).

## Coverage contract

Native tools are generated from the running application's public invokable methods,
including default-argument overloads. They are not generated from OBS requests.
Arguments use actual Qt types and named JSON fields. Object arguments accept live
object IDs, checked against the required QObject type. Native QObject base lifecycle
methods such as `deleteLater` are excluded. Document edits go through the same
validated command/session layer used by the UI, preserving undo/redo and journaling.
Document notices and refused boolean mutations become tool errors. A void method
return means invocation completed; use state to verify its effect and wait for
asynchronous renderer/device work.

The generated surface includes:

| Area | Native coverage |
| --- | --- |
| Scenes and canvases | Create, rename, delete, reorder, duplicate, select, canvas sizes/FPS, nesting, collections |
| Sources | Discover types and settings schemas, create/reuse/bind/remove, rename, configure media/device/browser/text/colour properties |
| Scene objects | Position, scale, rotation, opacity, blend, fitting, crop, alignment, visibility/locking, groups, selection, clipboard |
| Filters | Discover, add, configure, enable/disable, reorder, remove |
| Audio | Buses, gain, pan, mute, monitors, sends/routing, node strips, audio encoders |
| Outputs | Video/audio encodes, graph connections/positions, destinations/services, presets, streaming, recording, replay |
| Studio operation | Program/preview, Studio Mode, transitions, stingers, quick transitions, manual transition progress |
| Projectors | Native windowed/fullscreen Program/Preview, monitor enumeration, close and asynchronous display status; see [projectors](projectors.md) |
| Countdown sources | Create, configure, start/stop/reset, reorder, import |
| Application settings | Preferences, cache, shortcuts, telemetry, connector configuration |
| Workspace | Panels, focus, docking/dividers, layouts, workspaces, browser docks |
| Project lifecycle | Collection create/open/rename/remove/import/export, save, command templates, batches, undo/redo |

`studio://coverage` is the authoritative live inventory. The integration check can
write `native-mcp-inventory.json` beside screenshots using `--check-shots DIR`.
New JSON-callable native methods automatically appear, preventing another manually
maintained OBS-shaped API from becoming the feature ceiling.

Menus/dialogs and UI-only operations have a second, real interaction surface:

- Inspect the object/visual tree, including dynamically instantiated controls,
  menu actions, control values, enabled/visible state and logical coordinates.
- Inspect exact action signatures and writable fields. Invoke menu/control actions
  or edit typed fields; disabled actions are refused. Pointer properties cannot be
  reassigned through `ui_write`.
- Send mouse clicks, double clicks, presses/moves/releases for drags, wheel input,
  keyboard shortcuts and input-method text to application windows/controls.
- Capture application windows and dialogs as PNG MCP image content.

IDs are stable while objects live; destroyed objects invalidate their IDs. Inspect
again after opening a menu/dialog or changing a workspace. Prefer native operations
for document edits; use UI interaction for menu/dialog behavior. No JavaScript eval,
shell execution or desktop-wide input tool is exposed.

## Verification

`./build-make/apps/libertas-ui/libertas-ui --check --check-mcp` creates an isolated
project/settings directory and runs the real UI plus the official MCP SDK. It checks:

- Dedicated Tools panel, independent port/token validation, private persisted
  settings, listener stop/restart and occupied-port errors.
- Native editing while the WebSocket listener is completely off.
- Tool/schema discovery; actual scene/source creation and source schema/settings;
  object position/scale/rotation; undo/redo and invalid argument/command handling.
- Filters, audio bus gain/mute, encoder settings, model rows, preferences, workspaces.
- Menu activation, dialog inspection, real mouse/key/text input and PNG capture.
- HTTP authentication, browser-Origin rejection, malformed JSON, unsupported GET;
  resources/prompts and optional stdio transport through the official SDK.

`replay_self_check` separately verifies WebSocket/Companion operation.
`native_mcp_self_check` and the stdio configuration test are registered with CTest
when Node and SDK dependencies are installed. The built-in server itself has no
Node/npm dependency.

## Remaining acceptance and external blockers

The control surface covers the implemented native/UI application; this is not a
claim that every feature, menu sequence and hardware combination has been tested.
Remaining work, with a concrete completion path:

| Item | What is needed | Completion path |
| --- | --- | --- |
| Actual agent-client acceptance | The user's MCP client and its configuration format | Add the HTTP endpoint/token; list tools, create a disposable scene/source, transform it, verify and undo |
| Capture/device features | Relevant cameras, audio devices, Blackmagic hardware and permissions | Discover runtime device choices; configure a disposable source; verify preview, device errors and cleanup |
| Real broadcasts | A safe destination and credentials | Configure encoder/destination through native tools, start, verify runtime output state and endpoint reception, stop |
| Account authorization | User account consent in the external browser/device flow | Start the app connector workflow; user completes provider consent; continue via connector/output tools |
| Desktop portal chooser windows | External portal UI lies outside this process | Pass file/directory paths through native settings/import/export APIs; user selects a file only when a portal interaction is specifically required |
| Every-menu regression matrix | Additional representative UI workflows | Enumerate menus with `ui_tree`, add focused interaction checks for each enabled feature; do not silently mark untested workflows passed |
| Disabled menu placeholders | Underlying application implementation | Implement the product feature first; its native method or UI automatically becomes available to MCP |

No human approval is needed for the local build/tests. Hardware, destination
credentials and account consent are external acceptance requirements, not reasons
to leave unrelated local implementation unfinished.

## Verified build (2026-10-28)

The `build-make` executable advertises **330 tools**: 232 document, 40 workspace,
19 preview, 7 cache, 5 connectors, 5 shortcuts, 4 preferences, 2 telemetry,
2 WebSocket-configuration, 1 theme, 5 application/command, and 8 UI tools. No public
invokable method on the registered native targets is omitted as non-JSON.

Five CTest checks passed: native MCP, replay/Companion, the full shell interaction
check, Companion controls, and stdio configuration. The optional 2.0.0 stdio archive
was installed into a temporary prefix and exercised against the native listener.
The real MCP panel screenshot was rendered and inspected. These results verify the
workflows listed above; external acceptance and exhaustive workflow testing remain
as explicitly listed in the checklist.
