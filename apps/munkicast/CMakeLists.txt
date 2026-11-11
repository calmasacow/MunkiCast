# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM   REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(GIO      REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(AVCODEC  REQUIRED IMPORTED_TARGET libavcodec libavutil)
find_package(Vulkan REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Gui)

add_executable(munkicast
    inspect.cpp
    main.cpp
    probe.cpp
)

target_compile_definitions(munkicast PRIVATE MUNKICAST_VERSION="${PROJECT_VERSION}")

target_link_libraries(munkicast PRIVATE
    munkicast::flags
    munkicast::lcore
    munkicast::ldoc
    munkicast::limport
    Qt6::Gui
    Vulkan::Vulkan
    PkgConfig::LIBDRM
    PkgConfig::PIPEWIRE
    PkgConfig::GIO
    PkgConfig::AVCODEC
)

install(TARGETS munkicast RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(MUNKICAST_BUILD_TESTS)
    # The inspector reads the committed golden project, which is the only test input in the tree
    # guaranteed to stay valid: if the format changes without the fixture being regenerated, this
    # fails alongside the golden tests rather than quietly reporting nonsense.
    add_test(NAME inspect_golden_project
             COMMAND munkicast project
                     "${CMAKE_SOURCE_DIR}/tests/unit/fixtures/golden-project-v3" --verbose)
    set_tests_properties(inspect_golden_project PROPERTIES
        TIMEOUT 30
        PASS_REGULAR_EXPRESSION "emergency scene: Emergency"
        FAIL_REGULAR_EXPRESSION "error "
    )

    # And the previous format, which must be migrated on the way in rather than reported as a
    # project full of errors. A reader that cannot read last month's project is not a reader.
    add_test(NAME inspect_migrates_an_older_project
             COMMAND munkicast project
                     "${CMAKE_SOURCE_DIR}/tests/unit/fixtures/golden-project-v1" --verbose)
    set_tests_properties(inspect_migrates_an_older_project PROPERTIES
        TIMEOUT 30
        PASS_REGULAR_EXPRESSION "version 1, brought up to 4"
        FAIL_REGULAR_EXPRESSION "error "
    )

    add_test(NAME inspect_refuses_a_non_project
             COMMAND munkicast project "${CMAKE_CURRENT_BINARY_DIR}/not-a-project")
    set_tests_properties(inspect_refuses_a_non_project PROPERTIES
        TIMEOUT 30
        WILL_FAIL TRUE
    )
endif()
