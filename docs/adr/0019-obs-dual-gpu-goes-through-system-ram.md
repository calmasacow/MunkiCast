<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0019 — OBS's dual-GPU encode round-trips through system RAM, and converts before readback

- **Status:** accepted, observed live
- **Date:** 2026-10-12
- **Confirms:** ADR-0007 (dual-GPU concurrent encode), ADR-0012 (cross-GPU transfer)
- **Improves:** ADR-0018 (the software fallback tier)

## What was observed

Measured on argus while the author was **actually live** - YouTube via NVENC at 1440p, a local
recording, and Twitch via QuickSync at 1080p, the exact dual-GPU configuration ADR-0007 is built
for. The author's own OBS Stats dock, before and after starting the Twitch output:

```
                             stream + recording     + Twitch (QSV)
average time to render frame        3.9 ms               8.0 ms
OBS CPU usage                       6.7 %                12 %
```

**Adding one 1080p output to an idle iGPU doubled the render time of the whole mix.** That is not
what adding a second hardware encoder should cost, and the OBS log says why:

```
NV12 texture support enabled
>>> app not on intel GPU, fall back to old qsv encoder
[qsv encoder: '... Twitch Output Main'] settings:
        width: 1920   height: 1080   target_bitrate: 8000
[qsv encoder: '... Twitch Output Main'] debug info:
        surf:           SysMem
```

`surf: SysMem`. The QuickSync texture path requires the application's render device to *be* the
Intel GPU. OBS composites on the NVIDIA GPU, so there is no shared surface to hand over, and it
falls back to the encoder's raw path. That flips `raw_active` true in `obs-video.c`, which
re-enables `download_frame()` - so the **entire mix is now read back to system RAM every frame**,
purely so the frame can be DMA'd straight back out to the iGPU.

The frame crosses PCIe twice to reach a device that was already on the bus. This is precisely the
problem ADR-0012 set out to measure, and OBS's answer to it is a full host round trip.

## The A/B, run live

The author then moved the Twitch output from QuickSync to a second NVENC session, which keeps it
on the texture path, and the readback disappeared:

```
                           Twitch on QSV      Twitch on NVENC
NVENC sessions                   1                  2
libobs:graphics thread        34.1 %             24.4 %
OBS process total            191 %              162.8 %
NVIDIA GPU utilisation        80 %               78 %
```

The graphics-thread drop is the signature of `download_frame()` no longer running: the second
output stopped charging the first one for its existence. (`top` samples over two seconds, so the
direction is solid and the last digit is not.)

The GPU row is the point worth keeping. **Adding an entire second 1080p60 encode changed NVIDIA
GPU utilisation by nothing** - because NVENC is dedicated silicon, not shader cores. A second
encode genuinely is close to free when it is not made to travel through system RAM to get there.
The 2x render-time penalty was never the cost of encoding; it was the cost of the detour.

The graphics-thread figure reproduces across two separate streaming sessions an hour apart:
24.4% both times, with the OBS process at 156.3 / 164.4 / 161.8 / 162.8%. Both destinations were
confirmed to be genuinely delivering, not merely connected - socket byte counters over 10 s give
**Twitch 8094 kbps** (against its configured 8000) and **YouTube 41110 kbps**.

