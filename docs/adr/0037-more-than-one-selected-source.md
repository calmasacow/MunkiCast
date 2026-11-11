<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0037 — More than one selected source, and what follows the leader

- **Status:** accepted
- **Date:** 2026-10-17

## Context

The Sources panel has had single selection since it was written, and `Document::selected_node_`
is one string that four panels read. The operator asked for the ordinary thing:

> UX think user should be abl ot select more than one Item in the sources panel. userning ctrl
> and shift click as it typical in any other apps and theny when we tack hot keys we will ad a
> ctrl+G hot key for createin a group from the items selected

Nothing in the model stood in the way — `AddNode` and `MoveNode` exist and batch — but
selection is read by the Properties panel, the mixer's routing line and the settings gesture,
all of which mean *one* node.

## Decision

**A leader and a rest.** `selected_node_` keeps its meaning — the node last clicked — and
`also_selected_` holds the others. Every existing reader of `selectedNode` keeps working
unchanged, which is why this is a small change rather than a sweep through four panels.
`selectedNodes` returns all of them **in scene order**, not click order, because what acts on
the list cares which is topmost and the operator reads the panel top to bottom.

**Ctrl toggles, Shift takes the run, a plain click collapses to one.** What every other list
does. Shift measures from an anchor — the last node picked without Shift — so dragging a range
back and forth keeps measuring from the same end instead of walking.

**Ctrl-clicking away the leader moves the leadership** to whatever is left, rather than leaving
the Properties panel pointed at a node that is no longer selected.

**The Properties panel goes blank above one selection**, showing the count and "Pick one to
edit its properties." Chosen by the operator over editing all of them or showing the leader's
values. Showing the leader would be the quiet trap: three rows highlighted, one node's numbers
on screen, and an edit that looks like it applies to all three and does not.

**Ctrl+G groups the selection, and the group lands at the topmost selected node's place.** So
the grouped items keep their draw order relative to everything else. The alternative — always
frontmost — silently changes what is drawn over what, which is a composition change disguised
as a tidy-up.

**A selected node already inside another selected node is dropped from the move.** Selecting a
group and something inside it plainly means "group these", and the inner one is already coming
along inside its parent. Refusing the whole gesture would be pedantry about a selection the
operator made on purpose.

**A right-click on something already selected does not change the selection**, so the menu can
act on all of it. A right-click elsewhere selects that one first, which is what every file
manager does.

## Rejected alternatives

- **One selection list, with no leader.** Cleaner in the abstract, and it would have meant
  touching every panel that reads `selectedNode` plus deciding what each does with zero or
  many. The leader already exists and already means the right thing.
- **Click order rather than scene order.** The group gesture needs "topmost", and an operator
  reading three highlighted rows expects them to act in the order they are drawn, not in the
  order they happened to be clicked.
- **Properties editing all selected nodes.** Offered and declined. It is the more powerful
  answer and a much larger one: every field needs a mixed-value state and every setter becomes
  a batch. Worth revisiting; not worth bundling into the selection change.
- **Refusing to group nodes with different parents.** Would mean dragging a source out of a
  group before it can join another, for a rule the operator never asked for.
- **Selection surviving a canvas switch.** The nodes it names are not in the scene being shown.
  Cleared instead.

## Consequences

- `selectedNodes`, `selectNodeExtended`, `groupSelectedNodes` and `parentOfNode` are new.
  Grouping is one `Batch`: an `AddNode` for the group and a `MoveNode` per member, so one Undo
  takes them all back out.
- `edit.group` joins the catalogue on `Ctrl+G`, rebindable like everything else, and the
  right-click menu carries "Group N sources" so it is reachable without the shortcut.
- A non-leading selected row is drawn a shade down from the leader. A range has to read as a
  range while it stays obvious which node Properties *would* be showing.
- Ungroup followed the same day, on the row's right-click beside Rename. Its children land at
  the group's own place rather than at the top of the scene, so a group/ungroup round trip
  leaves draw order where it was, and the freed children end up selected -- which is what the
  operator is looking at. They keep their own transforms, so whatever the group's transform
  was doing to them is lost; a group here is a real container (ADR's node model), not OBS's
  presentation-only folder.
- `Ctrl+A` selects the scene's top-level sources. Not their children: taking those too would
  mean a group and its contents both selected, which every operation then has to undo again
  with `inside_any`.
- Moving a source into an existing group is a right-click entry, `Move to "..."`, listing the
  groups it could go to and omitting the ones it cannot -- itself, or one inside the
  selection. There was otherwise no way to do it but making a new group. The gesture people
  reach for is a drag, which this panel cannot do yet; `moveSelectedIntoGroup` is what a drop
  would call when it can.
- Not built: no rubber-band selection.
- Delete and copy act on the whole selection, each as one `Batch`, so removing or pasting six
  is one undo. `removeSelectedNodes` and `copySelectedNodes` sit beside `groupSelectedNodes`
  and all three share `inside_any`: a node already travelling inside a selected parent is
  dropped, or it would be removed twice and pasted twice.
- The clipboard holds a list rather than one node, so a paste restores the whole selection and
  selects it -- which is what makes "copy, paste, drag the copies" work.
- `Ctrl+Shift+G` ungroups, matching the convention beside `Ctrl+G`. `layer.delete` is handled
  in the shell rather than resolving to a `RemoveNode` in the session, because it acts on a
  selection and the session does not have one; it had never been wired to anything before
  this, so `Delete` did nothing at all.
