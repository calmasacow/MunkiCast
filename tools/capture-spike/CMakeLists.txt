# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Phase-0 capture spike: proves the capture half end to end (ADR-0009 through ADR-0014).

add_executable(capture-spike main.cpp)
target_link_libraries(capture-spike PRIVATE munkicast::spike-core)
