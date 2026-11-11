# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(munkicast-vst main.cpp)
target_link_libraries(munkicast-vst PRIVATE munkicast::flags ${CMAKE_DL_LIBS})
