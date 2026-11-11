#!/usr/bin/env python3
# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native Wayland/Vulkan check; opens an isolated test window for two minutes.

Usage: python3 tests/check_browser_dock_vulkan.py UI_EXECUTABLE NEW_OUTPUT_DIRECTORY
Requires access to the kernel journal for NVIDIA fault detection.
"""
import http.server
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time

assert os.environ.get("WAYLAND_DISPLAY"), "A Wayland session is required"
binary = Path(sys.argv[1]).resolve()
output = Path(sys.argv[2]).resolve()
output.mkdir(parents=True, exist_ok=False)
ticks = []


class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/tick"):
            ticks.append(time.monotonic())
        body = b"""<html><title>Browser GPU regression check</title>
        <body style="background:#234;color:white"><h1>Browser animation test</h1>
        <canvas id="paint" width="1280" height="720"></canvas><script>
        const context = document.getElementById('paint').getContext('2d');
        let frame = 0;
        function draw() {
            context.fillStyle = `hsl(${frame % 360},60%,40%)`;
            context.fillRect(0, 0, 1280, 720);
            if (++frame % 30 === 0) fetch('/tick?frame=' + frame);
            requestAnimationFrame(draw);
        }
        requestAnimationFrame(draw);
        </script></body></html>"""
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Page)
threading.Thread(target=server.serve_forever, daemon=True).start()
dock = "browser:11111111-1111-4111-8111-111111111111"
layout = output / "layout.toml"
layout.write_text("version = 3\nactive = 0\n" + "".join(
    f'[[workspace]]\nname = "{name}"\nbuiltin = true\nroot = 1\n'
    f'[[workspace.node]]\nid = 1\nkind = "tabs"\npanels = ["{dock}"]\nactive = 0\n'
    for name in ("Build", "Live", "Encode")))
Path(str(layout) + ".browser-docks.json").write_text(json.dumps([
    {"id": dock, "name": "GPU regression check", "url": f"http://127.0.0.1:{server.server_port}/"}]))
env = dict(os.environ, QT_QPA_PLATFORM="wayland")
for key, directory in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                       ("XDG_CACHE_HOME", "cache")):
    env[key] = str(output / directory)
started = time.time()
with (output / "ui.log").open("w") as log:
    app = subprocess.Popen([str(binary), "--layout", str(layout)], env=env, start_new_session=True,
                           stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            assert app.poll() is None, "Test application exited; inspect ui.log"
            time.sleep(1)
    finally:
        # Only this isolated test session; no operator project is open.
        try:
            os.killpg(app.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(app.pid, signal.SIGKILL)
            app.wait()
        server.shutdown()
journal = subprocess.check_output([
    "journalctl", "-k", "-b", "--since", f"@{int(started)}", "--no-pager"], text=True)
faults = [line for line in journal.splitlines() if "NVRM: Xid" in line and f"pid={app.pid}," in line]
(output / "gpu-faults.log").write_text("\n".join(faults))
assert not faults, f"NVIDIA faults from test UI: {faults}"
assert len(ticks) >= 60, f"Browser animation stalled: {len(ticks)} heartbeats"
assert max(b - a for a, b in zip(ticks, ticks[1:])) < 5, "Browser stalled for over five seconds"
print(f"PASS: native Vulkan browser, {len(ticks)} heartbeats, no NVIDIA faults; {output}")
