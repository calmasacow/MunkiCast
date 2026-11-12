# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The workspace shell: one top-level window holding the whole workspace (ADR-0015), with the
# Adobe drop-zone docking gesture (ADR-0016) and live Omarchy theme following.
#
# This target holds no layout logic. Every gesture is a call into munkicast::llayout and every
# answer -- including whether a drop is legal -- comes back from there, so what the operator sees
# is what the headless tests cover.

# DBus is here for one thing: the file chooser behind a source's path setting. The call is the
# desktop portal's OpenFile, and this machine's portal config sends that to Omanta. Qt's own
# dialog is not used. Widgets is here for QColorDialog, which the colour field opens. That
# dialog already has the picker and the custom-colour slots, so they are not rebuilt here.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus Gui Network Qml Quick QuickControls2 QuickShapes Widgets WebEngineQuick)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(munkicast-ui
    mcpserver.cpp
    mcpserver.hpp
    remotecontrol.cpp
    remotecontrol.hpp
    replaywebsocket.cpp
    replaywebsocket.hpp
    connectors.cpp
    connectors.hpp
    assetcache.cpp
    assetcache.hpp
    countdowns.cpp
    colourdialog.cpp
    colourdialog.hpp
    filedialog.cpp
    filedialog.hpp
    document.cpp
    filters.cpp
    outputpresets.cpp
    collections.cpp
    document.hpp
    documentmodels.cpp
    documentmodels.hpp
    shortcuts.cpp
    shortcuts.hpp
    layoutmodels.cpp
    layoutmodels.hpp
    main.cpp
    preview.cpp
    preferences.cpp
    preferences.hpp
    preview.hpp
    rowmodel.hpp
    telemetry.cpp
    theme.cpp
    theme.hpp
    workspace.cpp
    workspace.hpp
)

