<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0006 — Composite-to-encode handoff: native Vulkan video encode

- **Status:** accepted (encoder half of the Phase-0 spike; capture half still open)
- **Date:** 2026-10-11

## Context

The plan identified the composite -> encode handoff as **the least-proven link in the design**,
because NVENC has no native Vulkan interop. Three candidate paths were named, to be chosen on
evidence rather than assumption.

Measured on the target machine (RTX 5080 + Intel UHD 770, nvidia-utils 610.57.04, mesa 26.2.2,
FFmpeg n9.0.1 / libavcodec 63.1.101, Vulkan 1.4.357):

| Path | Result |
|---|---|
| 1. CUDA external-memory import (`VK_KHR_external_memory_fd` -> `cudaImportExternalMemory`) -> NVENC | **Unavailable.** No CUDA toolkit installed and no `ffnvcodec` headers; would add a large build dependency |
| 2. FFmpeg `AV_PIX_FMT_VULKAN` hwframes -> `hwmap=derive_device=cuda` -> `h264_nvenc` | **Dead.** `Failed to map frame: -38` (`ENOSYS`). Vulkan->CUDA frame mapping is not implemented |
| 3. `VK_KHR_video_encode_queue` via FFmpeg's native Vulkan encoders | **Works.** See below |

`munkicast probe` confirms `VK_KHR_video_encode_queue` is supported on the RTX 5080 (and **not** on
the Intel UHD 770).

FFmpeg n9.0.1 ships `h264_vulkan`, `hevc_vulkan`, `av1_vulkan`, `ffv1_vulkan`, `prores_ks_vulkan`.
Encoding 2 s of 1920x1080@60 testsrc and decoding every frame back:

| Encoder | Output | Validity |
|---|---|---|
| `h264_vulkan` | 289,370 B | **valid** — 120/120 frames decode |
| `hevc_vulkan` | 186,387 B | **valid** — 120/120 frames decode |
| `av1_vulkan`  | 387,927 B | **CORRUPT** — dav1d: "Error parsing OBU data" on every frame |
| `h264_nvenc` (from CUDA frames)  | 249,482 B | valid — baseline |
| `h264_nvenc` (from sw frames)    | 1,155,615 B | valid — baseline |

## Decision

**Primary path: native Vulkan video encode.** Composite in Vulkan and encode in Vulkan on the same
`VkDevice`, with no CUDA dependency and no cross-API interop in the hot path. Codecs: **H.264 and
HEVC only.**

Share our own device with FFmpeg rather than letting it create its own: `av_hwdevice_ctx_alloc(
AV_HWDEVICE_TYPE_VULKAN)`, populate `AVVulkanDeviceContext` with our instance, physical device,
device, queue families and the `vkGetInstanceProcAddr` pointer, then `av_hwdevice_ctx_init()`. This
keeps one device, one allocator and one timeline-semaphore domain across composite and encode.

**Do not use `av1_vulkan`.** It produces bitstreams dav1d cannot parse. If AV1 is wanted, use
`av1_nvenc` and accept the readback or CUDA hop.

**Fallback ladder** (auto-demoted on failure, loudly logged, same shape as the dmabuf-import ladder):
1. `h264_vulkan` / `hevc_vulkan` — zero-copy, same device
2. `h264_nvenc` from CUDA frames — only if CUDA is present; costs an interop hop
3. `h264_nvenc` from system-memory frames — costs a GPU->CPU readback
4. `libx264` — CPU encode, last resort

## Rejected alternatives

**Path 1 (direct CUDA interop).** Would work, but requires the CUDA toolkit and `ffnvcodec` as build
dependencies, adds a second memory domain and a second synchronisation primitive to the hot path, and
is strictly worse than path 3 now that path 3 is proven. Keep as fallback tier 2 only.

**Path 2 (FFmpeg vulkan->cuda derive).** Not implemented in FFmpeg; `ENOSYS`. Nothing to decide.

**Writing our own `VK_KHR_video_encode_queue` implementation** instead of using FFmpeg's. Premature:
FFmpeg's works, and we would still need FFmpeg for muxing, RTMP and decode. Revisit only if its rate
control proves inadequate.

## Consequences

- **The handoff risk is largely retired, and the architecture is simpler than planned.** No CUDA, no
  `ffnvcodec`, one `VkDevice` end to end. The plan's assumption that this was the riskiest link was
  correct; the answer was better than expected.
- **New risk: FFmpeg's Vulkan encoder maturity.** `av1_vulkan` being outright broken is a yellow flag
  about how well-exercised this code is. Before Phase 1 ships, measure: rate-control accuracy under
  constrained bitrate, B-frame and look-ahead support, encode latency against NVENC, and long-run
  stability in the 8-hour soak. The fallback ladder exists precisely because this may not hold.
- Intel UHD 770 lacks `VK_KHR_video_encode_queue`, so the iGPU path must use VAAPI (below).
- Our RHI must expose the encode queue family, not just graphics/compute.

## Related finding: VAAPI is hijacked on this machine

`h264_vaapi` initially failed with "No usable encoding entrypoint found for profile
VAProfileH264High". Cause: `vainfo` reports **"VA-API NVDEC driver [direct backend]" on *both* render
nodes**, including Intel's `renderD129` — `nvidia-vaapi-driver` is claiming VAAPI globally, and NVDEC
is decode-only.

Forcing `LIBVA_DRIVER_NAME=iHD` yields "Intel iHD driver ... 26.2.4" with
`VAProfileH264High: VAEntrypointEncSlice` present, and `h264_vaapi` then encodes correctly
(173,494 B, 120/120 frames decode).

**Consequence:** MunkiCast must select the VAAPI driver explicitly per device rather than trusting
`libva`'s default, and `munkicast probe` must report the driver actually bound to each render node.
This is a good example of a dual-GPU environment hazard that would otherwise surface as a mysterious
user bug report.
