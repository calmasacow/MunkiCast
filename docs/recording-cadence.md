<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Recording cadence

USB capture and output use independent clocks. Frames can arrive in order but in
bursts: newest-only sampling repeats the previous picture just before a delivery,
then skips an input at the next tick. Zero missed encoding slots does not rule this
out. Driver sequence tracing distinguishes input loss from application sampling.

## Capture buffering

The existing V4L2 **Use buffering** setting now controls both ordered device dequeue
and a bounded software presentation queue. The queue uses valid monotonic driver
timestamp intervals, retaining the initial arrival phase. Missing/invalid timestamps
fall back to the negotiated period and driver sequence. It samples against the
scheduled output time with a two-period delay: approximately **33.3 ms at 60 fps**.
Disabling buffering keeps the newest-frame path. Saved settings are unchanged.

Sampling never waits for capture and never copies pixels. The capture pool has six
leases when buffering is enabled: at most four queued frames, one selected frame,
and producer space. Raw YUV leases allocate their actual format size. All placements
and canvases in a render cycle borrow the same selected source frame. Program
projectors and encoding continue to share the same full-resolution composition.

The source's own capture audio receives matching delay through the existing audio
DSP. A separate audio-input source is matched only by an explicitly identical
`device`/video `audioDevice`; unrelated/default microphones are not delayed. The
internal effect is never saved and existing manual delays remain additive. Verify
lip sync with real hardware; device and audio-driver latency remain independent.

Driver resets, sequence wrap, loss and long stalls are bounded/reanchored. This is
jitter tolerance, not genlock: different actual clock rates eventually require a
repeat or skip, stalls longer than the queue still repeat, and buffering cannot
create pictures missing or duplicated upstream. No render-time capture wait or
initial output-clock phase adjustment is used.

At matching input/output rates, selection now uses a quarter-period hysteresis
band around the usual timestamp window. This preserves consecutive frames while
small timestamp fluctuations cross a sampling boundary; sustained clock drift
still causes the necessary isolated repeat/skip. The target delay remains two
periods; the eligibility band is about 4.17 ms at 60 fps. Other output rates retain
timestamp selection. See the traced correction below for measured before/after data.

## Repeatable checks

Close the operator app so the check can use the capture devices. Use a temporary
copy of a saved show with buffering enabled, matching-rate input in Main Program,
and a working encoder. The check reads without saving and records to a new printed
temporary directory. Live GPU/capture hardware is required.

```sh
cmake --build build-make --target munkicast-ui test_video test_preview test_encode -j8
ctest --test-dir build-make -R '^(test_preview|test_video|test_encode)$' --output-on-failure
python3 tools/check-recording-cadence.py /path/to/show.munkicast --scene FS.W.CAM --source Elgato.4K.X.VC --seconds 20
python3 tools/check-recording-cadence.py /path/to/show.munkicast --scene FS.W.CAM --source Elgato.4K.X.VC --projector --seconds 20
```

The live check asserts no stale selected samples for the chosen source, zero missed
output slots and at least 97% of expected encoded frames. All sources are reported.
`advance=0` means reuse; `advance=2` means a skipped callback. `driver_sequence`
includes device frames skipped before callbacks. Counters establish sampling, not
pixel uniqueness: validate moving gameplay/a visible input frame counter separately.
A 60 fps projector on a 30 Hz monitor also drops display presentations independently
of the recorded output.

## Investigation, 2026-11-09

The supplied 1440p60 recording had repeated game pictures and occasional 33–34 ms
PTS gaps. Newest-only tracing reproduced paired repeats/skips with no missed output
slots. A 4 ms render wait and initial phase adjustment were tested and removed:
neither reliably eliminated repeats, and capture waiting consumes render budget.
The operator clarified that input-to-output delay is acceptable; render lag is the
priority. The old ordered-dequeue checkbox alone still repeated nine game samples
in about 1,200 output ticks without render waits, motivating paced buffering.

The initial Guermok diagnosis inferred a delivery deficit from application traces.
Direct verification below supersedes that interpretation: raw YUYV delivery is
faster than 60 Hz, with roughly 30 unique pictures/sec. Remaining acceptance: verify
the camera/HDMI path at true 60 unique pictures/sec, visible input counters and lip
sync; record HDMI during transitions/concurrent outputs; run long-show/load tests.
Keep saved operator projects and original recordings intact.

### Paced-buffer validation

The exact `build-make` launch tree rebuilt with warnings treated as errors.
`test_video`, `test_preview` and `test_encode` passed; the preview check also asserts
that source sampling receives the encoder's scheduled timestamp. Buffer tests cover
ordered burst delivery, repeated sampling, bounded leases, wrap/reset, disconnect
and idle recovery without sleeping.

Three isolated 20-second `FS.W.CAM` runs used temporary copies of the same show:

| Capture buffering | Program projector | Encoded frames | Reused game samples | Reused camera samples | Missed output slots |
| --- | --- | ---: | ---: | ---: | ---: |
| Enabled | Closed | 1,199 | 0 | 15 | 2 |
| Enabled | Windowed | 1,200 | 0 | 31 | 1 |
| Disabled, control | Closed | 1,200 | 295 | 25 | 1 |

