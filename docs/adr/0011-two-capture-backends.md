<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0011 — Two Linux capture backends; portal ScreenCast is the fallback, not the primary

- **Status:** accepted
- **Date:** 2026-10-12
- **Supersedes:** the single-capture-path assumption in the plan's §7 table and Phase 0/1

## Context

ADR-0009 and ADR-0010 spent a long time on `no more input formats` from portal ScreenCast. Real
bugs of ours were found and fixed. But after those fixes, on a freshly booted machine, **every**
advertised configuration still failed on output DP-1 — explicit NVIDIA modifiers, LINEAR,
`INVALID`, and even a format param carrying no modifier property at all. The first negotiation
attempt of a fresh session reported the source offering only `DRM_FORMAT_MOD_INVALID`, despite the
node's `EnumFormat` params listing thirteen modifiers.

Two decisive pieces of evidence then arrived, and they agree.

### 1. Omarchy itself defaults away from the portal, for this exact failure

`/usr/bin/omarchy-capture-screenrecording` — the script behind `omareel`, the distribution's own
screen recorder — says so in its own comments:

> `OMARCHY_SCREENRECORD_USE_PORTAL=true` skips the built-in slurp picker and uses
> gpu-screen-recorder's xdg-desktop-portal capture backend instead. [...] **Off by default because
> the portal path can fail EGL DMA-BUF modifier import on some configurations, leaving recording
> unable to start.**

and again at the call site:

> Default flow uses slurp + the kms backend, which **avoids the EGL DMA-BUF modifier import
> failures the portal path can hit on some configurations**.

The portal backend is kept only as an opt-in for the three things it alone supports: HDR-aware
capture, monitors driven by external GPUs, and window capture.

So the failure mode we hit is known, is independent of our code, and the reference implementation
on this very distribution routes around it by default. Continuing to treat portal ScreenCast as
*the* Linux capture path would have meant shipping a product whose primary capture path the host
distribution considers unreliable.

### 2. A control experiment: the same output captures perfectly by another route

`gpu-screen-recorder -w DP-1 -k auto -f 60 -fm cfr` on the output every portal attempt failed on:

```
459 frames, 2560x1440, h264, ~60 fps sustained, no picker, no portal
```

DP-1 is not broken. The portal path is broken for it. That distinction is the whole finding, and it
took a second backend to establish — which is the lesson: **a capture failure cannot be attributed
without a second, independent capture path to compare against.**

## Decision

Linux gets **two capture backends**, selected at runtime by probing, never at compile time:

1. **`ext-image-copy-capture-v1` (primary on wlroots-family and KDE).** Verified present on this
   machine, together with everything the design needs:

   | global | what it provides |
   |---|---|
   | `ext_image_copy_capture_manager_v1` | dmabuf frame capture, no portal in the path |
   | `ext_output_image_capture_source_manager_v1` | monitor sources, **and no picker dialog** |
   | `ext_foreign_toplevel_image_capture_source_manager_v1` + `ext_foreign_toplevel_list_v1` | window capture by toplevel identity, stable across restarts |
   | `zwp_linux_dmabuf_v1` | modifier negotiation we own end to end, with per-device tranches |
   | `linux_drm_syncobj_manager_v1` | the explicit sync ADR-0007 requires |

   This is better than the portal for a streaming application on four counts, not merely different:
   - **No picker click per session.** Re-picking a source every launch is one of the loudest
     complaints about OBS on Wayland, and it disappears entirely.
   - **Modifier negotiation with the compositor directly**, via `zwp_linux_dmabuf_v1` feedback
     tranches, which report the modifiers actually usable on a given device rather than the union a
     PipeWire node happened to advertise. The mismatch between those two is precisely ADR-0010's
     Cause 2.
   - **Explicit sync** through `linux_drm_syncobj_v1` instead of inferring completion.
   - **Window capture by identity**, so a source survives a restart without re-authorisation.

