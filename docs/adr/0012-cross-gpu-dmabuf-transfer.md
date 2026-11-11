<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0012 — Cross-GPU dmabuf transfer: the shared buffer must be in system memory

- **Status:** accepted, measured
- **Date:** 2026-10-12
- **Depends on:** ADR-0007 (dual-GPU concurrent encode), ADR-0011 (capture backend)

## Context

The show streams two outputs at once from two different GPUs: 2560x1440 via NVENC to YouTube, and
1920x1080 via VAAPI on the Intel iGPU to Twitch. That only works if a buffer produced on the NVIDIA
GPU can be read by Intel. ADR-0007 assumed it could, with a LINEAR fallback tier. This ADR measures
it.

## Result: it works, with one non-obvious requirement

```
=== cross-GPU transfer: 2560x1440 -> 1920x1080, then import on 226:129 ===
second device: Intel(R) Graphics (RPL-S)
second device can import 5 modifier(s) for AR24; LINEAR: yes
transfer buffer: 1920x1080 modifier LINEAR stride 7680
imported on second device: 8294400 bytes, non-black 1726227 / 2073600 (83.2%)
CROSS-GPU: PASS
```

The source frame was 83.1% non-black and the Intel-side readback is 83.2% of a smaller image — the
expected agreement. The result was written to a PPM and inspected: the desktop, correctly
downscaled, correct colours, text legible.

## The requirement: `host_memory`, not VRAM

The first attempt allocated the LINEAR transfer image the same way as any other exportable image —
from a `DEVICE_LOCAL` memory type. Intel imported it without complaint, created the BO, and then
failed at submit:

```
intel: the execbuf ioctl keeps returning ENOMEM
  BO: addr=0xffffeffeff600000-... size=8192KB handle=00011 name=imported
error import on the second device failed: vkQueueSubmit failed
```

A dmabuf backed by one card's VRAM cannot generally be addressed by another. **The import
succeeds** — nothing fails at the point of the mistake — and the error appears later, at queue
submission, in the other driver, as `ENOMEM`. That distance between cause and symptom is the whole
reason this is worth an ADR.

So a buffer intended for a second GPU is allocated from a memory type that is `HOST_VISIBLE` and
explicitly **not** `DEVICE_LOCAL`. That combination is system RAM. The second preference, merely
`HOST_VISIBLE`, is deliberately a fallback and not the first choice: on a discrete card a type that
is both host-visible and device-local is resizable-BAR VRAM, which is precisely what must be
avoided here.

## Consequences

- `vk_create_exportable` takes a `host_memory` flag. Buffers the compositor renders into stay in
  VRAM; buffers crossing to a second GPU do not.
- **Scale before transfer, confirmed as more than a preference.** The transfer buffer lives in
  system RAM, so every byte crosses PCIe twice — written by the source GPU, read by the target.
  Downscaling 1440p to 1080p before the transfer cuts that traffic by 44% and costs nothing, since
  the target needs the smaller image anyway. Transferring first and scaling on the far side would
  spend the scarce resource to save nothing.
- The cross-GPU path is the *second* encode's path only. The primary encode composites and encodes
  on one device with no transfer at all (ADR-0006).
- This also exercises `vk_import.cpp`'s import path — the same code the portal backend uses — so
  that path is no longer unproven either.

## Open

- Throughput at 60 fps sustained. One frame proves correctness, not that the PCIe round trip fits
  in a frame budget. A LINEAR 1080p BGRA frame is 8.3 MB, so 60 fps is ~500 MB/s each way; well
  within PCIe 4.0 x16, but the copy's CPU and GPU cost must be measured before Phase 1.5.
- Explicit sync. The transfer currently relies on a fence wait on the source device before the
  import. `linux_drm_syncobj_v1` is advertised by the compositor and
  `VK_KHR_external_semaphore_fd` by both devices; the real path should pass a timeline semaphore
  rather than stalling the submitting thread.
- Whether a P010/NV12 transfer buffer (what the encoder actually wants) imports as cleanly as
  BGRA, and whether the colour conversion belongs before or after the transfer.
