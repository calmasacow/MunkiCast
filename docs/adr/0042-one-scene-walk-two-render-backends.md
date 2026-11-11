<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0042 — One scene walk, two render backends

- **Status:** accepted
- **Date:** 2026-10-18
- **Depends on:** ADR-0005, ADR-0006, ADR-0011, ADR-0019, ADR-0040, ADR-0041

## Context

The CPU preview is an adequate fallback. The next path must keep captured pixels on the GPU,
without creating a second interpretation of the scene. Before changes, the existing 24 tests
pass. A driver of the real `composite_canvas` with a 4K BGRA screen measures 8.7 ms at 1080p and
36.5 ms at 4K, including allocation, over 30 frames. This is not the earlier live capture
measurement and does not replace it.

## Decision

### Shared walk

`lcomp` resolves a document into an ordered draw plan. It alone decides visibility, ordering,
placement, opacity, quarter turns, flips, source availability, gaps and cycles. Each operation
is a colour, decoded image, captured image or nested plan. A precomp has its own transparent
target; the outer canvas starts opaque black. Both backends consume this plan. Neither backend
looks at document nodes. Existing group and crop semantics stay unchanged by this extraction.

The CPU executor preserves the existing sRGB8 intermediate arithmetic. The GPU executor in
`lrhi` uses scene-linear RGBA16F, with straight source alpha converted to premultiplied alpha
while blending. The preview converts back to straight sRGB8. Half-float and byte intermediate
rounding can differ slightly; geometry, gaps and ordering must agree exactly. Pixel checks
cover transparent precomps, overlapping opacity, rotations and flips with a small stated
numerical tolerance. Unsupported features remain gaps in both backends.

### Capture ownership

Capture publishes a lease on a completed dmabuf slot. The slot cannot be captured into again
until consumers release that lease. The renderer holds one snapshot per connector through all
placements and canvases in a commit. A bounded dma-buf reader wait respects the producer's
implicit writer fence. GPU work acquires foreign ownership before reading and
releases it after submission completes. CPU fallback reads that same leased frame back.
No capture thread submits Vulkan work concurrently with the compositor's queue or command pool.
Capture waits remain damage-driven; GPU waits are bounded. Recoverable GPU failures demote the
renderer and report the error. A fence timeout terminates the render process: potentially live
resources cannot be reused safely. Neither failure presents unfinished GPU output as valid.

### Canvas and preview sizes

The composite target stays at canvas resolution, ready for the encode path. Preview requests
carry a maximum edge; zero means full resolution for existing callers and oracle checks.
The UI supplies its displayed size. Aspect ratio is preserved, no upscaling is requested, and
only the reduced sRGB8 image crosses to host memory on the GPU path. The CPU fallback may
reduce its completed picture; changing its raster grid would change nearest-neighbour results.
This version does not wire encoders or outputs into the product.

### No usable device

`munkicast-render` owns device and backend selection. Offscreen self-checks explicitly use CPU;
normal runs try Vulkan and retain CPU fallback. Device failure does not make colours, stills,
or precomps unavailable. Capture can remain unavailable when there is no usable capture device,
and is reported as a gap. Hardware checks explicitly request GPU so fallback cannot pass them.

### Two devices

Capture and composite use the same selected device. A compositor advertising a different DRM
device is refused with a diagnostic until cross-device capture is measured; successful import
alone is not proof. A secondary encoder will receive a buffer scaled and converted on the
composite device, allocated in host-visible non-device-local memory as ADR-0019 requires.
No second-device VRAM import or CPU readback is silently labelled zero-copy. Cross-device
encode remains a separate measured integration step.

## Consequences

The CPU self-check continues to drive the real process. Hardware checks additionally exercise
the actual GPU executor and process transport, compare pixels with the CPU, and measure capture
through preview readback. New rendering behavior belongs in the shared plan before either
executor. A second scene walk, a full-size capture readback on the GPU path, and an unleased
ring-slot pointer are rejected designs.
