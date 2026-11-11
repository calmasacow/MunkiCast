<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0003 — Wayland-native, no X11 code anywhere

- **Status:** accepted
- **Date:** 2026-10-11

## Context

The author's working OBS setup required running it in a dedicated X11 session. The usual reason is
not capture — it is **global hotkeys**: OBS's hotkey system uses X11 key grabs, which do not exist
under Wayland, so hotkeys silently do nothing and the pragmatic workaround is to run the whole
application under X11.

Verified present on the target machine (Hyprland 0.56.2, Aquamarine — not wlroots):

```
org.freedesktop.portal.GlobalShortcuts                  (xdg-desktop-portal 1.22.1)
hyprland_global_shortcuts_manager_v1
ext_foreign_toplevel_list_v1
ext_foreign_toplevel_image_capture_source_manager_v1
ext_image_copy_capture_manager_v1
zwlr_screencopy_manager_v1
hyprland_toplevel_export_manager_v1
```
`zwlr_export_dmabuf_v1` is **absent** — do not target it.

## Decision

MunkiCast contains **no X11 code at all**: no xshm, no xcomposite, no xcb, no graphics-hook injection.

| X11-era workaround | Native replacement |
|---|---|
| X11 session so global hotkeys work | `org.freedesktop.portal.GlobalShortcuts`, with `hyprland_global_shortcuts_manager_v1` as native path |
| `xshm` / `xcomposite` capture | portal ScreenCast -> PipeWire -> dmabuf |
| Per-window capture, re-picked each time | `ext_foreign_toplevel_list_v1` to enumerate + `ext_foreign_toplevel_image_capture_source_manager_v1` to capture |
| "Game capture" via injection | Does not exist on Wayland; will not be built. Toplevel capture covers it. |
| `v4l2loopback` for virtual camera | PipeWire virtual node (no kernel module) |
| Browser source under XWayland | Out-of-process web engine with `--ozone-platform=wayland` |

**Global shortcuts drive the hotkey architecture and are a Phase 1 deliverable.** The portal inverts
the X11 model: register shortcut *descriptions*, receive `Activated` signals. Hotkeys are therefore
named, declarative, rebindable through the compositor, and resolved to `Command`s on the bus — not a
key-grab table. Retrofitting that onto a grab-shaped design is a rewrite.

**Hard guardrail.** The session here is `QT_QPA_PLATFORM=wayland;xcb` with Xwayland live on `:0`, so
a Qt app *silently* falls back to XWayland when the Wayland path misbehaves — which is how one ends
up debugging an X11 app believed to be native. MunkiCast forces `wayland` with **no xcb fallback**,
and `munkicast probe` fails loudly if it finds itself on XWayland.

## Rejected alternatives

**Support X11 as a secondary backend.** Doubles the capture and windowing surface, and X11 is the
platform the author is explicitly escaping. XWayland *clients* are captured normally as ordinary
toplevels, so nothing is lost.

**`zwlr_screencopy` / `hyprland_toplevel_export` as the primary capture path.** Both are present and
may be useful later for lower-latency or picker-free capture, but the portal is the
compositor-agnostic, permission-correct path and must be the default. Revisit as an optimisation.

## Consequences

- No "game capture" feature on Linux, ever. Toplevel capture is the answer.
- Hotkeys are mediated by the compositor/portal, so the UI cannot show a raw key-grab editor; it
  shows registered actions and defers binding to the portal's UI where applicable.
- `munkicast probe` must detect and refuse XWayland — tested in CI.