**On NVENC contention, an earlier claim in this ADR was overstated and is withdrawn.** It read:
"three samples say there is none." Five further samples give latencies of 10503 / 11253 / 11089 /
7478 / 7450 us, against 7971 us for the single session - a range of 7.4-11.3 us-thousands and a
mean above the single-session figure. The honest statement is that NVENC latency is noisy at this
granularity and **a modest cost to the second session cannot be ruled out**; what can be said is
that it is nowhere near the cost of the sysmem detour, and that it did not move frame delivery
(59-60 fps throughout, 2 lagged frames in 21236 on the Twitch output's own stop record).

This is the second time in one session that a conclusion was drawn from a handful of samples -
ADR-0018 was written about exactly that error and then it was repeated here within the hour. The
rule needs to bind when the number is *convenient*, which is when it does not feel like a risk.

This is the Phase-1.5 bar, now with both a failing and a passing example measured on the same
machine on the same evening.

**This is not an argument that NVENC beats QuickSync, nor that one GPU beats two.** The author's
original reasoning - offload the secondary stream onto otherwise idle Intel silicon - is sound,
and remains the configuration this project intends to make *correct* rather than to replace. What
the A/B shows is that the incumbent cannot route a frame to the second GPU without a host round
trip, so on OBS today the two-chip setup is the slower one. Fixing that is ADR-0007's whole
purpose; when it works, spreading the load across two engines is better again, and matters more
on a machine with less NVENC headroom than this one.

## Why this matters more than any other finding so far

The author's stated reason for this project is years of trouble with OBS on Linux, and their
daily configuration is the one that triggers this. Two of our existing decisions are now validated
by a live measurement rather than by argument:

- **ADR-0007** (dual-GPU concurrent encode as a first-class feature, each VAAPI encoder in its own
  process) is not a nice-to-have; the incumbent degrades the *primary* output to add a secondary
  one.
- **ADR-0012** (the shared buffer is a dmabuf allocated `HOST_VISIBLE` and **not** `DEVICE_LOCAL`,
  scaled on the source GPU before transfer) is the fix. We allocate the transfer buffer ourselves
  and hand the Intel device a dmabuf it can import directly. Nothing is copied into a generic
  sysmem surface pool, no CPU reads a pixel, and scaling to 1080p before the transfer cuts the
  bytes by 44% (ADR-0012). Our second encode should cost the first encode **nothing**.

That last sentence is the promise this ADR exists to pin down, and it is the one to test at
Phase 1.5: *starting the second output must not change the first output's frame time.* OBS fails
that test by 2x. It is a concrete, measurable, user-visible bar.

## Second finding: OBS converts on the GPU *before* staging, and we do not

From `stage_output_texture` in `obs-video.c`, when `gpu_conversion` is set (the default), OBS
stages `convert_textures[i]` - the **already-converted NV12 planes** - rather than the RGBA output
texture. So even on its slow path OBS:

- reads back **1.5 bytes/pixel** (NV12), not 4 (BGRA): 5.3 MiB at 1440p rather than 14.1 MiB;
- pays **nothing** on the CPU for the colour conversion, because a shader did it.

Our `encode-spike` does both the other way round: it reads back BGRA and converts with `sws_scale`
on the CPU. ADR-0018's corrected measurements say exactly what that costs:

```
readback      1.870 ms     (14.06 MiB of BGRA)
bgra->nv12    4.351 ms     (on the CPU)
encode        0.500 ms
              --------
              6.721 ms
```

Converting in a shader before readback should remove the 4.351 ms outright and cut the readback by
2.7x, to roughly **1.2 ms total**. That is a ~5x improvement to the fallback tier using only
machinery we already have, and it should land before the Vulkan tier, not after it - it is
cheaper to build and it benefits every machine that can never reach the Vulkan tier at all.

ADR-0018 called the GPU colour conversion "the largest single software cost"; this is the evidence
for the order it should be done in.

### Built and measured

Done, in commit `87a1735`. Measured on DP-1 under sustained damage (`vkcube` on a 165 Hz output,
**1623 and 1602 captures** - not the handful of samples this document has twice had to apologise
for), both runs 600 frames at 60 fps:

```
                     CPU path (n=1602)   GPU path (n=1623)
readback / convert        1.704 ms            0.771 ms
colour convert (CPU)      4.779 ms              -
NV12 plane copy             -                 0.380 ms
encode                    0.514 ms            0.429 ms
                          --------            --------
total per frame           6.997 ms            1.580 ms
PCIe per frame           14.06 MiB            5.27 MiB
```

**4.4x faster and 2.67x less bus traffic**, taking the pipeline from 42% of a 60 fps frame budget
to 9.5%. The prediction above was ~1.2 ms; the extra 0.38 ms is the copy into FFmpeg's own frame
buffer, which the prediction did not account for and which a future zero-copy submission removes.

Correctness was established before the timings were believed. Against the swscale path on an
identical still frame: `YAVG 66.2078` vs `66.217`, `UAVG 132.215` vs `132.218`, `VAVG 125.964` vs
`125.968` - agreement to within rounding - and the decoded frame inspected visually. Under the
busy run the chroma still agrees to 0.002 while luma differs by 0.3, which is the cube being at a
different angle, not the conversion.

### The outlier, chased

The 61 ms sample above was worth chasing, so the convert was instrumented to time its stages
separately - *a stall that cannot be attributed cannot be fixed*. It recurred once on a 60 s run
over real video:

```
worst convert : frame 90, 40.475 ms = record 39.375 + gpu-wait 0.003 + copy 1.096
tail          : 1 over 5 ms, 1 over the 16.667 ms budget, of 2495
```

**The GPU took 0.003 ms.** The stall is entirely in building and submitting the command buffer -
host-side driver work, not the compute shader, not the transfer, not the fence.

It then refused to reproduce: three further runs over the same content gave **0 stalls in 5366
captures**, max 2.8 / 2.5 / 3.0 ms. A hypothesis that audio capture was contending was tested
directly - the clean run was `--no-audio` and both stalls had audio - and **rejected**, because
two of the three clean runs had audio enabled.

So the honest state is: ~2 events in ~8000 captures, located in submission, not reproducible on
demand, cause not established. Both occurrences followed a period of GPU idleness while the later
clean runs were back-to-back, which makes a driver or power-state transition the leading suspect,
but that is a hypothesis and it is not tested here.

What can be said is that the architecture already tolerates it: ADR-0013 gives the encoder its own
clock and the last-good-frame rule, so a 40 ms convert costs **one duplicated frame, not a dropped
stream**. That is the difference between a hitch and a failure, and it is by design rather than by
luck. It stays on the list for Phase 1.5 and will likely be closed by pre-recorded command buffers
and explicit sync, both of which remove the per-frame submission work entirely.

#### Looked for properly, 2026-10-14, and not found

The run above was ~8000 captures. This one was built for the question: ten runs of 6000 frames
through `encode-spike` on argus, audio on, with the GPU deliberately left idle for 90 s between
runs — the condition this ADR names as the leading suspect, given ten chances. **59,940 timed
converts.** At the rate recorded above that predicts about fifteen events.

```
worst convert in the whole series : 5.721 ms = record 0.463 + submit 1.173 + gpu-wait 1.199 + copy 2.886
second worst                      : 5.019 ms = record 0.289 + submit 0.156 + gpu-wait 1.663 + copy 2.911
over the 16.667 ms budget         : 0 of 59,940, in every one of the ten runs
mean 2.18 ms, p99 2.9 ms, run to run identical to within 0.01 ms
```

The `record` term — the 39.375 ms that *was* the stall — peaked at **0.463 ms**. The first run
after each idle period looks like every other run.

So the honest state has moved: from "~2 events in ~8000 captures, cause not established" to **not
observed in 59,940, under the conditions that produced it**. That is a negative, not a fix.
Nothing was changed to make it go away, so the most likely explanations are a driver or kernel
update between then and now, or a condition in the original runs that has not been identified —
and either way the cause is still not established. What is now unsupported is using this stall as
the argument for explicit sync or pre-recorded command buffers; those have to be justified on
their own merits.

Two methodological notes, both mistakes made on the way here. The first attempt at this
measurement ran `capture-spike`, which has no convert in it at all, and so could not have seen
the fault whatever it printed. And `capture-spike`'s inter-arrival maxima of 40–50 ms on a
damage-driven output look exactly like this stall and are not it; see ADR-0013.

### Where the time actually goes

The same instrumentation answered a question nobody had asked. In normal operation:

```
record 0.054 + submit 0.023 + gpu-wait 1.567 + copy 1.161
```

The GPU is not the cost, and neither is recording. **It is the copy** - and counting them properly
found **four**, not two:

1. staging buffer -> `Nv12Frame::data`, inside `vk_convert_nv12`;
2. `local_nv12` -> `latest.nv12`, handing the frame to the encoder thread;
3. `latest.nv12` -> the encoder thread's `nv12`, taking it;
4. `nv12.data` -> the AVFrame's planes.

Copies 2 and 3 are pure handoff between two threads and buy nothing: a `std::swap` exchanges the
buffers in O(1) and lets them rotate instead of being duplicated. Done, and the frames stay intact
(1800 frames, clean decode, smooth luma continuity across duplicates - a torn frame would show
there first).

**But the stage timings did not move, and that is the finding.** Copies 2 and 3 happened *outside*
every timer: copy 2 after the `convert+readback` clock stopped, copy 3 before the `encode` clock
started. They were real work - 2 x 5.27 MiB per new frame on the GPU path, 2 x 14.06 MiB on the
CPU path - and **no number in this document ever contained them.** At the 162 captures/s of the
vkcube run that is 1.71 GiB/s of memory traffic that was invisible.

This is the third time in one day that the expensive thing was outside the instrumentation:
swscale sat outside the `encode+convert` timer (ADR-0018), the readback's real cost was hidden
behind a two-sample mean (ADR-0018), and now two full-frame copies sat between the timers
entirely. The per-stage totals quoted above are therefore **lower bounds on both paths**, not
complete accounts. A timer that brackets a named stage does not measure a pipeline; only wall
clock over the whole loop does, and that is the instrument this spike should have had first.

### Confirmed on real content

Repeated against a fullscreen film on the same output, which is harder to compress and harder to
convert correctly than a desktop or a spinning cube:

```
                      CPU path (n=1141)   GPU path (n=2495)
readback / convert         2.214 ms            1.017 ms
colour convert (CPU)       4.968 ms              -
NV12 plane copy              -                 0.425 ms
encode                     0.503 ms            0.459 ms
                           --------            --------
total per frame            7.685 ms            1.901 ms
```

**4.0x**, consistent with the 4.4x on synthetic damage. The 60 s output is intact: 3600 frames,
both streams starting at 0.000000, audio span -9.7 ms over a minute, zero decode errors, 90.5 MiB.
An extracted frame shows correct **skin tones**, which is the test that matters here - a BT.601
matrix applied to BT.709 content shows up on faces before it shows up anywhere else.

## Third observation: media sources decode on the CPU

The per-thread breakdown of the live OBS process (191% CPU total) shows the cost is not in one
place:

```
libobs:graphics   34%     compositing + the readback above
PipeWire          29%     audio
mp_media x6     5-10% ea  video file sources, decoding on the CPU
v4l2: capture   5-10%     the Elgato
cuda-Ev...         5%     NVENC submission
```

The author's heaviest scene holds six-plus looping video files (`ffmpeg_source`), each decoding on
the CPU. Hardware decode for media sources is therefore a real differentiator and not a micro
optimisation - on this scene it is a larger CPU cost than compositing. Recorded here as an input
to Phase 2, not a decision.

## Consequences

- Phase 1.5 gains an explicit acceptance test: **enabling the second encode must not measurably
  change the first encode's frame time.** Numbers above are the incumbent's score on that test.
- The fallback tier's colour conversion moves to a shader, scheduled **before** the Vulkan tier.
- `vk_create_exportable`'s `host_memory` path (ADR-0012) is the mechanism for the hand-off and is
  already proven; what remains open there is a P010/NV12 transfer buffer rather than BGRA, which
  this ADR makes the priority question.
- Hardware decode for media sources is logged as a Phase-2 candidate with a measured reason.
