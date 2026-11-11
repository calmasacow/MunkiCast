#!/usr/bin/env bash
# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="$root/build/ffmpeg-9.1/prefix"
app="$root/build/linux/apps/munkicast-ui/munkicast-ui"
if [[ ! -x "$prefix/bin/ffmpeg" || ! -x "$app" ]]; then
    echo "Missing local FFmpeg 9.1 or application build; see docs/ffmpeg-9.1.md" >&2
    exit 1
fi
export PATH="$prefix/bin:$PATH"
export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$app" "$@"
