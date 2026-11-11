<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0008 — The reference target is `argus`, not the development box

- **Status:** accepted
- **Date:** 2026-10-11

## Context

ADRs 0006 and 0007 were measured on the development machine. The author streams from a **different**
Omarchy machine, `argus`, reached over SSH for this investigation. Its hardware is materially older,
which invalidated some assumptions and confirmed others.

| | OmaDevCom (dev) | **argus (reference target)** |
|---|---|---|
| CPU | i9-13900K, 32 threads | **i9-9900K, 16 threads** |
| RAM | — | 62 GiB |
| dGPU | RTX 5080 (Blackwell) | **RTX 2070 (TU106, Turing), 8 GiB** |
| iGPU | UHD 770 (Raptor Lake, Gen12) | **UHD 630 (Coffee Lake, Gen9.5)** |
| NVIDIA driver | 610.57.04 | 610.57.04 (NVENC 13.0/13.1, **AV1 unsupported**) |
| `AQ_DRM_DEVICES` | NVIDIA pinned first | **not set** |
| Omarchy / Hyprland | 4.0.4-1 / 0.56.2 | identical |

**Both machines must work**, but `argus` is the machine performance and capability decisions are
made against. Turing is the floor, not Blackwell.

## Measured on `argus`

**Vulkan video encode works on Turing.** `h264_vulkan` and `hevc_vulkan` both produced valid output
(120/120 frames decoded). ADR-0006's zero-copy primary path **holds on the real target**, which was
the single most important open question. `h264_nvenc` works as the baseline fallback.

**QSV cannot work on this host, and the reason generalises:**

| Path | Result |
|---|---|
| `h264_qsv`, default libva | FAILS — `Error creating a MFX session: -9` |
| `h264_qsv` with `LIBVA_DRIVER_NAME=iHD` | **also FAILS** — same error |
| `h264_vaapi` on the Intel node, default libva | **OK** — 120/120 frames |
| `h264_vaapi` with `LIBVA_DRIVER_NAME=iHD` | OK — identical output |

Root cause: `argus` has `libvpl` + `vpl-gpu-rt` (oneVPL, i.e. `libmfx-gen`), which supports **Gen12
and newer only**. The UHD 630 is **Gen9.5**, which requires the *legacy* Intel Media SDK
(`libmfx.so.1`) — absent. So the QSV runtime and the GPU generation do not match, and no environment
variable fixes that.

Note this differs from the dev box, where QSV worked once `LIBVA_DRIVER_NAME=iHD` was forced. **The
QSV failure mode is therefore machine-specific and runtime-version-dependent**, whereas VAAPI worked
on both machines with no special handling on `argus`.

The author's OBS runs as a **Flatpak** (`com.obsproject.Studio` 32.2.2 on
`org.freedesktop.Platform 25.08`), whose bundled runtime supplies a QSV stack the host lacks. Its log
shows `>>> app not on intel GPU, fall back to old qsv encoder` — i.e. OBS detects that compositing
happens on NVIDIA and takes a slower cross-GPU path. **Their working QSV today is a property of the
Flatpak runtime, not of the host**, and would not survive a native build.

## Decision

1. **Use VAAPI, not QSV, as the Intel/AMD encode path. Drop QSV entirely.** It offers nothing over
   VAAPI on these GPUs while adding a runtime-to-GPU-generation matching problem that is invisible
   until it fails at stream time. This supersedes ADR-0007's inclusion of QSV in the `Output`
   encoder API enum.
2. **Turing is the capability floor.** No AV1 encode on NVIDIA (`AV1 supported: false`), no 10-bit
   texture path (`P010 texture support not available` in their OBS log). H.264 and HEVC only.
3. `munkicast probe` must report, per render node, the libva driver bound **and** whether a *working
   encode entrypoint* exists — not merely whether an encoder name is present in `ffmpeg -encoders`,
   which is true even when the runtime cannot create a session.
4. **All capability claims are per-machine and must be probed at runtime, never compiled in.** Two
   machines of the same OS and driver version differed on QSV. The fallback ladders exist for this.

## Consequences

- ADR-0007's encoder enum becomes `VulkanVideo | NVENC | VAAPI | x264`.
- The 16-thread i9-9900K is a much tighter CPU budget than the dev box's 32 threads, so the x264
  last-resort tier is weaker than it looks, and sysmem-roundtrip fallbacks cost relatively more.
- Measurements taken on the dev box are not evidence about the target. Re-run anything that matters.

