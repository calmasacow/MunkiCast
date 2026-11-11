<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0027 — Scenes belong to a canvas, and a link is a cue rather than a copy

- **Status:** accepted
- **Date:** 2026-10-13
- **Depends on:** ADR-0020 (canvases are independent pipelines), ADR-0022 (the project is a
  directory), ADR-0023 (the command bus), ADR-0025 (the safe state)

## Context

A scene today is **project-level and canvas-agnostic**. `Project::scenes` is one flat list, and
what makes a scene look right on a portrait canvas lives in each node's `canvas_overrides` rather
than in a second scene. That claim was never argued in an ADR — it is a doc comment on `Scene` and
a paragraph in `docs/todo.md` — and the reasoning behind it was that two copies of a scene drift,
and the copy that drifts is always the one nobody is looking at.

That reasoning is sound about *copies*. It turned out not to be about *shows*.

The operator runs a widescreen programme and a vertical one at the same time. A vertical show is
not the widescreen one squashed; it is a different arrangement of the same cameras. Authoring it
as a pile of per-node overrides on the widescreen scene means authoring it **as a diff** — you
never see the vertical show, you see the places it disagrees with another show. The override map
also has to grow a key for every property anyone ever wants to differ, and every key is a second
code path at composite time.

Asked directly (2026-10-13), the operator settled it:

- Canvases are **completely independent**, and each owns its own scene list.
- **Nothing about a scene's contents ever crosses canvases.** Adding a camera, moving it, hiding
  it, deleting a scene, removing one source — none of it propagates.
- A link is not about content at all. It is about **what is on air**: when a scene is activated on
  the leading canvas and that scene has a link, the other canvas changes to the linked scene. With
  no link the other canvas does not move.
- Links are **one-way**. A link points from a scene on one canvas to a scene on another;
  activating the far side does nothing to the near side.
- A source used on both canvases is **one source, opened once**, as in OBS.

## Decision

### 1. A scene belongs to exactly one canvas

`Scene` gains `Id canvas`. `Project::scenes` stays a flat list rather than becoming
`Canvas::scenes`, because ADR-0022 stores one scene per file in `scenes/<ulid>.json` with a
manifest in `project.json`; nesting scenes inside canvases would either change that layout or
make the in-memory and on-disk shapes disagree about what a scene is. A flat list with an owner
field keeps both.

### 2. Nothing about contents is shared, so `canvas_overrides` goes

`Node::canvas_overrides` and `CanvasOverride` exist for exactly one purpose: making a single
shared scene look right on a second canvas. There are no shared scenes any more, so the field has
no purpose, and a field with no purpose is one that every later build must keep reading, writing
and honouring. It is deleted along with `SetNodeCanvasOverride`.

### 3. A link is a directed cue between two scenes on different canvases

```
struct SceneLink {
    Id id;
    Id from;   // a scene; activating it on its canvas's deck fires the cue
    Id to;     // a scene on a *different* canvas
};
```

They live in `Project::scene_links` rather than on either scene. A relationship is not a property
of either end, and deleting a scene has to prune every link that names it — one list is one place
to prune, and the linking panel is a view over that list rather than a walk of every scene.

When `from` becomes the program scene of its canvas's deck, the deck targeting `to`'s canvas takes
`to` to program using **that deck's own transition**, not the leading deck's. Two canvases are two
pipelines (ADR-0020) and a vertical show may well cut where the widescreen one dissolves.

Three rules keep it predictable:

- **Both ends must be on different canvases.** A link inside one canvas is a scene that activates
  itself.
- **At most one link from a given scene to a given canvas.** Two would be a cue with no answer to
  "which one". A scene driving *several* canvases is fine and is the three-canvas case.
- **One hop, never a chain.** If A drives B and B drives C, activating A takes B and stops. A
  chain is a cue sequence that no panel can show you, and with one-way links across three canvases
  it can close into a cycle. Anyone wanting C adds A → C, which is visible in the list.

### 4. Sources stay project-level and shared

`Project::sources` does not move. One camera, opened once, referenced from as many scenes and
canvases as you like. A per-canvas source list would mean either opening one device twice — which
fails outright — or a second list of references that must be kept in step with the first.

### 5. A cue can never take the emergency scene

