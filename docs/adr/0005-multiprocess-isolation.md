<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0005 — Multi-process isolation, and the honest scope of the stability promise

- **Status:** accepted
- **Date:** 2026-10-11

## Context

"Stability is paramount" is the project's primary technical requirement. The biggest available lever
is not the implementation language — it is refusing OBS's monolithic process model, where a single
bad plugin or driver fault kills an in-progress stream.

## Decision

Six process roles. `munkicast-session` is a tiny supervisor that owns the document and contains **no
third-party code**; `munkicast-render` owns the GPU, mixing, encoders and outputs; risky work
(FFmpeg demuxing untrusted files, browser engines, device SDKs) lives in pooled `munkicast-worker`
processes; the Qt/QML UI has **zero authority** and may be killed and relaunched.

Every document mutation is a serializable, validated, journaled, undoable `Command` sent to
`munkicast-session`. UI, hotkeys, Stream Deck, the websocket API and scripting are all merely command
producers — that is what makes "the UI has no authority" true rather than aspirational.

### The promise, scoped honestly

The claim is **"a contained CPU fault in a worker never drops the stream"** — *not* "nothing can drop
the stream." Process isolation does **not** contain:

- **GPU device loss.** `VK_ERROR_DEVICE_LOST` takes the whole device, which `munkicast-render` shares
  across every source. Requires an explicit recovery path: tear down and rebuild device and
  resources, resume outputs, log as a first-class event rather than a crash.
- **Stalled fences / a wedged worker still holding slots.** All waits are deadline-bounded; on expiry
  the compositor proceeds with the last-good frame and the worker is killed and restarted.
- **Memory pressure**, which is global. Hence `RLIMIT_AS` per child plus a render-side budget.
- **Blocking network writes.** A slow RTMP destination must never jam the mux or encode thread:
  per-destination non-blocking queues, bounded depth, explicit drop policy (non-reference frames,
  then whole GOPs, then disconnect-and-reconnect that one destination only).

### Where NOT to isolate

Built-in effects and compositing stay in-process — crossing a boundary per effect per frame is
absurd. One worker *per source instance* is too many (a 30-source scene would be 30 processes): pool
by risk class and portal session, and promote a repeat-crasher to its own process. Trivial sources
(colour, text, image) run in-render, but image and font *decoding* routes through the `media` worker.

## Rejected alternatives

**Monolith with careful coding.** This is OBS's position, and the outcome is visible in its crash
reports. Discipline does not survive third-party code.

**Thread isolation instead of process isolation.** Shares the address space, so it contains nothing.

**One process per source.** Rejected on resource grounds; see pooling above.

## Consequences

- An IPC boundary must be designed, fuzzed, and version-managed (`schema/ipc.capnp`). Real cost,
  accepted.
- Testing must cover **hung** workers and **stalled destinations** alongside killed workers, and must
  exercise device-lost recovery. TSan is per-process and cannot validate cross-process shared memory;
  that needs generation/sequence assertions plus delay-injection stress.
- The record path always writes fragmented containers so an in-progress file is always valid, and a
  render restart auto-resumes to a `-002` segment with RTMP auto-reconnect.
