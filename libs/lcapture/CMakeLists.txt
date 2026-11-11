# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Screen capture: the ext-image-copy-capture-v1 backend, moved here from tools/spike-core after
# ADR-0011 measured it and ADR-0041 gave a screen source somewhere to name its output.
#
# No Vulkan here, and no allocator. The client allocates the buffers this backend hands the
# compositor (ADR-0011), but it does so on the other side of `DmabufImage` -- today a spike's
# Vulkan context, tomorrow `lrhi`. That keeps this library to the protocol and off the device,
# and it is why the target does not link Vulkan at all.
#
# The portal path stays the universal fallback and is not here yet; `linux-portal/` is where it
# goes.

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec libavutil)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)

add_library(lcapture STATIC
    src/video.cpp
    src/audio.cpp
    src/dmabuf.cpp
    src/screen.cpp
    src/wlcap.cpp
)
add_library(munkicast::lcapture ALIAS lcapture)

include(WaylandProtocol)
# ext-image-capture-source references ext_foreign_toplevel_handle_v1 for window sources, so that
# protocol is generated too - it is what window capture by identity will be built on.
munkicast_wayland_protocol(lcapture staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml)
munkicast_wayland_protocol(lcapture staging/ext-image-capture-source/ext-image-capture-source-v1.xml)
munkicast_wayland_protocol(lcapture staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml)
munkicast_wayland_protocol(lcapture stable/linux-dmabuf/linux-dmabuf-v1.xml)

target_include_directories(lcapture PUBLIC include)
target_link_libraries(lcapture
    PUBLIC munkicast::flags munkicast::lcore munkicast::lrhi
    PRIVATE PkgConfig::WAYLAND PkgConfig::SWSCALE PkgConfig::AVCODEC PkgConfig::PULSE
)
