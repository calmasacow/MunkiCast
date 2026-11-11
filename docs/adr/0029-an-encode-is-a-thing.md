<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0029 — An encode is a thing, not a property of an output

- **Status:** accepted
- **Date:** 2026-10-13
- **Depends on:** ADR-0007 (dual-GPU concurrent encode), ADR-0020 (canvases are independent
  pipelines), ADR-0023 (the command bus)

## Context

`Output` owns its encoder by value:

```cpp
struct Output { Id id; std::string name; Id canvas; EncoderConfig encoder;
                Destination destination; bool enabled; };
```

One output, one encode. Four destinations therefore cost four encodes, and NVENC's concurrent
session count is a hard ceiling that ADR-0008's probe note already says must be measured rather
than assumed. Spending four sessions where two would do is the difference between a show that
runs and one that does not start.

The operator's actual setup today, in OBS with the Aitum multistream plugin:

| destination | canvas | encode |
|---|---|---|
| YouTube  | Main | 1440p60, ~40 Mbps |
| Twitch   | Main | downscaled to 1080p60, re-encoded at ~8 Mbps |
| Kick     | —    | **the Twitch bitstream**, sent to Kick |
| Facebook | —    | **the Twitch bitstream**, sent to Facebook |

Two encodes. Four destinations.

This was misread once already, and the misreading is worth recording because it is the obvious
one. Seeing "Reuse Output Video Encoder" in a mockup, the question looks like *when may two
outputs share an encoder instance, given they may want different resolutions?* — and the answer
looks like "only when resolution, rate and colour all agree, so the dropdown must filter itself".

That is the wrong question. Reuse is not sharing a *configuration*. It is sending the **same
encoded bitstream** to a second destination. The reusing output has no encoder settings of its
own at all, so there is nothing for it to disagree about. Its resolution is whatever was encoded,
because it is not encoding.

### How Aitum actually does it

Read at `Aitum/obs-aitum-stream-suite@72e61fc`, since the operator's description came from using
it. It is the reference-pointer model, and it resolves the pointer **at start time against a
running output**:

```cpp
// src/utils/widgets/output-widget.cpp
auto output_video_encoder_name = obs_data_get_string(settings, "output_video_encoder");
...
auto other_output = obs_get_output_by_name(other_output_name.c_str());
if (other_output) {
    auto vei = (int)obs_data_get_int(settings, "video_encoder_index");
    venc = obs_output_get_video_encoder2(other_output, vei);   // the *live* encoder object
    ...
}
if (!venc) {
    blog(LOG_WARNING, "failed to start output '%s' because '%s' was not started", ...);
    QMessageBox::warning(this, obs_module_text("OtherOutputNotActive"), ...);
    return nullptr;    // refuses to start
}
```

An output stores the **name of another output** plus an encoder index. Its locale file carries
the consequence verbatim:

> `OtherOutputNotActive="Unable to start output. This output is configured to use an encoder from
> another output, which is not currently active. Please start the other output first."`

So the ownership question this ADR raises is not hypothetical and the shipped answer is: **the
operator must start outputs in dependency order, and is told off when they do not.** Stopping
Twitch does not just drop Twitch; it is the thing Kick and Facebook were holding a pointer into.

That is a reasonable thing for a plugin bolted onto someone else's output list to do. It is not a
reasonable thing for us to copy, because we are choosing the model rather than fitting into one.

## Decision

An encode becomes a first-class object in the document.

```cpp
struct Encode {
    Id id;
    std::string name;      // "Main 1440p", "Twitch 1080p" -- what the UI offers to reuse
    Id canvas;             // what is being encoded
    EncoderConfig encoder; // codec, implementation, bitrate, rate control, keyframes
    std::optional<Vec2> rescale; // Twitch's 1080p out of a 1440p canvas
};

struct Output {
    Id id;
    std::string name;
    Id encode;             // which bitstream this destination receives
    Destination destination;
    std::string credential_ref;
    bool enabled = false;
};
```

`Project::encodes` joins `Project::outputs`. `Output` loses `canvas` and `encoder`: the canvas is
a property of the encode, and asking an output which canvas it is on becomes one hop.

"Reuse Output Video Encoder" is then not a feature of the document at all. It is the UI pointing
a second output at an encode that already exists, and the model has no idea anything was reused.

### What this makes true for free

- **Cycles cannot be expressed.** An output names an encode; an encode names a canvas. There is
  no edge that can point back.
- **Ownership stops being a question.** An encode is not owned by whichever destination happened
  to be configured first. Deleting the Twitch *output* removes a destination; Kick and Facebook
  keep receiving, because the encode they reference is still there and still referenced. Disabling
  Twitch disables one destination, not three.
- **The session count is a number you can read.** How many encodes exist is `encodes.size()`,
  which is the figure to compare against the measured NVENC ceiling. `validate()` can say
  "this project asks for six concurrent encodes" without walking anything.

### What it makes explicitly unavailable, which is correct

Destinations sharing an encode share everything about it: bitrate, keyframe cadence, resolution,
colour. There is no per-destination bitrate when sharing, because there is one bitstream. An
operator who wants Kick at a different bitrate wants a second encode, and now has to say so.

## Rejected alternatives

**Keep `EncoderConfig` on `Output` and add `std::optional<Id> reuse`.** The smallest diff, the
one the mockup's wording suggests, and — as the Context shows — what Aitum actually ships.
Rejected because the questions it raises are not hypothetical: Aitum has had to answer "what
happens when the output you point at is not running" with a refusal to start and a dialog telling
the operator to go start the other one first. That is a start-ordering constraint invented by the
data model and then handed to the person running a show.

