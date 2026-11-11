<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0001 — No libobs; OBS is a read-only reference oracle

- **Status:** accepted
- **Date:** 2026-10-11

## Context

MunkiCast is an independent alternative to OBS Studio. Two motivations, in priority order:
independence from the OBS project's governance, and the fact that Linux is structurally last in line
in OBS's engineering.

Measured against the fork at `/home/calmasacow/Development/obs-studio` (OBS 33.0.0-rc1):

| | Windows | macOS | Linux |
|---|---|---|---|
| Graphics backend | `libobs-d3d11` 7,268 + `libobs-winrt` 711 (dedicated) | `libobs-metal` 8,377 (dedicated) | `libobs-opengl` 8,880 — shared legacy layer, also everyone's fallback, carrying X11+Wayland+WGL+CGL |
| Capture code | `win-capture` 14,976 | `mac-capture` 3,961 + SCK | `linux-capture` 2,348 (X11-only, dead under Wayland) + `linux-pipewire` 4,144 |
| CI runners | `windows-2025-vs2026` | `macos-26` | `ubuntu-26.04` **only** |

There is **no Vulkan backend** (`ls -d libobs-*` → d3d11, metal, opengl, winrt; the only `vulkan`
match in the tree is `plugins/win-capture/graphics-hook/`, for hooking Vulkan games on Windows).
`obs-nix-platform.h` offers exactly `X11_EGL` and `WAYLAND`; GLX was removed. Arch + Hyprland +
Mesa 26.x + NVIDIA 610.x — our bedrock target — is never built or tested upstream.

Decisive enabling fact: **OBS plugin binary compatibility is not a requirement for this project.**
Only a handful of plugins matter to the author (`source-clone`, a stroke filter, browser sources),
all to be reimplemented natively. That removes the overwhelming argument for building on libobs.

## Decision

Use no libobs code. Keep the OBS fork checked out as a **read-only reference oracle**: read it to
learn how a problem was solved, then write our own. See `docs/reference.md` for specific pointers.

Reuse *libraries* instead: FFmpeg, PipeWire + xdg-desktop-portal, libdatachannel, SRT,
ScreenCaptureKit/VideoToolbox/CoreAudio, Windows.Graphics.Capture/WASAPI/MF.

**Lift zero lines, including the ISC-licensed files.** `libobs/util/` and `libobs/callback/`
(56 files, ISC, legally liftable) exist to paper over C — C++23 provides `std::string`,
`std::format`, `std::jthread`, `std::expected` natively. Taking them would import OBS idioms and
dual-license bookkeeping for no gain.

## Rejected alternatives

**libobs behind a MunkiCast-owned API.** The API would be the *intersection* of what we want and what
libobs can express. Multi-canvas, nested groups, adjustment layers, per-layer effect stacks,
non-separable blend modes and a named-bus audio matrix are all inexpressible in its model: groups are
a documented compatibility hack (a group is simultaneously a source in `groups[]` and an item in its
parent, with `group_item_backup` duplicates), audio cannot be reconfigured after `obs_reset_audio()`,
and the enabled core-module list is frozen at CMake configure time. The adapter would be mostly
`unimplemented`, and we would write the engine anyway *plus* the abstraction *plus* a fork.

**Writing a `libobs-vulkan` backend.** `libobs/graphics/device-exports.h` is ~101 entry points with
OpenGL semantics: implicit synchronization, no command buffers, no queue families, no image layouts,
no timeline semaphores. Our hardest problem — cross-GPU DMA-BUF import — *is* a synchronization and
format-modifier-negotiation problem, and that vtable has no vocabulary for acquire/release fences or
modifier intersection. Fixing it means changing the API, which means a permanent fork, which means
upstreaming into the governance we are leaving.

**A new frontend over libobs.** Would have fixed ~75% of the author's historical pain (which was
frontend-layer) for ~15% of the effort — but leaves us tracking obsproject releases, GPL-linked to
their code, inheriting their engine decisions and politically downstream. That is a reskin, not
independence, and independence is motivation #1.

## Consequences

- 1–2 years to real parity; the first six months will feel worse than OBS. Accepted knowingly.
- Nine platform integrations to hand-roll (3 graphics backends x capture/audio/encode). Mitigated by
  strict platform sequencing: Linux ships 1.0, Windows/macOS are 2.0.
- No plugin ecosystem at launch. Mitigated per ADR-0004.
- We own our architecture permanently, including the ability to fix it in year three.
