<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Reference pointers

Per ADR-0001, the OBS Studio fork at `/home/calmasacow/Development/obs-studio` (33.0.0-rc1) is a
**read-only reference oracle**. Read it to learn how a problem was solved, then write our own.
**Copy nothing** — libobs is GPLv2-or-later with no linking exception, and importing its idioms into
a clean-sheet C++23 codebase is a cost with no benefit.

> Note: most OBS integration documentation online is stale for this tree. The Qt UI moved from `UI/`
> to `frontend/`, and `obs_load_all_modules()` is now an empty stub — plugin path discovery moved
> into `frontend/plugin-manager/`. `docs/sphinx/frontends.rst` is out of date; do not trust it.

## Linux capture — the most valuable reading

| Path | Why |
|---|---|
| `plugins/linux-pipewire/pipewire.c` | The PipeWire stream lifecycle and buffer handling |
| `plugins/linux-pipewire/portal.c` | xdg-desktop-portal request/response plumbing over GDBus |
| `plugins/linux-pipewire/screencast-portal.c` | ScreenCast session setup, restore tokens |
| `plugins/linux-pipewire/formats.c` | **DRM format-modifier negotiation.** The crux of our dual-GPU problem — note that this path effectively accepts a format and then fails the import, which is what we must not do (see ADR-0001 and the plan's §3) |
| `plugins/linux-capture/` | Legacy X11-only. Of historical interest only; we build no X11 code (ADR-0003) |

## Audio

| Path | Why |
|---|---|
| `libobs/obs-audio.c` | Timestamp-driven mixing and sync: `calc_min_ts`, `find_min_ts`, `mark_invalid_sources`, adaptive buffering via `add_audio_buffering`. This sync logic is the hard-won part of any mixer and worth understanding deeply before writing ours |
| `libobs/audio-monitoring/{pulse,win32,osx}/` | Platform monitoring backends |

## Architecture, as cautionary reading

| Path | Why |
|---|---|
| `libobs/graphics/device-exports.h` | ~101 entry points with OpenGL semantics. Our RHI budget is <40 with explicit semantics. Read this to see what to avoid |
| `libobs/obs-video.c` | The render loop shape: tick -> displays -> main texture -> convert -> stage/readback -> output. Note `obs_core_video_mix` for per-canvas chains |
| `libobs/obs-scene.c` | The scene model, including why groups are a compatibility hack (`is_group`, `group_item_backup`) |
| `libobs/obs-module.c:174` | The ABI check that only blocks *forward* incompatibility — context for ADR-0004 |

## Scene-collection format (used by the native importer/exporter)

| Path | Why |
|---|---|
| `frontend/importers/studio.cpp` | `TranslateOSStudio()` — the canonical cross-OS source-id equivalence table. Copy the *design*, not the code |
| `frontend/importers/{classic,sl,xsplit}.cpp` | Prior art for importing foreign formats |
| `frontend/widgets/OBSBasic_SceneCollections.cpp` | `Save()` is the authoritative schema; `LoadData()` the reader |
| `libobs/obs.c:2443` | `obs_save_source` — the per-source key set |
| `libobs/obs-scene.c:1308` | `scene_save_item` — scene-item transform keys |

Format facts established by exploration, so they need not be re-derived:

- Only **two** collection versions exist: v1 (absolute coordinates) and v2 (relative, *with*
  absolute mirrors). **Reading the absolute keys** (`pos`, `scale`, `bounds`, `crop_left`...) plus
  top-level `resolution` covers both. No need to implement relative math for import.
- **`settings` blobs are sparse** — defaults are never serialized
  (`obs_data_to_json(data, with_defaults=false)`). An importer needs a per-source-id default table.
  This is precisely why MunkiCast serializes all defaults explicitly.
- Key-name traps: scene items use `align` / `bounds_align` / `bounds_crop` /
  `crop_left|top|right|bottom`, but the *clipboard* format in the same file uses `alignment` /
  `bounds_alignment` / `crop_to_bounds`, and the `crop_filter` plugin uses yet another spelling.
- Scene items reference sources by `source_uuid` first, falling back to `name`. Items with
  `group_item_backup: true` are duplicates the loader skips.
- A group appears **twice**: as a source in top-level `groups[]` and as an item in its parent scene.

## Wayland protocol XML

`/usr/share/wayland-protocols/staging/` already ships `ext-image-copy-capture-v1.xml` and
`ext-image-capture-source`. Only `hyprland-protocols` would need vendoring, and only if we ever add
a Hyprland-specific capture path.

## Development box monitor layout

Confirmed against the Hyprland display arrangement UI. Recorded because the portal picker selects
one output per session, and comparing runs that captured *different* monitors is not an experiment
(ADR-0009). Every capture run must log `mapping_id`.

| output | model | position | workspaces |
|---|---|---|---|
| `HDMI-A-3` | Samsung 3840x2160@60 | above, x=1805 | - |
| `DP-3` | MSI MAG322CQR 2560x1440 | left, x=0 | 7, 8, 9 |
| `DP-1` | Samsung Odyssey G5 2560x1440 | centre, x=2560 (focused) | 1, 2, 3 |
| `DP-2` | Samsung Odyssey G5 2560x1440 | right, x=5120 | 4, 5, 6 |
