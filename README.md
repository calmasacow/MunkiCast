<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Libertas-Studio

A live streaming and recording studio for Linux, built clean-sheet. Omarchy (Arch + Hyprland) is the
bedrock target; the rest of Linux follows, then macOS and Windows.

**Status: pre-alpha.** Nothing streams yet, and the program canvas says so on its face.

What exists is a workspace you can use and a document you can edit:

- **`libertas-ui`** — one window holding the whole workspace, with Adobe-style drop-zone docking,
  dividers, tab groups, live Omarchy theme following, and a layout that survives being saved and
  restored at any resolution.
- **Saved workspaces** — `Build` and `Live` across the top, plus any you add. Building a show and
  running one want different panels up; these are arrangements of the one surface, never OS
  windows. The built-in pair can be rearranged and reset but not renamed away, so there is always
  a known arrangement to get back to.
- **A document model** — scenes as trees, groups, adjustment and reference nodes, canvases, buses,
  decks, outputs, per-canvas overrides. Every panel in the workspace drives it: scenes, sources,
  properties, the audio mixer, and program/preview with the output list.
- **A command bus** — every change is one of forty-four validated, journaled, undoable
  commands. A dragged fader is one undo step, not four hundred. Unsaved work survives the process
  being killed.
- **A project on disk** — a git-diffable directory of JSON, saved as a transaction. A corrupt scene
  costs one scene, not the collection.
- **Keyboard and hotkeys** — one bindings table covering application and global scopes, with the
  workspace fully reachable without a pointer.
- **Machine telemetry** — CPU, memory, free disk, and GPU, VRAM and *encoder* load read live from
  NVML. The broadcast figures beside them read "—" rather than zero, because zero dropped frames
  is a claim about a running show.

The program canvas shows a picture of what this build can draw — a colour, a group of those, a
scene inside a scene made of those — published by `libertas-render`. Anything else is left
undrawn and the caption says how many. Capture, encode and output do not exist yet; those live
in `tools/` as measured spikes and have not been made product code. The panels that will drive
them say so on their faces — the audio mixer draws no meters and the controls panel has no Go
Live button — because a panel that looks finished while being incapable is a panel nobody can
tell the truth about later. With no render process running, the program canvas still reads "no
pipeline attached". See [`docs/adr/`](docs/adr/) — twenty-eight decisions, each recording what
was measured and what was rejected. Two of them are proposed rather than built: the scene model
the vertical workflow needs, and the rename that follows it.

## Why

Libertas is not an OBS fork and uses no `libobs` code. Two reasons:

- **Independence.** A new frontend over `libobs` would still mean tracking upstream releases,
  GPL-linking to their code and inheriting their engine decisions. That is a reskin, not
  independence.
- **Linux is structurally last in line in OBS.** There is no Vulkan backend — Windows gets a
  dedicated D3D11 backend and macOS a dedicated Metal one, while Linux rides the shared legacy
  OpenGL layer that also serves as everyone's fallback. Windows capture has ~2.3x the code Linux
  gets. Upstream CI builds exactly one Linux configuration (`ubuntu`), never Arch or Hyprland.

See [`docs/adr/0001-no-libobs.md`](docs/adr/0001-no-libobs.md) for the full reasoning and the
rejected alternatives.

## Design commitments

| | |
|---|---|
| **Wayland-native** | No X11 code at all. Global hotkeys via `org.freedesktop.portal.GlobalShortcuts`, capture via portal ScreenCast + PipeWire. ([ADR-0003](docs/adr/0003-wayland-native-no-x11.md)) |
| **Multi-process** | A tiny supervisor owns the document; the GPU, encoders and outputs live in one render process; risky work (media demux, browser engines, device SDKs) runs in pooled workers. The UI has zero authority and may be killed freely. ([ADR-0005](docs/adr/0005-multiprocess-isolation.md)) |
| **No in-process plugins** | Extensions are shader text, Wasm components, or separate worker processes — never `dlopen`'d into our address space. This is what keeps the engine changeable in year three. ([ADR-0004](docs/adr/0004-no-in-process-plugins.md)) |
| **Vulkan, explicit sync** | Composite in scene-linear RGBA16F. Encode on the same `VkDevice` via `VK_KHR_video_encode_queue` where available. ([ADR-0006](docs/adr/0006-nvenc-interop-path.md)) |
| **Multi-GPU encode** | Concurrent encode across different GPUs at different resolutions is a first-class requirement, not a bolt-on. ([ADR-0007](docs/adr/0007-dual-gpu-concurrent-encode.md)) |
| **Theme-aware** | Follows the live Omarchy theme; ships a token fallback so other platforms are theme-driven too. |
| **The UI owns its layout** | Wayland cannot restore a client's window geometry, so Libertas lays panels out inside one surface and persists proportions, never pixels. The program canvas is never a separate window and cannot be lost. ([ADR-0015](docs/adr/0015-ui-owns-its-layout.md), [ADR-0016](docs/adr/0016-panel-docking-drop-zones.md)) |
| **One transport** | Our processes talk over `SOCK_SEQPACKET` carrying whole messages and file descriptors, and nothing in it knows what a message means. An empty message is refused, because the kernel reports one exactly as it reports a dead peer. ([ADR-0026](docs/adr/0026-the-process-transport.md)) |
| **A safe state** | When the show cannot be supervised — a crash, a wedged frontend, or a panic button — the render process cuts to an emergency scene rather than broadcasting whatever happened to be on screen. ([ADR-0025](docs/adr/0025-the-safe-state.md)) |

