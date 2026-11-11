<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0020 — A canvas is a pipeline, not a scale factor; canvases must not serialize

- **Status:** accepted
- **Date:** 2026-10-12
- **Refines:** ADR-0019's Phase-1.5 acceptance test
- **Depends on:** ADR-0005 (process isolation), ADR-0007 (dual-GPU concurrent encode)

## The distinction the author drew

> "when I add vertical back into the mix that is a separate canvas as well where the current setup
> is all coming off the main canvas the only difference is one is rescaled before encode"

That is exactly the right distinction and ADR-0019's acceptance test did not make it. Two kinds of
"adding an output" exist and they cost categorically different things:

| | composites | scale + encode |
|---|---|---|
| another output off an existing canvas (1440p YouTube + 1080p Twitch) | **shared, 1x** | per output |
| another canvas (vertical 9:16) | **one more, in full** | per output |

The first is nearly free: one composition is rendered once and sampled at two sizes, and both the
scale and the encode run on dedicated silicon. ADR-0019 measured it as free - a second NVENC
session changed GPU utilisation by -2 points and encode latency not at all.

The second is a genuinely new render of a genuinely different composition. A 9:16 vertical canvas
is not a crop or a rescale of the 16:9 one; it is a different arrangement of sources. **Charging
for a second composite is correct and unavoidable**, and an acceptance test that forbids it would
be testing for something impossible.

## What OBS does, and the failure mode it creates

Read from `libobs`, each canvas is a `struct obs_core_video_mix` carrying its *own*
`copy_surfaces[NUM_TEXTURES][NUM_CHANNELS]`, its own `gpu_conversion`, and - importantly - its own
`raw_was_active` / `gpu_was_active`. So each canvas independently decides whether it needs the
system-RAM readback of ADR-0019. A vertical canvas with one encoder on a sysmem path gets a
readback **of its own**, on top of the main canvas's.

And they are serialized:

```c
static inline void output_frames(void)
{
    for (size_t i = 0; i < obs->video.mixes.num; i++)
        output_frame(obs->video.mixes.array[i]);   // one thread, one after another
}
```

`output_frames()` runs on the single `libobs:graphics` thread, inside the window that
`video_avg_frame_time_ns` measures. Sources are ticked once per frame, outside this loop, and then
*rendered per mix* - which is the right sharing, and not the problem. The problem is the loop
itself: **every canvas's cost lands in every other canvas's frame deadline, and a stall in one
stalls all of them.**

For the author's planned setup that is concrete: a hitch in the vertical canvas - a slow source, a
readback, an encoder hiccup - delays the 1440p YouTube frame, because the main canvas cannot be
presented until the loop comes back around. The vertical stream is the lower-stakes output, and in
this arrangement it holds the higher-stakes one hostage.

This is the same class of fault ADR-0005 exists to prevent, arriving by a route ADR-0005 did not
name: not a crashing plugin, just a slower second canvas on a shared thread.

## Decision

1. **A canvas owns a pipeline.** Composition, conversion and hand-off to its encoders are the
   canvas's own work, and that work is submitted independently of any other canvas. One canvas
   never waits on another's completion to present its own frame.
2. **Shared sources are composited once per canvas but acquired once per frame.** A capture card
   or window is uploaded/imported a single time and sampled by every canvas that references it.
   OBS's tick-once/render-per-mix split is correct and we keep it; it is only the serialized
   *presentation* we reject.
3. **A canvas's cadence is its own.** The vertical canvas may run at a different frame rate from
   the main one, and a late vertical frame is a dropped vertical frame - never a late main frame.
   This follows directly from ADR-0013: the encoder owns its clock, and here there is more than
   one encoder clock.
4. **Per-canvas health is reported separately**, so "vertical is struggling" is a visible,
   attributable state rather than a mysterious rise in one global frame time.

## The Phase-1.5 acceptance test, corrected

ADR-0019 said "starting the second output must not change the first output's frame time." Split
in two:

- **A. Another output from an existing canvas** must not measurably change any other output's
  frame time. Cost is a scale plus an encode, both on silicon built for it. *OBS passes this with
  two NVENC sessions and fails it by 2x when the second encoder forces a sysmem round trip.*
- **B. Another canvas** may cost one additional composite and no more, and **must not enter any
  other canvas's frame deadline.** Measured as: with the vertical canvas deliberately starved, the
  main canvas's frame time and drop count are unchanged. *OBS cannot pass this by construction -
  one loop, one thread.*

Test B is the one that matters for the author's stated plan, and it is a feature the incumbent
cannot offer rather than a bug it happens to have.

## Open

- Whether independent canvas submission is best served by separate threads feeding one Vulkan
  queue, or separate queues, or (at the far end) the separate processes of ADR-0005. Not decided
  here; the requirement is the observable behaviour, not the mechanism.
- The vertical canvas is also the natural first consumer of the cross-GPU path (ADR-0012): it is
  the lower-stakes, lower-resolution output, which is exactly what should be offloaded to the
  iGPU once that path costs the main canvas nothing.
