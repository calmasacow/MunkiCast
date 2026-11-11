<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0030 — The mixer shows sources, and a type says whether it has audio

- **Status:** accepted
- **Date:** 2026-10-16

## Context

The Audio Mixer was built as a console: one strip per **bus**, which is a place audio *goes*.
The operator launched the shell, looked at it and said the faders "make no sense". They did not.
Both strips in the golden project read "nothing feeding", so the panel drew two faders on
silence, and the thing anyone actually reaches for — the row per microphone — had never been
there. OBS puts sources in its mixer and does not show destinations at all.

Two details settled by the operator, in their words:

> Yes there should be channels for each source that has audio in the active scene. There should
> prolly be a divider as well that seperates sources and the outputs. Typically in OBS the
> outputs are not shown. That said I want them left in for now.

> source strips are left justified and destination strips are right justified with a divider
> just to the left of the left most destination slider

So: one row, strips still vertical, two groups justified to opposite edges. Not two stacked
sections, which was the first reading and was wrong.

Building it needed an answer to a question the code could not answer: **which sources have
audio?** `nodeProperties` said `hasAudio = true` for every source node whatever it was bound
to, so a colour card offered to be routed to a bus. The operator supplied OBS's own table,
source type by source type.

## Decision

**The mixer is two groups in one row.** Source strips pack from the left, bus strips from the
right, with a one-pixel divider immediately left of the leftmost bus strip. The gap between the
groups does most of the separating; the divider says the gap is a boundary rather than empty
space. In a narrow panel the source half scrolls and the destinations keep their room, because
the destinations are a fixed short list and the sources are not.

**A source strip is keyed by node, not by source.** The same Source placed twice in a scene is
two placements with two faders, because the routing lives on the node (ADR-0027). A strip
appears because its node is in the scene on screen and its type has audio, and it goes away
when the scene changes.

**The scene is the selected one, not the program one.** This panel is mostly read while
building a scene, and faders belonging to whatever is on air would be the wrong set at exactly
the moment they matter.

**`ldoc::has_audio(source_type)` is a model-level predicate**, sitting beside the existing
`is_self_contained` and built the same way, from a table. It is stated as the *silent* list:

    source.image, source.text, source.color, source.screen

Everything else — media, browser, camera, window capture, audio input, audio output, and
anything unrecognised — has audio. Membership follows what OBS does type by type, because an
operator arriving from OBS knows that table already and a mixer that disagrees with it reads as
a bug. The pair that looks inconsistent, **window capture audible but whole-display capture
not**, is inconsistent in OBS too and for a real reason: a window grab can carry that
application's audio, while a display grab is a video capture with no audio stream behind it.
Desktop audio is a separate source there, as it is here.

**An unknown type answers "has audio".** That is the opposite default to `is_self_contained`,
which answers "no" for anything it does not recognise, and the asymmetry is the decision. There,
inventing trust puts a source that can wedge onto the emergency card. Here the two mistakes are
also unequal, but they point the other way: a fader for something silent does nothing, while a
missing fader is audio going to air with no way to mute it.

**Audio edits gain node-keyed forms.** `setNodeGain`, `setNodeMute` and `routeNode` take a
scene and a node; `edit_selected_audio` is now written in terms of `edit_node_audio`. A mixer
draws many strips and none of them is the selection, so routing a strip must not require
selecting it first.

## Rejected alternatives

- **Two stacked sections, sources above buses.** The first reading of the request, and
  corrected by the operator. Stacking halves the height available to every fader in a panel
  that is already the shortest in the layout, and a fader's whole job is travel.
- **Stating the audible types instead of the silent ones.** Same table, opposite default, and
  the default is the point: a newer project file or a source manifest this build has never seen
  (ADR-0004) would silently lose its fader. An extra dead fader is the cheaper error.
- **Asking the bound Source whether it is *currently* producing sound.** A media file with no
  audio track would then have no strip, which sounds more accurate and is worse: the strip would
  appear and disappear as files are swapped, and nothing can answer the question at all until
  there is an audio host. The type is a stable claim; the stream is not.
- **Keying strips by Source rather than by node.** Would collapse the same camera placed twice
  into one fader, which is wrong under ADR-0027 — the routing is per placement — and would
  disagree with the Sources panel standing next to it.
- **Replacing the bus strips.** OBS has no destination strips and these may well go, but the
  operator asked for them to stay for now, knowing that. Removing them later is a decision to
  take deliberately rather than a tidy-up.
- **Per-canvas mixers.** Scenes and Sources panels are per canvas now, so this was the obvious
  symmetry. Not built: buses are project-level, so a per-canvas mixer would show the same
  destinations in every copy. Revisit if a second canvas ever gets its own buses.

## Consequences

- `hasAudio` now answers honestly, which changes existing behaviour: the `→` route button in
  the routing line is disabled for a colour, image, text or display-capture source, and for a
  node bound to nothing. That last case is new — an unbound node has no type to ask.
- The fader was lifted out of the bus delegate into `MixerFader.qml` and is now shared by both
  strip kinds. Its `objectName` changed from `busFader` to `mixerFader`; the self-check counts
  both halves.
- A project saved before this still opens unchanged. Nothing about the document moved: the
  predicate reads `Source::type`, which was always there.
- What is still missing: no meters, because nothing makes a sound yet; no solo on a source
  strip, because solo is a bus property in the model; and no way to change a strip's bus from
  the strip itself — `routeNode` exists and is covered, but the only route control on screen is
  still the selected-node line along the bottom.
