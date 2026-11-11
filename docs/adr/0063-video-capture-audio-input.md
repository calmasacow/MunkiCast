<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0063 - Audio input owned by a video capture source

- **Status:** accepted
- **Date:** 2026-10-20

## Decision

A V4L2 source stores `audioDevice`: `none`, `default`, or a PulseAudio/PipeWire
source identity. New sources use `default`; existing sources with no setting use
`none`, preserving their separate audio sources and avoiding unsolicited capture.
The creation/settings dialog offers None, Default (matching capture device), and
enumerated recording inputs. Default is not the system microphone: match the
video and ALSA endpoints by their common physical USB/PCI device in sysfs. Zero or
multiple matching inputs means unavailable; never guess by product name.

Audio belongs to the video source ID. One session serves its placements; the
existing routing, meters, gain, mute and monitor controls operate on that source.
None has no mixer strip. Configured but unavailable audio retains a strip with
unavailable status. New video placements route to Program when available. Enabling
audio on an existing silent video source routes unassigned placements to Program
in the same undo step; existing assigned routing is preserved.
Video sources kept active while hidden retain audio capture too, but hidden
placements do not enter the mix. Deleting the source releases both captures.
Changing audio does not reopen the video stream. Settings/name changes unrelated
to audio must not reopen audio capture. Failed opens retry at a bounded interval.

Explicit devices are checked before and after stream connection and cannot move
to another device. Unknown or missing saved identities remain visible in settings.
Separate audio sources remain independent; choosing the same input in both can
double the mix, so the dialog explains removing the redundant standalone source.
No libobs code or new dependency is introduced. Hardware matching is Linux-only;
devices without reliable physical identity require explicit selection.

## Validation

Drive real renderer audio with private test sinks, including None, switching,
unavailable/default-without-match, source deletion and off-scene retention. UI
checks exercise the audio selector and mixer visibility, names and undo. Verify
Elgato matching read-only on available hardware; do not change its video mode.
OBS export must report attached audio as unsupported rather than silently lose it.
Independent video/audio hardware clocks still require future timestamp-based drift
correction; this change does not claim sample-accurate A/V synchronization.
