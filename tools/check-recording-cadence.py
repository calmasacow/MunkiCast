#!/usr/bin/env python3
# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record an isolated saved show; assert capture sampling and output keep their cadence.

Requires live matching-rate V4L2 input in Program. Close the operator app first.
Writes only a new temporary directory; never saves changes to the supplied project.
"""

import argparse
import array
import collections
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("project", type=Path)
parser.add_argument("--scene", help="Main scene to select in the temporary snapshot")
parser.add_argument("--source", help="Assert cadence for this exact source name; report all")
parser.add_argument("--projector", action="store_true", help="Also open a windowed Program projector")
parser.add_argument("--renderer", type=Path, default=Path(
    __file__).resolve().parents[1] / "build-make/apps/munkicast-render/munkicast-render")
parser.add_argument("--seconds", type=int, choices=range(5, 121), default=12, metavar="5..120")
args = parser.parse_args()
project = json.loads((args.project / "project.json").read_text())
canvas = project["canvases"][0]
scenes = [path.read_bytes() for path in (args.project / "scenes").glob("*.json")]
if args.scene:
    scene = next(s for s in map(json.loads, scenes)
                 if s["name"] == args.scene and s["canvas"] == canvas["id"])
    for deck in project["decks"]:
        if deck["canvas"] == canvas["id"]:
            deck["program_scene"] = scene["id"]
encode = next(e for e in project["encodes"] if e["canvas"] == canvas["id"])
fps = canvas["fps"]["num"] / canvas["fps"]["den"]
assert fps > 0
directory = Path(tempfile.mkdtemp(prefix="munkicast-cadence-"))
print(f"Diagnostic recording and log: {directory}", flush=True)
client, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
child.setblocking(False)
client.settimeout(30)
log = (directory / "renderer.log").open("w")
env = dict(os.environ, MUNKICAST_CHANNEL_FD=str(child.fileno()),
           MUNKICAST_RENDER_BACKEND="gpu", MUNKICAST_TRACE_SAMPLING="1")
env.pop("MUNKICAST_TRACE_OUTPUT", None)  # Pixel hashes add work this cadence check does not use.
process = subprocess.Popen([str(args.renderer.resolve())], env=env,
                           pass_fds=[child.fileno()], stdout=log, stderr=log)
child.close()


def receive():
    data, ancillary, flags, _ = client.recvmsg(65536, socket.CMSG_SPACE(64))
    for level, kind, body in ancillary:
        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
            fds = array.array("i")
            fds.frombytes(body)
            for fd in fds:
                os.close(fd)
    assert not flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC), "Truncated IPC reply"
    assert data, "Renderer exited"
    assert data[0] != 8, f"Renderer fault: {data[1:].decode(errors='replace')}"
    return data


def send_document(kind, data):
    fd = os.memfd_create("cadence-project", os.MFD_CLOEXEC)
    try:
        assert os.write(fd, data) == len(data)
        client.sendmsg([struct.pack("<BI", kind, len(data))],
                       [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [fd]))])
    finally:
        os.close(fd)


def refresh():
    client.send(struct.pack("<BI", 4, 960))
    while receive()[0] != 7:
        pass


def record(start):
    path = str(directory / "recording.mkv").encode() if start else b""
    client.send(bytes([11, int(start)]) + encode["id"].encode() + b"0" * 26 + path)
    while True:
        data = receive()
        if data[0] == 12 and data[26] & 4:  # Explicit control reply, not preview statistics.
            clip_size = struct.unpack_from("<I", data, 43)[0]
            error = data[47 + clip_size:].decode()
            assert not error, error
            assert bool(data[1]) == start
            return struct.unpack_from("<QQQ", data, 2)


try:
    assert receive() == bytes([1, 12, 0]), "Update this check for the changed IPC version"
    send_document(2, json.dumps(project).encode())
    for scene in scenes:
        send_document(3, scene)
    refresh()
    until = time.monotonic() + 8  # Allow media/camera/cache startup outside the measurement.
    while time.monotonic() < until:
        refresh()
        time.sleep(1 / fps)
    if args.projector:
        client.send(bytes([14]) + json.dumps(dict(
            id=1, canvas=canvas["id"], preview=False, fullscreen=False, screen="")).encode())
        while True:
            reply = receive()
            if reply[0] == 15:
                status = json.loads(reply[1:])
                assert not status.get("error"), status
                assert status.get("windows"), status
                break
    record(True)
    until = time.monotonic() + args.seconds
    tick = time.monotonic()
    while time.monotonic() < until:
        refresh()
        tick += 1 / fps
        time.sleep(max(0, tick - time.monotonic()))
    if args.projector:
        client.send(bytes([14]) + b'{"status":true}')
        while True:
            reply = receive()
            if reply[0] == 15:
                status = json.loads(reply[1:])
                assert not status.get("error"), status
                windows = status.get("windows", [])
                assert windows and all(w["ready"] and not w["error"] for w in windows), status
                print("Projector:", windows, flush=True)
                break
        # Close before encoder flush, keeping subsequent projector-only samples out of
        # the recording measurement while still checking real presentation above.
        client.send(bytes([14]) + b'{"closeAll":true}')
        while receive()[0] != 15:
            pass
    frames, missed, lag = record(False)
finally:
    try:
        client.send(bytes([5]))
    except OSError:
        pass
    client.close()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=5)
    log.close()

assert process.returncode == 0, f"Renderer exit: {process.returncode}"
trace = (directory / "renderer.log").read_text()
marker = trace.find("info  recording ")
assert marker >= 0, "Recording never started"
trace = trace[marker:]
samples = collections.defaultdict(collections.Counter)
for source, advance in re.findall(r"CAPTURE_SAMPLE source=(.*?) sequence=\d+ advance=(\d+)", trace):
    samples[source][int(advance)] += 1
assert samples, "Program has no live V4L2 capture; this check needs matching-rate input"
for source, counts in samples.items():
    print(f"{source}: {dict(counts)} (capture sequence advances per output tick)")
print(f"Encoded: {frames}; missed output slots: {missed}; encoding lag: {lag}")
assert not args.source or args.source in samples, f"Source not sampled: {args.source}"
for source, counts in samples.items():
    if not args.source or source == args.source:
        assert counts[0] == 0, f"{source}: {counts[0]} stale input samples; inspect renderer.log"
assert frames >= args.seconds * fps * 0.97, "Output cadence fell below 97%"
assert missed == 0, f"Missed {missed} output slots"
