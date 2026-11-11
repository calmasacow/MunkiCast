# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Capture and Vulkan-import code shared by the Phase-0 spikes.
#
# This is deliberately a spike-local library, not part of libs/. It is evidence-gathering code:
# the shapes it proves out graduate into libs/ once Phase 1 designs around what they taught.

find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(LIBDRM   REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(WAYLAND  REQUIRED IMPORTED_TARGET wayland-client)
find_package(Vulkan REQUIRED)

add_library(spike-core STATIC
    portal.cpp
    stream.cpp
    vk_convert.cpp
    v4l2cap.cpp
)

# BGRA->NV12 runs on the GPU so the readback carries 1.5 bytes/pixel and the CPU never sees a
# colour conversion (ADR-0019).
include(CompileShader)
munkicast_compile_shader(spike-core shaders/bgra_to_nv12.comp)

# ext-image-copy-capture-v1 is the primary Linux capture backend (ADR-0011); the portal path
# stays as the universal fallback.
include(WaylandProtocol)
# ext-image-capture-source references ext_foreign_toplevel_handle_v1 for window sources, so that
# protocol must be generated too - and it is what window capture by identity will be built on.
munkicast_wayland_protocol(spike-core staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml)
munkicast_wayland_protocol(spike-core staging/ext-image-capture-source/ext-image-capture-source-v1.xml)
munkicast_wayland_protocol(spike-core staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml)
munkicast_wayland_protocol(spike-core stable/linux-dmabuf/linux-dmabuf-v1.xml)

target_include_directories(spike-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The convert pass builds a compute pipeline on lrhi's VkCtx, so it needs that library's internal
# header. Reaching into another target's src/ is not a pattern to copy: it is here because the
# NV12 pass is still spike code (ADR-0019) and lrhi is deliberately not growing an encoder to
# hold it.
target_include_directories(spike-core PRIVATE ${CMAKE_SOURCE_DIR}/libs/lrhi/src)

target_link_libraries(spike-core PUBLIC
    munkicast::flags
    munkicast::lcore
    munkicast::lcapture
    munkicast::lrhi
    PkgConfig::GIO_UNIX
    PkgConfig::PIPEWIRE
    PkgConfig::LIBDRM
    PkgConfig::WAYLAND
    Vulkan::Vulkan
)

add_library(munkicast::spike-core ALIAS spike-core)
