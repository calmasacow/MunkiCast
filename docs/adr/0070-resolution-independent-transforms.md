<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0070 - Resolution-independent transforms

- **Status:** accepted
- **Date:** 2026-11-13
- **Supersedes:** the stance in `libs/ldoc/include/ldoc/types.hpp` that a node's box belongs in
  canvas pixels
- **Depends on:** ADR-0013 (rational rates, never doubled), ADR-0027 (a vertical show is its own
  canvas, not a repositioned one)
- **Does not touch:** ADR-0020. That ADR is about what a canvas *costs* -- one more canvas is one
  more composite, and canvases must not serialize. Nothing here changes that.

## The problem

Changing a canvas from 1080p to 4K leaves every node exactly where it was, in pixels. A show
built at 1080p and reopened at 4K has its lower-left quarter occupied and the rest black. The
operator's fix today is to re-place every source by hand.

`SetCanvas` (`libs/ldoc/src/command.cpp`) validates and swaps the `Canvas` struct and does not
look at a scene. Only two things in the project react to the change at all: an encode's
width/height follow the canvas unless `encode.rescale` is set, and downscaled asset copies go
stale. Neither helps the geometry.

## What is already right

The model does not need a new concept. It already has the pair OBS calls a bounding box:

- `Layout.size` is the node's box, and `FitMode` says how the source's pixels fill it
  (`Natural`, `Stretch`, `Contain`, `Cover`, `FillWidth`, `FillHeight`).
- `Layout.alignment` and `Transform.pivot` are **already** normalized 0-1.
- `PrecompData::size` is **already** relative: `(0, 0)` means "the size of the canvas this is
  composited into".

So the separation `types.hpp` argues for -- where a node sits versus how its image fills its box
-- is kept. That separation is the reason OBS bounding boxes confuse people, and it stays. What
changes is only the **unit** the box and the position are measured in.

## Decision

**A node's position and box are stored as fractions of the canvas, not pixels.**

- `Transform.position` becomes a fraction of canvas width and height. `{0.5, 0.5}` is the centre
  at any resolution.
- `Layout.size` becomes a fraction of the canvas, keeping `(0, 0)` to mean "the source's natural
  size" -- which is itself resolution-independent, because it is a property of the source.
- `CropRect` stays in **source** pixels. A crop is a statement about the incoming image, and has
  nothing to do with the canvas it is drawn onto.
- `Transform.scale`, `rotation_deg`, `pivot`, `skew_deg` and `Layout.alignment` are already
  unit-free and do not change.
- Text's `size` field becomes a fraction of canvas **height**, so a title stays crisp at any
  resolution rather than being scaled up as an image. Height, not width, because that is how type
  size is read and because it does not change with aspect ratio.

`SetCanvas` then needs no rescaling pass at all: the numbers already mean the same thing at the
new size. That is the point of the change.

### What the operator sees

The UI keeps showing and accepting **pixels**, computed against the current canvas. Nobody places
a camera at 0.3194. The fraction is the storage format, not the interface, and the fit math is
already the single place that turns a node into a rectangle (`unscaled_content()` in
`libs/lcomp/src/composite.cpp`, with `solve_drawn_rect()` as its inverse backing the UI handles).

Text is the exception worth stating: its field stays a px number in the dialog, converted on the
way in and out, because a type size in percent of canvas height would be unreadable.

## Migration

Schema version 4 -> 5, in `libs/ldoc/src/migrate.cpp`, which holds no geometry rescaling today.

For each node, divide `position` and `size` by the size of the canvas the scene belongs to.
Divide a text node's `size` by that canvas's height. A scene whose canvas is missing -- which the
reader already tolerates -- falls back to the first canvas, as version 1 scenes already do.

**A migrated project must look identical at the size it was authored at.** That is the test, not
an aspiration: the drawn rects before and after must match. Migration is forward-only, as every
migration here is; a newer project opens read-only rather than being migrated backwards.

### Why not keep both models

An opt-out flag per scene, or pixels-for-old and fractions-for-new, means two geometry paths
forever and every future change made twice. The migration is arithmetic with an exact expected
answer, so the usual reason to hedge -- that the conversion might be wrong in ways nobody can
check -- does not apply here.

## Consequences

- A show can be rebuilt at a different resolution without re-placing anything. This is the whole
  point.
- `transform_supported()` (`libs/lcomp/src/composite.cpp`) still refuses skew and any rotation
  that is not a quarter turn, and such a node is still not drawn. This ADR does not widen that;
  arbitrary rotation remains its own piece of work.
- Mixed-aspect canvases still do not share a scene. A 16:9 arrangement is not a 9:16 arrangement,
  which is ADR-0027's point and is unaffected: fractions make a scene survive a *resolution*
  change, not an *aspect* change.
- Anything reading `position` or `size` as pixels has to multiply first. The fit math and the UI
  mirror (`Document::placed_rect`) are the two places that do, and both already exist.
- OBS import and export gain a conversion step at the boundary, since OBS stores pixels.
