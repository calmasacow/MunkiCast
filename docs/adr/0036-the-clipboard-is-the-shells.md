<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0036 — The clipboard is the shell's, and a paste keeps its coordinates

- **Status:** accepted
- **Date:** 2026-10-17

## Context

Asked whether copying a scene between canvases existed, the operator said they had assumed it:

> I assumed since tha tis the way it works in OBS and there is no reason that you shouldnt be
> able to jsut copy somethign in con vanvase then got to the other and paste it.

It did not. `duplicate_scene` existed and deliberately kept the copy on the same canvas, and
nothing copied a node at all.

## Decision

**Copy and paste are shell state, not commands.** `Document` holds either a copied node or a
copied scene, and `Ctrl+C` / `Ctrl+V` are handled in `Shortcuts::perform` beside the workspace
family rather than resolving to a command. There is no command that means "remember this for
later", and inventing one would put a clipboard in the undo history, where it does not belong.

**Not the system clipboard.** What is held is a node or scene out of *this* project, referring
to this project's sources by id. Serialising it for another process would mean a format nothing
else can read and ids that mean nothing on the other side. Not in the layout either: a copied
source is not worth restoring on the next launch.

**A pasted node is a new node.** Fresh ids via `ldoc::duplicate`, or `AddNode` would refuse the
paste as a duplicate id — correctly. The clipboard survives being used, so one copy can fill a
scene with six of something.

**A pasted scene keeps its name and takes the current canvas.** Scene names only have to be
unique within a canvas, and matching names across canvases are exactly what make a scene link
readable: Main's "Live" cues Vertical's "Live". `duplicate_scene` keeps the source canvas by
design, so the paste overrides it rather than the helper changing meaning.

**Coordinates are preserved, never scaled.** A 2560-wide layout pasted onto a 1080-wide
vertical canvas lands mostly off-frame. That is the operator's choice, and the right one: no
scaling turns a 16:9 composition into a 9:16 one, and a paste that silently rewrites the
framing is worse than one that lands somewhere visible and wrong.

**What is owed for that is a warning, and the operator specified its shape:**

> but just say there are elements that are off canvase are you sure? yes/no no need ot list
> things

So the count, not the list. It appears only when the count is non-zero, and only for a scene
paste — a single source pasted into a scene is one object the operator can see, and a dialog
for it would be noise.

## Rejected alternatives

- **Scaling the layout to the new canvas.** Convenient between same-aspect canvases, dishonest
  between 16:9 and 9:16, and the dishonest case is the one this feature exists for.
- **Scaling only when the aspect matches.** Does the right thing in both cases at the price of
  a rule the operator has to remember about their own paste. Rejected with them.
- **Refusing a paste between canvases of different sizes.** Makes the common case — landscape
  to vertical — the one thing you cannot do.
- **Naming the pasted scene "Copy of X".** Right within one canvas, wrong across them: it
  would leave the operator renaming it every time, and it breaks the name-matching that makes
  links readable.
- **Listing the off-canvas sources.** Asked for and declined by the operator. A count answers
  "did I just make a mess", which is the question.
- **The system clipboard.** Covered above.

## Consequences

- `copySelectedNode`, `copyScene`, `paste`, `pasteDescription` and `pasteWouldMissCanvas` are
  new on `Document`; `edit.copy` and `edit.paste` join the action catalogue with `Ctrl+C` and
  `Ctrl+V`, so they are rebindable like everything else.
- Right-click menus arrive on source rows and scene rows, reusing `StripMenu.qml` from
  ADR-0032. That is now its third caller, which is the point at which it stops being a mixer
  component; it keeps the name until something forces a rename.
- A pasted scene's precomps still point at the scenes they pointed at. That is usually right —
  an overlay used on Main is the same overlay on Vertical — and it means a paste can produce a
  cross-canvas precomp, which the model allows.
- `pasteWouldMissCanvas` judges "off canvas" by position and box size alone. A node with no
  explicit size reports as a point, so a source that would be scaled to fill the frame by the
  renderer may count as off-canvas when it is not. It over-warns rather than under-warns,
  which is the right way round, and it gets honest when the renderer can answer the question.
