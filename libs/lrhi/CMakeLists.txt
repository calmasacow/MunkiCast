# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Vulkan device: allocation, dmabuf export and import, and readback to host memory.
#
# Moved here from tools/spike-core after ADR-0011 and ADR-0019 measured it, because the render
# process needs a device and ADR-0005 always said that is where the GPU lives. Not rewritten:
# the numbers in those ADRs describe this code.
#
# It owns a device and nothing above it. No scene walk, no capture protocol, no encoder -- a
# caller hands it an image to allocate or a frame to read back. The NV12 convert pass stays in
# the encode spike and hooks in through `VkCtx::on_destroy`, which is what lets this library be
# that spike's dependency rather than its dependent.

find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_library(lrhi STATIC
    src/vulkan.cpp
    src/composite.cpp
)
include(CompileShader)
munkicast_compile_shader(lrhi shaders/composite.comp)
munkicast_compile_shader(lrhi shaders/preview.comp)
munkicast_compile_shader(lrhi shaders/preview.comp NAME preview_wide DEFINES WIDE)
munkicast_compile_shader(lrhi shaders/filter.comp)
munkicast_compile_shader(lrhi shaders/yuv.comp)
munkicast_compile_shader(lrhi shaders/nv12.comp)
add_library(munkicast::lrhi ALIAS lrhi)

target_include_directories(lrhi
    PUBLIC include
    # vk_ctx_internal.hpp is shared with the encode spike's convert pass, which builds a pipeline
    # on this context. Deliberately not in include/: a caller outside gets an opaque VkCtx.
    PRIVATE src
)
target_link_libraries(lrhi
    PUBLIC munkicast::flags munkicast::lcapture munkicast::lcomp
    PRIVATE munkicast::lcore Vulkan::Vulkan PkgConfig::LIBDRM
)