`Canvas::emergency_scene` stays per canvas and is never the `to` of a link. The safe state is not
a cue; it is what happens when cues stop arriving (ADR-0025). A safety net that something else can
fire is not a safety net.

### 6. Main is the first canvas, and cannot be deleted or renamed

The cue model needs a leader, and the operator's answer is the one OBS gives: `canvases[0]` is
Main, it exists from the moment a project does, and nothing can remove or rename it.

"The first one in the list" is normally a bad rule, because reordering silently changes meaning.
It is safe here only because Main cannot be moved out of first place, deleted, or renamed — the
immutability is what makes the positional rule honest, so the two must ship together. `AddCanvas`
at index 0, `RemoveCanvas` on Main and `SetCanvasName` on Main are all refused by the command
layer, not merely hidden in the UI (ADR-0023: a command validates itself).

Links therefore always run **from a scene on Main to a scene on another canvas**. The chain icon
appears only on non-Main canvases, which is what makes the linking panel's direction
unambiguous without ever drawing an arrow.

### 7. Migration

`kSchemaVersion` goes to 2. A version 1 project has one flat scene list and may carry
`canvas_overrides`:

- Every scene is assigned the **first canvas**, which is the only one the shell has ever worked
  against.
- `canvas_overrides` are dropped. If any are present the loader **reports how many it dropped and
  on which scenes**, through the same channel that reports a quarantined scene file (ADR-0022).
  Silently discarding authored work is how a format bump earns a reputation.

## Rejected alternatives

**Keep scenes canvas-agnostic and grow the override map.** Today's model. Rejected because it makes
authoring a vertical show an exercise in editing a diff, and because the map must gain a key for
every property anyone ever wants to differ — each one a branch at composite time and a case in
every serialiser. The drift argument it was built on only applies to copies that are *meant* to be
the same thing; two shows are not.

**Full duplication with content synchronisation.** What `docs/todo.md` guessed the answer would be:
a link with a declared scope, where adding a layer follows but moving it does not. Rejected by the
operator, and rightly — it requires answering "which properties follow" for every property, and
every answer is wrong for somebody. It also puts a second author on your scene: a vertical
operator's edit changes the widescreen show.

**Two-way links.** Rejected: a vertical operator could change what the main programme is
broadcasting by touching their own canvas. One canvas leads and it is the show.

**Links as a field on `Scene` (`Scene::drives`).** Rejected: deleting a scene then means walking
every other scene in the project to prune references to it, and the linking panel becomes a join
rather than a list.

**`Canvas::scenes` as owned vectors.** Rejected: it contradicts the on-disk layout in ADR-0022,
which is one file per scene with a manifest, and makes the thing a scene file is a child of
ambiguous.

## Consequences

**Easier.** A vertical show is authored as a show. Each canvas's scene list is its own, which is
what the scenes panel already looks like it is showing. The linking panel is a table view over
`scene_links` with add and remove, and it is the only place links exist.

**Harder, and new.** The shell currently takes `canvases.front()` and says so in a comment — every
panel is implicitly working against canvas zero. Scenes, the sources panel and the properties
panel all become canvas-scoped, so "the canvas I am working on" has to stop being a placeholder
and become a real selection in the UI. That is the bulk of the work, not the model change.

**The command set grows and changes.** `AddScene` carries a canvas. `AddSceneLink` and
`RemoveSceneLink` are new. `SetNodeCanvasOverride` is deleted. The forty-command count moves, and
the README says forty.

**Must be tested.** A link whose target scene was deleted; a link whose target *canvas* was
deleted; the one-hop rule, with a three-canvas project arranged so a chain would close a cycle;
that designating an emergency scene refuses a scene that is the `to` of a link, and that a link
refuses an emergency scene; that a version 1 project with overrides loads, reports, and round-trips
as version 2; and the existing property test — random command sequences, undo and redo returning
the project byte for byte — extended over the two new commands.

## Implementation note, 2026-10-14 — the model half, and two things this ADR did not settle

The document model landed on this date: `Scene::canvas`, `Project::scene_links`, `AddSceneLink`
and `RemoveSceneLink`, the deletion of `Node::canvas_overrides` and `SetCanvasOverride`, Main's
immutability in the command layer, the version 1 → 2 migration, and a `golden-project-v2`
fixture beside the frozen v1 one. **The UI half named under "Harder, and new" is not built**: the
shell still works against `canvases.front()`, so a new scene lands on the first canvas and the
scenes panel still shows every canvas's scenes at once.

