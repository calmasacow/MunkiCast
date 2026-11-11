<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0038 — The Sources panel drags, the way a layers panel does

- **Status:** accepted
- **Date:** 2026-10-17

## Context

[ADR-0037](0037-more-than-one-selected-source.md) added grouping and, lacking a drag, put
"move into a group" on the right-click menu as one entry per group. The operator rejected it on
sight:

> ok I dont want to have to right click to add to group and that is gonna turn into problem if
> we have a setup witha lot of nested groups that list will get really long I want drag to
> reorder or into a group jsut like the way laers and groups wwork in the photoshop layers
> panel

Right on both counts. The menu grows with the project, and reordering was already two buttons
(▲ ▼) that only move a node within its own parent — so the panel had no gesture for the thing
a layers panel is *for*.

## Decision

**Press and move to drag, drop on a row.** A drag starts only after the pointer has moved four
pixels, so a click with a shaky hand is still a click.

**Three zones per row**, as in Photoshop: the top sliver is *above*, the bottom sliver is
*below*, and the middle is *into* — but only on a group. A row that is not a group has no
middle: its height splits in two, because there is nothing to go inside. Thresholds are 7px and
19px of a 26px row, so the two edge zones are easy to hit deliberately and hard to hit by
accident.

**Above and below place the node among the target's own siblings**, at whatever depth the
target sits. That is how a nested tree is navigated by dragging: to put something inside a
group you drop *into* the group or *above* something already in it.

**Dragging a row outside the selection takes just that row**, and selects it. The selection is
not a cargo net — picking up something you had not selected should not bring six other things
with it.

**A drop into a node's own subtree is refused**, which is the one move that would stop the tree
being a tree. `canDropAt` answers it and the panel asks before drawing, so the indicator never
appears where the drop would fail. No refusal message, because the operator never gets to
attempt it.

**The feedback is three things at once**: the dragged rows fade to 0.4, a 2px accent line sits
between rows for above/below, and a group receiving an *into* drop is outlined rather than
line-marked. A line at the top edge of a group would be indistinguishable from "above the
group", which is a different move.

**The menu's "Move to «group»" entries are gone.** "Move out of group" stays, because there is
nothing above the top level to drop onto.

## Rejected alternatives

- **Keeping the menu as well as the drag.** Two ways to do one thing, one of which grows
  without bound. `groupsForMove` was deleted rather than left unused.
- **QtQuick's `DragHandler` / `Drag.active` with a `DropArea` per row.** The panel is a
  `ListView` of 26px rows over a flattened tree; a drop area per row plus a third of a row's
  precision is more machinery than tracking one pointer position against an index. The panel
  already knows its row height.
- **Reordering by dropping between rows only, no "into".** Then a group can only be filled by
  dropping onto something already inside it, and an empty group can never be filled at all.
- **Letting a drag reparent across canvases.** The panels are per canvas (ADR-0027) and a node
  belongs to a scene; a drag that left the panel would mean something the model does not.

## Consequences

- `dropSelectedAt`, `canDropAt`, `nodeAtRow` and `rowOfNode` are new on `Document`. The last
  two exist so the panel can turn a pointer position into a target without reading model roles
  by numeric offset — the mistake that silently re-pointed four assertions last week.
- A multi-row drag moves each node in order, and the landing index is decremented when a node
  is moving *down* within its own parent, because every node before it shifts up as it leaves.
  That off-by-one is the whole reason this is one function rather than a loop at the call site.
- `moveSelectedIntoGroup` stays, now reached only by "Move out of group". A drop calls
  `dropSelectedAt`.
- The self-check drives a real press-move-release through the window, and asserts that a
  two-pixel wobble is still a click. Testing `dropSelectedAt` alone would prove the arithmetic
  and nothing about whether a drag reaches it.
- Not built: no auto-scroll when dragging past the top or bottom of a long list, and no
  spring-loading (hovering a collapsed group to open it) — groups do not collapse yet.
