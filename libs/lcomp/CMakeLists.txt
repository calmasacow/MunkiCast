# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The scene walk and the preview that carries its picture to another process.
#
# No Qt, no GPU, no capture. A colour, an image (PNG, JPEG, or WebP), a group and a precomp can
# be drawn here, and everything else is reported as a gap rather than approximated (ADR-0040).
# The encode target is still a Vulkan composite in the render process; this library does not
# own a device.

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPNG  REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(LIBJPEG REQUIRED IMPORTED_TARGET libjpeg)
pkg_check_modules(LIBWEBP REQUIRED IMPORTED_TARGET libwebp)
pkg_check_modules(RNNOISE REQUIRED IMPORTED_TARGET rnnoise)
pkg_check_modules(SPEEXDSP REQUIRED IMPORTED_TARGET speexdsp)

add_library(lcomp STATIC
    src/composite.cpp
    src/filters.cpp
    src/lut.cpp
    src/audio_filters.cpp
    src/vst.cpp
    src/image.cpp
    src/preview.cpp
)
add_library(munkicast::lcomp ALIAS lcomp)
target_compile_definitions(lcomp PRIVATE MUNKICAST_VST_EXE="$<TARGET_FILE:munkicast-vst>")
add_dependencies(lcomp munkicast-vst)

target_include_directories(lcomp PUBLIC include)
target_link_libraries(lcomp
    PUBLIC munkicast::flags munkicast::ldoc munkicast::lipc
    PRIVATE PkgConfig::LIBPNG PkgConfig::LIBJPEG PkgConfig::LIBWEBP
            PkgConfig::RNNOISE PkgConfig::SPEEXDSP
)