All reported zero encoding lag. Logs/recordings are respectively
`/tmp/munkicast-cadence-ujhd8rna`, `/tmp/munkicast-cadence-9si94cp8` and
`/tmp/munkicast-cadence-tf6wvat5`; temporary files are not permanent test fixtures.
The projector was ready/error-free and reported 1,195 submitted / 1,165 presented
pictures. Its run enabled existing GPU benchmark instrumentation.

These pre-timestamp-correction runs passed game-input reuse but **failed the strict
end-to-end check**: render stalls missed output deadlines and the camera reused
samples. Missed-slot diagnostics reported approximately 19–23 ms total render
work in the buffered runs, versus 17.6 ms in the disabled control. Buffering does
not account for all render stalls and does not establish zero performance cost.
GPU timestamps show separate Main/secondary canvas uploads and readbacks; profile
CPU plan/copy time and shared-source upload reuse before changing scheduling or
introducing an asynchronous GPU/encoder pipeline. Keep capture waits out of renders.

Operator acceptance still needs moving input counters, audio/video lip-sync checks
and physical HDMI recording under the actual show load.
Those isolated checks did not modify the saved operator project. The later live
Elgato check below enabled **Use buffering** in the running document.

### Direct camera verification, 2026-11-09

The MunkiCast UI and renderer were closed. `v4l2-ctl` captured 900 buffers per format
directly from the Guermok at 1920 × 1080, configured for 60 fps. Arrival time was
reconstructed from each monotonic driver timestamp plus `delta now`, independently
of the application. No driver sequence gaps occurred. Results after 120 startup
buffers:

| Format | Actual delivered buffers/sec | Median interval | Maximum interval over full run | Unique pictures/sec |
| --- | ---: | ---: | ---: | ---: |
| YUYV | 61.2933 | 16.312 ms | 16.982 ms | 29.9437 |
| MJPEG | 60.0109 | 16.663 ms | 19.049 ms | 29.9374 |

Separate 600-frame FFmpeg captures used passthrough frame timing and frame MD5s.
YUYV was hashed without transcoding; MJPEG was decoded to raw YUYV before hashing.
After excluding the first 120 frames (startup includes black/test bars), 245/479
YUYV frame transitions and 240/479 MJPEG transitions were byte-identical. Changed
pictures predominantly last two USB frames. Thus roughly 30 unique pictures/sec
already arrive from the camera/capture path **without MunkiCast running**. Buffering
or a transport-format switch cannot create the missing unique pictures.

The 61.29 Hz YUYV clock also exposed an application bug: advancing its presentation
queue by the advertised 60 Hz period accumulated phase error and periodically
flushed/refilled the queue. Capture now retains valid V4L2 monotonic timestamps;
the queue follows their intervals while preserving its initial arrival phase,
fixed two-period delay and nominal-period fallback. It still never waits for input.
A deterministic 20-second regression simulates this faster camera with delivery
jitter, requiring no repeated selected samples and allowing the expected skips
when resampling 61.29 Hz to 60 Hz. Video and preview tests passed on the rebuilt tree.

Additional direct simultaneous-device tests, including reversing startup order and
using four/six mapped buffers, kept YUYV near 61.29 buffers/sec with no driver gaps.
A standalone probe using the actual `VideoCapture` library, raw YUV callbacks and
both devices measured 59.993 buffers/sec, zero driver gaps and a maximum callback
interval of 17.254 ms. Driver/negotiation paths can therefore differ; a bare USB
test does not certify full-show capture behavior.

After the timestamp correction, isolated 20-second full `FS.W.CAM` recordings used
both devices, both normal canvases and the original show workload:

| Camera format, buffering enabled | Encoded frames | Reused camera samples | Reused game samples | Missed output slots | Encoding lag |
| --- | ---: | ---: | ---: | ---: | ---: |
| YUYV | 1,201 | 19 | 0 | 0 | 0 |
| MJPEG | 1,202 | 0 | 0 | 0 | 0 |

The MJPEG run passed the unchanged strict all-source cadence check. Logs/recordings
are `/tmp/munkicast-cadence-v9tg3a88` and `/tmp/munkicast-cadence-fu0o4zel`. This
verifies one short mixed-scene workload, not a long-show performance guarantee.
**MJPEG plus buffering passed this individual run**; the later recheck below did
not reproduce that clean result, so it is not a reliable completed workaround. The YUYV
full-show selection deficit is still open; direct device and standalone library
checks did not reproduce it. Profile it under actual render load rather than
assuming missing USB frames from selected-sample counters alone.

Zero reused selected samples means every output tick received a new capture buffer;
it does **not** mean the picture changed. Both camera formats still carry roughly
30 unique pictures/sec from upstream. No saved camera format/buffering option was
changed automatically.

Diagnostics, frame hashes and summary JSON are in
`/tmp/munkicast-camera-verify-20261109` (temporary, not permanent fixtures). Commands
for repeating direct timing/content checks while the app is closed:

```sh
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=YUYV --set-parm=60 --stream-mmap=6 --stream-poll --stream-count=900 --stream-show-delta-now --verbose
ffmpeg -nostdin -f v4l2 -input_format yuyv422 -video_size 1920x1080 -framerate 60 -i /dev/video0 -map 0:v -c:v copy -frames:v 600 -fps_mode passthrough -f framemd5 /tmp/camera-frames.md5
```

