<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0035 — An encode names its canvas, and can be moved between them

- **Status:** accepted
- **Date:** 2026-10-17

## Context

[ADR-0029](0029-an-encode-is-a-thing.md) made an encode a document object that outputs point
at, and `Encode::canvas` has been a field since. The UI never let anyone set it: `addEncode`
took `current_canvas()` and nothing could change it afterwards. So a vertical canvas could be
created, given scenes, given panels — and never streamed, even in principle, because no encode
could be made to read it.

Asked where the choice should live, the operator picked the Outputs Manager, matching where
encode reuse was already settled.

## Decision

**Each encode block in the Outputs Manager carries a canvas dropdown.** The encode is where the
canvas belongs — one encode reads one canvas and feeds many destinations — so this sits on the
encode, not on the output rows under it.

**The dropdown is always visible, not only when there is more than one canvas.** "Which canvas
does this encode read" is worth stating even when there is one answer. It was implied before
and that is exactly how a vertical canvas ended up unstreamable without anyone noticing.

**Moving an encode retitles it when nobody renamed it.** The generated name carries the
resolution (`1080p60 8 Mbps`), the canvas decides the resolution, so a name the operator did
not type has to follow the move. `retitle_if_auto` already does this for rescales; this is the
same rule applied to the same field changing for a different reason.

## Rejected alternatives

- **A canvas picker when adding an encode, fixed thereafter.** Half the problem: it would let a
  vertical encode be created, and still leave an existing encode stuck on the wrong canvas with
  no way out but deleting it and losing its destinations.
- **Taking the canvas from whichever is current when `+` is pressed, shown read-only.** What
  happens today minus the mystery. Still means the Canvas panel's selection silently decides
  something in a different window.
- **A canvas column on the output rows.** An output does not read a canvas; its encode does.
  Putting it there would suggest two outputs on one encode could read different canvases,
  which is the opposite of what ADR-0029 established.

## Consequences

- `Document::setEncodeCanvas` is new; `EncodeRow` carries `canvasId` beside `canvasName`,
  because a dropdown needs something stable to compare against.
- An encode can now name a canvas that is later removed. `RemoveCanvas` already refuses while
  an encode points at it — "point it at another one first" — and that refusal now names a
  gesture that exists, which it did not before this.
- Nothing encodes anything yet, so this is a control over a capability that does not exist. It
  is built now because the document shape is already final and the gap was invisible from the
  UI: the manager looked complete while one canvas was unreachable.
