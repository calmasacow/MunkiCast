<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0017 — The Phase-0 gate passes, and the cost is all in the readback

> **Corrected by [ADR-0018](0018-the-readback-was-my-bug.md).** The `readback 46.746 ms` figure
> below is an artefact of two defects in our own readback path (uncached staging memory,
> per-frame allocation) measured over a sample of two frames. The real cost is **1.870 ms**.
> The conclusion drawn from it here - that the software path cannot sustain 1440p60 and caps near
> 21 fps - is **withdrawn**: it sustains 60 fps with ~10 ms of headroom. The `encode+convert`
> label is also wrong; the timer excluded the colour conversion. Read ADR-0018 before quoting any
> per-stage number from this document.

- **Status:** accepted, measured
- **Date:** 2026-10-12
- **Closes:** Phase 0's go/no-go condition
- **Depends on:** ADR-0006 (encoder ladder), ADR-0011 (capture backend), ADR-0013 (damage-driven capture)

## The gate

Phase 0 required one thing: *a playable H.264 file with synced audio on this machine*, produced by
capture -> colour convert -> encode -> mux. `tools/encode-spike` does that.

```
=== Phase-0 gate ===
  video         : h264_nvenc, 300 frames, 6.87 MiB
  audio         : aac (48000 Hz / 2 ch), 234 packets, 239616 sample-frames fed
  audio span    : 4.992 s of audio for 4.992 s of wall time (+0.0 ms)
  wall          : 4.99 s for 300 frames (60.10 fps)
```

Verified independently of the tool that wrote it, which matters - a spike reporting its own success
is not evidence:

```
0,h264,0.000000,5.021354      <- both streams start at exactly 0
1,aac,0.000000,5.013333
ffmpeg -i gate.mp4 -f null -  <- every frame decodes, no errors
nb_read_frames=300            <- all 300 present
color_range=tv, color_space=bt709
```

A frame extracted from the middle of the file was inspected: the actual desktop, correct colours,
correct geometry, text legible. **Gate passed.**

Note `color_range=tv` and `color_space=bt709` in the output. Per ADR-0014 these are stated by us
and written into the bitstream, not inherited from a libswscale default that varies with
resolution. An unset matrix in H.264 means "assume BT.601", which would be wrong for everything
we produce.

## ADR-0013 was right, and it cost a hung program to re-learn

The first version of the encode loop did the obvious thing: capture a frame, encode it, repeat. It
hung. Not slowly - it produced a 28-byte file and sat in `poll` for three minutes using four
seconds of CPU.

The cause is exactly what ADR-0013 recorded. `ext-image-copy-capture-v1` delivers a frame when the
output is damaged, and a desktop that nobody is touching is not damaged. Over a five-second run on
a still desktop:

```
captures delivered by the compositor: 2
capture   mean 1537.776 ms   p50 3074.179   max 3074.179
duplicated: 299 of 300 output frames had no new capture (99.7%)
```

**Two captures in five seconds**, one of which took over three seconds to arrive. Any design that
derives output cadence from capture arrival produces a file whose frame rate is a measure of how
busy the desktop happened to be, and stops entirely when it is still.

So the structure ADR-0013 prescribed is now built and proven: **capture runs free on its own
thread publishing its newest frame, and the encoder ticks on its own clock**, taking whatever is
current and re-encoding the last frame when nothing changed. That is not a workaround - a viewer
is owed a constant frame rate whether or not the screen moved. The 99.7% duplication figure is
correct behaviour and the tool reports it as such rather than as a fault.

## Where the time actually goes

```
capture          (blocked on damage - measures the desktop, not us)
readback         mean  46.746 ms   max 55.569
encode+convert   mean   0.568 ms   p50 0.568   p99 1.096   max 1.975
```

**The encoder is nearly free and the readback is the entire cost.** NVENC plus a full
BGRA->YUV420P conversion of a 2560x1440 frame costs 0.568 ms against a 16.67 ms budget at 60 fps.
Pulling that same frame out of GPU memory to host costs **46.7 ms** - roughly 2.8x the whole frame
budget, and 80x the encode.

That caps the achievable capture rate at about 21 fps regardless of how fast the encoder is, so
**the software path cannot sustain 1440p60 on a busy desktop.** It passed this gate only because a
still desktop produced two captures in five seconds. This is the bottom rung of ADR-0006's ladder
behaving exactly as a bottom rung should: correct, always available, and too slow for the show.

*Caveat on the number:* with only two captures delivered, `readback` has a sample size of two. The
magnitude is not in doubt - tens of milliseconds, orders above the encode - but the precise figure
needs a busy-desktop run before it is quoted as a measurement.

## Consequence: the Vulkan tier is not an optimisation

ADR-0006 established `h264_vulkan` as the top of the ladder on the grounds that composite and
encode could share one `VkDevice` with no interop in the hot path. This measurement turns that from
a nice property into the load-bearing one: **the readback is the only thing standing between us and
real-time 1440p60, and the Vulkan tier removes it entirely** by handing the encoder the image the
compositor already rendered into. Nothing else on the ladder can.

That is the next piece of work. The ladder shape stays as ADR-0006 specified it, and the tiers
below remain the honest fallback for machines without `VK_KHR_video_encode_queue`, with the
limitation now quantified rather than assumed.

## Consequence: the audio origin is the first encoder tick

Both streams must share one time origin or the file plays with a fixed offset that reads as drift.
The subtlety is *which* instant, and the first attempt got it wrong by one step: audio was drained
at `encoder_open`, but the wait for the first captured frame sits between that and the first tick,
and audio keeps running through it. The result was 51 ms of pre-roll in the file, labelled as
concurrent with t=0:

```
before: audio span 3.051 s of audio for 2.992 s of wall time (+58.2 ms)
after : audio span 4.992 s of audio for 4.992 s of wall time ( +0.0 ms)
```

Audio carries the clock, and its presentation timestamps come from its own sample counter, which is
exact by construction. Video is placed against that same origin. Wall-clock arrival time is used
for neither.

## Structure

`tools/spike-core` now holds the capture and Vulkan-import code shared by both spikes
(`portal`, `stream`, `vk_import`, `wlcap`, `v4l2cap`). It stays spike-local rather than moving into
`libs/`: it is evidence-gathering code, and the shapes it proves graduate into `libs/` once Phase 1
designs around what they taught.
