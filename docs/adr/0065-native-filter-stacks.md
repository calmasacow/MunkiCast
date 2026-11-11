<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR-0065: Native source filters and ordered processing

Status: Accepted for implementation, 2026-10-21.

## Context

The operator requires the stock OBS filter suite, native processing and source
context-menu editing. Existing node effects and bus inserts are document records;
enabled effects currently refuse rendering or silence a bus. OBS filter JSON is
retained but not translated. A menu without processing would misrepresent support.
MunkiCast remains independent: no libOBS linkage or embedded OBS runtime.

## Ownership and persistence

Add ordered effect stacks to shared Sources and Scenes. Existing node effects
remain placement/group effects; existing bus inserts remain bus effects. A source
filter edit affects every placement of that source. A nested-scene filter edits
the referenced Scene, including its Program rendering. Group effects belong to
the native group placement, consistent with ADR-0064's group import model.

Effects gain an optional display name. Type, enabled state, parameters and stable
ID remain the processing identity. One undoable SetEffects command replaces an
owner's stack, supporting add/edit/reorder/toggle/remove without separate mutable
UI state. Global ID validation includes all stacks. Duplicating a scene regenerates
its owned effect IDs; shared source IDs and their effects remain shared.

Project format advances to version 3. Version 2 migrates with empty shared stacks;
existing node/bus stacks remain intact. Older readers must refuse new projects
rather than silently removing source masks or other filters on resave. Journals,
native export and OBS import/export preserve order, names and disabled entries.

## Processing

One native filter catalog supplies parameter defaults, ranges and supported domains
to import, UI and processing. The scene walk resolves filters and resulting local
dimensions before placement. Source filters run before placement transforms;
group/scene filters run after their children composite against transparency.
Crop/scale dimensions feed the same transform-handle calculations used for drawing.

CPU and GPU implement the same resolved video stages. GPU stages operate on device
images without whole-frame CPU readback. CPU remains the functional headless and
no-device path. Color/alpha conventions are explicit at stage boundaries; HDR
transfer conversion is selected explicitly when source metadata is unavailable.
Missing assets, malformed parameters and unsupported active filters report gaps,
never silently draw the unfiltered source. Disabled filters do not change output.

Stateful scroll/delay and audio stages retain state by effect identity, reset when
parameters change, and release it when removed. History allocations are bounded;
unfulfillable delay requests report unavailable output instead of unbounded memory
growth. Audio processes fixed stereo 48 kHz blocks; source filters precede routing,
node/group/scene processing follows ownership, and bus inserts precede bus gain.
Sidechains resolve stable source identity, not whichever scene row is selected.

## Required stock inventory

Video: image mask/blend, crop/pad, color correction, scaling/aspect ratio, scroll,
render delay, color key, LUT, sharpening, chroma key, asynchronous video delay,
luma key, HDR tone mapping and SDR white-level adjustment.

Audio: gain, three-band EQ, polarity inversion, noise gate, compressor (including
sidechain), limiter, expander, upward compressor, and noise suppression (Speex and
RNNoise, using installed standalone libraries). Linux VST 2.x hosting is isolated
in a helper process; plugin failure cannot crash the compositor or mixer. Vendor
Windows/macOS-only filters without Linux equivalents are outside this inventory,
per the operator's platform rule. Third-party OBS plugins are separate porting work.

## Editor and verification

The source editor includes a live isolated picture above its controls. The UI
adds a transient canvas/scene/deck only to its renderer snapshot while this editor
is open. It references the existing source IDs so capture workers remain shared.
The transient deck is explicitly excluded from audio routing: opening an editor
must not double program audio or activate off-scene audio. It never enters the
saved document, undo history, output configuration, or collection catalog.
Closing the editor removes this transient view. Audio-only owners have no video
preview. Slider drags commit on release, retaining one undo step per gesture.

Sources and Program context menus expose Filters. Editor lists ordered audio/video
filters applicable to the owner, with add, edit, enable/disable, reorder and delete.
Actions resolve stable owner/effect IDs so a model refresh cannot redirect an edit.
Changes are undoable, persist and update rendering. Missing imported assets/plugins
remain visible with actionable status and editable paths.

Each completed section needs real pipeline assertions, parameter/persistence tests,
CPU/GPU comparisons where applicable, warning-free builds and a green full suite
before commit/push. Imported Marathon masks, corrections and crop are acceptance
fixtures; the original collection remains untouched during verification.
