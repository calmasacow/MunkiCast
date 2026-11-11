<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0013 — Capture is damage-driven; the encoder needs its own clock

- **Status:** accepted, measured
- **Date:** 2026-10-12
- **Closes:** the throughput question left open by ADR-0012

## What was measured, and a correction

A first throughput run reported `mean 9.03 ms (110.8 fps), p50 6.07, p99 36.75, 72/600 frames over
the 60 fps budget`, and that tail was read as a latency problem worth fixing. **That reading was
wrong**, and so was the first fix attempted for it.

Adding a buffer ring (2, 4, 8 slots in place of 1) changed nothing. Re-running on a quieter desktop
made every configuration *worse*, which is the opposite of what a serialization bottleneck does.
The missing instrument was the compositor's own `presentation_time`:

```
  per frame     : mean 26.56 ms (37.7 fps)  min 2.15  p50 30.28  p99 39.64  max 40.33
  presented gap : mean 26.64 ms (37.5 fps)  p50 30.29  p99 39.64
```

Inter-arrival time and the compositor's presented-frame interval agree to within 0.1 ms, across
every ring size and every run. **The capture path adds no measurable latency over presentation.**
What varies is how often the screen changes: ~110 fps with a video playing on a 165 Hz output,
~33 fps on a largely static desktop. The "tail" was the desktop not changing.

The lesson is methodological and general: *inter-arrival time cannot measure a capture path on a
damage-driven protocol.* It measures the content. The instrument must be the producer's own
timestamp, and that instrument should have been in place before any number was reported.

### Confirmed at scale, 2026-10-14

Ten runs of 10,000 captures each on argus, against a *named* output (HDMI-A-1, 2560x1440 @
59.95 Hz) with a deliberate damage source on it — 100,000 frames. Inter-arrival and the
compositor's presented-frame interval agree in every single run:

| | inter-arrival p99 | presented gap p99 | inter-arrival max |
|---|---|---|---|
| runs 01–06 | 23.29–23.68 ms | 23.63–23.91 ms | 39.89–50.15 ms |
| runs 07–10 | 18.69–18.71 ms | 16.85–16.87 ms | 22.02–23.01 ms |

The two groups differ because the damage source changed character part way through the series,
for a reason that was not established — in the first six the *screen itself* was skipping a
refresh at p99, and the capture path followed it to within 0.3 ms. In the last four the screen
held every vsync and the capture path's worst frame in 40,000 was 23.0 ms, about one refresh
interval of slop.

Two things follow. The claim at the top of this section now rests on a hundred times the original
sample. And **the 40–50 ms maxima in the first six runs are the content, not the capture path** —
they are not ADR-0019's stall, which is a *convert*, measured with different instruments
([ADR-0019](0019-obs-dual-gpu-goes-through-system-ram.md)). Reading them as the same fault was
the first thing this ADR warns against.

## Consequence 1: this is correct behaviour, and we must not "fix" it

`ext-image-copy-capture-v1` delivers a frame when the output is damaged. Burning GPU time copying
an unchanged screen 60 times a second would be strictly worse. So the capture source is inherently
variable-rate, and **the output cadence is the encoder's responsibility, not the capture worker's**:

- `munkicast-render` owns one monotonic clock and drives encode at the output's configured rate.
- When no new frame has arrived by the deadline, the compositor pipeline re-presents the last good
  frame. For an unchanged screen the encoder sees an identical frame and emits a cheap P-frame, so
  a static scene costs almost nothing in bitrate - which is the behaviour viewers expect.
- This is the same mechanism already required for a *stalled* worker (ADR-0005's last-good-frame
  rule), so it is one path, not two.

## Consequence 2: capture rate and output rate are independent, and must stay so

The reference setup makes this concrete. The author's primary display runs at **165 Hz**, and when
an Elgato capture card is attached it is fed a **1440p mirror at 119.88 Hz** while the show encodes
at **59.94 fps**. Three different rates in one pipeline, none of them a multiple of the encode rate.

So frame selection is resampling, and it must be explicit:

- Timestamp every captured frame with the compositor's `presentation_time`, not arrival time.
  Arrival time includes our own scheduling jitter and is not a property of the frame.
- The encoder picks, for each output period, the most recent frame whose presentation timestamp
  precedes the deadline. Dropping surplus frames from a 165 Hz source to a 59.94 fps output is
  normal operation, not an error, and must not be logged as frame loss.
- Never derive the output rate from the capture rate. A 165 Hz source and a 59.94 fps output share
  no common beat, so any attempt to lock them produces visible judder.
- **Audio remains the master clock** (unchanged from the plan). Video resamples to it.

## Consequence 3: keep the buffer ring anyway

The ring made no difference here, and the honest reason is that the capture path was never the
bottleneck in this test. It stays, because in the real worker a slot is held while the frame is
composited, scaled, transferred across PCIe and encoded - work this spike does not do. A
single-buffer design would then genuinely serialize the compositor against the encoder. The ring is
retained on that argument, not on a measurement, and that distinction is recorded rather than
blurred.

## Consequence 4: a timeout ends a wait; it is never a way to poll

*Added 2026-10-14, measured on argus under Hyprland.*

When the spikes gained an idle timeout — so that a still screen would be explained rather than
hung on — the first version put it on the capture wait itself: time out, destroy the pending
frame, ask for another. **That invents frames.** Once a pending `ext-image-copy-capture-v1` frame
is destroyed, the compositor answers the next request immediately, with unchanged content. On a
screen genuinely redrawing about every 5 s, a 1 s timeout reported a capture every 1002 ms —
each one the timeout plus 2 ms — and filled the capture timings with the length of the timeout
rather than with anything the screen did.

That is the error at the top of this ADR arriving by a different door: a number that looks like a
capture rate and is really a property of the measuring apparatus. So, for `lcapture`:

- A capture wait either waits or gives up. If it gives up it says so and stops. It does not
  re-request, and a bounded wait must never be used to sample.
- A dry spell is noticed from the **encoder** side, which already owns a clock and the
  last-good-frame rule (Consequence 1). It watches the capture sequence number stop advancing and
  never touches the compositor to do it. `encode-spike` is written that way, and
  `wlcap_capture_slot`'s header says so where someone would reach for the timeout.

## Still open

- The ceiling under sustained synthetic damage is **still** not isolated, even after the
  100,000-frame series above. That series did get a controlled damage source onto a named output,
  which is what this item asked for — but the output refreshes at 59.95 Hz and the capture tracked
  it exactly, so what it measured was the monitor, not a maximum. A ceiling needs a source that can
  outrun the output, which means a high-refresh output or an offscreen one. Until then the honest
  claim remains "adds no measurable latency over presentation", not a fps number.
- Per-frame cost of the cross-GPU transfer at the output cadence (ADR-0012 still open).
- Explicit sync via `linux_drm_syncobj_v1`; the spike still stalls on a fence.
