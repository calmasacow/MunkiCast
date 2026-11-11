<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0024 — The session boundary exists before the process split does

- **Status:** accepted, implemented
- **Date:** 2026-10-13
- **Depends on:** ADR-0005 (multi-process isolation), ADR-0023 (the command bus)

## Context

ADR-0005 puts the document in `munkicast-session` and gives the UI zero authority over it. That
process does not exist yet, and the UI needs a document now.

The tempting move is to hold a `ldoc::Project` in `munkicast-ui` and split it later. That is the
move that makes the split a rewrite: every place that reaches into the document directly, every
repaint driven by a synchronous return value, and every error reported by an exception has to be
found and changed, and the ones that are missed fail at runtime rather than at compile time.

## Decision

`libs/lsession` holds an abstract `Session` and an in-process `LocalSession` that implements it.
Everything that edits a document goes through the interface. When the document moves to another
process, a proxy over `lipc` implements the same operations and nothing above it changes.

The interface is small on purpose — read the project, run a command, undo, redo, save, subscribe —
and three rules make it portable. They are rules about the **caller**, not the implementation:

1. **Nothing outside holds a mutable `Project`.** Reading is a const reference; writing is
   `run(Command)`. A caller that reaches around this stops compiling the day the document moves,
   which is the failure mode we want.

2. **Update from the notification, never from the return value of `run()`.** `run()` returns an
   error for the operator to read and nothing else. Over a socket it may one day return before the
   command has been applied; a caller that repaints on the return value would then repaint too
   early, and one that repaints on the notification does not change at all.

3. **Errors are values.** Nothing throws across this boundary, because one day it is a boundary
   exceptions cannot cross.

### Notifications carry the command that was applied

Including for undo, where it is the inverse. This is what lets a future remote implementation keep
a **local replica** of the document — which it must, since a UI cannot do a round trip to another
process to lay out a list — and follow along by applying what it is told rather than re-fetching.
It also makes `Change` the same shape as a journal entry, which is not a coincidence: both answer
"what happened to this document".

Making `DocumentHistory::undo()` return the applied command rather than `void` was the change that
enabled this, and it costs nothing.

## Two bugs this flushed out

**Saving threw away the undo history.** `save()` was calling `DocumentHistory::reset()` to pick up
the new generation, which also cleared both stacks — so every save silently made the preceding
hour unundoable. The generation is *file metadata*: not something the operator did, and not
something they can undo. `note_saved()` sets it and leaves the history alone.

**`save_as()` deadlocked against its own lock.** It acquired the project lock and then called
`open_project()`, which acquires it again. That was harmless while locks were owned by the process
(ADR-0022 changed them to open-file-description locks for exactly the reason that it is *not*
harmless), and became a hang the moment the lock started working properly. A fix in one place
turning a latent bug into a visible one, two layers up, is the argument for both of them.

## Smaller decisions worth naming

- **Recovered work is dirty on open.** Journal entries replayed on open are changes the project
  file does not hold. Reporting the document as saved would let the operator close it and lose
  exactly the work that was just recovered.
- **An undo is journaled like anything else.** A crash straight after an undo must not bring the
  undone change back.
- **A journal that cannot be opened is a warning, not a refusal to open the project.** The
  operator can still work; they just cannot be promised that unsaved work survives a crash, and
  that is worth saying loudly rather than making the project unopenable. Tested by making the
  journal read-only.
- **`dirty` is conservative.** Undoing back to the state that was last saved still reports dirty.
  Tracking it exactly needs a marker in the undo stack that survives the stack being bounded; the
  conservative answer prompts a save that was not needed, which is the harmless direction.

## Still open

- **The process split itself**: `lipc`, `munkicast-session`, and the proxy. This ADR is the
  preparation for it, not the thing.
- **The action registry**, which is the next piece to sit on this boundary. Decided now so it is
  not decided by accident later: an action is **data** — `{id, title, scope, default_trigger}` —
  where `scope` is `Application` or `Global`, declared per action, and both scopes resolve to the
  same `Command` sent to the same `Session`.
  - `Application` actions (undo, delete layer, rename) are handled by the UI's own key handling
    and never touch the portal. Making them global would be actively harmful: Ctrl+Z firing while
    the operator types in a browser.
  - `Global` actions (start/stop stream, switch scene, push-to-talk) are declared to
    `org.freedesktop.portal.GlobalShortcuts` and must keep working when the UI is dead — which is
    one of Phase 1's own acceptance tests, so the portal session belongs to the session process.
  - Scope is **fixed per action to begin with**. Making it user-editable later is then an override
    map in settings — a data change — rather than moving actions between two dispatch mechanisms.
