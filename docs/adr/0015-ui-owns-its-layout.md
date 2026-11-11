<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0015 — The UI owns its layout; no client-side window geometry, ever

- **Status:** accepted
- **Date:** 2026-10-12
- **Relates to:** ADR-0003 (Wayland-native, no X11)

## Context: the pipeline was never the problem

Phase 0 has been measuring capture, transfer and encode, and it is worth recording plainly that
**the operator's existing pipeline already works.** In production on `argus`: a game machine shown
in a window, the centre display at 1440p165 or 120, mirrored out at 119/120 Hz, captured at
1440p59.94, encoded to Twitch and YouTube. A six-hour unlisted YouTube broadcast produced roughly
**20 dropped frames in total, all from render-lag, none from capture, encode or network.**

The actual, repeated, show-threatening failures are these, in the operator's own words:

> "fighting OBS's issues with moving a window or snapping a dock or sometimes nothing at all
> crashing it, or randomly rearranging things and losing the main canvas, or having it pop out and
> go off screen completely"

Every one of those is a **window-management and layout-state failure**, not a media failure. That
is the problem MunkiCast exists to solve. Phase 0's measurements are necessary groundwork, but they
are not where the product's value is won.

## Why this is structural on Wayland, and cannot be fixed in OBS

OBS's UI is Qt `QDockWidget`s that can float as independent top-level windows, with layout
persistence that saves and restores their geometry. That design assumes a client may ask to be
placed at a remembered screen position. **On Wayland a client cannot do that.** `xdg_shell` has no
positioning request, a client cannot learn its own absolute position, and nothing restores a
toplevel to a saved coordinate.

There is a staging protocol intended for exactly this — `xdg-session-management-v1`, present in
`/usr/share/wayland-protocols/staging/`. It is **not implemented here.** Hyprland 0.56.2 advertises
60 globals on this machine and `xdg_session_manager_v1` is not among them:

```
xdg_wm_base                      v7     <- plain xdg-shell: no positioning
xdg_activation_v1                v1
xdg_toplevel_tag_manager_v1      v1     <- the sanctioned alternative
xdg_wm_dialog_v1                 v1
xdg_system_bell_v1               v1
(no xdg_session_manager_v1)
```

So a floating-dock layout cannot be restored on this desktop by any means. A dock that goes
off-screen cannot be programmatically retrieved, because the application is not permitted to know
where it is or to ask to be moved. Under X11 the same code appears to work, which is precisely why
the behaviour reads as "Linux is an unsupported afterthought" — the design is X11-shaped and
Wayland has no equivalent to offer it.

## Decision

**MunkiCast's layout state is internal to one surface, owned and serialised by MunkiCast.**

1. **One top-level window holds the entire workspace.** Panels — scenes, sources, audio mixer,
   canvas, dock-style tools — are regions laid out *inside* that surface by our own layout engine.
   Splitters, tabs and docking are drawn by us, not by the window manager.
2. **The program canvas is never a separate OS window.** It is a region of the main surface. It
   cannot be lost, cannot be moved off-screen, and cannot be closed by accident, because there is
   no OS-level window for the compositor to misplace.
3. **Detached panels are permitted, but their placement belongs to the compositor.** When a panel
   is torn off — genuinely useful on a multi-monitor rig — it becomes a toplevel that we tag via
   `xdg_toplevel_tag_manager_v1` with a stable identifier (`munkicast:panel:audio-mixer`). The
   compositor places it, and the user writes a Hyprland window rule if they want it pinned. We
   record *which* panels are detached and what they contain; we never record where they are.
4. **No absolute geometry is ever written to disk.** Saved layout is a tree of relative
   proportions, so it is resolution-independent and restores correctly on a different monitor
   arrangement — including the single-monitor laptop case, which is where OBS layouts are most
   often destroyed.
5. **Layout writes are atomic and versioned**, like every other piece of session state (write to a
   temp file, `fsync`, rename). "Randomly rearranging things" is what a torn or partially-written
   layout file looks like on next launch. A layout that fails to parse falls back to the default
   and says so, rather than restoring something incoherent.
6. **Layout is a command like any other** (see the plan's command-producer model), so rearranging
   panels is undoable and a bad drag is never destructive.

## Consequences

- Rejected: Qt `QDockWidget` floating docks, and any design where a user-visible piece of the UI is
  an independently positioned OS window. This is the single largest structural departure from OBS's
  frontend, and it is the one most directly tied to the operator's actual pain.
- The layout engine is ours to build and must be good — tearing, resizing, tabbing and snapping all
  have to feel native, because we no longer inherit them from the toolkit.
- This constraint is not a Wayland concession that costs us on other platforms. A self-owned layout
  behaves identically on macOS and Windows, where the X11-style assumption would otherwise produce
  three subtly different and separately broken behaviours.
- The multi-monitor workflow that floating docks were serving is preserved through detached,
  compositor-placed, tagged panels — with the honest limitation, surfaced in the UI, that *where*
  they land is the compositor's decision and is configured there.
