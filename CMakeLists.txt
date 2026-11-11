# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.28...4.4)

project(MunkiCast
    VERSION 0.0.1
    DESCRIPTION "Live streaming and recording studio"
    HOMEPAGE_URL "https://github.com/calmasacow/MunkiCast"
    LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Keep build trees out of the source tree.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a build/ directory.")
endif()

option(MUNKICAST_WERROR     "Treat warnings as errors"            OFF)
option(MUNKICAST_SANITIZE   "Enable ASan+UBSan"                   OFF)
option(MUNKICAST_TSAN       "Enable ThreadSanitizer"              OFF)
option(MUNKICAST_BUILD_TOOLS "Build spikes and dev tools"         ON)
option(MUNKICAST_BUILD_TESTS "Build tests"                        ON)
set(MUNKICAST_DEPS "auto" CACHE STRING "Dependency strategy: system|vcpkg|auto")
option(MUNKICAST_USE_SYSTEM_FFMPEG "Use system FFmpeg instead of the pinned local build" OFF)

# FindPkgConfig caches results across environment changes. Switching engines must re-probe.
if(NOT "${MUNKICAST_CONFIGURED_SYSTEM_FFMPEG}" STREQUAL "${MUNKICAST_USE_SYSTEM_FFMPEG}")
    get_cmake_property(pkg_cache_variables CACHE_VARIABLES)
    foreach(variable IN LISTS pkg_cache_variables)
        if(variable MATCHES "^(__pkg_config_checked_|pkgcfg_lib_)")
            unset(${variable} CACHE)
        endif()
    endforeach()
    set(MUNKICAST_CONFIGURED_SYSTEM_FFMPEG "${MUNKICAST_USE_SYSTEM_FFMPEG}"
        CACHE INTERNAL "Last configured FFmpeg selection")
endif()

if(NOT MUNKICAST_USE_SYSTEM_FFMPEG)
    set(MUNKICAST_FFMPEG_PREFIX "${CMAKE_SOURCE_DIR}/build/ffmpeg-9.1/prefix")
    if(NOT EXISTS "${MUNKICAST_FFMPEG_PREFIX}/lib/pkgconfig/libavcodec.pc" OR
       NOT EXISTS "${MUNKICAST_FFMPEG_PREFIX}/bin/ffmpeg")
        message(FATAL_ERROR
            "Local FFmpeg 9.1 is missing. See docs/ffmpeg-9.1.md for setup, or configure "
            "with -DMUNKICAST_USE_SYSTEM_FFMPEG=ON to use system FFmpeg.")
    endif()
    set(ENV{PKG_CONFIG_PATH}
        "${MUNKICAST_FFMPEG_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    list(APPEND CMAKE_BUILD_RPATH "${MUNKICAST_FFMPEG_PREFIX}/lib")
endif()

include(MunkiCastFlags)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "Only Linux is supported at this stage. macOS and Windows arrive in Phase 3 (see the plan).")
endif()

if(MUNKICAST_BUILD_TESTS)
    enable_testing()
    if(NOT MUNKICAST_USE_SYSTEM_FFMPEG)
        add_test(NAME ffmpeg_selection COMMAND "${CMAKE_COMMAND}"
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DCHECK_DIR=${CMAKE_BINARY_DIR}/ffmpeg-selection-check
            -DGENERATOR=${CMAKE_GENERATOR}
            -P "${CMAKE_SOURCE_DIR}/tests/check_ffmpeg_selection.cmake")
    endif()
endif()

# Vendored Vulkan C headers: a distro update can remove vulkan-headers while keeping the loader.
# Set before any find_package(Vulkan), which then finds these instead of /usr/include.
set(Vulkan_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include"
    CACHE PATH "Vulkan headers (vendored)" FORCE)

add_subdirectory(libs/lcore)
add_subdirectory(libs/lipc)
add_subdirectory(libs/llayout)
add_subdirectory(libs/ldoc)
add_subdirectory(libs/lsession)
add_subdirectory(libs/ltheme)
add_subdirectory(libs/lcomp)
add_subdirectory(apps/munkicast-vst)
add_subdirectory(libs/limport)
add_subdirectory(libs/lcapture)
add_subdirectory(libs/lrhi)
add_subdirectory(libs/lencode)
add_subdirectory(apps/munkicast)
add_subdirectory(apps/munkicast-source)
add_subdirectory(apps/munkicast-render)
add_subdirectory(apps/munkicast-ui)

if(MUNKICAST_BUILD_TOOLS)
    add_subdirectory(tools/spike-core)
    add_subdirectory(tools/capture-spike)
    add_subdirectory(tools/encode-spike)
endif()

if(MUNKICAST_BUILD_TESTS)
    add_subdirectory(tests/unit)
endif()

message(STATUS "")
message(STATUS "MunkiCast ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  sanitizers : asan/ubsan=${MUNKICAST_SANITIZE} tsan=${MUNKICAST_TSAN}")
message(STATUS "")
