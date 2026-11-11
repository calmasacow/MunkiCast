<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0018 — The 46 ms readback was our bug, not a PCIe limit

- **Status:** accepted, measured
- **Date:** 2026-10-12
- **Corrects:** ADR-0017, which reported a readback cost that was an artefact of two defects in
  our own code and drew a conclusion from it that was wrong.

## How this was caught

ADR-0017 reported `readback mean 46.746 ms` against `encode 0.568 ms` and concluded that the
software path "cannot sustain 1440p60 on a busy desktop", capping capture near 21 fps. The author
replied that their OBS frame times are **4–6 ms**.

That single number was enough to overturn the finding, and the arithmetic should have raised the
alarm before anyone had to say it. A 2560x1440 BGRA frame is 14.06 MiB. At 46.746 ms that is
**315 MB/s**. PCIe 4.0 x16 is roughly 25 GB/s. The measurement was **two orders of magnitude
below the bus** - far too slow to be a transfer, and therefore not a measurement of one.

The lesson is the same one ADR-0013 taught and is worth stating as a rule: **before a measurement
is attributed to hardware, divide it into the known limit of that hardware.** A number that
implies 1/60th of the available bandwidth is describing our code.

## Defect 1: the staging memory was uncached

`readback_image` chose its memory type by scanning for the first match of
`HOST_VISIBLE | HOST_COHERENT` and taking it. On the RTX 5080 that is type 2. Type 3 is the same
thing plus `HOST_CACHED`:

```
memoryTypes[2]: HOST_VISIBLE | HOST_COHERENT                 <- what we picked
memoryTypes[3]: HOST_VISIBLE | HOST_COHERENT | HOST_CACHED   <- what a readback needs
```

Host-visible memory without `HOST_CACHED` is write-combined. WC is built for streaming *writes* to
the GPU: writes coalesce in the fill buffers and go out in bursts. **Reads from it bypass the
cache entirely** - no prefetch, no line reuse - so a `memcpy` *out* of WC memory runs at a few
hundred MB/s. A readback destination is read, so this is precisely the wrong memory, and nothing
reports an error: the allocation succeeds, the pixels are correct, and the only symptom is the
clock.

Memory type selection is now an explicit preference order, cached first, and `DEVICE_LOCAL` is
excluded from the first two passes - on a discrete card a type that is both device-local and
host-visible is resizable-BAR VRAM, which is the same mistake wearing a different hat.

## Defect 2: every frame allocated and freed its own staging

Buffer, 14 MiB `vkAllocateMemory`, command buffer and fence were created and destroyed per call,
with a `vkMapMemory`/`vkUnmapMemory` around each copy. The allocation alone means first-touch page
faults on 3600 pages every frame. These now live in `VkCtx` and the mapping is permanent.

## Result

