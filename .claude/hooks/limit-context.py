#!/usr/bin/env python3
# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Refuse Read calls that would pin a large blob into the Claude Code transcript.

Widget work was costing ~500K tokens a turn because full-window screenshots and
whole copies of main.cpp / document.cpp stayed in the session and were resent
on every later request. This hook runs before Read.
"""

import json
import subprocess
import sys
from pathlib import Path

MAX_TEXT_BYTES = 40_000
MAX_READ_LINES = 200
MAX_IMAGE_BYTES = 80_000
MAX_IMAGE_EDGE = 1280
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".tif", ".tiff"}


def emit(decision, reason):
    json.dump(
        {
            "hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": decision,
                "permissionDecisionReason": reason,
            }
        },
        sys.stdout,
    )
    sys.stdout.write("\n")


def deny(reason):
    emit("deny", reason)
    raise SystemExit(0)


def as_int(value):
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.strip().lstrip("-").isdigit():
        return int(value.strip())
    return None


def image_edge(path):
    try:
        probe = subprocess.run(
            ["identify", "-format", "%w %h", str(path)],
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if probe.returncode != 0:
        return None
    parts = probe.stdout.split()
    if len(parts) < 2:
        return None
    try:
        return max(int(parts[0]), int(parts[1]))
    except ValueError:
        return None


def main():
    try:
        data = json.load(sys.stdin)
    except json.JSONDecodeError:
        return
    if data.get("tool_name") != "Read":
        return

    tool_input = data.get("tool_input") or {}
    raw = tool_input.get("file_path") or tool_input.get("path") or tool_input.get("target_file")
    if not isinstance(raw, str) or not raw.strip():
        return

    path = Path(raw).expanduser()
    parts = path.parts
    if "build" in parts or any(part.startswith("build-") for part in parts):
        deny(
            f"Refusing to read {path}. build/ is generated output. "
            "Read the source under apps/ or libs/, or tail a log."
        )

    suffix = path.suffix.lower()
    if suffix in IMAGE_SUFFIXES:
        if not path.is_file():
            return
        size = path.stat().st_size
        edge = image_edge(path)
        too_heavy = size > MAX_IMAGE_BYTES
        too_wide = edge is not None and edge > MAX_IMAGE_EDGE
        if too_heavy or too_wide:
            detail = f"{size} bytes"
            if edge is not None:
                detail += f", longest edge {edge}px"
            deny(
                f"Refusing to read {path} ({detail}). "
                "A full capture stays in the transcript and is resent on every later turn. "
                "Scale it first: magick INPUT -resize '1280x1280>' -quality 60 /tmp/munkicast-view.jpg "
                "and Read that file once."
            )
        return

    if not path.is_file():
        return
    size = path.stat().st_size
    if size <= MAX_TEXT_BYTES:
        return

    limit = as_int(tool_input.get("limit"))
    offset = as_int(tool_input.get("offset"))
    ranged = limit is not None and 1 <= limit <= MAX_READ_LINES and (offset is None or offset >= 0)
    if ranged:
        return

    deny(
        f"Refusing to read {path} whole ({size} bytes). "
        f"Read a range: offset plus limit of at most {MAX_READ_LINES} lines. "
        "For a panel widget, start at apps/munkicast-ui/qml/<Name>Panel.qml."
    )


if __name__ == "__main__":
    main()
