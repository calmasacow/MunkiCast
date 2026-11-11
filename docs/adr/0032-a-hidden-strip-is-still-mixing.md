<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0032 — A hidden strip is still mixing, and a locked one still moves

- **Status:** accepted
- **Date:** 2026-10-16

## Context

The mixer's footer carried a `+` that opened a bus-kind picker. The operator removed the
premise:

> on the plus button int he bottom left corer of the audio panel need to be changes that
> shouldnt be a add button there will be no way to add mixes that wouldnt make any sense. But I
> do need a eye then a lashes eye button that will ne a toggle for "show hidden". and we need to
> add a right click menu when you click in a mixer channel for now the only to menuy items I
> have to put in there will be "Lock Level" and "Hide source"

Adding a bus from the mixer never made sense: a bus exists because something in the show needs
it, and [ADR-0030](0030-the-mixer-shows-sources.md) made the mixer a view of what is in the
scene rather than a place where audio objects are created. The source strips were already
beyond adding — they appear because a node is in the scene and its type has audio — so the `+`
spoke for half the panel and the wrong half.

What replaces it is the pair of problems that show up once a scene has more than a few audible
sources: too many strips to read, and a fader that can be knocked while live.

## Decision

**The `+` and the bus-kind picker are removed.** Buses are created elsewhere or not at all;
nothing is lost here because nothing else in the shell routed through this control.

**Hiding and level-locking are per strip, and they are document state.** Both go on
`AudioRouting` (for a source node) and on `Bus`:

    bool hidden = false;        // not drawn unless "show hidden" is on
    bool level_locked = false;  // the fader will not move

They belong to the show rather than the workspace, for the same reason a scene's favourite flag
does (ADR-0027): an operator who hid six strips and locked the music bed has described *this
show*, and would have to do it again on another machine if it lived in the layout.

**Neither reuses `Node::visible` or `Node::locked`.** Those are the canvas's: `visible` is
whether the source is drawn, `locked` is whether it can be dragged in the preview. A source can
be perfectly visible on air and clutter in the mixer, and a node unlocked for layout can have a
fader nobody should touch mid-show. Folding either pair together would mean hiding a strip
blanked the picture.

**Hidden means hidden from this panel, and nothing else. Locked means the fader will not move,
and nothing else.** This is the rule the rest of this has to keep. A hidden strip is still
routed, still at its gain, still going to air; a locked strip is still live and can still be
muted. Neither is a mute and neither may become one — the panel is a window onto the mix, not
the mix.

**"Show hidden" is a view toggle, and it is workspace state**, not the document: it is about
what one operator is looking at right now, not about the show. Drawn as an open eye when
hidden strips are showing and a lashed, closed eye when they are not, which is the operator's
own description and matches the two-state convention everywhere else.

**A hidden strip shown by that toggle is drawn dimmed**, so "hidden but currently revealed" is
not mistaken for an ordinary strip. Without that the toggle is a trap: an operator would reveal
hidden strips, forget, and later wonder why a strip vanished for someone else.

**The strip count is always stated.** When anything is hidden, the footer says how many, so a
missing microphone is answerable from the panel rather than being a mystery. That is the whole
risk of a hide feature in a live tool, and it costs one line of text to close.

## Rejected alternatives

- **Keep the `+` and make it add a source.** The mixer's membership rule is the scene; adding a
  source here would mean adding it to the scene, which is the Sources panel's job and is one
  gesture away.
- **Hidden and locked as workspace state.** Tempting for "hidden", since it is close to a view
  filter. Rejected because the set of strips worth hiding is a property of the show, and
  because splitting the pair — one in the document, one in the layout — would mean a
  right-click menu whose two items undo differently. One of them would not undo at all.
- **Reusing `Node::visible` for hiding.** Covered above: it would blank the source on air.
- **Reusing `Node::locked` for the fader lock.** It is the preview's drag lock. An operator who
  locked a logo so they would stop nudging it has said nothing about its audio.
- **A lock that also prevents muting.** A locked strip has to stay mutable: mute is the control
  reached for in an emergency, and a lock that blocks it turns a tidiness feature into a
  hazard. Locking stops the fader, which is the thing that gets knocked.
- **Hiding with no indication that anything is hidden.** The failure case is someone hunting a
  strip that is not there during a show.

## Consequences

- `kSchemaVersion` does not move. Two booleans defaulting to `false` are compatible both ways:
  an older build ignores them and renders the same audio.
- `setNodeHidden`, `setNodeLevelLocked`, `setBusHidden` and `setBusLevelLocked` join the
  node-keyed setters from ADR-0030. No new command: `SetNodeAudio` and `SetBus` already carry
  whole values.
- `MixerFader` gains a `locked` property. It refuses drags and wheel steps, and draws its knob
  muted so the refusal is visible before it is discovered.
- The eye is drawn (`EyeIcon.qml`) for the same reason the chain is: 👁 resolves to Noto Color
  Emoji here and colour emoji cannot be tinted. Its closed state is rough -- it reads more like
  an umbrella than a shut eye -- and the operator accepted that for now: "it will get change
  for permanent icons later it is fine during dev". Listed with the mixer's other styling work
  in `docs/todo.md`.
- First context menu in the shell. It is drawn here rather than taken from QtQuick.Controls'
  `Menu`, for the reason ADR-0015 gives for the rest of the shell: a styled popup from Controls
  brings a platform style to fight with the theme. If a second panel wants one, this moves to a
  shared component rather than being copied.