2. **portal ScreenCast -> PipeWire (universal fallback, and the only option in a sandbox).** Kept
   because it is the sole path that works under Flatpak confinement, on GNOME (which does not
   implement `ext-image-copy-capture-v1`), and for the HDR / external-GPU cases the Omarchy script
   calls out. The work in ADR-0009 and ADR-0010 is not wasted: this backend stays, with its bugs
   fixed and its diagnostics intact.

A third backend, **DRM/KMS plane capture**, is explicitly *not* adopted. It is what
`gpu-screen-recorder` uses and it works, but it needs elevated privileges — gsr ships a separate
`gsr-kms-server` helper for exactly that reason — and a streaming studio that asks for
`CAP_SYS_ADMIN` to capture a screen is not a product we want to defend.

## Proven, end to end, on 2026-10-12

`capture-spike --output <name>` with no portal and **no picker**, on the development box:

```
=== what the COMPOSITOR itself offers ===
  buffer size   : 2560x1440
  dmabuf device : 226:128
  format AR24 (13 modifiers)   format XR24 (13 modifiers)
=== AR24: 6 common modifier(s) ===   (0x300000000606010-015)
allocated 2560x1440 AR24 modifier NVIDIA(0x300000000606014) planes 1 stride 10240 -> dmabuf fd 32
```

| output | result | modifier | non-black |
|---|---|---|---|
| DP-1 (Samsung G5, 2560x1440@165) | **PASS** | `0x300000000606014` | 83.1% |
| DP-2 (Samsung G5, 2560x1440@165) | **PASS** | `0x300000000606014` | 100% |
| DP-3 (MSI MAG322CQR, 2560x1440@165) | **PASS** | `0x300000000606014` | 100% |
| HDMI-A-3 (Samsung, 3840x2160@60) | **PASS** | `0x300000000606014` | 97.8% |

Confirmed on the reference target `argus` as well (RTX 2070, Turing, HDMI-A-1 2560x1440@60):
**PASS**, same modifier `0x300000000606014`, 100% non-black. So the result holds across two NVIDIA
generations five years apart (Turing and Blackwell) and across DisplayPort and HDMI.

The readback was written to a PPM and inspected: it is the actual desktop, correct colours and
geometry. **This closes Phase 0's capture question and proves the Vulkan path that ADR-0009 and
ADR-0010 left open** - a dmabuf shared with the compositor, filled by it, read back correctly.

Two things are worth noting about *which* modifier was chosen. The driver picked
`0x300000000606014` unprompted from the six-modifier candidate list - the same modifier Hyprland
publishes as its fixed layout, and the same one ADR-0010 guessed at. And every output chose it,
including the 4K panel, so the choice is a property of the device and format rather than of the
monitor.

The structural reason this works where the portal path did not: **the client allocates**. We create
the image with `VkImageDrmFormatModifierListCreateInfoEXT` carrying the whole intersection and let
the driver choose; the compositor then either renders into that buffer or rejects it outright.
There is no "negotiated but unallocatable" state for it to fall into, which is precisely ADR-0010's
Cause 2.

## Consequences

- `munkicast probe` must report which backends are available and which it would choose, since
  "capture does not work" is now a question with several possible answers.
- The capture worker gains a backend abstraction at the *frame source* level — narrow, because both
  backends terminate in the same place: a dmabuf with a modifier, plus an acquire/release fence.
- Selection order on Linux: `ext-image-copy-capture-v1` when the globals are present and not
  sandboxed, else portal ScreenCast. Overridable by configuration, because the user whose setup
  breaks is the one who needs the override.
- The plan's §7 table, which lists "portal ScreenCast -> PipeWire -> dmabuf" as *the* replacement
  for X11 capture, is amended accordingly. The no-X11 commitment is untouched: both backends are
  Wayland-native.
- **Immediate practical benefit for development:** the `ext-image-copy-capture-v1` path needs no
  picker, so stage 3 (the still-unproven Vulkan dmabuf import) can finally be iterated on without a
  human clicking a dialog for every run. Every measurement so far has been rationed by that.
