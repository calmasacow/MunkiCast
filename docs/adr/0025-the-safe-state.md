<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0025 — When the show cannot be supervised, broadcast something safe

- **Status:** accepted, not yet implemented
- **Date:** 2026-10-13
- **Depends on:** ADR-0005 (multi-process isolation)
- **Origin:** the operator, in conversation

## Context

ADR-0005 isolates the render path from the UI so that a crash does not end the broadcast. Working
through what that actually means on air, the operator raised the objection that makes the design
incomplete:

> "if it happens on certain scenes it could make for something we might not actually want being
> sent out."

That is correct and it is the hole in the argument. Keeping the transmission alive is only a win
while what is being transmitted is something the operator would have chosen. A UI crash while the
program scene is a full desktop, a camera pointed at an empty chair, or a window with a private
message in it turns "the stream survived" into "the stream survived and kept broadcasting
something nobody was watching over."

Stopping the stream is the safe answer but an expensive one: viewers see offline, the VOD splits,
the recording tears, the audience leaves. Continuing blind is cheap and occasionally terrible.

## Decision

A third answer, and it is the operator's: **the project names an emergency scene, and the render
process switches to it when the show cannot be supervised.**

The connection stays up, the recording stays continuous, the viewer count holds — and what goes
out is a thing the operator chose in advance, not whatever happened to be on screen.

This is what broadcast has always done. It is a technical-difficulties card, and it is a
well-proven answer to exactly this problem.

### 0. It is a scene, and that is what makes it cheap

An earlier draft of this ADR had it as a flat media file, on the reasoning that the safe state has
to render with no supervision and a scene might contain the very source that faulted. The operator
corrected it: it should be **an emergency scene, preferably a single media source such as a
loopable video clip.**

That is better, and it removes special cases rather than adding them:

- It needs no new primitive. A deck already points at scenes; the emergency scene is another
  scene reference.
- Real shows want branding on the card — a logo, a wordmark, "back in a minute" — and a file
  means re-rendering a video every time any of that changes.
- It can be edited and previewed with the tools that already exist.
- **Rehearsing it is switching to a scene**, so "show me what my safety net looks like" is not a
  feature to build.

The risk the file was avoiding does not go away, it just moves to where it can be checked: see the
pre-flight below. The "preferably one self-contained media source" part of the operator's phrasing
is the mitigation, not a stylistic preference — a scene built from one local file depends on
nothing that can fault.

### 0b. One emergency scene per canvas

An earlier draft claimed multi-canvas fell out for free, because a scene carries per-canvas
overrides. That was wrong, and the reason is worth recording: **an override can reposition a card
but cannot swap the source binding.** A shared emergency scene would mean either letterboxing a
16:9 clip into a 9:16 canvas, or carrying two media layers with per-canvas visibility — and the
second violates the single-clip constraint below, which is the thing that makes the net reliable.

So the emergency scene is a property of the **canvas**: `Canvas::emergency_scene`. Outputs are
already per-canvas, triggers can be per-canvas, and a vertical show wants a separately authored
vertical clip rather than a scaled landscape one.

A canvas with none set falls back to black and silence, and pre-flight says so.

### 0c. A limit on what an emergency scene may contain

The safety net is reliable in proportion to how little it depends on. The shape to aim for is
**one media layer playing a local, looping clip** — which is not restrictive in practice, because
a pre-rendered clip can carry any branding, motion or text the operator wants.

Sources are classified by whether they can fault:

| | |
|---|---|
| **allowed** | local media, image, colour, static text |
| **refused** | capture devices, window and screen capture, browser sources, network streams, `Reference` or `Precomp` into the live show |

Everything in the refused column can be exactly what failed. A browser source that wedged and took
the UI with it is not a thing to put on the holding card.

This is enforced **when the scene is designated**, not when it is needed: that is a deliberate,
off-air act with a human present, so refusing is clear and costs nothing. Pre-flight re-checks it,
because a scene can be edited after it was designated — and there it warns rather than refuses,
since blocking a broadcast is worse than saying so.

**Where** that refusal lives matters, and ADR-0023 already settled it: not in the command. A
command may only refuse states the document model itself refuses, or an inverse can become
unapplicable and undo breaks at an arbitrary depth. So `SetCanvasEmergencyScene` permits what the
model permits, `validate()` reports an unsuitable emergency scene as a warning, and the
*designation flow* is what refuses — it checks first, explains, offers to fix, and only then
issues the command. Exactly as the UI offers only scenes that exist rather than the command
rejecting ones that do not.

### 0d. Designating it checks the clip loops, and offers to fix it

The safe state can last minutes. A clip that plays once and cuts to black is a net that expires,
usually at the moment it is finally needed.

