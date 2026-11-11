<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0016 — Panel docking follows the Adobe drop-zone model

- **Status:** accepted, specified
- **Date:** 2026-10-12
- **Depends on:** ADR-0015 (the UI owns its layout)

## Context

ADR-0015 established that MunkiCast lays out panels itself inside a single surface, because Wayland
offers no way to restore client-side window geometry. That decision bought us a requirement: having
given up the toolkit's docking, we have to build one, and it has to be better than what it replaced.

The operator specified the target directly:

> "I would much prefer the method that Adobe used in Premiere Pro and After Effects... when a window
> is dragged 'by its handle' it enters a movable state. Then when you move it over another window
> there is a diagonal 4-way split with a section in the center of the target window you are over. As
> you drag over one of those 4 outer segments it will highlight. If you release over one of the
> outer 4 segments that window will divide the space and insert the dragged window on that side.
> Releasing over the center segment means that window will consume the same space as the target
> window it is being dropped on and the 2 windows will operate as a tabbed group."

## The geometry, and why the diagonals matter

The target panel is divided by **its own two diagonals**, with a rectangle carved out of the middle:

```
+---------------------------------------+
| \                                   / |
|   \            TOP                /   |
|     \                           /     |
|       +-----------------------+       |
|       |                       |       |
| LEFT  |        CENTER         | RIGHT |
|       |        (tab)          |       |
|       |                       |       |
|       +-----------------------+       |
|     /                           \     |
|   /            BOTTOM             \   |
| /                                   \ |
+---------------------------------------+
```

This is not a 3x3 grid, and the difference is the whole point. With diagonals, **every point in the
panel belongs to the zone it is nearest to in the direction that matters**, so the target is as
large as it can possibly be and the gesture stays forgiving at speed. A grid gives small corner
cells that mean nothing and forces the user to aim. Adobe's geometry never asks the user to aim.

Hit-testing, with the cursor at `(x, y)` in a panel of size `w x h`, `u = x/w`, `v = y/h`:

```
if inside the centre rectangle      -> CENTER   (tabbed group)
else if v < u and v < 1 - u         -> TOP
else if v > u and v > 1 - u         -> BOTTOM
else if v > u and v < 1 - u         -> LEFT
else                                -> RIGHT
```

Because `u` and `v` are normalised, the diagonals follow the panel's aspect ratio rather than
cutting at 45 degrees — a wide panel gets wide top and bottom zones, which is the correct
behaviour and is what the sketch shows.

The centre rectangle is sized as a proportion of the panel, **clamped to a minimum and maximum in
physical units**, so that it stays reliably hittable in a small panel without swallowing a large
one.

## Decision

1. **A panel is dragged by its handle** — its tab or title gripper — not by its body. The body
   belongs to the panel's own content, which may itself be draggable. Grabbing the handle puts the
   drag into a distinct movable state.
2. **Hovering a target panel shows its five zones; the zone under the cursor highlights.** The
   highlight previews the *resulting* region, so the user sees the outcome before committing, not
   an abstract indicator.
3. **Releasing on an outer zone splits the target's space** along that axis and inserts the dragged
   panel on that side. The target keeps the remainder. The split defaults to even and is then
   adjustable by dragging the divider.
4. **Releasing on the centre zone forms a tabbed group**: the dragged panel takes the target's exact
   space and both become tabs within it.
5. **Dragging a tab out of a group detaches it**, which is the inverse of rule 4 and the only way
   the model stays learnable — every gesture has a visible undo gesture.
6. **The outer edges of the workspace are also drop targets**, docking against the whole window
   rather than any one panel. Without this, there is no way to create a full-height side column
   next to an existing row.
7. **Escape cancels an in-flight drag**, and a completed dock is a single undoable command
   (ADR-0015 rule 6). A mis-drop costs one keystroke, never a rebuilt layout.
8. **A drop that would leave any panel below its minimum usable size is refused** — the zone does
   not highlight and the drop does nothing. Layouts fail at the gesture, visibly, rather than
   producing a 3-pixel panel the user then has to find and fix.

## Deferred: dragging clear of the application

Releasing a panel outside the application window should tear it off into a floating window. This is
deliberately **deferred until the docked model is complete**, but the constraint is recorded now
because it changes how it must be built, and discovering it late would be expensive.

**Detection works.** Wayland gives the client an implicit pointer grab for the duration of a button
press, so motion events keep arriving after the cursor leaves our surface. We can tell precisely
when a drag has gone outside the window. What we cannot learn is what is *under* the cursor out
there - only that it is not us, which is all this gesture needs.

**Placement does not work, and cannot be made to.** This is the same wall as ADR-0015: `xdg_shell`
has no positioning request, so a newly created toplevel cannot be placed at the point where the
user released it. The panel will appear wherever the compositor decides. Hyprland will apply its
own rules; on other compositors it may be centred, tiled, or placed under a cursor-follows policy
we do not control.

So the Adobe behaviour - the window materialising exactly under your cursor - is **not reproducible
on Wayland**, and we should not pretend otherwise or spend effort chasing it. What we can do:

- Tag the torn-off toplevel via `xdg_toplevel_tag_manager_v1` (ADR-0015 rule 3) with a stable
  identifier, and **ship a documented Hyprland window-rule snippet** for Omarchy so the behaviour
  is good out of the box on our bedrock platform.
- Size the new window to the panel's previous size, which we *do* control, so only its position is
  surprising.
- Keep the gesture reversible: the panel remembers where it came from, so re-docking is one action.

Note that `xdg_positioner` does not rescue this. It positions `xdg_popup`s precisely relative to a
parent surface, but a popup is transient and dismissed on click-outside - it is not a window a user
can keep, focus and move. It is the wrong primitive for a persistent floating panel.

## Consequences

- The layout model is a **tree**: split nodes (with an axis and relative proportions) and tab-group
  leaves. This matches ADR-0015's requirement that only relative proportions are persisted, so a
  layout restores correctly at any resolution and on any monitor arrangement.
- Tab groups need a real tab bar with overflow handling, reordering, and a close affordance — a
  meaningful piece of work in its own right, not a detail of the docking gesture.
- Detached panels (ADR-0015 rule 3) are a separate toplevel each containing its own layout tree, so
  the same docking model works inside a torn-off window with no special cases.
- The drop-zone overlay is drawn by us, over live panel content, so it needs to composite cleanly
  against arbitrary backgrounds — the canvas in particular. Translucent fill plus a clear border,
  themed from Omarchy's palette.
- This is a differentiator worth the effort. It is the interaction model professionals already have
  in their hands from Premiere and After Effects, and it is strictly better than what OBS offers -
  which, per ADR-0015, cannot work correctly on Wayland at all.
