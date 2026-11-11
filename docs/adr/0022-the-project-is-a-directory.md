<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0022 — A project is a directory, and saving it is a transaction

- **Status:** accepted, implemented
- **Date:** 2026-10-12
- **Depends on:** ADR-0021 (`ldoc` holds workspace-independent document state)

## Context

OBS keeps a scene collection in one JSON file. Two consequences follow, and both are failures
people actually hit: a single bad byte anywhere in it loses every scene at once, and the file is
large, machine-written and unreviewable, so nobody notices a problem until the collection will not
load. The format also stores settings sparsely, which is why OBS's own importer has to carry a
per-source-id table of what an absent key used to mean.

## Decision

```
MyShow.munkicast/
  project.json          canvases, buses, decks, sources, outputs, settings, scene manifest
  assets.json           the asset manifest
  scenes/<ulid>.json    one file per scene
  .munkicast/
    lock                an OS-held lock
    staging-<gen>/      the next generation, until it is committed
    quarantine/         scene files that would not parse
```

### 1. One file per scene, so corruption is bounded

A scene that fails to parse is **moved to `.munkicast/quarantine/`**, reported by name with the
path its bytes went to, and every other scene opens. The project is usable, nothing is silently
dropped, and the operator can decide what to do with the file. Losing a whole collection to one
bad byte is not reproducible here, which is the entire reason the directory exists.

### 2. Everything is written explicitly, including defaults

A file that states every value cannot acquire new meaning when a later build changes a default.
Reading is lenient (an absent key takes this build's default, so an older project opens) and
writing is strict. A key that is *present but the wrong type* is an error, because that is
corruption or a bad hand-edit rather than an older version, and reinterpreting it loses work.

An enum value this build does not recognise is likewise an error, not a fallback. Quietly turning
an unknown blend mode into `normal` changes how the show looks and then writes that change to
disk on the next save.

### 3. Saving is a transaction, not a sequence of atomic writes

Per-file `tmp` → `fsync` → `rename` protects each file individually and gives **no** consistency
across `project.json`, `assets.json` and `scenes/*.json`. An interruption can leave a project
naming scenes that were never written.

1. Everything is written into `.munkicast/staging-<gen>/` and fsynced. The project directory is
   untouched until this finishes.
2. Scene files and the asset manifest are **hard-linked** into place, not moved. Staging stays
   complete, so an install interrupted part-way can still be finished from it. Moving them would
   empty staging at exactly the moment it is needed.
3. `project.json` is installed last and carries the new generation. It is the commit point, and
   it can never name a file that is not already on disk.
4. Scene files for deleted scenes are removed *after* the commit, so the old `project.json` never
   refers to a file that has already gone.

Recovery on open is roll-forward only. A staging directory whose generation is newer than the
committed one means the install never finished; if it is complete, it is finished, and if it is
not, the interruption happened while writing, nothing was installed, and it is discarded. The
previous generation is intact in both cases. **No roll-back path is needed, and so none exists** —
one recovery path cannot disagree with another.

### 4. The lock is an open-file-description lock

`fcntl(F_OFD_SETLK)`, not a pidfile and not plain `F_SETLK`.

A pidfile goes stale on a crash and leaves people deleting lock files by hand, which teaches them
to ignore the lock. Plain POSIX record locks are owned by the *process*, so a second
`open_project()` inside one process takes the lock again and both callers believe they hold it —
which is the case that matters for us, with a UI and a background importer in one process. OFD
locks are owned by the file description and are still released by the kernel however the holder
dies. Phase 3 wants `flock()` on macOS and `LockFileEx()` on Windows for the same property.

### 5. A newer schema version opens read-only

Parsing it, dropping the fields this build does not understand, and writing it back is how a
downgrade silently destroys work.

### 6. Two things are tagged in the JSON, and both for the same reason

JSON has one number type, and jsoncpp writes a `double` of `3.0` as `3`, which parses back as an
integer — so a parameter declared as a float would silently become an integer on the first save
and stay one. A bare JSON number is therefore always a double, and whole numbers carry
`{"int": 7}`. Likewise a ULID is written `{"id": "..."}` rather than bare, because a 26-character
string is otherwise indistinguishable from a text parameter that happens to look like one.

Floats are overwhelmingly the common case in parameters, so the untagged form stays the readable
one. Both of these were found by the serialise→parse→serialise property test rather than by
reasoning, which is the argument for having it.

### 7. There is nowhere in the format to put a stream key

`Destination` carries a `credential_ref` — a name — and the value lives in a store outside the
project directory. The directory is git-diffable and easy to share when asking for help, and a
key in it means every one of those acts leaks it. A live key was already read out of a config
dump during this project's own development. A format that *cannot* carry the secret cannot leak
it, which is more reliable than any amount of redaction discipline. A test asserts the serialised
output contains no field that could hold one, so adding such a field fails the build.

## What the tests found

Writing the interrupt-at-every-step test immediately found a bug in the recovery code it was
written to exercise: `install_staging` checked for each staged file as it installed it and
returned on the first one missing, having already installed the ones before it. An incomplete
staging directory therefore produced exactly the mixture the protocol exists to prevent — some
scenes from the new generation, the rest from the old. Staging is now verified complete before
anything is installed.

The lock test found the `F_SETLK` process-ownership trap above. Both are the kind of defect that
reads as correct and fails only under interruption, which is to say: only in front of an
operator, mid-show, once.

## Consequences

- A project directory can live in git, and a change to it can be reviewed.
- Opening a project returns a list of `Problem`s rather than succeeding or failing. Quarantined
  scenes, missing files, recovered saves and every structural complaint from `validate()` arrive
  together, because a document has many independent things wrong with it after a bad import and
  fixing them one error at a time is whack-a-mole.
- Every save rewrites every scene file. For the project sizes in question this is a few hundred
  kilobytes and one fsync batch; only rewriting changed scenes needs per-scene content hashing,
  and is worth doing when a project gets large enough to notice.
- Still open: the append-only `journal.jsonl` that gives undo across restarts, backup rotation,
  and the forward-only migration path with golden fixtures. The schema version and the read-only
  refusal that migrations hang off are in place.