So designating a scene checks whether its media source is set to loop, and if it is not, **asks
whether to set it** — rather than recording a warning the operator reads later or never. The
answer is an ordinary `Command`, so it is journaled and undoable like any other edit.

### 1. It is a state, not an event, with several triggers

The render process enters the safe state when:

- **the UI dies** — the easy case;
- **the UI stops responding** — a heartbeat with a deadman timer, because a wedged frontend is
  more common than a dead one and a dead process is far easier to detect than a live one that has
  stopped making decisions;
- **the operator asks for it** — the same mechanism is a **panic button**, which is a feature
  people want for its own sake: someone walks into the room, a notification lands, a drink goes
  over. Being a deliberate control is not a secondary use; it is the one that will get used most.
- **the program path cannot be rendered** — a wedged browser source, a capture card unplugged mid
  show.

One mechanism, several ways in. Building it as "what to do if the UI crashes" would produce
something that only handles the rarest trigger.

### 2. Audio falls back too

The failure this is easiest to ship: video switches to a holding card while the microphone keeps
transmitting. The card says "we will be right back" over live room audio, which is worse than
either honest option.

The safe state covers the audio path. The clip's own audio, a music bed, or silence — never live
audio under a safe picture.

### 3. Coming back is deliberate

Reconnecting is not resuming. The UI being alive again does not mean the operator is at the
keyboard — it may have been restarted by a supervisor while they were out of the room.

So the UI returns **into** the safe state, visibly and unmistakably, and going live again is an
explicit action. A frontend that reconnects and quietly resumes the previous scene reintroduces
the whole problem at the moment of recovery.

### 4. Fallback applies to anything leaving the machine

Local recording keeps the real content.

The purpose is to not broadcast unsupervised content. A local file is not broadcast: nobody saw
it, and replacing twenty minutes of real footage with a holding card is a loss that cannot be
undone, in exchange for no safety at all. A recording can be trimmed before it is uploaded;
footage that was never written cannot be recovered.

Because an `Output` is already `{canvas, encoder, destination}`, this is a property of the
destination rather than a special case in the render path.

### 5. The safety net is checked before it is needed

The emergency scene is validated when the stream starts, not when it is wanted. A safety net that
turns out to be a missing file at the worst possible moment is worse than having none, because the
operator believed they had one.

Pre-flight asks four things:

1. **Does every canvas have one?** A canvas without an emergency scene falls back to black and
   silence, which is safe but not what anyone chose.
2. **Does it exist and render?** The scene is composited once and its sources opened.
3. **Is it still self-contained?** Re-checked here because the scene may have been edited since it
   was designated. A warning rather than a refusal at this point.
4. **Does its media still loop?** Likewise.
5. **Is it primed?** Decoders opened, first frames decoded, audio confirmed, held ready — so
   entering the safe state is a switch rather than a load. The moment the show has already gone
   wrong is the wrong time to find out the disk is busy.

If it cannot be loaded at all, that is said loudly **before going live**, and the fallback degrades
to black and silence — never to the live scene.

## Consequences

- This makes the process split strictly better than a single process rather than a trade. The
  objection to isolation was that an unsupervised broadcast may be worse than no broadcast; with
  a safe state, the isolated design keeps everything that is good about continuity and removes
  what was dangerous about it.
- `munkicast-render` owns the safe state and can enter it with no help from anything else. It
  cannot depend on the UI, the session process, or a disk read at the moment of entry.
- The document model grows `Canvas::emergency_scene`, plus the audio policy, the deadman interval
  and which outputs the fallback applies to. These are project state rather than settings, since a
  different show wants a different card.
- The deadman interval is a real tuning problem in both directions. Too short and a long repaint
  or a GC pause takes the show off air; too long and seconds of unwanted content go out. It is
  configurable, with a conservative default, and the heartbeat must come from somewhere that
  stalls when the operator's ability to act stalls — not from a timer that keeps ticking while
  the window is frozen.
- Falling back on one canvas and not another is a legitimate state, since the triggers can be
  per-canvas.
- Source types need a **self-contained** property. Until source manifests exist it is a built-in
  table of type names; it belongs in the manifest beside the rest of a source type's declaration,
  and should move there rather than growing.
- The emergency scene should be visibly marked in the scene list, and it should be hard to delete
  by accident — deleting the safety net and discovering it at the wrong moment is the failure this
  ADR exists to prevent, arriving by a different route.

## Still open

- Whether a replay buffer should keep running through the safe state. Probably yes: the moments
  before someone hits panic are exactly the ones worth keeping.
- Whether a project with no emergency scene set should be allowed to go live, or should be given
  a generated default — a black card with the project name — so that the net is never simply
  absent.
- Whether the pre-flight warnings should be checkable without going live, as part of a "rehearse"
  action that switches to the emergency scene for a few seconds while off air.
