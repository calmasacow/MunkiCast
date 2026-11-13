<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Todo

**The backlog now lives in GitLab issues**, not in this file. Triaged 2026-11-13 from the
2026-10-29 audit; finished work and deliberate limits were removed rather than carried forward.

<https://continuum.hosaka-corp.com/munkidev/munkicast/-/issues>

A model change still needs an ADR. A supported API or a saved setting is not proof that the
renderer, the UI, the hardware and the workflow are finished.

## Milestones, in priority order

| Milestone | What it covers |
| --- | --- |
| [1 Thumbnails and previews](https://continuum.hosaka-corp.com/munkidev/munkicast/-/milestones/1) | Per-source frames in the preview protocol; thumbnails in the source list; working previews in source settings. |
| [2 Stats refactor](https://continuum.hosaka-corp.com/munkidev/munkicast/-/milestones/2) | Per-destination output stats (dead today); CPU/GPU/MEM moved to the top bar with whole-machine CPU. |
| [3 Transform refactor](https://continuum.hosaka-corp.com/munkidev/munkicast/-/milestones/3) | Resolution-independent transforms, OBS-style bounding boxes. Reverses the ADR-0020 stance, so an ADR comes first. |
| [4 HDR](https://continuum.hosaka-corp.com/munkidev/munkicast/-/milestones/4) | Capture, then output, then preview. An ADR for the colour pipeline comes first. |

Unmilestoned issues are labelled `qa`, `projectors`, `menus` or `backlog`. Two issues are
labelled `adr-needed` and block their milestone's code; `needs-hardware-test` marks work the
self-check and ctest cannot certify.

## Not tracked as issues

The `ponytail:` comments in the source are deliberate ceilings with their upgrade path named in
place, not pending work: one worker per retained source, alpha clips over 64 MB streaming from
disk, clone chains stopping at 64, eight decoded filter masks, MKV as the only recording
container, the 32-slot NVENC cap, OAuth keys without keyring refresh, a stateless MCP server,
collections-only OBS export, and the naive CSV split.

## History

Design discussion and the earlier backlog remain in Git history (`b9bb757:docs/todo.md`), the
[ADRs](adr/), and the [development history](development-history.md).
[Current status](current_goal.md) records what is usable now.

### Validation record, 2026-10-29

A panel regression reproduced blank scene and source rows on collection switches that
`shell_self_check` had passed. After the fixes: the final `munkicast-ui` target rebuilt; 32
ordinary tests passed with the GPU test passing explicitly under `MUNKICAST_TEST_GPU=1`; all eight
UI and integration tests passed. **41 distinct tests passed** with nothing skipped, `--check
--check-panels` passed, and no QML reference, type, binding-loop or assignment errors appeared.
