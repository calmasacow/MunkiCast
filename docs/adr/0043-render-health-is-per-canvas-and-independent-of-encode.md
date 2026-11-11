<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0043 — Render health is per canvas and independent of encode

- **Status:** accepted
- **Date:** 2026-10-18
- **Depends on:** ADR-0020, ADR-0040, ADR-0042

## Context

`munkicast-render` now composites preview frames without an encoder, while Broadcast Stats still
shows an obsolete "no render process" message. Render cost and render delay are useful before
any output exists. A total across canvases would hide a slow canvas inside a fast one.

## Decision

The renderer reports completed preview count and cumulative render duration for each canvas.
Average render time is cumulative duration divided by completed count. The UI counts scheduled
live-preview ticks discarded while a previous render is still in flight. This measures render
backpressure; elapsed time between composites does not count, because static scenes, unchanged
captures, and pauses are not dropped frames. The configured canvas FPS is shown separately from
observed preview updates.

Completed count and render duration travel with each frame over preview protocol v4. The UI
counts skipped live-preview ticks locally and keeps a reset baseline per canvas; reset clears
the displayed counts and average without resetting the renderer or another client. No encoder
counters are inferred: encoding and outputs remain unavailable until implemented.

## Consequences

- Broadcast Stats shows canvas target FPS, average render time, completed renders, and live-preview
  ticks skipped under render backpressure for each canvas while the renderer is attached.
- A canvas with no completed preview has unavailable timing and zero completed frames.
- The reset control clears displayed render and output counter baselines. Output values remain
  unavailable until an output reports them.
- These are preview-render measurements. They do not claim sustained program output or
  end-to-end presentation latency.
