# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Reuse one build directory: fresh configurations cannot catch stale pkg-config results.
foreach(system_ffmpeg ON OFF ON)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${CHECK_DIR}"
                -G "${GENERATOR}" -DMUNKICAST_USE_SYSTEM_FFMPEG=${system_ffmpeg}
                -DMUNKICAST_BUILD_TESTS=OFF -DMUNKICAST_BUILD_TOOLS=OFF
        RESULT_VARIABLE outcome OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT outcome EQUAL 0)
        message(FATAL_ERROR "FFmpeg selection configure failed: ${output}\n${errors}")
    endif()
    file(STRINGS "${CHECK_DIR}/CMakeCache.txt" directories
        REGEX "^pkgcfg_lib_FFMPEG_(avcodec|avformat|avutil|swscale|swresample):FILEPATH=")
    list(LENGTH directories count)
    if(NOT count EQUAL 5)
        message(FATAL_ERROR "Missing FFmpeg library directories in cache")
    endif()
    foreach(directory IN LISTS directories)
        string(REGEX REPLACE "^[^=]*=" "" directory "${directory}")
        get_filename_component(directory "${directory}" DIRECTORY)
        if(directory STREQUAL "${SOURCE_DIR}/build/ffmpeg-9.1/prefix/lib")
            if(system_ffmpeg)
                message(FATAL_ERROR "System selection kept the local FFmpeg cache")
            endif()
        elseif(NOT system_ffmpeg)
            message(FATAL_ERROR "Default selection kept system FFmpeg: ${directory}")
        endif()
    endforeach()
endforeach()
