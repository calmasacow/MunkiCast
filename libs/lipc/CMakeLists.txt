# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The transport between our processes (ADR-0005).
#
# Bytes and file descriptors, and nothing about what either means. The protocol belongs to the
# processes that speak it, so that changing the protocol does not mean changing the transport --
# and so that this layer can be tested without a renderer, a GPU or a display.

add_library(lipc STATIC
    src/channel.cpp
    src/fd.cpp
)
add_library(munkicast::lipc ALIAS lipc)

target_include_directories(lipc PUBLIC include)
target_link_libraries(lipc PUBLIC munkicast::flags)