Two questions this ADR answers in two different ways, resolved here rather than left to the
reader:

**Must a link start on Main?** Section 6 says links "always run from a scene on Main"; section 3
discusses A cueing B cueing C and answers it with the one-hop rule. The command enforces section
3's three rules — different canvases, at most one link from a scene to any one canvas, never
cueing an emergency scene — and **does not** require `from` to be on the first canvas. The
linking panel only ever offers Main's scenes, so "main leads" is enforced where the operator
makes the choice; and because firing is one hop, a chain that somehow got stored does nothing
surprising. ADR-0023's rule decides it: a command may only refuse what the model itself refuses,
and "a link from a non-Main scene" is a policy about which links are worth making, not a state
the model has no meaning for.

**An emergency scene now has a canvas of its own.** `Canvas::emergency_scene` is per canvas
precisely so a vertical show gets a separately authored vertical clip — and now that scenes have
owners, a canvas can name a net that belongs to a *different* canvas, which is the letterboxed
landscape that reasoning rejects. `validate()` warns. A warning and not an error: it does render,
it just renders wrong, and the v1 fixture does exactly this, so making it an error would mean a
project that opened yesterday fails to open today over something nobody has had a chance to fix.

**There is no `SetSceneLink`.** A link is nothing but its two ends, so changing one is removing it
and adding another, and the linking panel commits a whole tick-list as one `Batch` anyway.

**A batch now flattens nested inverses.** Removing a scene has to put its links back, so
`RemoveScene`'s inverse is a `Batch` — and a batch cannot contain a batch. `Apply(Batch)` splices
a member's batch inverse flat, keeping that member's own order while reversing the members. Found
by the existing random-sequence property test, which is what it is for.

## Amendment, 2026-10-14 — the operator settles the direction, and it is a model rule

The implementation note above resolved "must a link start on Main?" by saying no, and leaving
it to the panel. The operator has since stated the rule, and it is narrower than either reading
of this ADR:

> main scenes cant be linked to main scenes and no linking occurs on main itself. in the scenes
> panel of a "non-main" canvas the linking icons will exist on a scene that can be clicked and
> then a list of all of the "main canvas" scenes will be shown, where one or several can be
> selected. Secondary canvases can not link scenes from canvases other than main. Scenes in Main
> canvas have no linking icons at all. linking is purely a secondary canvas function and all
> links are only to main scenes.

So the shape is fixed, and it is the *reverse* of how the popup was being imagined. A secondary
scene's link list does not answer "what do I cue" — it answers **"which Main scenes bring me
up"**. Several Main scenes may select one secondary scene; that is the common case, not the edge
one, and the model already allows it because the one-per-canvas rule is keyed on the *source*.

### The source of a link must be a Main scene, and the command enforces it

Reversing the earlier decision. ADR-0023 asks whether a refusal is about a state the model has
no meaning for, or merely a policy about which states are worth reaching — a command may only
refuse the first. The earlier note put "a link from a non-Main scene" in the second bucket. That
was wrong, now that the operator has said what a link is: linking is a function of a secondary
canvas, Main carries no link control anywhere in the product, and nothing watches a secondary
canvas for cues. A link whose source is not on Main is not a link that could ever fire. That is
a meaningless state, not an unfashionable one, so `AddSceneLink` refuses it.

`validate()` warns rather than errors on one that is already stored, by the same reasoning as
every other dangling-reference warning here: a project written by hand or by yesterday's build
still opens and still renders, and the link simply never fires.

This also makes section 6 of this ADR correct as written and retires the section 3 vs section 6
conflict: links always run from a scene on Main. The one-hop rule in section 3 stays, but it now
describes something unreachable rather than something merely harmless.

### Ticking a Main scene that is already taken moves the link

If Main's "Camera A" already brings up "Wide" on Vertical, and the operator ticks "Camera A" in
the popup for Vertical's "Tight", the link **moves** — and the popup says so in words, naming
the scene that is losing it. The alternative, refusing the tick and sending the operator to the
other scene's popup to clear it first, makes re-pointing one cue a two-popup job for no safety:
the move is visible, undoable in one step, and the thing being protected is a link the operator
is in the middle of re-aiming on purpose.

