# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The render process. It publishes a preview of each canvas, captures the screens that preview
# names, and records or streams one canvas to a configured destination.
# ADR-0005 puts the encoder here, and puts the GPU here, which is why the device lives in this
# process rather than in lcomp. ADR-0040 is why the preview is a separate process rather than a
# function the UI calls.

add_executable(munkicast-render
    main.cpp
    source_worker.cpp
    projectors.cpp
)
add_dependencies(munkicast-render munkicast-source)
target_compile_definitions(munkicast-render PRIVATE MUNKICAST_SOURCE_EXE="$<TARGET_FILE:munkicast-source>")

target_link_libraries(munkicast-render PRIVATE
    munkicast::flags
    munkicast::lencode
    munkicast::lcomp
    munkicast::lcapture
    munkicast::lrhi
    munkicast::lcore
)

# linux-dmabuf glue is already linked through lcapture. Share its generated header,
# rather than compiling a second definition of the protocol's interface symbols.
target_include_directories(munkicast-render PRIVATE "${PROJECT_BINARY_DIR}/libs/lcapture/wl-protocols")
include(WaylandProtocol)
munkicast_wayland_protocol(munkicast-render stable/xdg-shell/xdg-shell.xml)
munkicast_wayland_protocol(munkicast-render stable/viewporter/viewporter.xml)
munkicast_wayland_protocol(munkicast-render stable/presentation-time/presentation-time.xml)
munkicast_wayland_protocol(munkicast-render unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)
pkg_check_modules(PROJECTOR_WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(PROJECTOR_DRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(PROJECTOR_JSON REQUIRED IMPORTED_TARGET jsoncpp)
target_link_libraries(munkicast-render PRIVATE PkgConfig::PROJECTOR_WAYLAND PkgConfig::PROJECTOR_JSON PkgConfig::PROJECTOR_DRM)
