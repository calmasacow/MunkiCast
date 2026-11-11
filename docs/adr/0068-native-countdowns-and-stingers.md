<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR-0068: Native countdown bindings and file-backed stingers

Status: Accepted

## Context

The Countdown Timers menu is a placeholder. Cut is the only working transition.
Both need native user workflows without libOBS. Existing source settings and deck
transition IDs can describe these features without a second plugin-only document.

## Decision

Countdowns bind to existing text sources. Their countdown.* settings hold duration,
mode (remaining, elapsed, target time), formatting, end message, optional end scene,
and start/pause/reset state. Controls commit settings once per action; workers draw
changing text from a shared calculation without journal writes each tick. Paused
state and elapsed offset persist. Running timers retain their UTC start anchor across
worker restarts. Worker-local monotonic interpolation avoids wall-clock jumps during
playback. A dock manages multiple bindings and imports an explicitly selected OBS
countdown companion config by matching unique text-source names. Unsupported plugin
options must be reported, never silently represented as supported.

A stinger is an ordinary media source tagged transitionStinger, with file, duration,
and cut-point settings. A deck selects transition.stinger:<source ID>. The renderer
preloads it, overlays its frames through the existing CPU/GPU scene plan, switches
underlying scenes at its cut point, and returns to the destination when finished.
The same effective scene drives audio routing, including the transition audio.
A new scene selection interrupts the previous transition deterministically. Initial
project open does not trigger a transition. Missing/deleted media falls back to Cut
with a diagnostic. Imported native OBS stingers become selectable media entries;
obsolete StreamElements wrappers remain excluded.

## Limits and validation

Duration and cut point are explicit milliseconds. This first stinger workflow does
not claim track-matte support or OBS crossfade equivalence. Invalid timing and file
paths are refused. Checks drive actual source workers, scene switches and UI controls;
verify alpha overlay, cut timing, retained startup, countdown text changes and pause/
reset, import mappings, serialization and CPU fallback.
