# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Qt6 6.8 REQUIRED COMPONENTS Gui Multimedia Widgets WebEngineWidgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SOURCE_SWSCALE REQUIRED IMPORTED_TARGET libswscale libavformat libavcodec libavutil)
add_executable(munkicast-source main.cpp decklink.cpp)
target_include_directories(munkicast-source SYSTEM PRIVATE ../../third_party/decklink)
target_link_libraries(munkicast-source PRIVATE munkicast::flags munkicast::lcomp munkicast::lcapture
    Qt6::Gui Qt6::Multimedia Qt6::Widgets Qt6::WebEngineWidgets PkgConfig::SOURCE_SWSCALE ${CMAKE_DL_LIBS})
