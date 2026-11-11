<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0023 — Every mutation is a command, and every command carries its own undo

- **Status:** accepted, implemented
- **Date:** 2026-10-12
- **Depends on:** ADR-0021 (`ldoc`), ADR-0022 (the project directory)

## Decision

Every change to a document is a `Command`: validated, serialisable, journaled and reversible. The
UI, global hotkeys, a Stream Deck, the websocket API and scripting are then all the same thing —
producers of commands — rather than five code paths that each mutate the document slightly
differently and disagree about undo.

Two properties make everything else fall out of this one.

### Applying a command returns its inverse, and the inverse is itself a command

Undo is not a second mechanism with its own bugs. It is `apply()` again, running a command built
from the state the first one replaced. Undoing *that* returns the original effect, which is what
goes on the redo stack. One code path, one set of bugs — and because the inverse is an ordinary
command, it serialises and journals like anything else.

This is deliberately not the snapshot approach `llayout::History` uses. A workspace layout is a
few dozen nodes, so snapshotting it is cheaper than the bookkeeping; a project is scenes full of
nodes, and two hundred snapshots of that is a different proposition. Commands also carry *what the
operator did*, which is what an Undo menu item has to say.

### A command that cannot be applied changes nothing

Every command validates completely before it mutates. `MoveNode` is the awkward case — it has to
remove the node before it can know whether the destination accepts it — so it puts the node back
on the way out. The property test asserts this after every refused command across thousands of
random sequences.

## The line between a precondition and a reference

This is the rule the implementation turned on, and it was not obvious until a property test
refused to pass.

**A command validates the things it acts on. It does not validate the things it merely refers
to.** `SetDeckProgram` requires the *deck* to exist, because otherwise there is nothing to act on.
It does not require the *scene* to exist.

The reason is undo. Pointing a deck at a new scene captures the previous pointer as its inverse.
If that scene is later deleted, the inverse now names something gone — and undo still has to
restore it, because that is precisely the state being undone. Generalised:

> **A command may only refuse states the document model itself refuses.**

Which forced a second decision.

## A dangling reference is a warning, not an error

ADR-0021's validator originally called a reference to a missing scene, source, bus or node an
error. It is now a warning, and `Error` is reserved for documents that cannot be rendered as
written: duplicate or nil ids, Reference and Precomp loops, invalid geometry or frame rates, a bus
that sends to itself, an enabled output with no destination, a deck or output with no canvas.

A layer whose source was deleted draws nothing — exactly as a layer with no source does. The only
difference is that this one records that something was deleted, which is worth saying and not
worth refusing to open over. Treating it as an error would mean a scene could not be deleted while
anything cloned it and a source could not be removed while any layer used it: a web of refusals
that turns ordinary editing into a puzzle, and the kind of thing that makes people keep a second
copy of their project "just in case".

Stopping the operator from *choosing* a scene that does not exist is the UI's job. It offers the
ones that do.

## The journal

`.munkicast/journal.jsonl`: one JSON object per line, appended and **fsynced as each command is
applied**, tagged with the generation of the saved project it was applied to. On open, the entries
tagged with the generation actually loaded are replayed; saving commits a new generation and
clears it.

- **JSON Lines, not one JSON document.** A document has to be closed to be valid, and a log that
  is only valid once it has been closed is useless for a crash. A torn final line — a process that
  died mid-write — costs that one entry and nothing before it, and is reported as such rather than
  passed over.
- **Fsynced per command**, because the one command that matters is the last one before the crash.
- **Filtered by generation on replay.** A crash *after* a save can leave entries behind that are
  already in the project; replaying them would apply the same edits twice. An entry that will not
  replay is reported as an error rather than skipped, because a project that quietly disagrees
  with its own log is worse than one that says so.
- **A command this build does not know is refused, not ignored**, or a journal written by a newer
  build would replay as a different session than it recorded.

## What the property tests found

The plan asks for "random command sequences against `ldoc` asserting undo/redo round-trip". It
earned its place twice over:

1. The precondition/reference distinction above. The failing sequence was: delete the scene a deck
   points at (legal, a warning), then point the deck elsewhere; its inverse now names the deleted
   scene and undo refused it. That is a real defect — undo silently stopping at an arbitrary depth
   — and no hand-written case was going to produce that ordering.
2. A group being moved inside its own child, which detaches the subtree from the scene entirely.

Both are now dedicated cases alongside the random sequences, because a property test tells you
*that* something is wrong and a named case tells you *what*.

## Still open

- **Compound commands.** A `Batch` whose inverse is the reversed list of its members' inverses,
  giving all-or-nothing semantics for operations that are several changes to the operator's eye:
  grouping a selection, or deleting a scene *and* repointing the decks that named it. Today
  deleting such a scene leaves the deck pointing at nothing and says so, which is correct but less
  tidy than it could be.
- The 20 commands here cover scenes, nodes, effects and decks. Canvases, buses, sources, assets
  and outputs are still edited by touching the model directly, which means those edits are not
  undoable. They follow the same pattern and are mechanical.
- Command ids and an author field, for the multi-process story: `munkicast-session` owns the
  document and everything else sends it commands.
