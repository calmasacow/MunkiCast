# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Phase-0 go/no-go gate: capture -> colour convert -> encode -> mux, with synced audio.
# See docs/adr/0006-nvenc-interop-path.md for the encoder ladder this walks.

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswscale libswresample)

add_executable(encode-spike
    main.cpp
    audio.cpp
)

# The encoder itself moved to libs/lencode when the shell grew a use for it. The spike keeps
# working off the same code rather than a copy that would drift.
target_link_libraries(encode-spike PRIVATE
    munkicast::spike-core
    munkicast::lencode
    PkgConfig::FFMPEG
)

add_executable(munkicast-encode-bench benchmark.cpp)
target_link_libraries(munkicast-encode-bench PRIVATE munkicast::lencode munkicast::lcore PkgConfig::FFMPEG)
