<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR-0066: Media decoding policy

Status: Accepted

## Context

Media workers use Qt's FFmpeg backend, which selects hardware devices at runtime.
The operator needs an explicit software fallback control. VP8/VP9 WebM alpha is
block side data; native and hardware decoders discard it. OBS selects libvpx for
these streams too. Hardware decoding must never silently remove transparency.

## Decision

Media and playlist sources store `hardwareDecode`, a boolean defaulting to true.
The control requests hardware decoding when available; it does not promise a
particular device or zero-copy delivery. A disabled control sets Qt's supported
FFmpeg decoding-device override to an empty list before constructing the player.
Each source has its own worker process, isolating Qt's cached device policy.
Enabled sources retain runtime device detection and software fallback.

Transparent VP8/VP9 WebM always uses libvpx, independently of this request. The
settings explain this exception. Existing sources retain automatic selection.
OBS import/export translates `hw_decode` for Media Source.

Render-time display uses the last 500 ms of received render work. Frame and missed
frame counters retain their since-reset meaning. Idle windows show zero render
work rather than retaining a lifetime average.

## Consequences

Hardware decode does not make the current CPU RGBA worker transport zero-copy.
Alpha playback remains software decoded; profiling must cover conversion, scaling,
transport and composition, as well as the codec. No libOBS dependency is introduced.

Equal-sized source images retain their existing storage. Qt's per-process GUI
conversion thread pool is disabled in source workers to avoid concurrent workers
each spawning up to eight conversion threads. Alpha presentation interpolates
Qt's 50 ms position updates
with a monotonic clock rather than delivering frames in position-update bursts.

Media/playlist worker frames carry decoded pixel dimensions, independently of the
saved output box. The shared scene plan scales and letterboxes them into that box
before source/node filters, cropping and placement. GPU composition therefore
performs resizing on device; CPU fallback executes the same plan. Equal-size
frames need no extra surface. Decode and RGBA conversion remain in the worker.

A first worker configuration gets a 30-second codec/device-probe deadline; later
acks retain the five-second watchdog. Concurrent real media opens exceeded the
old five-second initial deadline and were restarted before configuration finished.
Native RGBA frames remain bounded to 8192 per edge and 128 MiB.

A media resize with no letterbox folds into the source filter chain, avoiding an
extra isolation surface. The GPU scales uploaded sRGB pixels directly, preserving
premultiplied-alpha interpolation. Bilinear sampling uses four fixed taps.
Immutable worker-frame leases identify reusable resized GPU images. This cache
holds at most eight / 128 MiB, counts toward the compositor's 512 MiB budget,
and releases entries absent from the current plan. A changed frame or filter
stack refreshes the result; placement and cropping still execute each composite.
Worker refresh deadlines include processing time instead of adding a fixed
16 ms sleep after each reply.

Workers retain a QImage lease while publishing, avoiding a temporary pixel copy.
Each worker frame receives a fresh size-sealed memfd, filled once and write-sealed
before sending. Worker clients may retain a read-only mapping only after verifying
WRITE/SHRINK/GROW seals; ordinary clients still copy. Mapping ownership survives
worker shutdown and later frames. This is wire-compatible with protocol 6 because
fresh Frame descriptors were already supported. Mutable preview slots retain
copying semantics. No mutable shared pixels are borrowed asynchronously.

Scratch images match unused resources by shape, and unused uploads remain pooled
(up to 128 MiB idle staging) across video cache hits. Spare image retention stays
within the existing 512 MiB image budget, preventing alternate-frame allocation
churn without an unbounded cache.

Silent alpha clips use a monotonic playback clock without constructing Qt's media
player or probing hardware decoders. Clips with audio retain Qt's audio pipeline
and disable its video track, while libvpx supplies alpha video. WebP likewise
needs no media player. Playlist transitions initialize the player only when needed.