Only the device capture format was changed for testing; YUYV/1080p60 was restored.
Saved projects/settings were not edited. The capture card does not support
`VIDIOC_QUERY_DV_TIMINGS` and exposes no camera HDMI/shutter controls through V4L2.
**Physical camera verification remains:** check camera HDMI output rate and
shutter/exposure settings, then compare a known moving frame counter through this
card versus a known-good 60 fps capture path. This distinguishes the camera's
output from capture-card frame conversion; current tests establish the combined
upstream limit, not which of those two devices is responsible.

### Recheck after the reported camera frame-rate change

The operator reported changing the camera from 29.9 fps. Fresh direct captures,
with MunkiCast closed, each collected 900 frames and excluded the first 120:

| Format | Measured delivered buffers/sec | Unique picture changes/sec | Identical transitions / measured transitions |
| --- | ---: | ---: | ---: |
| YUYV | 59.9969 | 29.9599 | 390 / 779 |
| MJPEG | 60.0000 | 29.9615 | 390 / 779 |

Both streams still contain consecutive byte-identical pairs: 388 two-frame runs,
one three-frame run and one boundary single frame in each measured window. YUYV
was hashed without transcoding; MJPEG was decoded before hashing, with passthrough
timing. Transport delivery is now about 60 fps, but the camera/capture path still
supplies about 30 unique pictures/sec. These observations do not establish which
physical camera setting was changed or whether capture-card conversion is involved.
Verify actual HDMI output rate, camera live/movie mode and shutter/exposure settings.

Frame hashes, logs and JSON summaries:
`/tmp/munkicast-camera-recheck-ADJyV3Pd`. Saved MunkiCast camera settings remain
1080p60 / Auto / buffering disabled; no saved source settings were edited.

A subsequent 20-second full `FS.W.CAM` check using the same temporary
MJPEG + buffering show encoded 1,200 frames with 14 reused camera samples, one
missed output slot and zero encoding lag. Game input had no reused samples and
one skip corresponding to the missed output slot. The strict check failed;
`/tmp/munkicast-cadence-3mek5dkn` retains the log/recording. The earlier clean MJPEG
run is not a reliable general workaround. Both upstream picture rate and remaining
application capture/render timing need acceptance; neither is certified fixed.

### Windows comparison and Linux mode checks

The operator initially reported a smooth, apparently 60 fps Windows preview from the same
camera through an HDMI splitter, with a separate capture card on each computer.
This makes Linux/card negotiation a valid suspect. The earlier Linux tests establish
where duplicates are observed, not that the camera itself is limited to 30 fps.
Separate cards and splitter branches remain uncontrolled variables; visual smoothness
also needs a frame-content measurement for a precise cross-platform comparison.

The Linux card is `345f:2130`, Guermok/UltraSemi MS2130, firmware descriptor revision
`31.00`, bound to `uvcvideo`, and connected at **5000 Mb/s**. Thus it is not currently
on USB 2. Additional direct raw-frame tests, excluding 120 startup frames:

| Linux request | Measured delivered buffers/sec | Unique picture changes/sec | Repeated transitions |
| --- | ---: | ---: | ---: |
| YUYV 1280 × 720, 60 fps | 60.0269 | 29.9508 | 240 / 479 |
| YUYV 1920 × 1080, 50 fps | 49.9988 | 30.0619 | 191 / 479 |
| YUYV 1920 × 1080, 30 fps | 30.0036 | 29.9124 | 1 / 329 |

Unique picture rate stays near 30 when reducing bandwidth or changing output frame
rate; it does not simply halve every requested rate. That rules against a simple
requested-rate-halving interpretation and does not establish a generic Linux cap.
Diagnostics: `/tmp/munkicast-camera-linux-modes-20261109`; original 1080p60 YUYV
device mode restored, saved projects untouched.