# The QML files live in qml/ rather than beside the C++; QTP0004 generates the extra qmldir
# that makes a subdirectory a proper part of the module.
# NoticeLog is a QML-side singleton: a rolling record of everything the shell has said, which
# needs no C++ and no state the session owns.
set_source_files_properties(qml/NoticeLog.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# Must precede qt_add_qml_module: the property is read when the qmldir is generated.
qt_policy(SET QTP0004 NEW)

qt_add_qml_module(munkicast-ui
    URI MunkiCast
    VERSION 1.0
    QML_FILES
        qml/AddSourceDialog.qml
        qml/FiltersDialog.qml
        qml/FilterSetting.qml
        qml/AudioMixerPanel.qml
        qml/BrowserDockPanel.qml
        qml/BrowserDocksDialog.qml
        qml/BroadcastStatsPanel.qml
        qml/CanvasManagerDialog.qml
        qml/CollectionDialog.qml
        qml/CacheConfirm.qml
        qml/CacheProgressDialog.qml
        qml/CacheStateDialog.qml
        qml/CanvasPanel.qml
        qml/ChainIcon.qml
        qml/BroadcastBar.qml
        qml/EncodeView.qml
        qml/EncodeGraph.qml
        qml/EncodeNodeCard.qml
        qml/EncodeProperties.qml
        qml/GraphEdges.qml
        qml/EyeIcon.qml
        qml/MixerFader.qml
        qml/NoDragScroll.qml
        qml/PasteWarning.qml
        qml/StripMenu.qml
        qml/CheckLabel.qml
        qml/ChoiceField.qml
        qml/ColourField.qml
        qml/CountdownSettingsDialog.qml
        qml/CountdownsPanel.qml
        qml/StingerDialog.qml
        qml/ControlsPanel.qml
        qml/StudioModeView.qml
        qml/DialogButton.qml
        qml/DialogGeometry.qml
        qml/DragGhost.qml
        qml/DropOverlay.qml
        qml/FavouritesPanel.qml
        qml/InfoPanel.qml
        qml/LogPanel.qml
        qml/Main.qml
        qml/MainMenu.qml
        qml/McpSettingsDialog.qml
        qml/NoticeLog.qml
        qml/ReplaySettings.qml
        qml/WebsocketSettingsDialog.qml
        qml/NumberField.qml
        qml/OutputsPanel.qml
        qml/PanelBody.qml
        qml/PanelFrame.qml
        qml/PreferencesDialog.qml
        qml/TransformMenu.qml
        qml/ProgramOverlay.qml
        qml/PropertiesPanel.qml
        qml/SceneLinkDialog.qml
        qml/ScenesPanel.qml
        qml/SceneReorder.qml
        qml/SettingRow.qml
        qml/ShortcutsPanel.qml
        qml/SourceSettingsDialog.qml
        qml/SourcesPanel.qml
        qml/StatRow.qml
        qml/StatusBar.qml
        qml/TabStrip.qml
        qml/TextLine.qml
        qml/ArmSwitch.qml
        qml/ToolButton.qml
        qml/ToolTipText.qml
        qml/WorkspaceTabs.qml
)

target_link_libraries(munkicast-ui PRIVATE
    munkicast::flags
    # Listing monitors is a Wayland registry read, not a capture: no GPU, no session, no device.
    # The UI asks directly rather than through the render process so the dropdown is right before
    # anything has started capturing (ADR-0041).
    munkicast::lcapture
    munkicast::lcomp
    munkicast::lcore
    munkicast::llayout
    munkicast::limport
    munkicast::lsession
    munkicast::ltheme
    Qt6::Network
    Qt6::DBus
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickShapes
    Qt6::Widgets
    Qt6::WebEngineQuick
)

# The shell spawns this. A path baked in at build time is the one the self-check runs, rather
# than whatever happens to be first on PATH.
target_compile_definitions(munkicast-ui PRIVATE
    MUNKICAST_RENDER_EXE="$<TARGET_FILE:munkicast-render>")
if(NOT MUNKICAST_USE_SYSTEM_FFMPEG)
    target_compile_definitions(munkicast-ui PRIVATE
        MUNKICAST_FFMPEG_BIN_DIR="${MUNKICAST_FFMPEG_PREFIX}/bin")
endif()
add_dependencies(munkicast-ui munkicast-render)

# Qt's generated sources (moc, qrc, the QML type registration) are not ours to keep warning-free,
# and -Werror on a CI build would fail on them rather than on anything we wrote.
set_source_files_properties(
    "${CMAKE_CURRENT_BINARY_DIR}/munkicast-ui_qmltyperegistrations.cpp"
    PROPERTIES COMPILE_OPTIONS "-w"
)

# The headless self-check: load the real QML tree, drive the real gestures, touch no screen.
if(MUNKICAST_BUILD_TESTS)
    add_test(NAME panel_loading_self_check COMMAND $<TARGET_FILE:munkicast-ui> --check --check-panels)
    set_tests_properties(panel_loading_self_check PROPERTIES TIMEOUT 90 LABELS "needs-qt-threads"
        ENVIRONMENT "QT_IM_MODULE=none;MUNKICAST_RENDER_BACKEND=cpu")
    # Started from nothing every run. --project opens a directory that already exists, so a
    # project left by an older build -- a different schema, or a shape the check no longer
    # expects -- made the suite fail for a reason that had nothing to do with the change
    # being tested. Deleting it by hand was the standing workaround; this is the fix.
    add_test(NAME scene_reorder_self_check COMMAND $<TARGET_FILE:munkicast-ui> --check --check-scene-reorder)
    set_tests_properties(scene_reorder_self_check PROPERTIES TIMEOUT 30 LABELS "needs-qt-threads"
        ENVIRONMENT "QT_IM_MODULE=none;MUNKICAST_RENDER_BACKEND=cpu")
    add_test(NAME studio_self_check COMMAND $<TARGET_FILE:munkicast-ui> --check --check-studio)
    set_tests_properties(studio_self_check PROPERTIES TIMEOUT 30 LABELS "needs-qt-threads"
        ENVIRONMENT "QT_IM_MODULE=none;MUNKICAST_RENDER_BACKEND=cpu")
    add_test(NAME replay_self_check COMMAND $<TARGET_FILE:munkicast-ui> --check --check-replay)
    set_tests_properties(replay_self_check PROPERTIES TIMEOUT 60 LABELS "needs-qt-threads"
        ENVIRONMENT "QT_IM_MODULE=none;QT_LOGGING_RULES=qt.qpa.*=false;MUNKICAST_RENDER_BACKEND=cpu;LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/lsan.suppressions:print_suppressions=0")
    add_test(NAME shell_self_check COMMAND "${CMAKE_COMMAND}"
             -DCHECK_EXE=$<TARGET_FILE:munkicast-ui>
             -DCHECK_DIR=${CMAKE_CURRENT_BINARY_DIR}
             -P "${CMAKE_CURRENT_SOURCE_DIR}/run-self-check.cmake")
    # Qt initialises GL even on the offscreen platform, and the GPU driver leaks a little
    # one-time state on every run. The suppression file names libraries, never functions of ours,
    # so this cannot hide a leak we introduce.
    set_tests_properties(shell_self_check PROPERTIES
        # QT_IM_MODULE=none keeps the input-method plugin out: a headless check has nothing to
        # type into, and loading fcitx leaks 1.7 KB through D-Bus on every run. Not loading it is
        # better than suppressing it.
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_IM_MODULE=none;QT_LOGGING_RULES=qt.qpa.*=false;LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/lsan.suppressions:print_suppressions=0"
        # Includes Chromium startup and the embedded browser interaction checks.
        TIMEOUT 180
        # Excluded from the TSan preset by this label.
        #
        # Not a judgement about this test: Qt here is a distribution build with no TSan, and
        # starting the QQmlThread makes TSan's *own* thread registry fail an internal CHECK --
        # "live_.try_emplace(user_id, tid).second != 0" -- which is a crash of the tool rather
        # than a report that could be suppressed. Suppressing the races underneath it was tried
        # first and worked; the CHECK failure is what remains, and nothing in this project can
        # fix it short of a TSan build of Qt.
        #
        # What the TSan preset is for is our own threaded code, and that is still covered: the
        # ULID generator across eight threads, and lsession's listeners. The shell has one
        # thread of its own.
        LABELS "needs-qt-threads"
    )
endif()

qt_add_resources(munkicast-ui connector_catalog PREFIX "/connectors" FILES data/obs-services.json)
# Destination icons live with their source CSV so the artist replaces them in one place.
file(GLOB destination_icons RELATIVE "${PROJECT_SOURCE_DIR}/docs/streaming-destination-icons"
    CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/docs/streaming-destination-icons/svg/*.svg")
list(TRANSFORM destination_icons PREPEND "${PROJECT_SOURCE_DIR}/docs/streaming-destination-icons/")
qt_add_resources(munkicast-ui destination_icons PREFIX "/destinations"
    BASE "${PROJECT_SOURCE_DIR}/docs/streaming-destination-icons"
    FILES "${PROJECT_SOURCE_DIR}/docs/streaming-destination-icons/destinations.csv" ${destination_icons})

find_package(PkgConfig REQUIRED)
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)
pkg_check_modules(MSGPACK REQUIRED IMPORTED_TARGET msgpack-c)
target_link_libraries(munkicast-ui PRIVATE PkgConfig::SOUP PkgConfig::MSGPACK)

target_compile_definitions(munkicast-ui PRIVATE
    MUNKICAST_REPLAY_CHECK_SCRIPT="${CMAKE_SOURCE_DIR}/tests/unit/check_replay_websocket.py")

qt_add_resources(munkicast-ui remote_protocol PREFIX "/remote" FILES data/remote-protocol.json)

# Integration checks use Node's built-in runner; C++ builds do not install npm packages.
if(MUNKICAST_BUILD_TESTS)
    find_program(MUNKICAST_NODE node)
    if(MUNKICAST_NODE AND EXISTS "${CMAKE_SOURCE_DIR}/integrations/companion/node_modules")
        add_test(NAME companion_controls COMMAND "${MUNKICAST_NODE}" --test
            "${CMAKE_SOURCE_DIR}/integrations/companion/test.js")
    endif()
    if(MUNKICAST_NODE AND EXISTS "${CMAKE_SOURCE_DIR}/integrations/mcp/node_modules")
        add_test(NAME native_mcp_self_check COMMAND $<TARGET_FILE:munkicast-ui> --check --check-mcp)
        set_tests_properties(native_mcp_self_check PROPERTIES TIMEOUT 90 LABELS "needs-qt-threads"
            ENVIRONMENT "QT_IM_MODULE=none;MUNKICAST_RENDER_BACKEND=cpu")
        add_test(NAME mcp_schema COMMAND "${MUNKICAST_NODE}" --test
            "${CMAKE_SOURCE_DIR}/integrations/mcp/test.js")
    endif()
endif()
