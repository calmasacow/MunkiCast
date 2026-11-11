<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0007 — Dual-GPU concurrent encode is a first-class requirement

- **Status:** accepted; the QSV portion is **superseded by ADR-0008** (VAAPI replaces QSV)
- **Date:** 2026-10-11

## Context

The author's production workflow, on a second Omarchy machine (not the development box):

- **Intel QuickSync -> Twitch at 1080p60**
- **NVIDIA NVENC -> YouTube at 1440p, sometimes 4K60**

Both simultaneously, from one composite. The Intel GPU is kept enabled *specifically* as a second
encoder, not as a display adapter.

This reclassifies three things the plan had mis-weighted:

1. **Per-destination encode at differing resolutions is day-one**, not a Phase-2 differentiator.
2. **Cross-GPU frame transfer is a core path**, not the fallback edge case it was listed as.
3. **The Vulkan-native encode path (ADR-0006) covers only the NVIDIA output.** `munkicast probe`
   confirms the Intel UHD 770 does **not** support `VK_KHR_video_encode_queue`, so the Intel output
   must go through QSV or VAAPI.

### Measured on the development box

Concurrent dual-GPU, dual-resolution encode works today. 10 s of 2560x1440@60 (600 frames), split to
an Intel QSV 1080p output and an NVIDIA NVENC 1440p output in one process:

```
wall 2.87 s  (~3.5x realtime)
dual_twitch_1080p : h264 1920x1080  600/600 frames  1,389,884 B
dual_youtube_1440p: h264 2560x1440  600/600 frames  1,754,693 B
```

That is the *naive* path — software frames between filter and encoders, CPU scaling. Encouraging
headroom, but 4K60 plus real compositing will be materially heavier, so it is a feasibility result,
not a performance budget.

### QSV is broken by default on a dual-GPU NVIDIA+Intel box

| Configuration | Result |
|---|---|
| `h264_qsv`, default libva binding | **FAILS** — `Error creating a MFX session: -9` |
| `h264_qsv` with `LIBVA_DRIVER_NAME=iHD` | OK — 120/120 frames |
| `h264_vaapi` with `LIBVA_DRIVER_NAME=iHD` | OK — 120/120 frames |

Root cause is the same as ADR-0006's VAAPI finding: `nvidia-vaapi-driver` claims libva globally, so
the Intel render node resolves to "VA-API NVDEC driver", which is decode-only. QSV sits on oneVPL
over VAAPI and therefore cannot create a session at all.

## Decision

**Treat concurrent multi-GPU encode as a first-class architectural requirement.**

1. **An `Output` names its encode device explicitly**, as a DRM node plus an API selector, not just
   an encoder name:
   `Output { id, canvas_id, scale_target, encoder: {api: VulkanVideo|NVENC|VAAPI|x264, drm_node,
   codec, rate_control...}, destination, enabled }`.
   (QSV was dropped per ADR-0008: its runtime must match the GPU generation, and it fails on the
   actual target machine where VAAPI works.)
   Device choice is persisted in the project, because "Twitch on the iGPU, YouTube on the dGPU" is a
   deliberate user decision, not something to re-derive heuristically each launch.

2. **MunkiCast sets `LIBVA_DRIVER_NAME` per encode worker**, derived from the target node's kernel
   driver (`i915`/`xe` -> `iHD`, `amdgpu` -> `radeonsi`), rather than inheriting the session
   environment. Because this is a per-process environment variable and we need *different* values
   for different devices concurrently, **each VAAPI/QSV encoder runs in its own encode worker
   process.** This falls out of the multi-process design (ADR-0005) rather than fighting it, and it
   is the deciding argument for encoders being separable processes rather than threads.

3. **Scale before transfer, always.** Composite once at canvas resolution on the compositing GPU,
   apply each output's scale *on that GPU*, and only then move the frame to its encode device. A
   1080p NV12 frame is ~3.1 MB (~186 MB/s at 60 fps) — trivial over PCIe; moving 1440p or 4K first
   and scaling on the far side wastes multiples of that.

4. **Cross-GPU transfer ladder**, auto-demoted and loudly logged:
   1. Export an exportable `DRM_FORMAT_MOD_LINEAR` image from the compositing device as a dmabuf and
      import it on the encode device (`VK_EXT_external_memory_dma_buf` / VAAPI dmabuf import).
      Cross-vendor NVIDIA -> Intel sharing is **unproven and must be measured by the spike.**
   2. GPU -> system memory readback on the compositing device, upload on the encode device. Known to
      work (it is what the measurement above effectively did); costs PCIe bandwidth both ways.
   3. Drop that output with a user-visible fault, keeping other outputs alive.

5. **The NVIDIA output keeps the ADR-0006 zero-copy path** (`h264_vulkan`/`hevc_vulkan` on the
   compositing device), since composite and encode share a `VkDevice` there.

## Rejected alternatives

**Single-GPU encode with CPU fallback for the second output.** Wastes hardware the author
deliberately provisioned, and x264 at 1080p60 alongside a 4K60 composite is a poor use of the CPU.

**Deriving encode-device assignment automatically each launch.** The mapping is an explicit
preference (iGPU for the smaller stream so the dGPU is free for the game and the main encode).
Auto-derivation would silently change behaviour after a driver or hardware change, mid-stream.

**Setting `LIBVA_DRIVER_NAME` once for the whole application.** Impossible: a single process cannot
hold two different values, and we need Intel's `iHD` and NVIDIA's path concurrently. Hence per-worker
processes.

## Consequences

- **The spike's capture half gains a second question**: does cross-vendor NVIDIA -> Intel dmabuf
  import work with a linear modifier, or must we fall back to a sysmem roundtrip? Both paths ship
  regardless; the question is which is the default.
- `munkicast probe` must report, per render node: kernel driver, the libva driver actually bound, the
  driver that *would* work if forced, available encode entrypoints, and
  `VK_KHR_video_encode_queue` support. Partly implemented; QSV session checking still to add.
- Encode workers become their own process class in the supervision model.
- **The development box is not the target.** All measurements here must be re-taken on the streaming
  machine; `munkicast probe` is the instrument for that.
