<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0071 - The HDR colour pipeline

- **Status:** accepted
- **Date:** 2026-11-14
- **Amends:** ADR-0014, whose measurements were taken with HDR disabled on the wire and which
  pins the reference card to `XRGB8888`. Its *findings* stand; its scope is widened below.
- **Depends on:** ADR-0013 (capture is damage-driven), ADR-0042 (executors never interpret
  document nodes)
- **Order of work:** capture, then output, then preview. Each is useful on its own.

## The problem

`ldoc::ColorSpace` has five values and nothing reads them except display code. Underneath, every
stage disagrees about what it is carrying:

| Stage | Today |
|---|---|
| GPU compositor | Works in **RGBA16F** internally, converts every output to 8-bit `R8G8B8A8_UNORM` |
| CPU compositor | 8-bit only. `Picture` is documented as "straight sRGB8, tightly packed" |
| Encoder | Has a `main10` -> `P010LE` path, fed 8-bit RGBA, so main10 means **upconverted SDR** |
| Capture | Maps P010, then **refuses** HDR outright: "HDR or BT.2020 capture is not supported yet" |
| Preview IPC | `PreviewFrame` has no format field; the consumer assumes `Format_RGBA8888` |
| Tone-mapping filters | Work, and say honestly that they map *into* SDR |

So the compositor already has the headroom and throws it away at every exit. The dishonest case is
the encoder: an operator can select main10 today and get a 10-bit stream carrying nothing.

## What this ADR fixes, and what it does not

**Fixes:** HDR *input* is accepted rather than refused, HDR *output* carries real 10-bit, and the
preview says what format it is sending.

**Does not fix:** the compositor's blending maths. It blends in linear light from sRGB
(`srgb_to_linear` and the 256-entry table in `libs/lcomp/src/composite.cpp`), which is correct for
SDR and is *not* PQ or HLG aware. Blending HDR and SDR layers in one scene correctly is a separate
problem and is not solved here.

## Decision

### 1. The working space is scene-linear, and the document says what a canvas means

`Canvas::color_space` becomes load-bearing: it is the **output** transfer function and primaries
for that canvas, not a label. The compositor works in linear light regardless, which it already
does; the canvas's colour space decides what the encode and the preview are asked to produce.

A canvas is SDR (`Srgb`, `Rec709`) or HDR (`Rec2100Pq`, `Rec2100Hlg`). `Rec2020` is primaries
without a transfer function and is treated as SDR with wide primaries.

The field must become settable in the Canvas dialog, which today offers size and FPS only. An
unreadable, unsettable field that changes the picture is worse than no field.

### 2. Conversion happens once, at the edges

Every source is converted **into** the working space on the way in, and the canvas is converted
**out of** it once, per output. No stage converts twice, and nothing in the middle asks what the
original format was. That is ADR-0042's rule applied to colour: an executor receives pixels and a
colour space, never a document.

Concretely: an HDR capture is linearised at capture, an SDR source is linearised with the sRGB
curve it already uses, and the canvas is encoded with its own transfer function at the end.

### 3. Tone-mapping is explicit, never implicit

An HDR source on an SDR canvas is tone-mapped, and an SDR source on an HDR canvas is scaled to a
reference white. Both are stated in the UI as the existing filters already are. What is refused is
*silent* conversion that looks like a colour bug to the operator.

The existing `filter.hdr-tonemap` and `filter.sdr-white` filters stay, and their honest note in
`FiltersDialog.qml` is updated rather than deleted, because it will still be true for an SDR
canvas.

### 4. Capture accepts HDR instead of refusing it

The refusal in `libs/lcapture/src/video.cpp` becomes a conversion. The measured constraint from
ADR-0014 decides what is worth asking the card for, and it is unforgiving on the reference
hardware:

| At | `P010` 4:2:0 10-bit offers |
|---|---|
| 1440p | 60 / 59.94 / 50 |
| 2160p | 30 / 29.97 |

So **10-bit capture and high frame rate are mutually exclusive** on the Elgato 4K X: there is no
1440p120 10-bit path and no 4K60 10-bit path. ADR-0014's rule -- pick the format for the
configured output rate, not the peak -- already covers this, and it means a 10-bit request at
4K60 must be refused with the reason, not quietly served at 30.

ADR-0014's own measurements were taken with HDR disabled on the wire. They remain valid for that
case; what is added is that an HDR signal is no longer rejected outright.

### 5. Output carries 10-bit only when it can

`main10` becomes real: the compositor exports 10-bit, and the encoder is fed P010 rather than
upconverted RGBA. The GPU fast path hard-fails on anything but NV12 today, so it either gains a
P010 path or falls back to the CPU path with that stated.

If a 10-bit encode cannot be satisfied, it fails with the reason. It does not silently produce a
10-bit container full of 8-bit pixels, which is what happens now.

### 6. The preview declares its format

`PreviewFrame` gains a pixel format and a stride rule, and the consumer reads it instead of
assuming `Format_RGBA8888`. The preview may stay SDR -- it is a UI surface on an SDR desktop --
but it must *say* that it is tone-mapped rather than appear to be the output.

This is the smallest of the three and the one an operator notices least, which is why it is last.

## Consequences

- Four libraries change: `lcapture`, `lcomp`/`lrhi`, `lencode`, and the preview protocol in
  `lcomp`. The protocol version bumps when `PreviewFrame` changes.
- Memory and bandwidth roughly double wherever 16F or P010 replaces 8-bit. The GPU compositor
  already pays this internally; the CPU compositor does not, and a 10-bit CPU path is a real cost
  for a fallback that exists for machines without a usable GPU. **A 10-bit CPU compositor is out
  of scope**: an HDR canvas requires the GPU path, stated rather than discovered.
- `lcomp::Picture` is documented as 8-bit sRGB in its header. That comment becomes wrong the
  moment a 10-bit picture exists, so the type carries its format or a second type exists. Deciding
  which is the first piece of work under item 5.
- Every existing project is SDR and stays SDR: the default `Rec709` means today's behaviour, so
  nothing changes for a show that does not ask for HDR.
- No HDR hardware has been tested end to end. Each item below needs one live run, because neither
  ctest nor the self-check certifies a signal on a wire.

## Why not keep refusing HDR

The refusal is honest but it is a dead end: a 4K X fed by a console or a modern PC sees HDR on the
wire as a matter of course, and "unsupported" means the operator's only option is to turn HDR off
at the source. Accepting and tone-mapping is strictly better than refusing, and it is the first
item precisely because it is useful before any of the rest exists.

## Why not one big change

Capture, output and preview are independently useful and independently testable. Accepting HDR
input helps an all-SDR show immediately. Real 10-bit output helps an HDR show with no HDR sources.
Doing them together would mean one change nobody can verify a piece of.
