<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0039 — Nothing in this application scrolls by being dragged

- **Status:** accepted
- **Date:** 2026-10-17

## Context

With dragging live in the Sources panel ([ADR-0038](0038-the-sources-panel-drags.md)), the
operator found the gesture fighting the surface under it:

> some elemens are grabbing and scrolling the whole wind rathe rthan dragging but not all . no
> element should initiate scroll there should be no drag scroll anywhere in this app

Qt's `Flickable` is draggable by default, and every scrolling surface in this shell is one —
eighteen of them. A row that starts a drag and a list that starts a flick are the same press,
and which one wins is a race decided by `pressDelay` and drag thresholds.

The ruling is wider than the bug. A list in a studio tool is a list of things to *grab*: a
source to reorder, a strip to ride, a scene to take. A surface that slides when pulled will
eventually slide while someone is trying to pull on what is in it, and during a show that is
not a cosmetic failure.

## Decision

**Every `Flickable` in the shell sets `interactive: false`.** All eighteen, found by the one
line they all carry (`boundsBehavior: Flickable.StopAtBounds`) rather than by memory.

**The wheel comes back through `NoDragScroll.qml`**, declared inside each one. `interactive:
false` disables the wheel too — `QQuickFlickable::wheelEvent` returns early when the flickable
is not interactive — so turning off the drag without this would leave surfaces that cannot be
scrolled at all. It handles both a mouse's `angleDelta` (120 units a notch, three rows) and a
touchpad's `pixelDelta`, and sends the wheel sideways when sideways is the only way the view
can move, which is what the mixer's horizontal strip list needs.

**A control that wants the wheel declares its own `WheelHandler`.** Qt offers a wheel event to
handlers from the innermost item outward, so a deeper handler wins and the enclosing
`NoDragScroll` never sees it. `MixerFader`'s wheel moved from its `MouseArea`'s `onWheel` to a
handler for exactly this reason: a `MouseArea` does **not** win that contest, and until it
moved, a wheel over a fader scrolled the strip list instead of trimming the fader.

## Rejected alternatives

- **`interactive: false` alone.** What the one-line version of this change would have been,
  and it silently breaks the wheel everywhere. The self-check asserts both halves because
  fixing one by breaking the other is the obvious failure here.
- **Turning it off only where a drag exists.** The operator asked for it everywhere, and
  "everywhere except the panels that have not got a drag gesture yet" is a rule that decays:
  the next panel to gain one would reintroduce the bug.
- **Raising `pressDelay` or the drag threshold so the row wins the race.** Tuning a race
  rather than removing it. The surface would still move, just less often, which is worse than
  either extreme.
- **A `ScrollView` wrapper per panel.** Brings QtQuick.Controls styling into a shell that
  draws its own everything (ADR-0015), for scrollbars nothing has asked for.

## Consequences

- `NoDragScroll.qml` is new and appears eighteen times. It takes no arguments: a pointer
  handler's `parent` is the item it was declared in, and it walks up to the nearest
  `Flickable` so it works whether the declaration lands on the view or on its content item.
- One bug in writing it is worth recording, because it looks right: `acceptedModifiers:
  Qt.NoModifier | Qt.ShiftModifier` reads as "plain or shifted" and is not. `NoModifier` is 0,
  so the OR is exactly `ShiftModifier` and **a plain wheel stops working**. The property is
  left at its default and the handler reads the modifier itself.
- The self-check walks the live scene graph for `QQuickFlickable` and fails if any is
  interactive, so a panel added later cannot quietly reintroduce drag-scroll. It also turns a
  real wheel over a long list and over a fader, because those are the two things this change
  can break.
- Not covered: no scrollbars. Nothing indicates that a list has more below it, which matters
  more now that a list cannot be flicked to find out. Filed in `docs/todo.md`.
