<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0041 — a screen source names its output

- **Status:** accepted
- **Date:** 2026-10-18
- **Depends on:** ADR-0003 (the Wayland client has no privileged view), ADR-0011 (two capture
  backends, measured), ADR-0013 (capture is damage-driven), ADR-0026 (the process transport),
  ADR-0040 (the program picture comes from the render process)

## Context

`libs/lcapture/` is an empty directory with an empty `linux-portal/` inside it. The measured
capture code lives in `tools/spike-core/wlcap.cpp`, where ADR-0011 put it on purpose: spike-local
evidence, to graduate into `libs/` once Phase 1 had been designed around what it taught.

ADR-0040 drew the first real picture and said, in as many words, that `lcapture` stays empty —
porting the spike in that commit would have mixed a display-only path into a check that has to run
offscreen. That reason has not gone away. What has changed is that the picture exists now, so the
next piece can be the capture library rather than the thing that draws its output.

There is a contradiction in the document model to settle first, and it is the reason this is an
ADR and not just a commit.

A screen source's settings panel says, on its face:

> Which screen is chosen in the portal's own picker when capture starts. A Wayland client cannot
> enumerate or choose one for itself (ADR-0003).

That was true when the portal was the only backend. ADR-0011 then measured both and chose
`ext-image-copy-capture-v1` as primary, specifically because
`ext_output_image_capture_source_manager_v1` enumerates monitors and needs **no picker dialog** —
`capture-spike --output DP-1` captured 459 frames at 2560x1440 with no portal and no picker, on
an output where every portal attempt had failed. The spike's own header already states the
consequence: the connector name is "the same identifier Hyprland and the portal's `mapping_id`
use, so sources are comparable across backends and stable across restarts - unlike a portal
picker selection."

So the primary backend can name an output, the model has nowhere to write the name down, and the
settings panel tells the operator the opposite of what the shipped backend does. A screen source
added today cannot say which screen it is.

## Decision

**A screen source stores the connector name of the output it captures.** `source.screen` gains one
field, `output`, holding a string like `DP-1`.

The connector name is the identity, not an index and not an opaque handle. An index reorders when a
monitor is unplugged; a portal handle is a per-session grant that means nothing after a restart,
which is precisely the thing ADR-0011 found operators complain loudest about. The connector name is
what the compositor advertises, what Hyprland's config calls it, and what the portal reports as
`mapping_id` — so one stored string identifies the same screen on both backends and survives a
restart.

The portal backend fills the field in from `mapping_id` when its picker returns. It does not stop
using the picker: on the portal path a picker is how a client is *allowed* to choose, and ADR-0003
has not changed. What changes is that the answer is remembered rather than asked again.

**An empty `output` means the first output**, which is what `wlcap_begin_session` already does with
an empty name. A source created before this field existed, or created and not yet pointed at a
screen, captures something rather than failing — and a one-screen machine, which is most of them,
needs no decision at all.

**The settings note is rewritten**, because it currently describes a backend that is no longer
primary. The new wording says the connector is named directly on the compositor path and filled in
by the picker on the portal path.

### What this commit builds

`libs/lcapture/` becomes a real library holding the ported `wlcap` code, with `source.screen`'s new
field and a unit test. Nothing draws a captured frame yet: `lcomp` is untouched, and a screen
source stays the gap ADR-0040 made it, counted in the panel's caption.

That split is deliberate. A library that captures a frame and a compositor that draws one are two
changes, and the first can be proved without a display while the second cannot be proved without
one.

### What the test can and cannot reach

The suite runs headless over SSH, on Qt's offscreen platform, on a box with no RTX card and no
`vainfo`. Real capture needs a live Wayland compositor with `ext-image-copy-capture-v1` bound. The
test therefore covers the half that is ours — that a stored connector name round-trips through the
document, that an absent one means the first output, that the schema migration leaves an old
project readable — and stops at the protocol boundary.

It does **not** assert that a frame arrives. Pretending otherwise is the trap ADR-0013 already
names: a capture test that cannot distinguish a still screen from a broken path will report
success on both. `capture-spike` remains the thing that proves frames arrive, on a machine with a
screen, and it says so in its output rather than in a green tick.

## Rejected alternatives

**Store nothing and show the picker every launch.** This is today's model and needs no ADR, which
is its only argument. ADR-0011 measured the cost: re-picking a source every launch is one of the
loudest complaints about portal-based capture, and the primary backend does not need a picker at
all. Keeping the picker because the model is shaped for it would be the model dictating the
protocol.

**Store a portal session handle or restore token.** It expires, it is per-session, it means nothing
to the compositor backend, and it cannot be compared between the two. The connector name is the
only identifier both paths share.

**Store an output index.** Unplug a monitor and every source after it in the list captures a
different screen, silently, mid-show. A name that stops resolving is a fault the operator can be
told about; an index that still resolves to the wrong thing is not.

**Decide now what happens when the named output is gone.** It is a real question — black frame, or
ask — and it is a different one. An output that has been unplugged is a running-show failure, which
belongs with the supervisor ADR-0040 says this process does not yet have. Answering it here would
put a policy field on the document before anything can honour it.

**Port the spike and draw the frame in one commit.** ADR-0040's reason stands: the self-check runs
offscreen, and a commit that both introduces a display-only path and makes the program picture
depend on it cannot be bisected when a frame stops arriving. Two commits, and the first one is
provable headless.

**Move `wlcap` and leave the model alone.** Then `libs/lcapture/` has a function that takes an
output name and a document that cannot supply one, and the first caller invents a default — which
is the ADR-0024 failure shape again, one call site at a time.

## Consequences

- A screen source identifies its screen in the project file, so a saved show opens pointed at the
  same monitor. That is the first setting on the capture half that survives a restart.
- The two backends become comparable in the model, not just in the spike: one field answers "which
  screen" for both, which is what makes the portal path a genuine fallback rather than a separate
  feature.
- `libs/lcapture/` stops being an empty promise. The code in it is the code `capture-spike`
  measured, moved rather than rewritten, so ADR-0011's numbers still describe it.
- Nothing on screen changes. A screen source is still a gap in the program picture, still counted
  in the caption. The next commit is the one where that stops being true, and it needs a machine
  with a display to prove.
- `wlcap_capture_frame`'s unbounded wait moves into `libs/` unfixed, as a known defect with no
  callers, exactly as `docs/todo.md` already records it. Moving code is not the commit to change
  its behaviour in.
