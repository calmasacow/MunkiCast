# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The workspace layout tree (ADR-0015) and the Adobe drop-zone docking model (ADR-0016).
#
# Deliberately free of Qt. The layout engine is the piece most likely to be driven into states no
# hand-written case would reach, so it has to be testable headlessly, at thousands of gestures a
# second, with no compositor and no window on anybody's screen.

find_package(tomlplusplus CONFIG REQUIRED)

add_library(llayout STATIC
    src/dropzone.cpp
    src/history.cpp
    src/layout.cpp
    src/serialize.cpp
    src/workspaces.cpp
)
add_library(munkicast::llayout ALIAS llayout)

target_include_directories(llayout PUBLIC include)
target_link_libraries(llayout
    PUBLIC munkicast::flags
    PRIVATE munkicast::lcore tomlplusplus::tomlplusplus
)
