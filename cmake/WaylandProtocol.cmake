# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates client-side glue for a Wayland protocol XML with wayland-scanner.
#
# The XML files are system data from wayland-protocols, not vendored: the versions a compositor
# actually implements are the ones shipped alongside it, and pinning our own copies would mean
# generating bindings for a protocol revision the compositor may not speak.

find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

# munkicast_wayland_protocol(<target> <relative/path/to.xml>)
#
# `path` is relative to the wayland-protocols data dir. Adds the generated .c and client header to
# `target` and puts the header on its include path.
function(munkicast_wayland_protocol target path)
    get_filename_component(basename "${path}" NAME_WE)
    set(xml "${WAYLAND_PROTOCOLS_DIR}/${path}")
    if(NOT EXISTS "${xml}")
        message(FATAL_ERROR "Wayland protocol XML not found: ${xml}")
    endif()
    set(outdir "${CMAKE_CURRENT_BINARY_DIR}/wl-protocols")
    set(header "${outdir}/${basename}-client-protocol.h")
    set(source "${outdir}/${basename}-protocol.c")
    file(MAKE_DIRECTORY "${outdir}")

    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${xml}" "${header}"
        DEPENDS "${xml}"
        COMMENT "wayland-scanner client-header ${basename}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${source}"
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${xml}" "${source}"
        DEPENDS "${xml}"
        COMMENT "wayland-scanner private-code ${basename}"
        VERBATIM)

    target_sources(${target} PRIVATE "${header}" "${source}")
    target_include_directories(${target} PRIVATE "${outdir}")
    # Generated C: never hold wayland-scanner's output to our own warning settings.
    set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-w")
endfunction()
