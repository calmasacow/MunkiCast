<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0004 — No in-process plugin ABI; three extension tiers instead

- **Status:** accepted
- **Date:** 2026-10-11

## Context

OBS's plugin model is `dlopen`'d shared libraries running inside the host's address space. Observed
consequences in the reference tree:

- **It defeats crash isolation.** Every source, filter and encoder shares one address space; one null
  dereference in third-party code takes the stream down.
- **It freezes the architecture.** Once outsiders ship binaries compiled against your structs, the
  structs can never change. This is *why* OBS still has groups-as-a-compatibility-hack and audio that
  cannot be reconfigured after `obs_reset_audio()`.
- **The version check is weaker than it looks.** `libobs/obs-module.c` rejects only *forward*
  incompatibility (`ver > LIBOBS_API_VER`, patch masked off) with no symbol or struct-layout
  verification — a plugin built against libobs 27 loads into 33 and may corrupt memory silently.
- **The licence question is permanent and unsettled**, because in-process linking is exactly where
  "derivative work" is litigated.

## Decision

No `dlopen`'d in-process plugin ABI, ever. Three extension tiers:

1. **Shader-text effects** — a Slang shader plus a `schema/effects/*.toml` manifest (param schema,
   pass count, shader entry, needs-backdrop / needs-mips / needs-previous-frame). Compiler-validated,
   no rebuild per release, works on every backend. Covers most of what OBS *filter* plugins do.
2. **Wasm component-model plugins** — for logic: scene automation, generated sources, integrations.
   Sandboxed by construction, capability-scoped, crash-isolated, architecture-independent, versioned
   through a WIT contract rather than a frozen C struct.
3. **Out-of-process native workers** — the escape hatch for code that genuinely must be native
   (DeckLink and capture-card SDKs, NDI, GPU vendor SDKs). Runs as its own process speaking the same
   IPC protocol as the built-in `capture`/`media` workers: full native speed, and a crash is a
   supervised restart rather than a dropped stream.

**Important scoping:** shader compilation guarantees memory safety, **not execution safety**. A
shader can still hang the GPU and trigger a device reset, so tier 1 depends on the device-lost
recovery path (ADR-0005) plus loop/instruction-count limits in the manifest validator for untrusted
shaders.

## Rejected alternatives

**A versioned native C ABI done "properly"** (opaque handles, no struct exposure, strict semver).
Better than OBS's, but it still runs third-party native code in our address space, which contradicts
the stability promise. The isolation boundary is the product.

**A stable ABI only for "trusted" first-party-signed plugins.** Signing proves provenance, not
correctness; a signed plugin segfaults identically.

## Consequences

- An OBS plugin cannot be ported by recompiling; it must be rewritten as a shader, a Wasm module, or
  a worker. **This is friction no competitor imposes** — so the three tiers must be genuinely
  pleasant to author, and first-party effects should be written *using* the shader-effect path to
  prove it.
- Extension authors may licence their work however they wish, since extensions are data, sandboxed
  Wasm, or separate programs over a socket — never linked code. GPLv3 binds only forks of MunkiCast.
- Tier 2 and the extension framework are deferred until a working show demands them.
