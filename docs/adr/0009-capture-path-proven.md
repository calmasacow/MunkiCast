<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0009 — Capture path: portal -> PipeWire -> dmabuf works; Vulkan import unproven

- **Status:** accepted for stages 1-2; **stage 3 (Vulkan import) is written but NOT yet proven**.
  **The root-cause section below is superseded by ADR-0010**: the phantom node is real but was not
  the primary cause of `no more input formats`. That was a modifier-parsing bug of ours.
- **Date:** 2026-10-11

## Context

Phase 0's remaining unknown was whether we could drive portal ScreenCast ourselves and receive
zero-copy dmabuf frames with explicit DRM format-modifier negotiation — the thing the plan called
the real go/no-go.

## Result: it works

`tools/capture-spike --stage 2` on the development box (Hyprland 0.56.2, portal 1.4.1,
PipeWire 1.6.8, NVIDIA 610.57.04):

```
portal ScreenCast version 6
AvailableSourceTypes 0x7 (monitor|window|virtual), AvailableCursorModes 0x3 (hidden|embedded)
node 377  2560x1440 @ (0,0)  type=monitor  mapping_id=DP-3
negotiated 2560x1440 BGRA (drm AR24) modifier LINEAR fps 0/1
fixating modifier LINEAR for BGRA
buffer type: DmaBuf   planes: 1   [0] offset=0 stride=10240 maxsize=0
frames: 30 dmabuf, 0 shm
```

Stride 10240 = 2560 x 4, correct for BGRA. **Single plane, LINEAR modifier** — the best possible
case for ADR-0007's cross-GPU transfer, since a linear single-plane buffer is the layout most likely
to import on a second vendor's GPU.

## What the protocol actually requires

Four things, each of which produced `no more input formats` when missing or wrong. Recorded because
none is obvious and all cost real time:

1. **Predict the Request object path and subscribe before calling.** Every portal method returns a
   Request path and answers later via a `Response` signal. The path is derived from our unique bus
   name (leading `:` stripped, `.` -> `_`) plus a token we choose, so we must subscribe *before*
   issuing the call or the reply can outrun the subscription.
2. **Modifier negotiation is a two-round dance.** Advertise the modifier list as a
   `CHOICE_Enum` with `SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE`; the server
   replies with the *intersection*, still unfixated; we must then re-advertise **exactly one**
   modifier via `pw_stream_update_params` and wait for the next `param_changed`. Omitting this
   fixation step fails negotiation outright.
3. **`SPA_POD_PROP_FLAG_DONT_FIXATE` must be honoured by checking the prop flags** on the returned
   format, not assumed. `spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier)` then test
   `flags & SPA_POD_PROP_FLAG_DONT_FIXATE`.
4. **Buffer data types must be declared after format agreement.** `SPA_PARAM_Buffers` with
   `SPA_PARAM_BUFFERS_dataType` as a flags-choice including `1u << SPA_DATA_DmaBuf`. Without
   DmaBuf in the mask, frames arrive as shm even when the modifier was negotiated.

Two C++-specific traps: SPA's `SPA_RECTANGLE`/`SPA_FRACTION` are C compound literals whose address
cannot be taken in C++ (use named constants), and `std::format` needs a compile-time format string
so runtime-built shell commands must be concatenated.

## Correction: the restore-token theory was wrong

An earlier draft of this ADR blamed intermittent `no more input formats` failures on stale
`--restore-token` values. **That conclusion was incorrect** and is retracted. Two real causes were
found afterwards, and a third remains open:

1. **Four monitors, each a different capture source.** This machine has DP-1, DP-2, DP-3 (all
   2560x1440@165) and HDMI-A-3 (3840x2160@60). The portal picker selects one per session, and the
   spike logs `mapping_id` — runs that "succeeded" were DP-3 and DP-1, failures were other picks.
   Comparisons of modifier lists across runs were therefore comparing *different sources*, which is
   not an experiment. **Any future measurement must pin the output** (log and assert `mapping_id`).
2. **Leaked portal sessions.** `org.freedesktop.portal.Session.Close()` was never called, so every
   run left a live capture session on the compositor. Adding the close improved consecutive-run
   success from roughly 1-in-3 to 3-in-4. A `SessionCloser` RAII guard now runs on every exit path.
   **A real implementation must close its sessions**; leaking them degrades the compositor for every
   subsequent client, not just itself.
3. **The actual root cause** - a restored session returns a node id that does not exist. See the
   section below.

## A harness bug to avoid repeating

A `--sweep` mode was added to test every modifier variant inside one portal session (one picker
interaction instead of dozens). It reported *all* variants failing, including ones that had just
succeeded standalone — a result that is **invalid**, because:

- `run_stream` calls `pw_init()` and `pw_deinit()` per invocation, and those are not reliably
  re-entrant within a process;
- the fd from `OpenPipeWireRemote` is effectively single-use, so `dup()`-ing it per attempt does not
  give independent connections.

The fix is to hold **one** `pw_init` + `pw_context` + `pw_core` for the process lifetime and create
and destroy only `pw_stream` objects inside it — which is also how the real capture worker must be
structured, since it will hold a long-lived connection and restart streams on reconfiguration.

## What the compositor actually offers

