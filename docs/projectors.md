<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Program and Preview projectors

Right-click the Program picture or either Studio Mode monitor, including an empty
picture. **Program Projector** offers **Windowed**, **Fullscreen** on the compositor's
chosen monitor, and **Fullscreen — monitor name** for each connected monitor.
**Preview Projector** offers the same choices while Main's Studio Mode is enabled.
Program projectors work with Studio Mode off. Projectors contain only video: no
selection handles, labels, controls or pointer cursor.

Close a projector with Escape, its window close action, or **Close Projector** in
the same context menu. Turning Studio Mode off closes Preview projectors, leaving
Program projectors running. Removing a canvas, switching collections, quitting the
app or losing the renderer closes the affected windows. A removed monitor closes
its fullscreen projector instead of silently moving the HDMI feed elsewhere.
Projectors are session windows; they are not restored automatically on startup.

## Output path and timing

The renderer owns native Wayland xdg-shell windows and exports Vulkan DMA-BUFs
directly to the desktop compositor. Program uses the same composition, including
transitions, as recording/streaming, before encoder-specific resizing, YUV conversion
and compression. A matching output cadence shares one render and full-resolution
sRGB conversion with the encoder. Preview uses the separate staged Studio canvas.
Changing dock size or hiding/pausing the UI does not reduce the projector resolution
or stop its renderer-owned canvas clock.

No projector pixel buffers pass through Qt, the UI image provider or a CPU frame
queue. Each window uses at most three release-tracked display buffers. A buffer is
reused only after Wayland releases it and Vulkan reacquires ownership. A slow display
drops projector updates instead of waiting on that display or stalling the encoder.
The renderer performs one aspect-preserving GPU blit per submitted projector in its
existing composition submission/fence; letterbox bars are black. A display-only
cycle reads back only a one-pixel diagnostic image, not a full CPU canvas. Unrelated
canvases are skipped when no UI preview was requested.

Fullscreen buffers use the monitor's physical mode size, including under desktop
scaling. Match the canvas and monitor resolution for a one-to-one image. Match the
monitor refresh rate to the show FPS (or an exact multiple). A 30 Hz monitor cannot
display every frame of a 60 fps show. Multiple canvas/display clocks do not change
a recording's frame cadence or timestamps.

This removes a queued UI preview from the HDMI path; it does **not** guarantee zero
physical latency or zero GPU cost. Desktop compositing, scanout, refresh mismatch,
GPU transfers and recorder processing still apply. Direct scanout is the desktop
compositor's decision. Same-GPU buffers use device memory; unknown/different compositor
GPUs use shareable system memory to avoid unsafe cross-GPU VRAM imports. Opening or
resizing a projector allocates bounded buffers and can briefly cost more than steady
state. End-to-end HDMI latency needs a hardware measurement under the real show.

## Requirements and current limits

- Wayland with xdg-shell, linux-dmabuf v3 or newer, viewporter, and a compatible Vulkan
  renderer/exportable format. Named monitor targeting uses wl_output v4 names.
- Unsupported GPU sharing or a CPU renderer produces an operator notice. There is no
  silent full-frame CPU-copy fallback masquerading as the low-latency GPU path.
- Up to four projectors, 8192 pixels per edge, a shared 384 MiB display-buffer budget
  (nominal pixel bytes). Allocation/import/setup failures are reported; setup times
  out after five seconds. Existing compositor render-memory limits also apply.
- Output is 8-bit SDR sRGB, matching the current canvas export. Independent HDR/10-bit
  display signaling, dedicated audio-over-HDMI routing, source-only projectors,
  multiview and OBS projector geometry restoration are separate work.
- Windowed resizing/decorations follow the compositor. Fullscreen physical pixels
  are independent of windowed desktop scaling. HDMI display mode/refresh selection
  remains in the desktop's monitor settings.

## Control and verification

Native MCP discovers `Preview.openProjector(canvasId, preview, fullscreen, screen)`,
`projectorScreens()`, `projectors()`, `closeProjector(id)` and `closeProjectors()`.
An accepted open request is asynchronous. Poll `projectors()` for ready/error and
submitted/dropped/presented/discarded counters. `ready` means a GPU frame was
submitted; `presented`/`discarded` are actual Wayland presentation feedback when the
compositor supports it. Status is requested at 4 Hz by the UI; no unsolicited pixel
frames accumulate when the UI stops reading. OBS WebSocket projector request mapping
remains separate from these native controls.

