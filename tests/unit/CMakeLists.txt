# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# ltest.hpp is the whole harness; see the comment at the top of it for why there is no framework.
add_library(ltest INTERFACE)
add_library(munkicast::ltest ALIAS ltest)
target_include_directories(ltest INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ltest INTERFACE munkicast::flags)

function(munkicast_unit_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE munkicast::ltest ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

munkicast_unit_test(test_actions          munkicast::lsession)
munkicast_unit_test(test_capture          munkicast::lcapture)
munkicast_unit_test(test_video            munkicast::lcapture)
munkicast_unit_test(test_audio            munkicast::lcapture)
munkicast_unit_test(test_filters          munkicast::lcomp)
add_library(vst_fixture MODULE vst_fixture.cpp)
target_link_libraries(vst_fixture PRIVATE munkicast::flags)
target_compile_definitions(test_filters PRIVATE MUNKICAST_VST_FIXTURE="$<TARGET_FILE:vst_fixture>")
add_dependencies(test_filters vst_fixture)
munkicast_unit_test(test_source_worker    munkicast::lcomp munkicast::lcore)
target_sources(test_source_worker PRIVATE ../../apps/munkicast-render/source_worker.cpp)
target_compile_definitions(test_source_worker PRIVATE
    MUNKICAST_SOURCE_EXE="$<TARGET_FILE:munkicast-source>"
    MUNKICAST_IMAGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/images")
add_dependencies(test_source_worker munkicast-source)
munkicast_unit_test(test_command          munkicast::ldoc)
munkicast_unit_test(test_composite        munkicast::lcomp)
target_compile_definitions(test_composite PRIVATE
    MUNKICAST_IMAGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/images")
munkicast_unit_test(test_document         munkicast::ldoc)
munkicast_unit_test(test_dropzone        munkicast::llayout)
munkicast_unit_test(test_journal          munkicast::ldoc munkicast::lcore)
munkicast_unit_test(test_fuzz             munkicast::ldoc munkicast::llayout munkicast::lsession)
munkicast_unit_test(test_golden           munkicast::ldoc munkicast::lcore)
target_compile_definitions(test_golden PRIVATE
    MUNKICAST_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
munkicast_unit_test(test_ipc              munkicast::lipc)
munkicast_unit_test(test_json             munkicast::ldoc)
find_package(PkgConfig REQUIRED)
pkg_check_modules(RECORD_TEST_FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswscale)
munkicast_unit_test(test_preview         munkicast::lcomp munkicast::lcapture PkgConfig::RECORD_TEST_FFMPEG)
munkicast_unit_test(test_encode          munkicast::lencode PkgConfig::RECORD_TEST_FFMPEG)
target_link_options(test_encode PRIVATE "-Wl,--wrap=write")
find_program(RTMP_TEST_FFMPEG NAMES ffmpeg REQUIRED)
target_compile_definitions(test_preview PRIVATE
    MUNKICAST_TEST_FFMPEG="${RTMP_TEST_FFMPEG}"
    MUNKICAST_VST_FIXTURE="$<TARGET_FILE:vst_fixture>"
    MUNKICAST_RENDER="$<TARGET_FILE:munkicast-render>"
    MUNKICAST_IMAGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/images")
add_dependencies(test_preview munkicast-render vst_fixture)
set_tests_properties(test_preview PROPERTIES ENVIRONMENT "MUNKICAST_RENDER_BACKEND=cpu")
munkicast_unit_test(test_gpu_composite munkicast::lrhi munkicast::lencode PkgConfig::RECORD_TEST_FFMPEG)
target_compile_definitions(test_gpu_composite PRIVATE
    MUNKICAST_IMAGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/images")
set_tests_properties(test_gpu_composite PROPERTIES SKIP_RETURN_CODE 77)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GPU_TEST_SWSCALE REQUIRED IMPORTED_TARGET libswscale)
target_link_libraries(test_gpu_composite PRIVATE PkgConfig::GPU_TEST_SWSCALE)
munkicast_unit_test(test_project_io       munkicast::ldoc munkicast::lcore)
find_package(PkgConfig REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Gui)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
munkicast_unit_test(test_obs_import munkicast::limport munkicast::lcomp Qt6::Gui PkgConfig::JSONCPP)
target_compile_definitions(test_obs_import PRIVATE
    MUNKICAST_CLI="$<TARGET_FILE:munkicast>"
    MUNKICAST_IMAGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/images")
add_dependencies(test_obs_import munkicast)
# The asset cache's decision logic (ADR-0069). Only the free functions are exercised -- the
# recipe choice and the settings reading -- but the translation unit holds a QObject, so it
# needs moc and Qt's Qml module to link. The shell self-check covers everything that needs a
# running shell; this covers the branches a table of sizes can reach in milliseconds.
find_package(Qt6 REQUIRED COMPONENTS Gui Qml)
munkicast_unit_test(test_assetcache       munkicast::ldoc munkicast::lcore Qt6::Gui Qt6::Qml)
target_sources(test_assetcache PRIVATE
    ../../apps/munkicast-ui/assetcache.cpp
    ../../apps/munkicast-ui/preferences.cpp)
target_include_directories(test_assetcache PRIVATE ../../apps/munkicast-ui)
set_target_properties(test_assetcache PROPERTIES AUTOMOC ON)

munkicast_unit_test(test_layout          munkicast::llayout)
munkicast_unit_test(test_layout_history  munkicast::llayout)
munkicast_unit_test(test_layout_serialize munkicast::llayout munkicast::lcore)
munkicast_unit_test(test_session          munkicast::lsession)
munkicast_unit_test(test_theme            munkicast::ltheme munkicast::lcore)
munkicast_unit_test(test_workspaces       munkicast::llayout munkicast::lcore)
munkicast_unit_test(test_ulid             munkicast::lcore)

find_package(Qt6 REQUIRED COMPONENTS Gui Network Qml)
munkicast_unit_test(test_connectors Qt6::Gui Qt6::Network Qt6::Qml)
target_sources(test_connectors PRIVATE ../../apps/munkicast-ui/connectors.cpp)
target_include_directories(test_connectors PRIVATE ../../apps/munkicast-ui)
set_target_properties(test_connectors PROPERTIES AUTOMOC ON)
qt_add_resources(test_connectors connector_catalog_test PREFIX "/connectors"
    BASE ../../apps/munkicast-ui FILES ../../apps/munkicast-ui/data/obs-services.json)