From `capture-spike --dump-formats` on a monitor node (development box, NVIDIA primary):

```
EnumFormat[0]  video/raw  format BGRA
  modifier { 12 x NVIDIA block-linear (0x03...), DRM_FORMAT_MOD_INVALID }
  size 2560x1440   framerate 0/1   maxFramerate [1/1, 120/1]
EnumFormat[1]  video/raw  format { BGRA, BGRx }      <- shm fallback, no modifier prop
  size 2560x1440   framerate 0/1   maxFramerate [1/1, 120/1]
```

Note `framerate` is **fixed at 0/1** ("source-driven") with the real bound in `maxFramerate`, and
that the modifier list is per-format. `DRM_FORMAT_MOD_LINEAR` is *not* in the advertised list on
this node, yet LINEAR is what the fixation round settled on — so the advertised enumeration is not
the whole truth, and the fixation result is the only authority on what we actually got.

## Honest status of stage 3

The Vulkan import path is **written, compiling, and never successfully exercised.** It has not failed
either: negotiation never survived long enough in a stage-3 run to hand it a frame. Nothing here
should be read as evidence either way about whether the import works.

What stage 3 *did* establish, via `vkGetPhysicalDeviceFormatProperties2` +
`VkDrmFormatModifierPropertiesListEXT` on the RTX 5080 for `DRM_FORMAT_ARGB8888`:

```
importable: NVIDIA(0x300000000606010 .. 0x300000000606015), LINEAR     (7 total)
```

And one genuinely interesting mismatch: a successful stage-2 run settled on
**`NVIDIA(0x300000000e08014)`**, which is **not** in the device's importable list, while the node's
advertised set contained both `0x...606010-015` and `0x...e08010-015`. If that holds up, it is
exactly the "accept a format then fail at import" trap the design set out to avoid, and the reason
modifier intersection must be computed from the Vulkan list rather than assumed. **It needs
confirming on a pinned output before being treated as fact.**

## ROOT CAUSE: a restored session returns a phantom node id

Proven by instrumenting the probe to distinguish "node exists but has no formats" from "node never
appeared in the registry at all":

```
info  CAPTURE SOURCE: DP-3 (monitor) 2560x1440 node 39
warn  node 39 NEVER APPEARED in the registry after 5000 ms (52 other nodes visible)
      - the portal handed back a node id that does not exist
  INVALID + LINEAR (known good) : FAIL (no more input formats
                                  [param_changed never fired: server rejected every param])
```

`org.freedesktop.portal.ScreenCast` accepts a `restore_token`, returns a session handle and a node
id, and reports success — but **that node is never registered in PipeWire.** Note the id (39) is far
below the ids of live nodes, i.e. plainly stale. Every subsequent step then fails in a way that
mimics a format problem: no formats to intersect, so `param_changed` never fires and the stream
errors with `no more input formats`.

### Two retractions, in order

1. An early draft blamed stale restore tokens. **That was correct**, but it was asserted without a
   mechanism and on confounded evidence (see below), so it did not deserve to be believed.
2. A later draft *retracted* that and blamed a node-readiness race, on the grounds that a 5000 ms
   wait did not help. **That retraction was wrong.** The wait did not help because the node did not
   exist, not because it was slow. The readiness poll was measuring the wrong thing until it could
   report `node_found`.

The confounders that made this take so long, all of them methodological:

- **Four monitors.** The picker selects a different capture source per session, so cross-run
  comparisons compared different sources. Fixed by requesting monitors only and logging
  `mapping_id` on every run.
- **Leaked sessions.** `Session.Close()` was never called, and `timeout`'s SIGTERM skipped the
  destructor. Both fixed; the signal handler is now mandatory, not a nicety.
- **An invalid sweep harness.** Per-attempt `pw_init`/`pw_deinit` plus a dup'd single-use fd. Fixed
  by one long-lived session with streams created inside it.
- **No way to tell the failure modes apart.** Everything surfaced as `no more input formats`.

### Required behaviour for the real capture worker

1. **Validate a restored session before trusting it.** After `Start`, bind the node through the
   registry and confirm it exists *and* advertises at least one `SPA_PARAM_EnumFormat`. The
   `pw_wait_node_ready` helper does this and reports which of the two failed.
2. **On failure, discard the token and fall back to a fresh session with the picker.** Never surface
   a capture error to the user for what is a stale-token condition.
3. Persist restore tokens only after a session has successfully produced frames.
4. Keep the `node_found` / `params_seen` distinction in diagnostics permanently. Without it this bug
   is indistinguishable from a modifier incompatibility, which is where several days could go.

## Still open

- **Prove stage 3.** Bring the dmabuf fd into a `VkImage` via
  `VK_EXT_image_drm_format_modifier` + `VK_KHR_external_memory_fd`, with `sync_file` acquire/release
  on an exported timeline semaphore, and prove correctness by readback.
- The modifier list we advertise should come from
  `vkGetPhysicalDeviceFormatProperties2` + `VkDrmFormatModifierPropertiesListEXT`, not a guess.
- Cross-vendor import (NVIDIA-exported linear dmabuf -> Intel) for the VAAPI encode path.
- Re-run all of the above on `argus` (Turing + Gen9.5), per ADR-0008.
