# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(lcore STATIC
    src/atomic_file.cpp
    src/log.cpp
    src/ulid.cpp
)
add_library(munkicast::lcore ALIAS lcore)

target_include_directories(lcore PUBLIC include)
target_link_libraries(lcore PUBLIC munkicast::flags)
