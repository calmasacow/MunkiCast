<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0040 — The program picture comes from the render process

- **Status:** accepted
- **Date:** 2026-10-17
- **Depends on:** ADR-0005 (multi-process isolation), ADR-0006 (composite-to-encode handoff), ADR-0011 (two capture backends), ADR-0013 (capture is damage-driven), ADR-0024 (the session boundary), ADR-0026 (the process transport)

## Context

The shell edits a real document and nothing draws it. The Program panel is a framing guide that says "no pipeline attached", and the mixer says "no audio host — levels only". Both sentences are doing a job: a panel that looks finished while being incapable cannot be trusted later.

The pieces that would fill the panel already exist as measured spikes, and the process that should own them does not exist at all. `munkicast-render` was an empty directory. So was `lcomp`. Putting the first picture in the UI, because that is where the panel is, is the move ADR-0024 exists to prevent: every caller that reaches into the thing directly has to be found again the day it moves, and the ones that are missed fail during a show.

Four questions had to be answered before any of that code was written.

1. Which capture backend lands in `lcapture` first.
2. Where the compositor lives, given the scene tree is already final.
3. How a frame reaches a QML item in a different process.
4. Whether audio comes with the picture or after it.

## Decision

**The Program panel shows frames published by `munkicast-render`. It does not draw them.**

`lcomp` walks the scene tree and produces one picture per canvas. `munkicast-render` is the process that runs it. The UI spawns that process, sends the document, and copies out the pixels it is sent. Spawning is a stand-in for `munkicast-session`, which still does not exist; the protocol does not know which process the parent is, so the stand-in can go away without the messages changing.

### What the first picture is allowed to be

A colour, a group of those, and a precomp of a scene made of those. `source.color` needs no device, no display and no portal, which means the headless self-check can demand a real pixel and get one. That is the whole of the reason it is first. A capture path the check cannot reach would be a picture nobody is allowed to break on purpose.

The walk follows the document's own order: `roots[0]` is the topmost node and is drawn last. A group's opacity multiplies into its children. Normal blending is associative, so flattening a normal group and drawing its children one by one are the same picture, and isolate is not a second path while Normal is the only mode implemented.

Anything else is a **gap**, and it is not drawn. A camera, a capture, a text source, a rotation, a blend mode other than Normal, an enabled effect: the pixels underneath show through and the panel says how many sources had no picture. Approximating any of those would make the panel lie, which is the failure this whole screen was built to refuse. An audio-only source is not a gap. It has no picture to fail at.

A colour with no colour stored is a gap too. The shell writes the type's default into the document when it creates one; inventing `#3b82f6` inside the compositor would hide a source that arrived some other way.

The picture is straight sRGB8, tightly packed, opaque black where nothing was drawn. The encode target is still scene-linear RGBA16F on a `VkDevice` (ADR-0006). This buffer is a view of the program, not the frame the encoder will be handed. `Rgba` is stored straight and linear — `from_srgb8` does not premultiply, whatever the comment on the type says — and the blend premultiplies at the moment it blends.

### How the frame crosses

A sealed `memfd`, announced over `lipc`, one per canvas, replaced when the size changes. Never one descriptor per frame: ADR-0026's four-minute leak is that mistake.

The document rides the same way. `project_to_json` does not contain scene bodies, and a scene can be larger than `kMaxPayload`, so the snapshot is the project file plus one memfd per scene. The renderer composites on commit and answers with one frame message per canvas, then an ack. The client copies the bytes out before it sends the next snapshot, and it keeps only the latest snapshot in flight, so a dragged fader cannot queue a project per command and the renderer is not writing a buffer the UI is reading.

The client sends from the session notification. `run()` may one day return before the document has changed (ADR-0024); a preview that published on the return value would show the previous scene and look like a compositor bug.

### What this commit deliberately does not start

**`lcapture` stays empty.** Its first backend, when it is written, is `ext-image-copy-capture-v1`. ADR-0011 already measured that as the primary path, and it is the one that needs no picker, so it is the one that can be developed. The portal stays the fallback it already is. Porting the spike in the same commit as the first picture would have mixed a display-only path into a check that is required to run offscreen, and ADR-0013's warning still stands for whoever writes the worker: destroying a pending frame and asking again invents frames. `wlcap_capture_frame` is still unbounded. Neither is fixed by pretending the spike is now a library.

**`laudio` stays empty.** The mixer already says there is no host. A meter with no host is an animation, and ADR-0031's rule — monitoring never changes the program mix — needs a program mix to not change. Audio follows a picture. It does not lead it.

**The emergency scene is not cut to.** ADR-0025's cut belongs to a render process that has outputs and a supervisor. This process has neither. The UI is currently both the viewer and the stand-in parent, so treating a stalled socket as "the show is unsupervised" would flash the emergency scene over the picture the operator is editing. A dead peer stops the preview and the panel goes back to saying the pipeline is gone. When this process grows outputs, peer-gone cuts to the canvas's emergency scene. Until then it must not.

## Rejected alternatives

**Composite in the UI process.** The panel is right there, and a colour fill does not need a GPU. Rejected because it is the ADR-0024 failure in a new shape: the day the picture moves to `munkicast-render`, every call site that built a `QImage` itself is a call site that shows a stale frame. The check drives the real process for that reason. A unit test may call `composite_canvas` directly; the panel may not.

**Import a dma-buf into the UI's EGL.** That is the encode-shaped answer, and it is the failure ADR-0011 spent its time on: a buffer the compositor and a client could not agree how to import. The Program panel is a view at panel resolution. It does not need the encode buffer, and putting Qt on the device that will one day encode would make a toolkit bug a dropped show. The GPU path stays inside `munkicast-render`, where ADR-0006 put it, and it is not this picture.

**One `memfd` per frame, or the pixels inline in the message.** The first leaks a descriptor a frame. The second does not fit, and ADR-0026 already says a payload that does not fit belongs in shared memory with the descriptor sent beside it.

**Portal capture first.** It needs a person at the picker, and on this machine it is the backend the distribution itself defaults away from (ADR-0011). Nothing about the preview protocol changes when `lcapture` arrives: a capture source stops being a gap and starts being pixels.

**Draw the gaps as a stand-in card.** A labelled rectangle where a camera will be is a fake picture. The absence is the panel's caption, not a texture.

**A silent audio host so the meters move.** Rejected in the mixer panel's own words. Levels stay document state until a host exists.

**Block the UI until the renderer has drawn.** A compositor that stalls would stall the shell. The socket is non-blocking, the latest document replaces the one in flight, and a dead renderer is a caption rather than a hang.

## Consequences

- `munkicast-render` publishes a preview and does nothing else. No encode, no output, no capture, no device. Adding those is further work in this process, not a new process and not a change to the message that carries the document.
- The panel's caption is the honesty mechanism. "no pipeline attached" means the process is not there. "nothing in the scene" and "N not drawn" mean it is there and has said what it could not draw. A bare `1920x1080` means the scene was drawn in full.
- Tests cover the walk without a process, the walk through the real process, and the panel: the self-check puts a red colour on the program scene and reads the window. A green run that only called `composite_canvas` would not have noticed a panel still drawing the guide.
- The software picture must not grow into a second compositor that disagrees with the GPU one. When `lrhi` exists, the scene walk is the part worth sharing. A blend mode, a rotation or a capture implemented only here would be the disagreement.
