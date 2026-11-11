# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The boundary between whoever edits a document and whoever owns it.
#
# Not in the plan's library list, which goes straight from ldoc to the munkicast-session process.
# It is here because the interface has to exist before the process does, or the UI gets written
# against a document it holds directly and the split becomes a rewrite rather than a change of
# transport. LocalSession implements it in-process today; an lipc proxy implements it later.

find_package(tomlplusplus CONFIG REQUIRED)

add_library(lsession STATIC
    src/actions.cpp
    src/local_session.cpp
)
add_library(munkicast::lsession ALIAS lsession)

target_include_directories(lsession PUBLIC include)
target_link_libraries(lsession
    PUBLIC munkicast::flags munkicast::ldoc
    PRIVATE tomlplusplus::tomlplusplus
)
