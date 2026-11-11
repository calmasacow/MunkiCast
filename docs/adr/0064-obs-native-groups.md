<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0064 - Preserve OBS groups as native scene groups

- **Status:** accepted
- **Date:** 2026-10-21

OBS groups import as native Group nodes with children, not public Scene records.
Each placement receives fresh node IDs; underlying source IDs remain shared.
True nested scenes remain Precomp nodes. Backup group items are ignored, stacking,
visibility, locks, crop, scale, rotation and blend remain on the group placement.

GroupData gains optional `size` in local pixels. Missing size retains existing
canvas-sized behavior. Imported groups use saved bounds, rendered as a bounded
local surface by the shared CPU/GPU draw plan. Sizes must be finite, positive,
at most 8192 per edge and at most 32M pixels. JSON omits absent size, preserving
existing fixtures. UI transform bounds use the same size. This is an additive
field; older readers will not preserve bounded group rendering on resave.

Reference-cycle checks run before group expansion. Expanded nodes have a separate
100,000-node budget, preventing repeated nested groups from amplifying input
without bound. Group-only collections are rejected; scene selectors never target
group records. Unknown filters remain reported as before.

The obsolete `streamelements_transition_audio_wrapper_source` is omitted by exact
type, including global and placed instances, with an import report entry. Raw OBS
JSON remains preserved. Other unknown source types remain visible placeholders.

Existing collections are not silently rewritten. Reimport creates a separate
corrected collection unless operator explicitly requests repair. Tests cover native
group structure, save/reopen, transformed pixels, repeated placements and wrapper
omission alongside preservation of genuine global audio.
