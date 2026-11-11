<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0028 — One word for a source, in the code as well as on the screen

- **Status:** accepted
- **Date:** 2026-10-13
- **Depends on:** ADR-0027 (scenes belong to a canvas) — they share one format bump

## Context

The screen settled on one word some time ago: anything that can go into a scene is a **source** —
a capture card, a webcam, a browser, a flat colour — differing by type, not by kind. The separate
"Inputs" panel was a second word for the same thing and was folded into Sources.

The code did not follow. A source placed in a scene is a `Node` whose kind is `NodeKind::Layer`
and whose payload is `LayerData`. The split was deliberate and is defensible: placing a source
gives it a transform, layout, blend and effects that the source itself does not have. Same thing,
two amounts of information.

It is also the only place in this program where the screen and the code use different words for
what the operator thinks of as one thing, and the entire explanation of the difference lives in a
comment at the top of `SourcesPanel.qml`. Asked, the operator's answer was to drop it: one word is
less confusing.

**The complication, stated plainly, because it does not go away.** There really are two things.
One camera is opened once and can sit in six scenes at six positions with six sets of effects
(ADR-0027 §4 makes that explicit). So this is not a merge of two types into one. It is a decision
about **which of the two keeps the word**, and the other one needs a name that is not "layer".

## Decision

The scene tree keeps being a tree of nodes. A node is a group, an adjustment, a mask, a reference
— or a **source**:

- `NodeKind::Layer` → `NodeKind::Source`
- `LayerData` → `SourceData`
- `SetLayerSource` → `SetNodeSource` (the only command carrying the old word; every other one is
  already `SetNode*`)
- `Document::addLayer` → `Document::addSourceNode`, and the QML-facing `layers` model and
  `layerProperties` follow

The project-level `Source` keeps its name, its type ids (`source.v4l2`, `source.browser`,
`source.color`) and its place in `Project::sources`. The word "layer" leaves the codebase.

```
scene
└─ Node { kind: Source, data: SourceData }
     └─ source: Id  ──►  Project.sources[]
                           Source { name, type, settings }
```

Read aloud: **a source node bound to a source**. The node is where it is and what is being done to
it; the source is what it is.

`Source` and `SourceData` differ by four characters, and that is the cost of the decision. It is
paid in one place: the comment currently at the top of `SourcesPanel.qml` moves to `node.hpp`,
where the two types actually meet, and says which is which.

## Rejected alternatives

**Leave it as two words.** The position until today, and the one this file's author argued for:
the distinction is real, so the code may as well name it. Rejected by the operator, and the
counter-argument is better than the argument — the screen says "source", the code says "layer",
and the one document reconciling them is a comment in a QML file that nobody reading the C++ will
ever open.

**Give `Source` to the scene item and rename the project-level one to `SourceDef`.** Closer to one
word in the place the operator actually looks. Rejected because the project-level type is the half
that is *finished*: the add dialog, the recently-created list, the use counts, the settings schema
and every `source.*` type id are built around it being `Source`. This option moves all the churn
onto the working half to spare the half that is about to be rewritten anyway.

**Collapse the two types into one.** Rejected on the operator's own answer: one camera used twice
is one camera, not two. Two placements of one device is the requirement, so two types is the
model. A single type would have to carry either one transform shared by six scenes or a map of
transforms keyed by scene, which is the second type wearing a disguise.

## Consequences

**One format bump, not two.** This change and ADR-0027 both alter what is on disk, so they ship as
one `kSchemaVersion` 2 with one migration and one set of fixtures. Doing them separately would
mean two migrations, and the second would have to be written against the first.

**Wide but mechanical.** `ldoc` (node, command, json, validate), `lsession`, the `munkicast-ui`
bridge and models, every panel that says "layer", the CLI `inspect`, and the tests. The compiler
finds all of it; the JSON reader is the one place it will not, since field names are strings.

**Must be tested.** That a version 1 project whose nodes say `"kind": "layer"` loads as source
nodes and round-trips as version 2; that `serialise → parse → serialise` remains a fixed point
across the rename; and that the existing property test — random command sequences asserting undo
and redo return the project byte for byte — passes unchanged over the renamed command.

**What does not change.** Nothing the operator sees. That is the point of doing it: the screen was
already right.

## Implementation note — the C++ landed first, the wire format did not

*Added 2026-10-13.* The rename in the code is done: `NodeKind::Source`, `SourceData`,
`SetNodeSource`, and in `munkicast-ui` `Document.nodes`, `nodeProperties`, `selectedNode`,
`addSourceNode` and the `setNode*` family. The layer/source explanation now lives at `SourceData`
in `libs/ldoc/include/ldoc/node.hpp`.

**The serialised strings were deliberately left alone**, and that is not an oversight. A node
still reads and writes `"kind": "layer"`, `SetNodeSource` still travels as `layer.set-source` in
the journal and on the websocket API, and `kSchemaVersion` is unchanged. Changing them here would
have meant a format bump now and another for ADR-0027 — the two migrations the Consequences above
exist to prevent. The words on the wire change in the single combined bump, with its migration and
the tests listed above. Each place the old string survives carries a comment saying so.

The four `layer.*` action ids in `lsession` also keep the old word: they are keys in the operator's
bindings file, which no schema version covers, so renaming them would silently drop a chosen key.
Their display names say "source".

One thing above turned out not to be quite true: the screen was *not* already entirely right. The
undo history ("Hide layer"), the Properties panel's empty state and its kind field, the mixer's
routing line and tips, the Info panel's count, the settings dialog's use count and the shortcut
names all still said "layer". They say "source" now, following the operator's call that the word
leaves the screen as well as the code.

## Implementation note, 2026-10-14 — the wire format, in the combined bump

The rest of it landed with `kSchemaVersion` going to 2, beside ADR-0027 and ADR-0029, which is
the single migration the note above exists to protect.

- **`"kind": "layer"` is now `"kind": "source"`.** `node_kind_from_string` still accepts
  `"layer"`, for as long as a version 1 project can be opened at all; nothing writes it.
- **`layer.set-source` is now `node.set-source`.** The reader accepts both. The journal is
  replayed during crash recovery, and refusing an entry written by last week's build would lose
  exactly the work the journal exists to save — so the old spelling stays readable rather than
  tidy. `command_name()` only ever returns the new one, which is what the websocket API's
  stability promise is about.
- **`kind_str()` in `document.cpp` and `kind_word()` in `inspect.cpp` are deleted.** They existed
  only to say "source" where `to_string` said "layer", and both call sites are back on
  `to_string`. Leaving them would have quietly kept two vocabularies alive, which is what this
  ADR is against.
- **The four `layer.*` action ids in `lsession` are still untouched**, for the reason given
  above: they are keys in the operator's bindings TOML, which no schema version covers.

The frozen `golden-project-v1` fixture still contains `"kind": "layer"`, which is what makes the
compatibility claim a test rather than an assertion.
