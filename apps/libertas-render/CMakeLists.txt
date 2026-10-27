# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The render process. It publishes a preview of each canvas, captures the screens that preview
# names, and records or streams one canvas to a configured destination.
# ADR-0005 puts the encoder here, and puts the GPU here, which is why the device lives in this
# process rather than in lcomp. ADR-0040 is why the preview is a separate process rather than a
# function the UI calls.

add_executable(libertas-render
    main.cpp
    source_worker.cpp
)
add_dependencies(libertas-render libertas-source)
target_compile_definitions(libertas-render PRIVATE LIBERTAS_SOURCE_EXE="$<TARGET_FILE:libertas-source>")

target_link_libraries(libertas-render PRIVATE
    libertas::flags
    libertas::lencode
    libertas::lcomp
    libertas::lcapture
    libertas::lrhi
    libertas::lcore
)