The honest trade-off is recorded too: clean sheet is the harder road, 1-2 years to real parity, and
the first six months will feel worse than OBS.

## Building

Requires a C++23 compiler, CMake 3.28+, and development headers for Vulkan, libdrm, PipeWire, GIO
and FFmpeg (including libswscale), plus libpulse for PulseAudio/PipeWire audio. On Arch:

```sh
sudo pacman -S --needed base-devel cmake ninja vulkan-headers vulkan-icd-loader \
                        libdrm pipewire libpulse glib2 ffmpeg
```

Plus Qt 6.8+, Qt Multimedia/WebEngine, jsoncpp and tomlplusplus:

```sh
sudo pacman -S --needed qt6-base qt6-declarative qt6-wayland qt6-multimedia qt6-webengine \
                        jsoncpp tomlplusplus
```

The default build uses the pinned, project-local **FFmpeg 9.1 development build**.
Set it up first using [the dependency instructions](docs/ffmpeg-9.1.md).

```sh
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
```

Presets use Unix Makefiles: `linux`, `linux-debug`, `linux-ci` (warnings are errors),
`linux-asan`, `linux-tsan`. `linux-system-ffmpeg` builds separately against system FFmpeg
for rollback; `linux-ffmpeg-9.1` remains a compatibility alias.

## Running it

```sh
./build/linux/apps/libertas-ui/libertas-ui --project ~/Shows/MyShow.libertas
./build/linux/apps/libertas/libertas probe
```

[Custom browser docks](docs/browser-docks.md) are available under **Docks > Custom Browser Docks…**.

When using the older `build-make` launch path, rebuild that same directory before restarting:

```sh
cmake -S . -B build-make
cmake --build build-make --target libertas-ui -j8
./build-make/apps/libertas-ui/libertas-ui
```

Without `--project` the shell opens an unsaved document. `--check` drives the whole shell
headlessly — loading the real QML, running the real gestures, grabbing frames and asserting what
was drawn — and forces the offscreen platform, so it cannot reach a display even by accident.

## Importing OBS Scene Collections

```sh
./build/linux/apps/libertas/libertas import-obs collection.json ~/Shows/Imported.libertas 3840 2160 60 1
./build/linux/apps/libertas-ui/libertas-ui --project ~/Shows/Imported.libertas
```

Destination must not exist. Width, height and rational frame rate are required: OBS normally
stores them in the separate profile, not the scene collection. The importer preserves the input
as `obs-original.json` and writes `obs-import-report.md`. Review that report before using the show.
Source identities, scenes, ordering, supported transforms, crops and audio routes are converted;
OBS groups become nested scenes with saved bounds. Linux capture devices may need reselection.
Fonts, media dimensions, filters, arbitrary rotations, transitions and plugin-specific behavior
need review; import does not promise pixel parity. No devices or URLs are opened during import.

## Testing

```sh
ctest --preset linux          # twenty tests
ctest --preset linux-ci       # the same, with warnings as errors
ctest --preset linux-asan     # the same, under ASan + UBSan
ctest --preset linux-tsan     # the threaded code, under TSan
```

The shell's own check is one of those twenty: `libertas-ui --check` loads the real QML, drives
the real gestures through the same API the panels use, grabs frames and asserts what was drawn —
200 assertions, on the offscreen platform, so it cannot reach a display even by accident. It is
excluded from the TSan preset, because Qt here is built without TSan and starting its QML thread
makes the tool itself fail an internal check; the reason is recorded beside the exclusion.

The layout engine, the document model and the command bus carry property tests: random docking
gestures asserting every invariant after each one, random command sequences asserting that undo
and redo return the project byte-for-byte, and serialise-parse-serialise as a fixed point. Three of
the sharper bugs in this repository were found by those rather than by reading the code, and each
has a named case beside the random one — a property test tells you *that* something is wrong, a
named case tells you *what*.

## `libertas probe`

Reports everything about a machine that affects capture, composite and encode: GPUs and their DRM
nodes, Vulkan extension support per device (including video encode), the libva driver actually bound
to each render node, available encoders, portal interfaces, PipeWire, and the current Omarchy theme.

It exists because capability varies more between machines than it has any right to — two machines on
the same distro and driver version disagreed on QuickSync availability, Vulkan device ordering and
AV1 support. **Nothing about device capability is decided at compile time.** Attach its output to any
bug report.

It exits non-zero if it finds itself running under XWayland, which is refused deliberately rather
than silently degrading.

## Licence

GPL-3.0-or-later. See [`docs/adr/0002-gpl3.md`](docs/adr/0002-gpl3.md) for why, including the
rejected permissive options.
