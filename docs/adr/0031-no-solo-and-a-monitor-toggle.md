<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0031 — No solo, and a monitor toggle instead

- **Status:** accepted
- **Date:** 2026-10-16

## Context

`Bus::solo` has been in the document since the audio model was first written, copied in from
every mixing console without the question being asked. [ADR-0030](0030-the-mixer-shows-sources.md)
left it in place and noted that adding solo to a *source* strip would be a model change.

The operator ruled on it instead, looking at the mixer with source strips in it:

> there is no solo. too dangerous to have if ther eend up bein a lot faders and it gets
> triggered it would be real bad when live the only buttons should be toggle for mute and one
> for monitor.

That is right, and the reason is specific to this application rather than to mixers in general.
On a console, solo is a monitoring aid: it changes what the engineer hears in their headphones
and nothing reaches the audience. Here `Bus::solo` is a property of the **document**, which is
what the program mix is built from, so a solo would mute every other bus *on air*. A mis-click
during a show takes the stream down to one source, and nothing about the control says so. The
risk grows with the number of strips, and ADR-0030 just turned a two-strip panel into one with
a strip per audible source in the scene.

What an operator actually wants from that part of the console is to **hear one thing locally
without changing the broadcast**. That is monitoring, and the reference the operator supplied
(a strip from Elgato's Wave Link) carries exactly two buttons under the fader: mute, and a
monitor toggle drawn as a headphone.

## Decision

**`Bus::solo` is removed from the document.** Not deprecated or hidden behind a disabled
button: a field the UI never writes is a field the next person restores a control for.
`SetBusSolo` goes with it, along with the role, the model column and the "S" button.

**Every strip gets exactly two buttons: mute and monitor.** The same pair on a source strip and
on a bus strip, because the operator's instruction was about the panel and not about one half
of it.

**Monitoring is per node and per bus**, stored as `monitor` on `AudioRouting` and on `Bus`.
It is a boolean and not OBS's three-way (off / monitor only / monitor and output), because the
third state is "monitor **and** keep sending", which is what a plain on/off already means here:
monitoring never removes anything from the program mix. OBS needs the distinction because its
monitor setting doubles as a routing switch; ours does not, and inventing a state that silences
the broadcast would reintroduce exactly the hazard solo was removed for.

**Monitoring cannot affect the program mix. This is the rule the rest of this must preserve.**
When the audio host exists, the monitor flag selects a local output and nothing else. Any later
feature that makes a monitor button change what goes out needs its own ADR saying why, because
it would make the safe control unsafe.

## Rejected alternatives

- **Keep solo, guard it with a confirmation.** A control that needs a confirmation every time
  is a control nobody wanted. It also does not help the case that matters: the mis-click
  happens *during* a show, when a modal is the second emergency.
- **Keep solo but scope it to monitoring, as a console does.** This is what solo ought to mean
  and it is what the monitor toggle now does. Keeping the name would mean a button labelled
  "S" that does not do what "S" does on the console it was copied from — worse than not having
  it, because the operator would have to learn that it is safe here.
- **Leave `Bus::solo` in the format, unused.** Dead fields come back. Removing it is a format
  change the reader already tolerates: unknown keys are ignored, so a project written before
  today loads with the flag dropped, and nothing in the document ever acted on it.
- **A three-state monitor, following OBS.** Covered above: the third state exists in OBS to
  resolve an ambiguity we do not have.
- **Monitor on the bus only.** Monitoring one source is the ordinary case — "is this mic
  live?" — and a bus-only control would mean soloing a source onto a scratch bus to hear it,
  which is the removed gesture wearing a hat.

## Consequences

- `kSchemaVersion` does **not** move. Removing a field the reader can ignore and adding one
  that defaults to `false` are both backward and forward compatible: an older build reading a
  newer project ignores `monitor`, and gets a project it can still render.
- The "S" button is gone from the bus strip; both strip kinds now draw mute and monitor.
- Nothing monitors anything yet, and the panel says so — the footer's "no audio host — levels
  only" already covers it. The flag is document state the audio host will read when it exists,
  in the same way the faders are.
- `SetBusSolo` disappearing is a command-surface change. The command count in the docs moves
  with it; `SetNodeAudio` and `SetBus` already carry the new flag, so no command was added.
