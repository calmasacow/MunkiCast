<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0069 — Prepared show assets on local storage

- **Status:** accepted; stages 1 to 3 implemented
- **Date:** 2026-10-22 (supersedes the Proposed revision of 2026-10-20)
- **Depends on:** ADR-0024 (the document is the only channel to the renderer)
- **Origin:** the operator, in conversation, after the measurements below

## Context

A collection references its media and stills by the paths the operator typed. The
reference show — 27 assets, 5.92 GiB — reads every one of them live through a gvfs
(SMB) mount at 194 MB/s. A full byte copy takes about 31 seconds.

Two costs were measured on that show, not estimated:

- A stat on that mount intermittently took 10-14 ms and stalled one frame a second,
  until commit fac75f6 moved slow stats to a watcher thread. The *reads* are still on
  the mount.
- Per-worker CPU during a live benchmark: 89-116% of a core for the 4K trailer
  (3840x2160 H.264), 45-49% for a 1080p H.264 clip, 8-12% for each VP9 alpha clip.
  Hardware decode is on for all of them, so that cost is Qt's frame conversion to
  RGBA rather than decoding: a 4K frame is 33.2 MB of RGBA (995 MB/s at 30fps)
  against 14.7 MB at canvas size (442 MB/s).

## Decision

Prepared local copies, under an operator-controlled flag, with the state of every
asset visible on its own source row.

**Substitution happens in one place.** `Preview::publish` already copies the project
into `link_->pending` before sending it, and already mutates that copy. The prepared
path is written there and nowhere else. Three things follow, and they are the reason
for the choice:

- the saved project keeps the operator's own paths, so turning caching off is a flag
  rather than a migration;
- `munkicast-render` and `munkicast-source` need no new path logic — they receive the
  prepared path as if it had been typed into the settings field;
- nothing new is stat-ed on the render thread.

**Staleness is `{mtime, size}`, not a content hash.** The earlier revision keyed
prepared files by a hash of the original's content. There is no hashing in this
repo, and hashing 5.9 GB on every staleness check is not viable on a machine that is
also broadcasting. Prepared filenames are a fresh ULID per preparation, recorded in
`index.json`, which also means a rebuild during a show never overwrites the file a
source is reading.

**Stats stay off the frame.** One sweep over the indexed originals and prepared files
runs on a `std::jthread` and posts a single map back, for the same reason fac75f6
exists. `stateOf` and `preparedPath` are asked once per asset on every document
change and never stat.

**A show in progress keeps the copy it started with.** An original that changes
mid-show reads as stale and says so; it does not silently return to the network.
Rebuild is an explicit action.

**Failure is per asset.** A failed entry is recorded with its reason rather than
dropped, so the row can be red and the dialog can say why. That source keeps reading
its own path; the assets beside it stay prepared.

### Stages

1. **Local copy.** A byte copy of every referenced file. This is the whole of the
   measured network cost. *Implemented.*
2. **Progress and cancellation.** Two bars — one for the file, one for the build —
   because one 384 MiB asset in 27 makes a single overall bar look stuck. Partial
   files are written to `.part` and renamed on success. *Implemented.*
3. **Downscale to canvas.** Video wider or taller than the canvas, re-encoded with
   NVENC H.264. Measured: 3840x2160 to 2560x1440 took 31 s for a 104 s clip (3.4x
   realtime, fully on GPU), 384 MiB to 277 MiB, cutting per-frame conversion 2.25x.
   One asset in this collection qualifies, and it is the one that costs a whole core.
   *Implemented*, with three details the measurements did not predict:
   - **It falls back to software.** NVDEC decodes a fixed list of codecs, and NVENC has
     a minimum frame size. An oversized asset in ffv1 or ProRes, or a very small
     target, fails on the GPU and succeeds on the CPU. The gain being bought is in the
     *playback* conversion cost, which does not care how the file was prepared, so a
     slower preparation is still worth having.
   - **The scaled size keeps the source aspect ratio**, fitted inside the canvas, and
     is rounded to even dimensions. Encoding at the canvas's own aspect would bake in
     a stretch no later setting could undo; an odd dimension is either refused or
     silently rounded, depending on the encoder.
   - **The canvas size is part of staleness.** A downscaled copy is made for a
     particular number of pixels, so resizing the canvas makes it the wrong clip and it
     reads as stale. A byte copy has no such tie. The size used is the *largest* canvas
     in the project, not the selected one: preparing for the smaller of two canvases
     would leave the compositor upscaling on the other.
4. **Packed alpha.** Colour above, alpha below, in one H.264 frame, unpacked by a
   compute shader. Deliberately last, and the gain is smaller than it looks:
   single-threaded libvpx decodes 30 s of the 1080p alpha clip in 6.5 s, matching the
   8-12% measured live, and an NVDEC-plus-CPU-unpack pipeline measured *slower* than
   libvpx on the same clip (2.90 s against 1.39 s for 300 frames) because the
   colour/alpha interleave costs ~249 MB/s at 1080p30. Packed alpha only pays off once
   the compositor unpacks on the GPU. It also needs a `kPreviewProtocol` bump, because
   `PreviewFrame` has no layout field and the worker currently decides alpha from the
   `.webm` suffix.

H.264 for both transcodes, not HEVC: NVDEC and NVENC handle both, but H.264 is what
Qt's FFmpeg backend opens most reliably, and at these sizes the bitrate saving does
not pay for the risk.

### Rejected

- **Rewriting the collection's paths.** Reversible only by a second rewrite, and a
  saved project full of cache paths is a project that breaks when the cache is cleared.
- **Content-addressed keys.** See above: 5.9 GB per check.
- **A separate preparation process.** ffmpeg is already a child process. Keeping the
  orchestration in-process avoids inventing an IPC protocol for a progress bar.
- **Decoded-pixel and BC7 still caches, and VRAM-resident clips** (stages 2 and 4 of
  the earlier revision). The stills in this collection total 0.5 MiB and decode once
  into an in-process cache. The measured cost is elsewhere.

### What distinguishes a video from a still

The duration, not the dimensions. ffprobe reports a size for a PNG as readily as for a
clip, so a rule written on size alone re-encodes an 8000x6000 still into a few frames
of H.264 and ruins the asset. The check lives in `recipe_for` rather than only where
probing happens, so it holds for any probe result at all — which is what
`test_assetcache` pins down.

## Consequences

The cache is roughly the size of the show: 5.92 GiB for the reference collection, less
once stage 3 downscales. Preferences reports what it is holding and offers Clear.

Nothing about correctness depends on preparation. With caching off, or with an asset
unprepared, failed or missing, the show reads the original — which is what it did
before this existed.

Deleting the cache deletes only the files `index.json` names, plus the index and any
`.part` leftovers. Never the directory itself: the operator can point it anywhere, and
a recursive delete of a configurable path is the one mistake here that cannot be
undone.

`source.v4l2` and every other live type have no cache, no checkbox and no indicator.
Their device key is a device node, and copying `/dev/video0` is not a thing that
finishes.
