<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0033 — Deleting a source from the manager takes its placements with it

- **Status:** accepted
- **Date:** 2026-10-17

## Context

`Project::sources` has always been a flat, project-level list: a `Source` has no canvas and no
scene, and a scene holds *nodes* that reference one. So one camera opened once can appear in a
Main scene and a Vertical scene, which is what [ADR-0027](0027-scenes-belong-to-a-canvas.md)
means by canvases sharing sources. What the shell never had was a way to *remove* a source.
`RemoveSource` existed as a command and nothing in the UI called it, so the source list only
ever grew.

The operator asked for the Add Source dialog to become that manager:

> I'm starting to thing that we need a global source manager. as a matter of fact I think that
> we can just reuse the add source mapenl for this and make ti the source manager. then all we
> have to do is make it so in the area with teh existing sources of a type are show build a way
> to remove them. for now just a right click on them and select delete. then show a
> confirmations dialog to make sure.. If the source s is in use in a scene somewhere you list
> the scenes that it isxeest in and then you tel them it will be removed from all fo those
> scenes and then ahve a double confirm.

That last clause is the interesting one, because it contradicts what `RemoveSource` does.

## Decision

**The Add Source dialog is the source manager.** No second window: it already lists every
source by type, which is the whole content of a manager, and a separate one would be the same
grid with a different title.

**Right-click a source tile to delete it.** The dialog's subheading says so, because a gesture
with no visible affordance is a gesture nobody finds.

**Deleting from the manager removes the source *and* every node bound to it**, as one `Batch`
— so one Undo puts all of it back. This is deliberately *not* what the bare `RemoveSource`
command does, and the difference is the point:

- `RemoveSource` alone leaves those nodes bound to nothing, which `validate()` reports as a
  warning. That is the right behaviour for the *command*, under
  [ADR-0023](0023-the-command-bus.md): refusing would mean a source could never be deleted
  while anything referenced it, and the command layer does not get to decide what an operator
  meant.
- The *manager* is a place where an operator has been shown the consequence in words and has
  agreed to it twice. "It will be removed from Live, Intro, Outro" is a promise, and leaving
  three placeholders behind would break it.

Both stay. The command is the primitive; the manager is a gesture built from it.

**One confirmation when the source is unused, two when it is in use.** The first asks, and says
how many scenes are affected. The second names them. So the destructive press is never the one
aimed at the menu item, and the operator reads the list of scenes *before* agreeing rather than
discovering it afterwards.

**The scenes are named, not counted.** "Removed from 3 scenes" is a number; "removed from Live,
Intro, Outro" is a decision. Each scene is listed once however many placements it holds, since
the question is which scenes change.

## Rejected alternatives

- **A separate Source Manager window.** The operator's own suggestion was to reuse this dialog,
  and it is right: the grid already is the manager.
- **Refusing to delete a source that is in use.** The behaviour `RemoveSource` deliberately
  avoids (ADR-0023). It makes a source permanently undeletable until the operator hunts down
  every placement by hand, which is exactly the work the manager should be doing.
- **Deleting the source and leaving the nodes**, i.e. calling `RemoveSource` directly. Honest
  to the command layer but dishonest to the dialog, which just said the source would be removed
  from those scenes. It also leaves scenes containing nodes that draw nothing and that the
  operator did not put there.
- **Letting the second confirmation choose between the two.** Considered, and rejected as a
  decision nobody wants to make while deleting something. The rebind case — "I want the hole,
  I am putting a different camera in it" — is better served by rebinding the node, which the
  Sources panel already does.
- **A single confirmation for everything.** Fine for an unused source, and this does exactly
  that. Not fine when the press silently edits three other scenes.

## Consequences

- `Document::scenesUsing` and `Document::removeSourceAndUses` join the invokables.
  `removeSource` stays, is still the plain command, and is still what anything else should use.
- Deleting a source used ten times is one undo step. The batch holds one `RemoveNode` per
  placement plus the `RemoveSource`, and `Batch` already inverts as a whole.
- The dialog keeps its name in the code (`AddSourceDialog.qml`) while being the manager. Worth
  renaming when something else touches it; not worth a rename commit that only moves a file.
- Nothing stops a source being deleted while it is on air, because nothing is on air yet. When
  the render path exists, that is a question for the session process rather than this dialog.