The model rule is unchanged; the panel commits the move as `RemoveSceneLink` + `AddSceneLink`
inside one `Batch`, which is already how a tick-list commits.

### "Clear links" unticks, and does not commit

"Link" is the only commit point in the popup. "Clear links" empties the tick-list and writes
nothing, so closing the popup any other way — Escape, the window button — always leaves the
project exactly as it was found. Clearing and re-linking in one visit is therefore one undo
step, not two, and the chain icon does not change colour until something is actually stored.

### The Canvas panel is a tab beside Outputs

Asked where it should live by default; the operator's answer was that it is just a panel so it
could go anywhere, but by default it should be a tab in the same space as the Outputs panel.
That is also the arrangement that makes the two panels' footers read as one idea: both get the
same `+ − gear` row, and in both the gear opens a manager window rather than editing in place.

### Each canvas gets its own Scenes and Sources panels

Superseding the first cut of the UI, which had one scenes panel filtered to whichever canvas
was current. The operator's correction:

> the sources and scenes panels need to be "Sources: (CanvasName)" and "Scenes: (CanvasName)"
> so when a secondary canvas is created not only is a Canvas Panel created ... but separate
> Source and Scene panels for that canvas must be created

A filter and a second panel are not the same thing, and the difference is the point of having
more than one canvas. Filtering shows one canvas at a time: to put a camera on the vertical
scene you leave the landscape one, and the two can never be seen together. Separate panels show
both at once, which is what building a dual-canvas show actually looks like.

So each canvas has its own scene list, its own node tree, and **its own selected scene** —
switching canvas and switching back lands where you left off, because the selection was never
shared. Panel ids are `scenes:<canvasId>` and `sources:<canvasId>`, and creating a canvas
creates its pair, tabbed beside Main's; removing the canvas removes them, because a tab titled
for a canvas that no longer exists can never show anything and the operator did not close it so
would not think to. A pair is created once, the first time its canvas is seen: after that
whether they are on screen is the operator's business, and re-adding one they closed would be
the shell arguing with them every time the document changed.

**Main keeps the plain ids `scenes` and `sources`.** It is the one canvas that always exists,
cannot be removed and cannot change places, so it is the one whose panel id can be a constant —
and that is also what lets a layout file written before any of this keep loading. The title is
still resolved: "Scenes: Main".

Two things follow from there being more than one of these panels at once. The canvas the shell
is working on now **follows the click** — picking a row in "Scenes: Vertical" is how an operator
says "Vertical now", and requiring a trip to the canvas list first would be a step that exists
only because of how the selection is stored. And `setCanvasEmergencyScene` takes its canvas from
the *scene* rather than from whichever canvas is current: a scene belongs to exactly one canvas,
so "which canvas is this the safety net for" has one answer, and it must not be possible to get
it wrong by having the wrong panel focused.

### And its own Program panel, 2026-10-15

The amendment above read "Separate Source and Scene panels for that canvase must be created" as
two panels. It is three. Looking at the built shell the operator said:

> so question I see that there are sources and scenes panels for main and verical. But there is
> not vertical canvase itself

The *canvas itself* — the picture — had been missed, because the original "Canvas Panel" in that
sentence was read as the canvas *list* that now sits beside Outputs. A second canvas you cannot
see is one you cannot frame anything on, so `program` joins `scenes` and `sources` as a
per-canvas panel kind, on exactly the same rules: `program:<canvasId>`, Main keeps the bare
`program`, created with the canvas and removed with it, tabbed beside Main's.

One thing is different from the two lists, and it is why this is not purely a workspace change.
A scene list looks the same whatever canvas it is of; a canvas does not. The panel drew a fixed
16:9 frame, so a 1080x1920 vertical canvas would have been letterboxed into landscape and read
as a second horizontal output — the exact confusion the separate panels exist to prevent. The
frame now takes its aspect from the canvas's own `width`/`height` through
`Document::canvasSizeOf`, and says the resolution under the name, so the panel is wrong in a way
that is visible rather than wrong in a way that looks plausible.
