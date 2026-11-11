# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The document model: the non-OBS-shaped scene/node/source/bus/canvas model, shared by the UI, the
# session process and the OBS importer. No Qt, no GPU, no FFmpeg -- the model has to be usable
# from a headless importer and testable without any of them.

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)

add_library(ldoc STATIC
    src/command.cpp
    src/command_json.cpp
    src/document.cpp
    src/history.cpp
    src/journal.cpp
    src/json.cpp
    src/migrate.cpp
    src/project_io.cpp
    src/node.cpp
    src/types.cpp
)
add_library(munkicast::ldoc ALIAS ldoc)

target_include_directories(ldoc PUBLIC include)
target_link_libraries(ldoc
    PUBLIC munkicast::flags munkicast::lcore
    PRIVATE PkgConfig::JSONCPP
)