With a shared `Encode`, there is nothing to order. Starting Kick starts the encode Kick names if
it is not already running; starting Twitch afterwards finds it running and attaches. Stopping
Twitch stops one destination. No output is privileged, because no output owns the encoder.

It also makes "which destinations share this encode" a graph walk rather than a lookup, and
admits chains (A reuses B reuses C) that mean nothing.

**One encode per canvas.** Tidy, and wrong on the operator's own setup: Main carries both the
1440p YouTube encode and the 1080p Twitch one.

**Let the UI silently share an encode when two outputs' settings happen to match.** Rejected: two
outputs that coincidentally agree today would become one encode, and editing one of them would
have to either fork it or change both. Neither is predictable from the screen. Sharing is a thing
the operator states, not a thing an optimiser infers.

## Consequences

**A format change, and it should ride with the others.** ADR-0027 and ADR-0028 already bump the
project format. If this is built alongside them it is one version and one migration; built later
it is a second. Migration from today's shape is mechanical: every existing output becomes one
encode plus one output pointing at it, which is exactly what the current model means.

**The command set moves.** `SetOutputEncoder` and `SetOutputBitrate` become `SetEncode`.
`AddEncode` and `RemoveEncode` are new. `AddOutput` takes an encode rather than a canvas.
`RemoveEncode` must refuse while any output references it, or the refusal has to name them.

**The Outputs panel becomes two lists**, or one list grouped by encode — which is also the
clearest way to show the thing an operator most needs to see before going live: how many encodes
this show is actually asking the GPU for, and which destinations are riding on each.

**Start ordering stops being a concept.** This is the concrete win over the reference model and
the thing to protect in review: no output may ever fail to start because a *different* output is
not running.

**Must be tested.** An output whose encode was deleted; an encode with no outputs (legal, but it
costs a session and reaches nobody, so `validate()` warns); the migration turning one old output
into an encode-plus-output pair and round-tripping; and the operator's own four-destination,
two-encode arrangement as a named fixture, because it is the shape this ADR exists for.

## Implementation note, 2026-10-14 — the model half

`Encode` landed on this date, in the same version 1 → 2 bump as ADR-0027 and ADR-0028.
`Project::encodes`, `Output::encode` in place of `Output::canvas` and `Output::encoder`, and
`AddEncode` / `RemoveEncode` / `SetEncode` by symmetry with outputs. The golden v2 fixture is
the shape this ADR was written for: a 1440p encode to YouTube, and one 1080p encode made once
and sent to both Twitch and Kick. `munkicast inspect` reports it as **"2 concurrent encode(s)
asked of the GPU"** for three destinations, which is the pre-flight number this change exists
to make readable.

Three decisions the ADR left open, settled here:

**Removing an encode an output still names is refused.** Unlike removing a source — which leaves
its nodes drawing nothing, exactly as they would unbound — an output with no bitstream is not a
degraded state the model has a meaning for. It is the same line a deck's canvas sits on:
`validate()` calls it an error, so the command refuses it (ADR-0023). Point the outputs
elsewhere, or delete them, first.

**A new output gets its own encode, not a share of an existing one.** Sharing is the point of
this ADR, but it has to be asked for. An output that silently joined another's bitstream would
make changing its bitrate change someone else's live stream, which is the surprise this model
exists to make visible rather than to hide. The outputs panel does the reverse honestly: editing
a shared encode says, by name, which other destinations changed with it.

**The migration merges, rather than making one encode per output.** Version 1 outputs that asked
for the same canvas and the same encoder settings become one encode. The two are
indistinguishable in the document, and only one of them is true about the GPU: three outputs
configured identically really were three encodes before and really are one now. Each is named
from what it is — `1080p60 8 Mbps`, built from the canvas and the bitrate — and never from the
output that happened to own it, for exactly the reason this ADR's consequences give.

**Not built.** Start All / Stop All, which this ADR's ordering-free argument is about, has
nothing to start: no output actually runs yet, and the shell still reports "Start/Stop Streaming
is not wired up". Backtrack remains undesigned. An encode left unreferenced by removing its last
output is kept rather than collected, since nothing yet says which of the two the operator meant.

## Amendment, 2026-10-14 — where reuse is made

The model, the migration and the inspector all landed with the implementation note above, and
the outputs panel now *shows* a shared encode by name on every destination that carries it. But
nothing in the shell could *make* one: `addOutput` always creates a fresh encode, and the golden
fixture shares one only because it was written by hand. The Aitum-shaped case this ADR exists
for — encode 1080p once, send it to Twitch and Kick and Facebook — was unreachable from the UI.

Asked where that belongs, the operator chose the **outputs manager window**, opened by the gear
on the Outputs panel's footer, over a per-row encode picker.

The reason it is the better of the two is the one this ADR is about. A dropdown on a row answers
"what does this destination use", one row at a time, which is the output-centred view that made
the encoder look like a property of an output in the first place. The manager window is
encode-centred: it lists encodes, hangs destinations off them, and makes "two encodes, three
destinations" a thing you can see at a glance instead of something you infer by comparing
bitrates down a column. The number the inspector prints — concurrent encodes asked of the GPU —
is then visible in the same shape in the UI.

The row stays read-only about which encode it uses. Editing an encode from a row still edits the
shared encode and still emits the notice naming the other destinations that moved with it.
