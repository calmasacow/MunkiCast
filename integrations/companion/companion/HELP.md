# Libertas Studio

In Libertas Studio open **Tools > WebSocket Server Settings**. Enable the server,
set a password, select **Libertas Studio**, click Apply. In this connection enter
that computer's host/IP, port (default 4455), and the same password.

Scene buttons take a scene directly to program. Preview and Take use Studio Mode.
Output controls require a configured Encode graph; Replay also must be enabled in
Backtrack Properties. A refused output operation appears in Companion's log.

Actions include program/preview, Take, Studio Mode, input mute/volume,
stream/record/replay start/stop/toggle, Save Replay, any supported API request,
and native document commands/methods. JSON fields support Companion variables.
Use `LibertasGetCapabilities`, `LibertasGetMethods`, and
`LibertasGetCommandTemplates` to discover the current API. Scene and output
presets, active feedback, program/preview variables and output variables are included.

Presets include **Studio > Studio mode toggle** with enabled feedback, plus
**Outputs > stream with timecode** and **record with timecode**. The output presets
toggle their output and show `HH:MM:SS` beneath the label, refreshed every second
from the server. Stopped outputs display `00:00:00`. The original output presets
remain available. Elapsed times are also available as `stream_timecode` and
`record_timecode` variables. Studio Mode actions support On, Off, and Toggle.

For existing OBS buttons, use Companion's OBS Studio connection with the Libertas
server in OBS mode. Unsupported features return explicit errors.

Built for Companion's Node 22 module runtime (Companion 3.x installations providing
that runtime, and later compatible versions). Import the `.tgz` through Companion's
module manager. A configured Companion installation is needed for physical button
and installed-module acceptance testing.