[UltraSemi's MS2130 specification](https://www.ultrasemi.com/en/ic/macrosilicon/ms2130.html)
lists default 1080p60 output and Linux support. This is advertised capability, not
evidence that this particular card/firmware/HDMI branch is delivering unique 60 fps.
[Independent chip research](https://people.osmocom.org/steve-m/hsdaoh_slides/hsdaoh.html)
also documents that configured USB output rate is independent of input frame rate.

**Operator action needed to isolate the cause:**

1. Keep one physical capture card, its HDMI cable and its splitter output fixed.
   Move only its USB connection between Windows and Linux. Capture moving content
   at identical 1080p60 settings; measure raw/decoded picture hashes on both hosts.
2. If true unique 60 fps works only on Windows with that same card/branch, compare
   negotiated formats, firmware and UVC control/stream negotiation on the two hosts.
   No driver or firmware fault is confirmed until this comparison.
3. If the limitation follows a card/HDMI branch across hosts, swap splitter outputs
   and then capture cards separately to distinguish card firmware/hardware from
   splitter/cable behavior. Verify actual source HDMI timing as part of that test.

Physical swaps and Windows-side capture are unavailable from the current Linux
session. No firmware flash or kernel parameter change is justified by these data.

### Windows confirmation: latest finding

The operator subsequently confirmed that Windows also captures only 30 fps. This
supersedes the earlier visual impression of 60 fps and makes a Linux-only limit
unlikely. Linux raw-frame measurements still establish about 30 unique pictures/sec;
the Windows result is operator-reported, not measured from this Linux session.
The camera's live HDMI output, splitter conversion and capture-card behavior remain
unisolated. No specific camera or card fault is confirmed.

Next physical checks, in order:

1. Connect the camera directly to a capture card, bypassing the splitter, and measure
   unique moving pictures. If that restores 60 fps, investigate splitter mode/EDID.
2. If direct capture remains at 30, feed the card a known 1080p60 moving frame counter.
   A unique 60 fps result points back toward camera HDMI/live-mode/exposure settings;
   a 30 fps result keeps the card, its firmware and negotiation under investigation.

These physical checks replace the earlier Windows-only failure hypothesis. Separate
MunkiCast capture-sampling repeats and render deadline misses remain open regardless
of the upstream camera picture rate.

### Elgato fast-motion repeats, 2026-11-09

The operator reports 14 repeated pictures in one second of a whip pan. The exact
file/timecode for that count is still needed. Independently, the original
`Main-Output-20261109-122751-552-01M70S23KZA2GQMZ20GDFJSQ4E.mkv` contains repeated
game pictures: seconds 290–300 decode to 600 frames with 16 near-identical
consecutive center-crop pairs amid substantial motion. Their mean absolute luma
differences are below 0.1/255 after scaling the central 1280 × 720 to 320 × 180;
lossy encoding means identical inputs need not decode byte-identically. The PTS
intervals are exactly the expected 16/17 ms, with no gaps or duplicate timestamps
in that section. These repeats are stored pictures, not solely player-generated
holds over missing timestamps. A wider 270–330 s scan also finds repeats; it does
not establish the cause or reproduce the reported 14-repeat second.

An isolated regression now feeds 128 rapidly moving, individually numbered pictures
through the application's color conversion, H.264 encoder and muxer, reusing and
overwriting the caller's pixel buffer after every submission. Software decoding
checks every frame ID and presentation timestamp, including the decoder's delayed
tail. Ordinary x264 coverage includes B-frame reordering. Opt-in NVENC coverage
uses the live recording's 2560 × 1440 / 60 fps, 40 Mb/s CBR, p4, high profile,
quarter-resolution multipass, spatial AQ, two B-frames and disabled lookahead.
This isolates encoding from capture/rendering; it cannot certify the live input
or exclude failures under other workloads.

```sh
cmake --build build-make --target test_encode -j8
MUNKICAST_TEST_NVENC=1 ctest --test-dir build-make -R '^test_encode$' --output-on-failure
```

The opt-in encoder suite passed all 15 cases, including the moving-frame check,
in 4.40 seconds. The exact `build-make` UI launch target rebuilt successfully;
its capture-settings help now explains the delay and newest-frame tradeoff.

The live Elgato source was still set to **buffering disabled** when inspected.
With no outputs active, only that setting was changed to enabled through native
MCP, then read back successfully. This changes the operator's live document and
may autosave; resolution and capture/output rates remain 1440p60. It does not prove
what settings were active throughout any earlier recording. A 120 Hz HDMI
passthrough and 60 fps USB capture are compatible; the output should select about
every other unique input picture. Monitor refresh alone does not measure source
picture uniqueness.

A 15-second buffered recording from the running app reported zero output misses,
zero encoding lag, about 60 capture callbacks/sec and about 5 ms Main render time.
However, the current input was a Windows desktop showing Resolve reviewing the
older recording. It is **not fresh-gameplay acceptance** and cannot establish a
fix. The app and capture devices were not restarted. Temporary measurements,
recording and screenshots are in `/tmp/munkicast-elgato-buffered-live-k0md1i9d`;
original-clip analyses and encoder logs are in
`/tmp/munkicast-elgato-live-20261109`.

The operator supplied the follow-up recording below, satisfying the request for
new gameplay footage. That check supersedes the earlier pending acceptance step.

### Buffered gameplay recheck, 2026-11-09

`Main-Output-20261109-154249-750-01M71473MPFFW4EVJDTZS6NHM3.mkv` contains
29:31.717 of 2560 × 1440 / 60 fps gameplay. All **106,274 video pictures** decoded
successfully. The live Elgato setting still reads buffering enabled, with about
60 capture callbacks/sec. The final application's output count matches the file:
106,274 submitted frames, **29 missed output slots**, zero reported encoding lag.
Current settings cannot independently certify settings at every earlier instant.

**Buffering has not resolved the reported fault.** A full low-resolution center-crop
scan identified a fast-motion cluster at **13:07–13:08**. A separate software decode
of 13:06–13:11 verified it at 320 × 180, using the original central 1280 × 720:

| File second | Near-identical consecutive pairs | Pairs bracketed by substantial motion |
| --- | ---: | ---: |
| 13:07 | 16 | 15 |
| 13:08 | 16 | 14 |

Near-identical means mean absolute luma difference below 0.1/255; substantial
neighboring motion means differences above 3/255 on both sides. For example,
13:07.250 differs from its predecessor by 0.050/255, while the surrounding changes
are 21.331 and 11.921/255. These are repeated pictures with small lossy-codec
differences, rather than a conclusion based only on the reported frame rate.
The section retains normal 16/17 ms timestamps, with no missing output slots.

All 29 missing slots occur separately in two short clusters at approximately
**8:35.067–8:35.683** and **23:35.017–23:35.650**. There are 23 gaps of 33/34 ms and
three of 50 ms; DTS remains increasing. The two clusters are 15 minutes apart,
but no cause for that periodicity is established. Kernel/user journals do not
provide a confirmed application cause. Missing slots cannot explain the repeated
stored pictures around 13:07. Zero encoding-lag counters cannot certify freshness.

Analyses, exact PTS gaps and the verified whip-pan frame comparisons are in
`/tmp/munkicast-elgato-new-20261109`. Static scenes also produce low pixel differences;
the whole-file candidate count is deliberately not presented as a duplicate count.
The earlier isolated NVENC test passed, but it excludes capture and GPU rendering.
The recording alone cannot distinguish those stages or prove an encoder defect.

#### Diagnostic setup for the follow-up recording

The existing `MUNKICAST_TRACE_OUTPUT` diagnostic now also logs `CAPTURE_INPUT`:
source ID, driver sequence, original capture timestamp, callback arrival timestamp
and a hash of **all captured pixel bytes**, before the presentation queue. Existing
`CAPTURE_SAMPLE` lines now include source ID and scheduled output time, allowing
selection to be joined to input pictures by source ID/driver sequence. Hashes are
comparable within one run and matching hashes are strong evidence of identical
capture data, subject to hash collisions. They do not reconstruct HDMI frame IDs.
Hashing and input logging occur before the render mutex and are disabled in normal
runs. This debug run adds capture-thread work and is not a performance benchmark.
The `build-make` UI/renderer target rebuilt successfully; `test_video` and
`test_preview` passed (51.00 seconds combined). The new live capture trace still
requires the follow-up run below; these tests do not certify hardware freshness.

The operator relaunched with the new trace and supplied the follow-up gameplay
recording below. The previous no-relaunch instruction applied to enabling
buffering; new diagnostics required a new renderer process.

```sh
MUNKICAST_TRACE_OUTPUT=1 ./build-make/apps/munkicast-ui/munkicast-ui 2>>/tmp/munkicast-capture-trace.log
```

The trace appends so earlier diagnostic data is preserved. For future diagnostics,
keep buffering enabled and use fresh input rather than playback of an older faulty
recording. Leave tracing off for ordinary use.

Trace interpretation:

1. Join selected driver sequences to input hashes/timestamps during visible repeats.
   A repeated selected sequence identifies reuse in sampling. New selected sequences
   with repeated input hashes place the repeats before composition/encoding; compare
   direct raw V4L2 capture with the app closed to separate device/input duplication
   from the application's capture path.
2. If input pictures change and selection advances but the output repeats, isolate
   GPU composition/readback from encoder submission using the same numbered input.
   Investigate timestamp jitter or clock tracking only with the measured input and
   selection intervals. Keep capture waits out of rendering.
3. Trace `OUTPUT_DEADLINE` stage timings if the separate missing-slot bursts recur.
   Fix the measured stage, repeat the short check, then validate a long recording
   with moving input counters and lip sync. The fast-motion issue remains open.

### Traced buffer-selection correction, 2026-11-09

The follow-up
`Main-Output-20261109-163406-861-01M71750MC20QPT6YVJ4PMF9HC.mkv` contains 25:04.981
of 1440p60 gameplay and **90,275 video pictures**. All output PTS map to the recorded
`CAPTURE_SAMPLE` times within 0.394 ms, allowing source selection and encoded
pictures to be compared directly. A software-decoded center crop at **13:57** has
14 repeated pictures; the trace shows exactly 14 reused driver sequences in that
second. Another nine selected-frame holds occur at 13:58. Across the full clip,
the application reused 27 selected frames and skipped 51 capture callbacks after
its first recording sample.

**The burst is a MunkiCast buffer-selection bug.** For example, when driver frame
80082 was reused at 13:57.017, frame 80083 had arrived 40.351 ms before the scheduled
output tick, with different pixel bytes. Frame 80084 was also already available.
Across 13:55–14:01, capture timestamps advanced by 16.612–16.712 ms with no driver
sequence gaps; arrivals were 13.417–19.337 ms apart. The queue had input to display.
Tiny driver timestamp fluctuations crossed the hard eligibility threshold, causing
the application to hold one frame and then advance two. The encoder faithfully
stored those repeated compositions; capture waiting would not fix this case.

Selection now prefers the next consecutive frame when matching-rate samples remain
within a bounded quarter-period hysteresis band. Outside that band it retains the
existing timestamp-based resampling, so different real clock rates and genuine
stalls still advance/hold as necessary. Repeated sampling at the same scheduled
time is idempotent. The fixed target delay, pixel leases and queue bounds remain;
the change adds no pixel copies or waits.

Offline replay used every input arrival/capture timestamp and render sample from
startup through the recording. The original buffer reproduced **every observed
driver selection**, including all 27 holds. Recompiling that replay with the fix:

| Same traced input/output schedule | Selected-frame holds | Skipped callbacks after first sample |
| --- | ---: | ---: |
| Original buffer | 27 | 51 |
| Corrected buffer | 0 | 24 |

The 27 removed skips match the 27 removed holds. This verifies the timing fix
against the actual 25-minute workload; it is **not a newly recorded end-to-end
acceptance test**. Raw capture also contains identical pixel hashes, including
still scenes; removing application holds does not promise every input picture is
unique. Temporary replay source/events/results, frame comparisons and reports:
`/tmp/munkicast-tracecheck-20261109`.

The new deterministic timestamp-jitter regression failed on the original code at
tick 1 and passes with the correction. A second test simulates 25 minutes each
with slightly faster/slower source clocks and driver timestamp jitter, requiring
necessary drift resampling without paired artificial holds/skips. Existing burst,
lease-bound, restart/wrap, invalid timestamp and 61.3 Hz camera checks also pass.
`test_video`, `test_preview` and `test_encode` passed together in 59.74 seconds.
The exact `build-make` launch target was rebuilt.

#### Remaining validation and separate deadline misses

Close the traced application after stopping outputs, then launch normally to use
the correction and disable the expensive capture-picture diagnostic:

```sh
./build-make/apps/munkicast-ui/munkicast-ui
```

Keep Elgato buffering enabled. Validate fresh fast pans around a visible input
frame counter, then a long recording and lip sync. If repeats remain, use the
input hashes/selection trace to determine whether they are already in capture or
arise after selection; the identified application hold/skip burst is fixed in
replay but fresh hardware acceptance remains outstanding.

This recording also has **23 genuinely missed output slots**, separate from the
13:57 selection burst. Deadline traces show combined render/encode/publish work
exceeding 16.67 ms: one cycle took 41.674 ms (render 16.548, encode path 19.751,
publish 5.309 ms). Bursts near 2:18 and 17:18 occur 15 minutes apart, matching the
wall-clock timing pattern in the preceding recording; one additional slot was
missed at 6:06.7. No background job or driver cause is confirmed. Zero reported
encoding-lag frames does not mean encoding never contributes to the frame budget.
Keep this performance work open: correlate CPU/GPU scheduling and per-stage
conversion/upload/readback costs during the next burst, reduce the measured cost,
then rerun recording/projector/concurrent-output acceptance. Do not add capture
waits or claim this selection correction eliminates deadline misses.

### Live validation after the selection fix, 2026-11-09

The operator supplied `/tmp/munkicast-capture-trace.log` and
`Main-Output-20261109-180217-026-01M71C6ET29S8DVMY1V409ZB23.mkv`. This is
4:34.261 of 1440p60 gameplay followed by end-of-run/menu screens. The appended
trace contains earlier sessions too; this analysis uses the final recording
marker and the capture session beginning at driver sequence 0 / monotonic
26066163879000 ns. All **16,436 video pictures** decoded successfully. File PTS
match the selected-frame timestamps within 0.345 ms.

**The buffer-selection correction passes this real recording:**

| Same recorded schedule | Reused selections | Skipped callbacks after first sample | Selections differing from actual trace |
| --- | ---: | ---: | ---: |
| Original buffer, offline replay | 38 | 58 | 1,853 |
| Corrected buffer, offline replay | 0 | 20 | 0 |
| Actual corrected application | 0 | 20 | — |

Thus the corrected replay matches every actual driver-frame selection, and none
of the 16,436 output frames reused a selected capture frame. This is hardware
validation of the identified sampling fix, not merely the earlier offline result.
It does not establish long-show stability across every scene/device or lip sync.

There are **152 identical incoming pixel-hash pairs**, and the same count among
selected pictures. Most of the high-count seconds (2:52–2:55) show the game's
`RUN_COMPLETE` screen. Identical still/menu pictures are not evidence of lost
capture or encoding. Capture hashes describe bytes before buffering/composition;
they do not guarantee that a moving game delivers a different picture every refresh.

A full decoded center-crop scan also identified 17 near-identical pairs bracketed
by substantial motion whose full capture hashes differ. These are **candidates,
not proof of another application duplication bug**: a full-frame hash can change
because of HUD/cursor/noise outside the crop, while the game view stays unchanged.
The current trace does not measure the magnitude/location of input differences.
An additional check of both whole frames at 640 × 360 confirms that these pairs
are also nearly identical in luma outside the original crop (whole-frame mean
differences 0.022–0.111/255). Thus they cannot simply be dismissed as a static
center crop. This check still does not recover raw input pixel differences or
establish whether chroma changed; attribution remains unresolved. Exact times and
whole-frame luma comparisons are in `candidate-whole-frame-summary.json` in the
analysis directory below.
Do not attribute these to the encoder, or claim every picture-level repeat has
been eliminated, from the hash alone. If these times remain visibly problematic,
compare regional/raw input differences with pre-encode composition before changing
timing or encoder settings; a visible source frame counter remains the strongest
way to establish unique source pictures.

#### Remaining output-deadline fault

The file has **19 missing output slots**, all at approximately
**4:07.700–4:08.517** (17 gaps of 33/34 ms and one gap of 50 ms). The trace accounts
for all 19 with `OUTPUT_DEADLINE`, including a cycle of **46.079 ms** against the
16.67 ms budget: prepare 1.514, render 23.301, encode path 13.475 and publish
7.764 ms. These are output gaps, separate from the corrected selected-frame holds.

The burst begins around wall-clock **18:06:24.7**, matching the preceding
15-minute pattern. The installed Omarchy agent panel has a default 900-second
refresh, and its journal reports collector work at 18:06:27, as during earlier
bursts. This is a correlated background-work candidate, **not a proven cause**.
No desktop configuration, collector, firmware or encoder setting was changed.
Profile CPU scheduling/memory pressure and GPU/encode/copy stages around the next
burst, then use a controlled comparison to establish causality before a fix.

The operator application was already closed when inspected; it was not killed
or restarted. No further application code changes were justified by this
validation. Reports, selected/input timestamps, cropped-picture analysis and
replay events are in `/tmp/munkicast-validation-1802-20261109`. Normal use should
leave `MUNKICAST_TRACE_OUTPUT` unset; full-picture hashing/logging is diagnostic
work and must not be treated as a zero-overhead performance measurement.

### Long streaming/recording follow-up, 2026-11-10

`/home/calmasacow/Volumes/media-store/RecordingTest/`
`Main-Output-20261109-233938-074-01M71ZG5ET6C73TABDNBWAHRDM.mkv` contains
64:48.554 of 2560 × 1440 H.264/NVENC at nominal 60 fps: **232,468 video packets**.
Presentation timestamps confirm **844 missed output slots**, distributed across
772 gaps. The worst gap is 183 ms at 0:30.083. These gaps exist in the saved file;
they are not merely remote-player buffering. Recording and live RTMP were both
active, sharing the same encode, according to the application's launch log.

Five recurring bursts around **3:59, 18:59, 33:59, 48:59 and 63:59** account for
219 of those missing slots in six-second windows. Their wall-clock times again
coincide with the desktop agent collector's fifteen-minute refresh. This remains
correlation, not a measured attribution. The remaining 625 slots occur elsewhere.
This run did not enable capture/deadline tracing; it cannot establish which input
pictures were reused or assign the old gaps to individual render/encode/I/O stages.

Inspection identified another definite render-blocking path: compressed packets
were muxed and written to recording files and RTMP sockets synchronously on the
render thread. Both storage latency (this recording uses SMB) and RTMP socket
backpressure could therefore interrupt composition and every shared destination.
Each destination now has its own writer, bounded to 1,024 queued compressed packets
and 64 MiB including in-flight bytes. Packets retain FFmpeg buffer references;
there is no raw-frame copy or capture wait. A full queue explicitly fails that
destination instead of silently discarding recording packets or blocking rendering.
Normal stop/finish drains queued packets, finalizes the container and reports
write/flush/fsync failures; start/stop operations can still wait for connection or
finalization. This change does not eliminate CPU/GPU scheduling stalls.

A deterministic test blocks one real file-write syscall while submitting 1,500
frames to two destinations. The original code blocks until the watchdog releases
the writer and fails five assertions. With the correction, all frames submit while
the write is still blocked, the saturated destination reports an explicit failure,
and the healthy destination retains all 1,500 packets. The encoder suite passes all
16 cases, including file-write failure, shared-output decoding and replay coverage;
the capture-buffer and preview integration tests also pass.

Ordinary runs now log `OUTPUT_DEADLINE` with the scheduled monotonic timestamp
and prior-cycle stage timings when output slots are missed. This inexpensive
failure-only evidence no longer requires expensive input-picture hashing.
A 45-second live baseline before this change produced 2,705 frames and zero
missed slots; that short, non-streaming run is not evidence that the intermittent
long-stream fault was absent. Packet reports and local diagnostic recordings are
under `/tmp/munkicast-validation-2339-20261110`.

#### Current 120 fps input and continued profiling

The saved show now requests 1440p120 from Elgato into a 60 fps canvas; live counters
confirm approximately 119.99 captured buffers/sec. The earlier hysteresis correction
handled only matching input/output periods. A new regression reproduces selecting
driver frame 1 instead of 2 at the second output tick when a 120 fps driver's even
frame has only 20 microseconds of timestamp jitter. The queue already contains the
desired frame. Selection now preserves strides 1–4 within the existing bounded
phase tolerance. Queue size, capture delay and pixel leases are unchanged; other
ratios and real drift retain timestamp resampling. Eight integer-ratio jitter cases
and 25-minute faster/slower clock simulations at both 60-to-60 and 120-to-60 pass.
This confirms another selection fault, without attributing the original file's
844 timestamp gaps to capture sampling.

`MUNKICAST_TRACE_SAMPLING=1` logs only selected driver sequences and times; it does
not hash captured pixels. `MUNKICAST_BENCHMARK=1` additionally enables the existing
GPU timestamps and per-encoded-frame conversion/codec/other timings. Both remain
diagnostic settings. A 90-second live check with these settings had zero missed
output slots, including a controlled, network-free scan of four providers' local
usage histories. Those local scans did not reproduce the periodic burst. Outside
the two startup selection transients, this run advanced Elgato by two driver frames
per output except one one-frame advance, consistent with the measured clock drift.
Driver advancement alone does not prove that incoming pictures differ.

Two four-minute live checks reproduced 22 missed slots apiece at the scheduled
fifteen-minute refresh. The profiled burst shows stable GPU clocks (approximately
1,410–1,425 MHz graphics / 6,801 MHz memory), with substantially increased CPU-side
costs. Mean conversion time rose from 2.238 to 5.020 ms (peak 8.322); mean codec/
enqueue time from 1.053 to 2.833 ms (peak 7.350). GPU preparation rose from 0.767 to
2.757 ms, while measured GPU work rose much less, from 2.371 to 3.033 ms. These GPU
figures combine the Main and Vertical canvases. Clock telemetry/process sampling
were diagnostic overhead; the same 22-slot burst was also observed without them.
These observations narrow the remaining fault to CPU-side work/scheduling and
do not establish that the desktop collector itself is responsible.

Recording now retains the full-resolution CPU readback allocation for each active
output canvas, instead of allocating/zeroing a new RGBA picture each tick. The GPU
compositor fills that existing storage; a tiny unused preview accompanies output-only
ticks, so the full-size buffer can use the same path. Removing an output releases
its retained storage. The existing changing-color projector/encoder pixel check
now exercises reuse across frames. All 15 GPU cases and the encoder, preview and
extended capture tests pass. Validation across the next scheduled burst is recorded
below.

The final rebuilt-app recording saved **5:23.114**, with **19,386 video packets and
zero missing output slots**, confirmed from the finalized file's packet timestamps.
The application subsequently exited with status 0 at 02:57:44, before the planned
02:58 periodic-refresh observation. Recording had already finalized successfully;
no new coredump or error-priority system journal entry accompanied the exit.
The operator confirmed closing it; the app was left closed. This run validates the working
recording path, not elimination of the previously measured periodic fault.

Remaining acceptance: resume the same two-canvas live workload through a scheduled
refresh, then simultaneous RTMP/SMB recording with visible source counters and lip
sync. Preserve failure-only deadline logs; use selected-sequence/GPU/codec timing
diagnostics only when needed. If the CPU burst remains, isolate the complete desktop
refresh (local scans alone did not reproduce it) and obtain an administrator-approved
CPU sampling trace: kernel perf sampling was denied on this machine. Do not change
desktop/account settings or grant scheduling capabilities merely from correlation.

#### Preview regression follow-up, November 10 afternoon

The operator reported approximately 12 ms rendering during recording, then a
visibly frozen preview after stopping despite a 2–3 ms reading. Capture counters
still reported approximately 120 Elgato buffers/sec, while repeated native MCP
queries observed only about one preview generation increment/sec. Render duration
is time spent composing a frame; it does not measure actual picture-delivery rate.

The running UI and renderer inherited `MUNKICAST_TRACE_OUTPUT=1`. This hashes every
captured pixel buffer and logs every arrival/selection: Elgato alone supplies about
664 MB/sec of bytes to hash at 1440p120, with the camera also running. The trace had
grown to approximately 559 MiB. It is unsuitable for normal performance acceptance.

After confirming recording/streaming/replay were stopped and the project clean,
the application closed normally and the exact `build-make` executable restarted:

```sh
env -u MUNKICAST_TRACE_OUTPUT -u MUNKICAST_TRACE_SAMPLING -u MUNKICAST_BENCHMARK \
  ./build-make/apps/munkicast-ui/munkicast-ui
```

Repeated generation-counter samples now measure approximately 60 preview updates/sec.
Main renders around 1.9–2.1 ms preview-only, 4.5–5.2 ms during a local recording,
and 4.9–5.6 ms during a recording to the operator's media-store destination. Each
30-second recording finalized with 1,806 video packets and zero missing timestamp
slots (presentation timestamps sorted to account for encoder frame reordering).
The live counter read 1,804 before encoder flushing; finalized packet counts include
the delayed frames. Both runs report zero encoding lag. After stopping, preview
delivery remains approximately 60 fps and rendering returns to around 2 ms.

The local file is `/tmp/munkicast-validation-2339-20261110/normal-regression-check-live.mkv`;
the destination check is
`/home/calmasacow/Volumes/media-store/RecordingTest/MunkiCast-QA-normal-smb-regression-20261110.mkv`.
The application remains open without diagnostic flags; failure-only deadline logging
is retained in `/tmp/munkicast-normal-preview.log`. No rendering optimization was
reverted on the strength of this comparison. Restarting also resets process state,
so these results do not prove hashing alone caused the freeze, eliminate the
previous periodic fault, or establish that all captured pictures are distinct.

The isolated cadence checker now enables only `MUNKICAST_TRACE_SAMPLING` and removes
an inherited `MUNKICAST_TRACE_OUTPUT`; its assertions never use input pixel hashes.
If delivery collapses again without tracing, capture per-canvas frame/generation
deltas and output state before restarting, then isolate preview scheduling/transport
from actual composition duration. Remaining long periodic/streaming acceptance above
still applies.
