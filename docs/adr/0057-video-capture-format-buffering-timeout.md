# ADR-0057: Video format, buffering and timeout controls

Status: accepted

## Decision

V4L2 settings expose video format, use buffering (default true), autoreset on
timeout (default false), and frames until timeout (default 5, range 2–120).
Formats use stable fourcc strings. Auto preserves the existing preference order;
explicit choices must be supported by the device at the requested resolution.
Device enumeration supplies choices and labels, including MJPEG and SDR P010
when supported by our decoder/converter. YV12 uses swapped chroma planes.

Buffering consumes the device's bounded capture queue in order. Disabling it
drains older queued buffers before conversion to publish the newest available
frame. The renderer additionally uses a bounded four-entry presentation queue
when buffering is enabled, sampled two negotiated frame periods behind scheduled
output time. Sampling never waits for input. Capture audio receives matching delay
without persisting synthetic effects. See [recording cadence](../recording-cadence.md)
for clock limits, explicit audio-device association and hardware acceptance.
Valid monotonic driver timestamp intervals take priority over the advertised frame
period; missing, stale, future or backward timestamps fall back to nominal timing.

Timeout uses negotiated frame periods and monotonic time. Optional reset stops
the stream, requeues mapped buffers and restarts it. Reset errors stop capture
with a diagnostic. This does not implement reconnect after physical unplugging.
Capture stays active off scene by default, as required by ADR-0056.

Settings use the existing source map; old projects retain Auto, buffered capture,
and no automatic reset. Invalid timeouts are refused at UI and capture boundaries.

## Checks

Drive real source settings and renderer commits. Hardware checks cover device
format enumeration, raw and MJPEG modes offered by the attached card, buffering
choices, off-scene lifetime and invalid options. Timeout reset needs a real
V4L2 interruption; do not claim unplug/reconnect validation from a settings test.
