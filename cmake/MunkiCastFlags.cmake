# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shared compile/link settings, exposed as the INTERFACE target munkicast::flags.

add_library(munkicast_flags INTERFACE)
add_library(munkicast::flags ALIAS munkicast_flags)

# -Wnon-virtual-dtor and -Woverloaded-virtual are C++-only, and generated Wayland protocol code
# is C: without this guard every one of those .c files compiles with a cc1 "valid for C++ but not
# for C" warning, which is noise in every build log on every machine.
target_compile_options(munkicast_flags INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
)

target_compile_options(munkicast_flags INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow
    -Wcast-align
    -Wunused
    -Wconversion -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    # Crash reports must be symbolisable: see ADR-0005.
    -fno-omit-frame-pointer
    -g
)

if(MUNKICAST_WERROR)
    target_compile_options(munkicast_flags INTERFACE -Werror)
endif()

# No exceptions cross an IPC boundary (ADR-0005). We still use them in-process, but every
# worker_main() and message handler wraps its body in a catch-all.

if(MUNKICAST_SANITIZE AND MUNKICAST_TSAN)
    message(FATAL_ERROR "MUNKICAST_SANITIZE and MUNKICAST_TSAN are mutually exclusive")
endif()

if(MUNKICAST_SANITIZE)
    target_compile_options(munkicast_flags INTERFACE
        -fsanitize=address,undefined -fno-sanitize-recover=all)
    target_link_options(munkicast_flags INTERFACE -fsanitize=address,undefined)
endif()

if(MUNKICAST_TSAN)
    target_compile_options(munkicast_flags INTERFACE -fsanitize=thread)
    target_link_options(munkicast_flags INTERFACE -fsanitize=thread)
endif()

# Split debug info keeps binaries small while staying debuggable with systemd-coredump.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-gsplit-dwarf" MUNKICAST_HAS_SPLIT_DWARF)
if(MUNKICAST_HAS_SPLIT_DWARF AND CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(munkicast_flags INTERFACE -gsplit-dwarf)
endif()

target_link_options(munkicast_flags INTERFACE
    LINKER:--build-id
    LINKER:--as-needed
)