## Verified Vulkan capability on `argus` (via `munkicast probe`, built and run there)

```
[0] NVIDIA GeForce RTX 2070      discrete   api 1.4.341   drm 226:1 / 226:128
    VK_EXT_image_drm_format_modifier yes   VK_KHR_external_memory_fd      yes
    VK_EXT_external_memory_dma_buf   yes   VK_KHR_external_semaphore_fd   yes
    VK_KHR_timeline_semaphore        yes   VK_EXT_physical_device_drm     yes
    VK_KHR_video_encode_queue        yes
[1] Intel(R) UHD Graphics 630    integrated api 1.4.354   drm 226:0 / 226:129
    ... all of the above                   VK_KHR_video_encode_queue      NO
```

Two consequences:

- **The Intel GPU supports dmabuf import plus explicit sync even though it cannot Vulkan-encode.**
  That is precisely what ADR-0007's cross-GPU transfer ladder tier 1 requires: export a linear
  dmabuf from the NVIDIA compositing device, import it on Intel, then hand it to VAAPI. The
  extensions are present on both GPUs of both machines, so the path is plausible — still unproven
  end to end, and the capture spike must measure it.
- **Vulkan device enumeration order differs between machines.** `argus` reports NVIDIA at index 0 and
  Intel at 1; the dev box reports the reverse. **Never select a device by index.** Match
  `VkPhysicalDeviceDrmPropertiesEXT` against the DRM node, as the design already specifies. A
  config file naming "GPU 0" would silently target the wrong card across machines.

## Development workflow

`argus` is **not always powered on**. Therefore:

- **Day-to-day build/test happens on the development box**; `argus` is an occasional validation gate,
  not part of the inner loop.
- **CI must never depend on `argus` being reachable.** The self-hosted hardware-encoder job stays
  optional and non-blocking, exactly as the plan's CI section specifies.
- **Runtime capability probing is mandatory, not a nicety.** The two machines already disagree on QSV
  availability, Vulkan device ordering, `AQ_DRM_DEVICES`, GPU generation and AV1 support. Nothing
  about device or encoder capability may be decided at compile time or cached across machines.
- Practical notes for working on `argus`: no passwordless sudo (package installs need the author),
  but none are required — it has `cmake`, `g++`, `make` (no `ninja`) and the system Vulkan headers,
  and the project configures and builds there unmodified with `-G "Unix Makefiles"`. `vainfo` is
  absent, so the probe's VAAPI section degrades to a note; capability was established functionally
  with `ffmpeg` instead.

## What PipeWire actually offers on `argus` (from their own OBS log)

The capture-side format negotiation was expected to need the spike to discover. Their existing OBS
log answers it directly:

```
[pipewire] Asking for monitor and window
[pipewire] Available capture sources:  - Monitor source  - Window source
[pipewire]     Format: 12 (Spa:Enum:VideoFormat:BGRA)
[pipewire]     Modifier: 0x0                  <- DRM_FORMAT_MOD_LINEAR
[pipewire]     Modifier: 0x300000000e08014    <- vendor 0x03 = NVIDIA, block-linear
[pipewire]     Size: 2560x1440
[pipewire] Created stream ...
```

This materially de-risks the capture half:

1. **Portal + PipeWire capture demonstrably works on `argus`** — their collection contains a working
   `pipewire-screen-capture-source` ('LocalBraveBrowser'), and both Monitor and Window source types
   are offered. So the platform path is sound; what remains unproven is *our* Vulkan import, not the
   compositor or the portal.
2. **`DRM_FORMAT_MOD_LINEAR` (0x0) is advertised alongside the NVIDIA block-linear modifier.** This is
   the key fact for ADR-0007's cross-GPU ladder: we can negotiate the NVIDIA modifier for same-GPU
   zero-copy composite, and explicitly negotiate **linear** for the frames destined for the Intel
   VAAPI encoder — rather than accepting an NVIDIA-tiled buffer and discovering at import time that
   Intel cannot read it. Modifier choice becomes a deliberate per-consumer decision.
3. Buffers arrive as **BGRA**, so the compositor's first operation is BGRA -> scene-linear RGBA16F.
4. Capture is at the full 2560x1440 canvas size.

**Still to prove in the spike:** importing each of those two modifiers into a `VkImage` via
`VK_EXT_image_drm_format_modifier` with explicit `sync_file` acquire/release, and whether a
linear-modifier dmabuf exported by NVIDIA actually imports on the Intel device (ADR-0007 tier 1). The
extensions required are present on both GPUs of both machines.