Re-measured on a deliberately busy output (`vkcube` on DP-2, 867 captures in 10 s - against
ADR-0017's sample of **two**, which was the other thing wrong with that number):

```
                     before (n=2)      after (n=867)
readback             46.746 ms         1.870 ms   p50 1.584  p99 3.464  max 7.143
```

**25x.** 14.06 MiB in 1.870 ms is 7.9 GB/s, which is what a PCIe 4.0 x16 readback should look
like. The spike now prints the memory type it chose, so this cannot silently come back:

```
readback staging: memory type 3 (HOST_CACHED, coherent), 14.06 MiB resident
```

## Defect 3: a stage was named in a label but not in the timer

ADR-0017 quoted `encode+convert mean 0.568 ms`. The timer started *after* `sws_scale`. The colour
conversion - the more expensive half of that label - was never measured. It is now timed
separately, and it is the largest of the three:

```
readback     mean 1.870 ms
bgra->nv12   mean 4.351 ms     <- was inside a label, outside the clock
encode       mean 0.500 ms
```

A stage that is not measured must not appear in a label. The 0.568 ms figure was not wrong about
the encoder; it was wrong about what it covered.

## The corrected conclusion

Per-frame pipeline cost is **6.72 ms** against a 16.67 ms budget at 60 fps. The software path
**does** sustain 1440p60 on a busy desktop, with roughly 10 ms of headroom. ADR-0017's "cannot
sustain, caps near 21 fps" is withdrawn.

The run confirms it end to end rather than by arithmetic: 600 frames, 60.04 fps wall, both streams
starting at 0.000000, audio span +1.1 ms over 10 s, zero decode errors, `color_range=tv`,
`color_space=bt709`, and an extracted frame that shows the real desktop.

**The Vulkan tier remains the right next step, but its justification changes.** It is no longer a
rescue for a path that cannot hold the rate; it is an optimisation that removes 6.2 of the 6.7 ms
and buys the headroom needed for 4K, for the dual-GPU second encode (ADR-0007), and for a machine
slower than this one. That is a real justification. It is not the one ADR-0017 gave, and the
difference matters: a project that believes its bottom tier is broken designs differently from one
that knows it works.

## Why OBS reports 4–6 ms, and what it actually measures

The author's figure is from the OBS Stats dock, **"Average time to render frame"**, confirmed on
argus mid-stream twice: **4.2 ms** (59.94 fps, 3 missed in 13369, 0.0%), and **3.9 ms on their
heaviest scene with stream and recording both running at 39 Mb/s**, at **6.7% CPU**.

That the heaviest scene is the *cheaper* of the two readings is the whole point: scene complexity
barely moves this number, because what it measures is GPU compositing, and compositing is not
where the cost is. The 6.7% CPU is corroborating evidence on its own - pulling 1440p60 to system
memory and converting it to NV12 on the CPU for two outputs could not possibly cost 6.7% of a
9900K. The work the number would have to contain is visibly absent from the machine.

Read from the OBS source (the reference oracle of ADR-0001), that number is
`video_avg_frame_time_ns`, which wraps `tick_sources` + `output_frames` + `render_displays` in
`obs-video.c`. Two properties make it not comparable to our first number:

1. **It usually contains no readback at all.** `output_frame()` guards the download with
   `if (raw_active)` - true only when a *raw* consumer is attached (x264, a raw recording, virtual
   camera, NDI). Both of the author's encoders are texture encoders: on Linux OBS 33 has
   `plugins/obs-nvenc/nvenc-opengl.c` + `nvenc-cuda.c`, and `obs-ffmpeg-vaapi.c` declares
   `OBS_ENCODER_CAP_PASS_TEXTURE`. With NVENC to YouTube and VAAPI to Twitch, **OBS never pulls
   the frame to system memory.** It is measuring compositing, not our pipeline.
2. **When it does read back, it is pipelined.** `NUM_TEXTURES` is 2; `stage_output_texture` stages
   into `cur_texture` while `download_frame` maps `prev_texture` - the surface staged a full frame
   earlier. The DMA has had ~16.67 ms to finish, so the map never blocks, and the stage surfaces
   are persistent. Both of our defects are things OBS does not do.

So the right comparison is not 4.2 ms against 46.7 ms; it is 4.2 ms against our **6.72 ms**, and
ours still carries a CPU colour conversion that OBS does on the GPU. That is the correct shape for
a software fallback tier, and it is the shape the numbers now show.

## Consequences

- Never quote a per-stage cost from a sample small enough to name. ADR-0017's n=2 is on the record
  as a caveat in that ADR; the caveat was not strong enough, because the number was quoted anyway
  and a design conclusion was built on it.
- A damage source on a *named* output is now a solved problem for benchmarking, which closes an
  item ADR-0013 left open - `vkcube` on the captured output took deliveries from 2 per 5 s to 867
  per 10 s. **It puts a window on the author's screen, so it is asked for before it is run.**
- The GPU colour conversion is now the largest single software cost and should land with the
  Vulkan tier, not after it.
