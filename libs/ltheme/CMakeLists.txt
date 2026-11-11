# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Theme tokens and the Omarchy provider. No Qt: a palette is a set of named colours, and the
# mapping from a theme's tokens to our roles is exactly the part worth testing without a display.

find_package(tomlplusplus CONFIG REQUIRED)

add_library(ltheme STATIC
    src/omarchy.cpp
    src/palette.cpp
)
add_library(munkicast::ltheme ALIAS ltheme)

target_include_directories(ltheme PUBLIC include)
target_link_libraries(ltheme
    PUBLIC munkicast::flags
    PRIVATE tomlplusplus::tomlplusplus
)
