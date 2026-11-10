# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# ltest.hpp is the whole harness; see the comment at the top of it for why there is no framework.
add_library(ltest INTERFACE)
add_library(libertas::ltest ALIAS ltest)
target_include_directories(ltest INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ltest INTERFACE libertas::flags)

function(libertas_unit_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE libertas::ltest ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

libertas_unit_test(test_actions          libertas::lsession)
libertas_unit_test(test_capture          libertas::lcapture)
libertas_unit_test(test_video            libertas::lcapture)
libertas_unit_test(test_audio            libertas::lcapture)
libertas_unit_test(test_filters          libertas::lcomp)
add_library(vst_fixture MODULE vst_fixture.cpp)
target_link_libraries(vst_fixture PRIVATE libertas::flags)
target_compile_definitions(test_filters PRIVATE LIBERTAS_VST_FIXTURE="$<TARGET_FILE:vst_fixture>")
add_dependencies(test_filters vst_fixture)
libertas_unit_test(test_source_worker    libertas::lcomp libertas::lcore)
target_sources(test_source_worker PRIVATE ../../apps/libertas-render/source_worker.cpp)
target_compile_definitions(test_source_worker PRIVATE
    LIBERTAS_SOURCE_EXE="$<TARGET_FILE:libertas-source>"
    LIBERTAS_IMAGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/images")
add_dependencies(test_source_worker libertas-source)
libertas_unit_test(test_command          libertas::ldoc)
libertas_unit_test(test_composite        libertas::lcomp)
target_compile_definitions(test_composite PRIVATE
    LIBERTAS_IMAGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/images")
libertas_unit_test(test_document         libertas::ldoc)
libertas_unit_test(test_dropzone        libertas::llayout)
libertas_unit_test(test_journal          libertas::ldoc libertas::lcore)
libertas_unit_test(test_fuzz             libertas::ldoc libertas::llayout libertas::lsession)
libertas_unit_test(test_golden           libertas::ldoc libertas::lcore)
target_compile_definitions(test_golden PRIVATE
    LIBERTAS_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
libertas_unit_test(test_ipc              libertas::lipc)
libertas_unit_test(test_json             libertas::ldoc)
find_package(PkgConfig REQUIRED)
pkg_check_modules(RECORD_TEST_FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswscale)
libertas_unit_test(test_preview         libertas::lcomp libertas::lcapture PkgConfig::RECORD_TEST_FFMPEG)
libertas_unit_test(test_encode          libertas::lencode PkgConfig::RECORD_TEST_FFMPEG)
target_link_options(test_encode PRIVATE "-Wl,--wrap=write")
find_program(RTMP_TEST_FFMPEG NAMES ffmpeg REQUIRED)
target_compile_definitions(test_preview PRIVATE
    LIBERTAS_TEST_FFMPEG="${RTMP_TEST_FFMPEG}"
    LIBERTAS_VST_FIXTURE="$<TARGET_FILE:vst_fixture>"
    LIBERTAS_RENDER="$<TARGET_FILE:libertas-render>"
    LIBERTAS_IMAGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/images")
add_dependencies(test_preview libertas-render vst_fixture)
set_tests_properties(test_preview PROPERTIES ENVIRONMENT "LIBERTAS_RENDER_BACKEND=cpu")
libertas_unit_test(test_gpu_composite libertas::lrhi)
target_compile_definitions(test_gpu_composite PRIVATE
    LIBERTAS_IMAGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/images")
set_tests_properties(test_gpu_composite PROPERTIES SKIP_RETURN_CODE 77)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GPU_TEST_SWSCALE REQUIRED IMPORTED_TARGET libswscale)
target_link_libraries(test_gpu_composite PRIVATE PkgConfig::GPU_TEST_SWSCALE)
libertas_unit_test(test_project_io       libertas::ldoc libertas::lcore)
find_package(PkgConfig REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Gui)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
libertas_unit_test(test_obs_import libertas::limport libertas::lcomp Qt6::Gui PkgConfig::JSONCPP)
target_compile_definitions(test_obs_import PRIVATE
    LIBERTAS_CLI="$<TARGET_FILE:libertas>"
    LIBERTAS_IMAGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/images")
add_dependencies(test_obs_import libertas)
# The asset cache's decision logic (ADR-0069). Only the free functions are exercised -- the
# recipe choice and the settings reading -- but the translation unit holds a QObject, so it
# needs moc and Qt's Qml module to link. The shell self-check covers everything that needs a
# running shell; this covers the branches a table of sizes can reach in milliseconds.
find_package(Qt6 REQUIRED COMPONENTS Gui Qml)
libertas_unit_test(test_assetcache       libertas::ldoc libertas::lcore Qt6::Gui Qt6::Qml)
target_sources(test_assetcache PRIVATE
    ../../apps/libertas-ui/assetcache.cpp
    ../../apps/libertas-ui/preferences.cpp)
target_include_directories(test_assetcache PRIVATE ../../apps/libertas-ui)
set_target_properties(test_assetcache PROPERTIES AUTOMOC ON)

libertas_unit_test(test_layout          libertas::llayout)
libertas_unit_test(test_layout_history  libertas::llayout)
libertas_unit_test(test_layout_serialize libertas::llayout libertas::lcore)
libertas_unit_test(test_session          libertas::lsession)
libertas_unit_test(test_theme            libertas::ltheme libertas::lcore)
libertas_unit_test(test_workspaces       libertas::llayout libertas::lcore)
libertas_unit_test(test_ulid             libertas::lcore)

find_package(Qt6 REQUIRED COMPONENTS Gui Network Qml)
libertas_unit_test(test_connectors Qt6::Gui Qt6::Network Qt6::Qml)
target_sources(test_connectors PRIVATE ../../apps/libertas-ui/connectors.cpp)
target_include_directories(test_connectors PRIVATE ../../apps/libertas-ui)
set_target_properties(test_connectors PROPERTIES AUTOMOC ON)
qt_add_resources(test_connectors connector_catalog_test PREFIX "/connectors"
    BASE ../../apps/libertas-ui FILES ../../apps/libertas-ui/data/obs-services.json)
