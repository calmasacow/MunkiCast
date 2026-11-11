<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0010 — Modifier negotiation: the real cause, and two more failure modes

- **Status:** accepted. Supersedes ADR-0009's conclusion about the *primary* cause.
- **Date:** 2026-10-12
- **Target:** development box, Hyprland 0.56.2 (Aquamarine), portal-hyprland 1.4.1,
  PipeWire 1.6.8, NVIDIA 610.57.04, RTX 5080 + Intel UHD 770, capture source DP-1

## Context

ADR-0009 concluded that `no more input formats` was caused by a **phantom node id returned by a
restored session**. That is a real failure and is documented below, but it was not the main cause.
Pinning the output and dumping the source's offered formats at the moment of failure — both
diagnostics that should have existed from the first hour — made the actual bug visible immediately.

## Cause 1: we parsed the modifier with the wrong API, and the wrong answer was a valid modifier

`spa_format_video_raw_parse()` cannot reduce a modifier property that is still an unfixated
`CHOICE_Enum` — which is exactly the state the mandatory two-round dance leaves it in. It does not
fail; it leaves `info.info.raw.modifier` at **0**.

**0 is `DRM_FORMAT_MOD_LINEAR`.** So a failed parse is indistinguishable from the server having
chosen LINEAR, and the code went on to fixate LINEAR with every appearance of success:

```
info  negotiated 2560x1440 BGRA (drm AR24) modifier LINEAR fps 0/1
info  fixating modifier LINEAR for BGRA
info  stream state paused -> error: no more input formats
```

The source never offered LINEAR at all. Measured, for DP-1:

| | modifiers |
|---|---|
| source offers | `0x300000000606010`–`015`, `0x300000000e08010`–`015`, `INVALID` — **no LINEAR** |
| this device can import | `0x300000000606010`–`015`, `LINEAR` |
| real intersection | `0x300000000606010`–`015` — six usable modifiers |
| what we fixated | `LINEAR` — not offered, rejected |

So negotiation was failing on a modifier the compositor had never mentioned, while six mutually
supported modifiers sat unused. Every earlier "it works" run had `DRM_FORMAT_MOD_INVALID` in the
advertised list, which is a wildcard and masks this entirely — which is why the bug survived a
proven stage 2.

**Required:** read the property yourself with `spa_pod_get_values()`, take every value in the
choice, and select by intersecting against the modifiers the Vulkan device reports as importable.
A single `uint64` read of a modifier property is a bug, not a shortcut. Never let a parse failure
fall through to a default, because in this API the default is a legal value.

After the fix, negotiation is correct and the stream reaches `streaming` for the first time:

```
info  server offers 2560x1440 BGRA (drm AR24) fps 0/1 with 6 modifier(s), unfixated
info  fixating modifier NVIDIA(0x300000000606015) for BGRA
info  stream state paused -> streaming
```

## Cause 2: a compositor advertises modifiers it cannot allocate with

`0x300000000606015` negotiates, fixates, and reaches `streaming` — then delivers **zero frames**
and errors. The compositor's own fixed preference, from the source's single-valued `EnumFormat[0]`,
is `0x300000000606014`.

This is ADR-0007's accept-then-fail trap occurring one stage later than expected: not at import,
but at the compositor's own buffer allocation. A negotiated modifier is therefore **not** a working
modifier, and no amount of up-front intersection can establish that it is.

**Required:** the fallback ladder has to be a loop, not a promise. On a post-fixation failure, drop
the modifier that was actually fixated, retry on the same session, and degrade through the
remaining candidates to LINEAR and then to shm. Log every rung: silently taking a slower path is
how performance problems become unexplainable. Implemented in `stream_once`.
**Not yet proven** — see Open below.

## Cause 3: readiness is necessary but not sufficient

ADR-0009's phantom node is real, and it has a second form that the readiness probe cannot catch. A
restored session returned node 403 — the node from the *previous* run — which was still in the
registry, still answered a format query, and had **0 `EnumFormat` params** by the time the stream
connected. It was gone from the graph entirely minutes later.

So a node can pass `pw_probe_node` and be a corpse. **The only proof a capture session works is
frames flowing through it.** Acquisition therefore takes the real stream attempt as its final
validation step: a restored session that fails in use has its token discarded and falls back to a
fresh picker session, rather than surfacing a capture error.

Amendment to ADR-0009: a restore token is persisted once its session is **validated**, not once
frames have flowed. Validation already rules out the phantom case, and the two facts are different
— the token records that the user authorised this source, whereas frames flowing tests our own
pipeline. A modifier bug of ours must not cost the user a picker click.

## Cause 4 (confounder): the compositor's capture state degrades across sessions

After roughly six sessions in one sitting, **even a freshly picked source** yields a node with 0
`EnumFormat` params and `param_changed` never fires. Closing sessions properly (ADR-0009) reduces
this but does not eliminate it. Restarting
`xdg-desktop-portal-hyprland.service xdg-desktop-portal.service`, or rebooting, clears it.

**Consequence for method:** a long debugging session silently changes the thing being measured.
Any negative result after several runs must be re-confirmed on a clean portal before it is
believed, and the real worker must treat "node with no formats" as a retryable environment fault
rather than a configuration error.

## Resolution of the ordering question

Candidate modifiers are now ordered by **the source's own preference**, read structurally from the
node's `SPA_PARAM_EnumFormat` params at validation time (`NodeFormats`): a property carrying one
fixed value is the compositor's current layout, a choice is the set it will consider. We then
intersect with what the Vulkan device can import and advertise that, preference first, with
`DRM_FORMAT_MOD_INVALID` as the last rung only.

This matters more than it first appears. Trying our own enumeration order first means deliberately
walking into Cause 2 - and the ladder may not be able to recover, because a failed attempt appears
to leave the node advertising no formats at all. If that holds, each rung would need its own portal
session and therefore its own picker click, which is not an acceptable design. Ordering by the
source's preference is what keeps the ladder a cheap safety net rather than the primary mechanism.

An empty intersection is also now reported before connecting, naming both lists, instead of
surfacing as `no more input formats` three callbacks later.

## Open

- Whether a failed attempt poisons the node for subsequent attempts on the same session. If it
  does, the ladder needs a fresh session per rung and the source-preference ordering becomes
  load-bearing rather than an optimisation.
- The fallback ladder is **written and compiles but has never engaged** — the run that would have
  exercised it hit Cause 4 instead. First thing to do on a clean session.
- Stage 3 (Vulkan dmabuf import) is still **unproven**: no frame has yet reached `vk_import.cpp`.
  Negotiation now yields a modifier the device claims it can import, so the import is next.
- Whether `0x300000000606014` (the compositor's own preference) in fact works where `015` did not.
  The ordering change above assumes it does; that assumption is untested.