```sh
cmake --build build-make --target munkicast-ui test_preview test_gpu_composite -j8
ctest --test-dir build-make -R '^(test_preview|studio_self_check|native_mcp_self_check)$' --output-on-failure
MUNKICAST_TEST_GPU=1 ./build-make/tests/unit/test_gpu_composite
# Opens a temporary test window, with an isolated generated show:
MUNKICAST_TEST_PROJECTORS=1 ./build-make/tests/unit/test_preview
# Optional: briefly occupies this monitor to check actual fullscreen configuration:
MUNKICAST_TEST_PROJECTORS=1 MUNKICAST_TEST_PROJECTOR_FULLSCREEN=HDMI-A-1 ./build-make/tests/unit/test_preview
# Alternating warm baseline/projector composition timings, 1080p and 2160p:
MUNKICAST_TEST_GPU=1 MUNKICAST_BENCH_PROJECTOR=1 ./build-make/tests/unit/test_gpu_composite
# Repeat for the shared system-memory path used across different GPUs:
MUNKICAST_TEST_GPU=1 MUNKICAST_BENCH_PROJECTOR=1 MUNKICAST_BENCH_PROJECTOR_HOST=1 ./build-make/tests/unit/test_gpu_composite
```

Unit checks compare exported projector GPU pixels against the full encoder picture,
including black aspect-fit bars and foreign-buffer reuse. Scheduler checks cover a
paused UI, independent 60 Hz display/30 fps recording, stopping recording while
projection continues, and no unsolicited socket backlog. The Studio UI check opens
menus on both blank monitors and checks Program availability outside Studio Mode.
Native checks exercise asynchronous Wayland buffer creation, real GPU presentation,
close/reopen and a missing monitor. They do not certify HDMI recorder latency or
long-show stability. See [todo.md](todo.md) for remaining hardware acceptance.

## Measured validation, 2026-11-08

The full build passed with warnings treated as errors. All 40 default-enabled tests
passed across the suite and focused UI rerun; the opt-in GPU suite passed separately
with 15 cases and no failures. The final Studio, shell and native MCP checks passed
after correcting timer/menu selectors made ambiguous by the new controls.

The RTX 2070 passed the explicit GPU pixel suite, the native windowed/close/reopen
check, and real fullscreen presentation on HDMI-A-2 at **3840 × 2160**. That monitor
runs at 30 Hz: the renderer can submit 60 fps while the compositor presents at the
monitor rate and discards superseded frames. This is expected refresh mismatch,
not an application frame queue.

Buffered V4L2 inputs use a bounded two-frame presentation delay, without waiting
inside renders; see [recording cadence](recording-cadence.md) for timing and live checks.

The opt-in benchmark alternates 50 measured baseline/projector samples after ten
warm-up renders. It renders a flat-color canvas, exports the full CPU encoder image
plus a 480-pixel dock preview, then adds one matching-aspect projector copy. Times
include synchronous composition/export, excluding encoding, Wayland/scanout and
recorder latency. The system-memory test uses linear buffers, as the cross-GPU path
does; it does not simulate the other GPU's compositor workload.

| Display buffer | Canvas | Baseline median / p95 (ms) | Projector median / p95 (ms) | Median difference (ms) |
| --- | --- | --- | --- | --- |
| Device memory | 1920 × 1080 | 3.196 / 6.467 | 3.274 / 5.476 | +0.078 |
| Device memory | 3840 × 2160 | 11.080 / 13.011 | 11.200 / 12.969 | +0.120 |
| Shared system memory | 1920 × 1080 | 2.918 / 5.601 | 4.243 / 6.357 | +1.325 |
| Shared system memory | 3840 × 2160 | 10.523 / 13.266 | 14.703 / 17.686 | +4.180 |

Small differences are within run-to-run timing noise. These are one machine's
measurements, not a zero-cost guarantee. Prefer the same rendering/compositing GPU
for demanding output. Fullscreen on a matching aspect avoids a redundant black
clear; letterboxed surfaces still need the clear. A projector without an existing
full-resolution encode also needs the full sRGB conversion that a scaled dock alone
would omit. Hardware recorder color/range, audio, end-to-end latency and long-show
acceptance remain on the [backlog](todo.md#projector-follow-ups).
