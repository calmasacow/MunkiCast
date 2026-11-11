<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0034 — A scene inside a scene is one node, and it is live

- **Status:** accepted
- **Date:** 2026-10-17

## Context

Asked what "a scene can live on two canvases" meant, the operator described the thing rather
than the mechanism:

> lets say taht I make a sceens that has all of my social media elemens and mabe a lower 3rd
> and let say a browser based crawl acroos the bottome. I can then take that scene and I can
> the add or copy that scene into another scene on any canvase.

That is OBS's nested scene, and the model already had it. `NodeKind::Precomp` and `PrecompData`
have been in `node.hpp` since the node model was written — "a whole scene rendered offscreen at
its own resolution and colour space" — and `validate()` has carried `check_precomp_cycles()`
for as long. What was missing was any way to make one: `make_node` knew the kind, and nothing
called it with that kind.

So this is a gesture for a model feature, not a model change.

## Decision

**Adding a scene lives in the add dialog**, as a "Scene" entry at the bottom of the kind list.
That dialog answers "what goes in a scene", and another scene is one of the answers — which is
where OBS puts it too.

**"Scene" is not a source type.** It is listed by a new `Document::addableKinds`, which is
`sourceTypes` plus this one entry. Putting it in `sourceTypes` was tried first and the
self-check rejected it within a minute: every consumer of that list treats its members as
source types, and the suite already asserted that each one has a settings schema. A scene has
no schema, cannot be made from the "+ Add new" row, and is not in `Project::sources`. The
dialog's list and the project's source types are different lists that happened to be the same
until now.

**It is a live reference, not a copy.** One overlay scene used in six places; edit the crawl
once and all six change. That is what makes the operator's case worth having — the alternative
is six copies of a lower third to maintain, which is the thing they were avoiding by making it
a scene in the first place.

**The node's size stays (0,0)**, which `PrecompData` defines as "the size of the canvas this is
being composited into". That is the honest answer to the question that dogged the earlier
copy/paste discussion: a 2560x1440 overlay scene dropped into a 1080x1920 vertical scene is
*told what to render at*, rather than having its contents scattered off the edge of a narrower
frame. Its colour space is taken from the canvas it was authored for.

**A loop is refused by the command, not only reported by the validator.** `AddNode` now checks
whether the node being added reaches, through any chain of precomps, the scene it is being
added to, and refuses with a message naming both scenes. The picker also omits those scenes, so
the refusal is a backstop rather than the usual path.

This is a departure from how every other reference in this model behaves, and the reason is
specific: a node bound to a deleted source draws nothing, a link to a missing scene never
fires, a dangling bus is a warning — all of them degrade. A precomp cycle does not degrade. It
has no bottom, and a renderer walking it recurses until it dies. `validate()` already called it
an error; a command that cheerfully creates a state the validator calls an error is a command
that lets the UI build an unrenderable project.

## Rejected alternatives

- **Copy the scene's contents in as ordinary nodes.** The operator's words allowed either
  reading ("add or copy"), and the interview settled it: live reference. A copy is sometimes
  wanted as a starting point, but it is a different gesture and should say so rather than
  being what "add a scene" quietly does.
- **A separate "Add Scene" dialog or a Scenes-panel gesture.** Two places to answer "what goes
  in this scene" is one too many; the operator picked the add dialog.
- **Letting `validate()` be the only cycle check.** Covered above. It is the right place to
  *report* it and the wrong place to be the only guard.
- **Refusing the cycle in the UI only.** Then a project built by any other means — a hand-
  edited file, an imported one, a future scripting surface — can still contain one. The
  command layer is where document invariants belong (ADR-0023).
- **Giving the precomp the shown scene's own size instead of (0,0).** Would mean an overlay
  authored on Main arrives at its full 2560x1440 inside a vertical canvas and has to be scaled
  by hand every time. (0,0) already means the useful thing.

## Consequences

- `Document::addSceneToScene`, `Document::scenesForPrecomp` and `Document::addableKinds` are
  new. The first is one `AddNode`, not a batch: the node carries its own data.
- The cycle check runs twice — once when listing what can be added, once when adding. The
  picker's copy is what the operator sees; the command's is what makes it true.
- `scenesForPrecomp` returns tiles shaped like source tiles, including `useCount` (how many
  other scenes show this one) and an `icon`, because the same QML delegate draws both. Fields
  are filled rather than left absent: an undeclared field renders as the literal word
  "undefined", which this shell has shipped once already.
- Still missing: nothing draws a precomp, because nothing draws anything. The node appears in
  the Sources panel and the document is correct; the picture waits on the render path.
- Deleting a scene that another scene shows is not yet refused or warned about. `RemoveScene`
  predates this gesture and should probably say something; filed in `docs/todo.md`.
