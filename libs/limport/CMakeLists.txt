# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Qt6 REQUIRED COMPONENTS Gui)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
pkg_check_modules(IMPORT_AVFORMAT REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
add_library(limport STATIC src/obs.cpp src/obs_export.cpp)
add_library(munkicast::limport ALIAS limport)
target_include_directories(limport PUBLIC include)
target_link_libraries(limport PUBLIC munkicast::ldoc PRIVATE munkicast::lcomp munkicast::lcore Qt6::Gui PkgConfig::JSONCPP PkgConfig::IMPORT_AVFORMAT)
