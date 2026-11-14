# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compiles GLSL to SPIR-V and embeds it as a C array, so a binary carries its own shaders and
# there is no runtime file lookup to get wrong in a Flatpak, an AppImage or a dev tree.
#
# GLSL rather than Slang here on purpose: ADR-0004 chose Slang for *plugin-authored effects*,
# which is a stable text-format contract with third parties. These are our own internal pipeline
# shaders, compiled at build time by whatever we like, and glslc is already a hard dependency of
# the Vulkan SDK we require.

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

# munkicast_compile_shader(<target> <shader.comp> [NAME <name>] [DEFINES <d>...])
#
# Generates <name>_spv.h in the build tree declaring `kNameSpv`, and adds it to <target>.
# NAME overrides the symbol and header stem, so one source can be compiled more than once with
# different DEFINES -- which is how preview.comp yields both an 8-bit and a 16-bit store.
function(munkicast_compile_shader TARGET SHADER)
    cmake_parse_arguments(ARG "" "NAME" "DEFINES" ${ARGN})
    get_filename_component(_name "${SHADER}" NAME_WE)
    if(ARG_NAME)
        set(_name "${ARG_NAME}")
    endif()
    set(_definitions "")
    foreach(_d IN LISTS ARG_DEFINES)
        list(APPEND _definitions "-D${_d}")
    endforeach()
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}")
    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    set(_out "${_out_dir}/${_name}_spv.h")

    # Pick the symbol name from the file name: bgra_to_nv12 -> kBgraToNv12Spv
    string(REPLACE "_" ";" _parts "${_name}")
    set(_sym "k")
    foreach(_p IN LISTS _parts)
        string(SUBSTRING "${_p}" 0 1 _first)
        string(SUBSTRING "${_p}" 1 -1 _rest)
        string(TOUPPER "${_first}" _first)
        string(APPEND _sym "${_first}${_rest}")
    endforeach()
    string(APPEND _sym "Spv")

    file(MAKE_DIRECTORY "${_out_dir}")
    add_custom_command(
        OUTPUT "${_out}"
        # -mfmt=c emits a brace-enclosed initialiser list; the header wraps it in a definition.
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.3 -mfmt=c ${_definitions}
                "${_src}" -o "${_out}.inc"
        # Quote each -D as a whole argument: "-DOUT=${_out}", not -DOUT="${_out}". The latter
        # passes the quote characters through to CMake's option parser and the value arrives
        # mangled.
        COMMAND "${CMAKE_COMMAND}"
                "-DOUT=${_out}" "-DINC=${_out}.inc" "-DSYM=${_sym}"
                -P "${CMAKE_SOURCE_DIR}/cmake/WrapSpirv.cmake"
        DEPENDS "${_src}" "${CMAKE_SOURCE_DIR}/cmake/WrapSpirv.cmake"
        COMMENT "Compiling shader ${SHADER}"
        VERBATIM)

    target_sources(${TARGET} PRIVATE "${_out}")
    target_include_directories(${TARGET} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
