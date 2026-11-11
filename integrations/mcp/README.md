# Native MunkiCast MCP

MunkiCast contains its own MCP server. It calls the native application directly;
it does not use the WebSocket listener, OBS protocol or Companion module. No Node.js
process is needed for clients supporting Streamable HTTP.

Open **Tools > MCP Server Settings**. Enable the server, choose a port (default
4466), use the generated access token, click Apply. Use **Show token** to copy it.
The endpoint defaults to `http://127.0.0.1:4466/mcp`. Configure your MCP client's HTTP
connection with `Authorization: Bearer YOUR_TOKEN`. For clients using this format:

```json
{
  "mcpServers": {
    "munkicast": {
      "url": "http://127.0.0.1:4466/mcp",
      "headers": { "Authorization": "Bearer YOUR_TOKEN" }
    }
  }
}
```

This is a stateless Streamable HTTP MCP endpoint, using JSON responses. Protocol
versions 2025-11-25, 2025-06-18 and 2025-03-26 are supported. Server capabilities are
tools, resources and prompts. GET/SSE and resource subscriptions are not advertised.
The listener binds only IPv4 loopback, rejects browser Origins and authenticates
every request. Settings live in `mcp.json` with owner-only permissions, separately
from `websocket.json`. Disabling stops this listener; Companion keeps operating.

## Application controls

Tool discovery reads the real application metaobjects. Every public invokable whose
arguments/results can be represented as JSON becomes an individual typed MCP tool.
Default-argument overloads appear as separate tools. No OBS request inventory limits
the native tool set. The live `studio://coverage` resource lists tools, non-JSON
methods and actual limitations.

- `document_*`: scenes, sources and their settings schemas, canvas creation, source
  placement, transforms, cropping, groups, filters, transitions, countdowns, buses,
  audio routing, encode/output graphs, collections, import/export and undo/redo.
- `workspace_*`: panels, docking, workspaces, browser docks and layout persistence.
- `preview_*`: renderer statistics, streaming, recording, replay and manual transitions.
- `preferences_*`, `shortcuts_*`, `cache_*`, `connectors_*`, `theme_*`, `telemetry_*`:
  the same application controls these panels use.
- `websocket_*`: configure the separate WebSocket feature without using it as MCP's transport.
- `app_project`, `app_command`, `app_command_templates`: full native documents and
  validated, undoable document commands, including batches.
- `app_properties`, `app_model`: selection, settings, native models and named roles.
- `ui_tree`, `ui_inspect`: windows, menus, dynamically instantiated dialogs, controls,
  properties, writable fields and exact action/method signatures.
- `ui_invoke`, `ui_write`: menu/control actions and typed property edits. Inspect the
  control first; setting a value alone does not emit its commit/click signal.
- `ui_pointer`, `ui_key`, `ui_text`: actual mouse gestures, keyboard shortcuts and text
  input within this application's windows. UI gestures use the same event handlers
  as human input, including controls without a dedicated data tool.
- `ui_screenshot`: application-window capture as MCP PNG image content.

Read `studio://application`, `studio://project` and `studio://coverage` first. Use
stable IDs from discovery. Prefer native document tools for edits; use UI controls
for menus/dialogs/gestures. Fetch the source's settings schema before changing its
settings. Check state after operations: recording/device/encoder work is asynchronous.
Output commands start actual outputs; follow the user's authorization. Password/token
properties are omitted from ordinary property inspection.

## Optional stdio compatibility

Only clients requiring stdio need Node.js 22.18+ and this package:

```sh
npm ci --prefix integrations/mcp
```

```json
{
  "mcpServers": {
    "munkicast": {
      "command": "node",
      "args": ["/absolute/path/MunkiCast/integrations/mcp/main.js"],
      "env": {
        "MUNKICAST_MCP_URL": "http://127.0.0.1:4466/mcp",
        "MUNKICAST_MCP_TOKEN_FILE": "/absolute/path/private/munkicast-mcp-token"
      }
    }
  }
}
```

The token file contains the token, optionally followed by a newline; use mode 600.
`MUNKICAST_MCP_TOKEN` is also supported. The stdio process forwards to the native MCP
server using the official SDK. It has no WebSocket settings or password. Version 2
replaces the old 1.x WebSocket adapter; old adapter configuration is not compatible.

## Validation and remaining external work

Run `./build-make/apps/munkicast-ui/munkicast-ui --check --check-mcp` with dependencies
installed for SDK integration checks. `npm test --prefix integrations/mcp` checks stdio
configuration validation. `npm pack` creates the optional stdio package.

Native tool discovery and UI access cover the implemented application surface;
this is not a claim that every feature/device combination has been acceptance-tested.
External desktop portal windows and OAuth browser windows are outside this process.
File paths and colour values can be passed through native APIs instead of pickers.
Account consent, real capture hardware and real broadcast destinations need the
corresponding environment/credentials. Disabled placeholders in the app remain
unimplemented; MCP does not create those features. See `docs/native-mcp.md` for the
coverage and completion checklist.
