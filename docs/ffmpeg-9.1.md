# Default FFmpeg 9.1 build

The existing `build-make` application and system FFmpeg 9.0.2 are preserved.
The default application uses FFmpeg's **9.1 development branch**, pinned to
[`47b92841e33b67c1699394415641b34d1157d8d7`](https://github.com/FFmpeg/FFmpeg/commit/47b92841e33b67c1699394415641b34d1157d8d7).
This is not a stable 9.1 release. Nothing under `/usr` is replaced.

From the repository root, after saving and closing the currently running application:

```sh
./build/linux/apps/libertas-ui/libertas-ui
```

Rollback: close that application and launch the original:

```sh
./build-make/apps/libertas-ui/libertas-ui
```

The default build links the local libraries and prepends their executable directory for
cache workers and probes. No project format, encoder settings, preview logic, or frame
scheduling changes. The running application has not been stopped automatically.

To rebuild with system FFmpeg in a separate directory:

```sh
cmake --preset linux-system-ffmpeg
cmake --build --preset linux-system-ffmpeg -j8
./build/linux-system-ffmpeg/apps/libertas-ui/libertas-ui
```

Manual configurations can use `-DLIBERTAS_USE_SYSTEM_FFMPEG=ON`.
The Git rollback point is `rollback/pre-ffmpeg-9.1-and-browser-docks`.

## Build and checks

Local dependencies live in `build/ffmpeg-9.1/prefix`; source checkouts live in
`build/ffmpeg-9.1/src`. They include NASM 2.16.03 and NVIDIA codec headers n13.0.19.0.
FFmpeg uses shared libraries, x86 assembly and LTO, with the system package's configure
options except unavailable Avisynth, LADSPA, OpenCL and AMF headers, and the removed
OpenGL configure option. Configure also reports no SPIR-V swscale backend.
NVENC/NVDEC, Vulkan, VAAPI, QSV, x264/x265, SVT-AV1 and the other installed codec
dependencies remain enabled. Hardware availability is still detected at runtime.

The local FFmpeg also needs [Arch's Chromium compatibility patch](https://gitlab.archlinux.org/archlinux/packaging/packages/ffmpeg/-/blob/main/0001-Add-av_stream_get_first_dts-for-Chromium.patch),
retained in `tools/ffmpeg-qt-webengine.patch`. Apply it to the pinned FFmpeg checkout
before building; the installed Qt WebEngine requires its `av_stream_get_first_dts`
symbol. The system FFmpeg already carries this patch.

### Fresh dependency setup

Skip this section when `build/ffmpeg-9.1/prefix` already exists. Run from the repository
root in Bash. The system FFmpeg package supplies the codec/filter dependencies used by
its configure options; NASM and NVIDIA headers below stay project-local.

```sh
prefix="$PWD/build/ffmpeg-9.1/prefix"
mkdir -p build/ffmpeg-9.1/src
curl -fL https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/nasm-2.16.03.tar.xz \
  -o build/ffmpeg-9.1/src/nasm.tar.xz
tar -xJf build/ffmpeg-9.1/src/nasm.tar.xz -C build/ffmpeg-9.1/src
(cd build/ffmpeg-9.1/src/nasm-2.16.03 && ./configure --prefix="$prefix" && make -j8 && make install)
git clone --depth 1 --branch n13.0.19.0 https://github.com/FFmpeg/nv-codec-headers.git \
  build/ffmpeg-9.1/src/nv-codec-headers
make -C build/ffmpeg-9.1/src/nv-codec-headers PREFIX="$prefix" install
export PATH="$prefix/bin:$PATH"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
git init build/ffmpeg-9.1/src/ffmpeg
git -C build/ffmpeg-9.1/src/ffmpeg fetch --depth 1 https://github.com/FFmpeg/FFmpeg.git \
  47b92841e33b67c1699394415641b34d1157d8d7
git -C build/ffmpeg-9.1/src/ffmpeg checkout --detach FETCH_HEAD
git -C build/ffmpeg-9.1/src/ffmpeg apply "$PWD/tools/ffmpeg-qt-webengine.patch"
(cd build/ffmpeg-9.1/src/ffmpeg && ./configure \
    --disable-debug --disable-static --disable-stripping --enable-cuda-llvm --enable-lto \
    --enable-fontconfig --enable-frei0r --enable-gmp --enable-gnutls --enable-gpl \
    --enable-lcms2 --enable-libaom --enable-libass --enable-libbluray --enable-libbs2b \
    --enable-libdav1d --enable-libdrm --enable-libdvdnav --enable-libdvdread \
    --enable-libfreetype --enable-libfribidi --enable-libgsm --enable-libharfbuzz \
    --enable-libiec61883 --enable-libjack --enable-libjxl --enable-libmodplug \
    --enable-libmp3lame --enable-libopencore_amrnb --enable-libopencore_amrwb \
    --enable-libopenjpeg --enable-libopenmpt --enable-libopus --enable-libplacebo \
    --enable-libpulse --enable-librav1e --enable-librsvg --enable-librubberband \
    --enable-libsnappy --enable-libsoxr --enable-libspeex --enable-libsrt --enable-libssh \
    --enable-libsvtav1 --enable-libtheora --enable-libv4l2 --enable-libvidstab \
    --enable-libvmaf --enable-libvorbis --enable-libvpl --enable-libvpx --enable-libwebp \
    --enable-libx264 --enable-libx265 --enable-libxcb --enable-libxml2 --enable-libxvid \
    --enable-libzimg --enable-libzmq --enable-nvdec --enable-nvenc --enable-shared \
    --enable-vapoursynth --enable-version3 --enable-vulkan --disable-decoder=magicyuv \
    --prefix="$prefix" --extra-ldflags="-Wl,-rpath,$prefix/lib" && make -j8 && make install)
```

With those local dependencies installed:

```sh
cmake --preset linux
cmake --build --preset linux -j8
LIBERTAS_TEST_NVENC=1 ctest --preset linux
```

Exact FFmpeg configure flags, source revision, environment, build logs, project snapshot,
benchmark CSVs and encoded samples are retained in `build/ffmpeg-comparison/`.
These generated artifacts and dependency installations are intentionally untracked.

## Comparison

`libertas-encode-bench` reads the first encode and its canvas/audio settings from the
same project snapshot for both builds. It passes identical generated RGBA frames and
audio through the production `lencode` conversion, encoder and muxer. One 120-frame
warmup precedes three 600-frame trials. Trials run without pacing; opening the encoder
and generating inputs are excluded, flushing is included. The measurements cover
conversion and encoding throughput, not full-scene rendering or preview smoothness.

```sh
build-make/tools/encode-spike/libertas-encode-bench \
  build/ffmpeg-comparison/project.json build/ffmpeg-comparison/baseline
build/linux-ffmpeg-9.1/tools/encode-spike/libertas-encode-bench \
  build/ffmpeg-comparison/project.json build/ffmpeg-comparison/updated
```

Use fresh output directories when repeating: existing recordings are never overwritten.

### Results — 2026-10-25

Main Output: 2560×1440, 60/1 fps, H.264 NVENC P5, CBR 40 Mbps, AAC 320 kbps,
48 kHz stereo. NVIDIA RTX 2070, driver 615.78.08. Application base commit `cea6260`;
both application builds use GCC 16.2.1, RelWithDebInfo. The original application and
desktop remained running during both benchmarks; no builds or test suites overlapped
the measured trials.

Medians of three trials (600 video frames plus matching audio per trial):

| Measurement | System FFmpeg 9.0.2 | Local FFmpeg 9.1-dev |
| --- | ---: | ---: |
| Throughput | 100.297 fps | 114.128 fps |
| Wall time | 5.982 s | 5.257 s |
| Process CPU time | 5.902 s | 5.212 s |
| Mean RGBA conversion | 7.976 ms | 7.816 ms |
| Mean video encoder submission | 0.583 ms | 0.618 ms |
| Whole-frame p95 | 11.062 ms | 9.058 ms |
| Frames over 16.667 ms | 0 | 0 |

Combined throughput improved **13.8%**, with **11.7% less process CPU time**.
This does not establish a universal recording speedup: the test uses generated video
and a sine tone, includes AAC encoding, and compares a distro build with a local build.
Video encoder submission itself did not improve. Conversion improved about 2%.

Both trial-1 files decode without errors and contain 600 video frames at 60 fps,
with a 10.005 s container duration. Their encoded H.264 payload SHA-256 hashes match:
`cfe0aeb1d4208d44e3ddcb02bddb7026c28ccf908da632ca0a10759e2bad789d`.

Before upgrading, `test_encode` (NVENC enabled) and `test_preview` both passed.
The new launcher also passed an offscreen `--version` smoke check.
After upgrading, the full suite finished with **31 passed, 1 skipped, 0 failures**,
including NVENC encoder tests, preview, media-source worker and the UI self-check.
`test_gpu_composite` is opt-in (`LIBERTAS_TEST_GPU=1`) and was skipped.

### Default-build integration — 2026-10-26

The standard `linux` preset now builds against local 9.1, including custom browser docks.
The full existing suite passed again (31 passed, 1 optional GPU check skipped), with NVENC
enabled. The added `ffmpeg_selection` regression check passed separately; it reconfigures
one build directory between system and local engines and checks the actual library paths.
