// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQml
import QtQuick
import Libertas

// ADR-0015 rule 1: one top-level window holds the entire workspace. Panels are regions inside
// this surface, laid out by our own engine. There is no second OS window, which is why nothing
// here can be moved off-screen, lost behind another application, or closed by accident.
Window {
    id: root

    visible: true
    width: 1600
    height: 950
    minimumWidth: 960
    minimumHeight: 620
    title: Document.projectName + (Document.dirty ? " \u2022 " : " \u2014 ") +
           qsTr("Libertas Studio")
    color: Theme.c.window

    // A line of text the status bar shows for a few seconds: a refused drop, a failed save, a
    // theme missing tokens. Transient, never a dialog -- nothing here should ever interrupt a
    // show to be acknowledged.
    property string notice: ""

    /// The Encode workspace is not a dock arrangement: it draws a fixed three-column view and
    /// its stored layout is never read. Bound to the name rather than asked through
    /// workspaceIsBuiltin(), which is an invokable and so would never re-evaluate.
    readonly property bool encodeActive: Workspace.activeWorkspaceName === "Encode"

    function report(text) {
        root.notice = text;
        noticeTimer.restart();
        NoticeLog.add(text);
    }

    Timer {
        id: noticeTimer
        interval: 7000
        onTriggered: root.notice = ""
    }

    Connections {
        target: Workspace
        function onNotice(text) {
            root.report(text);
        }
    }

    Connections {
        target: Theme
        function onNotice(text) {
            root.report(text);
        }
    }

    Connections {
        target: Preview
        function onNotice(text) { root.report(text); }
    }

    Connections {
        target: Document
        function onFiltersRequested(owner) { filtersDialog.open(owner); }
        function onNotice(text) {
            root.report(text);
        }
    }

    FiltersDialog { id: filtersDialog; parent: root.contentItem }

    Connections {
        target: Shortcuts
        function onNotice(text) {
            root.report(text);
        }
    }

    // ADR-0016 rule 7: Escape cancels an in-flight drag. A mis-drop costs one keystroke.
    Shortcut {
        sequences: ["Escape"]
        onActivated: Workspace.cancelDrag()
    }
    // Every application-scope binding comes from the registry rather than being written here,
    // so the table in settings and the keys that actually fire cannot disagree. Global ones are
    // deliberately absent: they are delivered by the portal to the session process, because a
    // hotkey that stops working when the frontend dies is the design ADR-0024 rejects.
    Instantiator {
        model: Shortcuts.applicationBindings

        delegate: Shortcut {
            required property var modelData

            sequence: modelData.trigger
            onActivated: Shortcuts.perform(modelData.ref)
        }
    }

    CollectionDialog { id: collectionEditor }

    // One of these for the whole shell: it shows itself whenever a cache build runs, wherever
    // the build was started -- the Caching menu, a source row's indicator, or Preferences.
    // z above the workspace, as FiltersDialog does. Parenting to contentItem is not enough:
    // the workspace Item is declared after this one, so at equal z it paints on top and the
    // overlay is invisible even while it is shown and taking key events.
    CacheProgressDialog {
        objectName: "cacheProgressDialog"
        parent: root.contentItem
        z: 10000
    }

    Rectangle {
        id: topChrome
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Math.max(mainMenu.height + workspaceTabs.height, broadcastBar.height)
        color: Theme.c.panelHeader
    }

    MainMenu {
        id: mainMenu
        anchors.left: parent.left
        anchors.top: parent.top
        mainWindow: root
        collectionDialog: collectionEditor
    }

    BroadcastBar {
        id: broadcastBar
        anchors.left: mainMenu.right
        anchors.right: parent.right
        anchors.verticalCenter: topChrome.verticalCenter
    }

    WorkspaceTabs {
        id: workspaceTabs

        anchors.left: parent.left
        anchors.right: mainMenu.right
        anchors.top: mainMenu.bottom
    }

    Item {
        id: workArea

        // Named so the headless check can ask where it actually is. Panel rects are in this
        // item's coordinates, not the window's, and the chrome above it is not fixed.
        objectName: "workArea"

        // Hidden on the Encode tab, never unloaded. The viewport feed below is what gives the
        // dock tree its size; take it away and panel rects come back zero-sized, so Build and
        // Live would render empty on the way back.
        visible: !root.encodeActive

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: topChrome.bottom
        anchors.bottom: statusBar.top

        onWidthChanged: Workspace.setViewport(width, height)
        onHeightChanged: Workspace.setViewport(width, height)
        Component.onCompleted: Workspace.setViewport(width, height)

        // One mouse area for every divider, beneath the panels.
        //
        // Dividers are hit-tested by the layout engine rather than by per-divider MouseAreas,
        // because a resize changes the layout on every mouse move, which rebuilds the divider
        // delegates -- destroying the very item holding the pointer grab. Keeping the grab on an
        // item that outlives the layout is not an optimisation, it is the only way the gesture
        // can work at all. Panels sit above this and consume their own events, so it only ever
        // sees the gaps.
        MouseArea {
            id: resizeArea

            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton

            property int hoverIndex: -1
            property int hoverAxis: -1
            property int activeAxis: -1
            property bool resizing: false

            cursorShape: {
                const axis = resizing ? activeAxis : hoverAxis;
                if (axis === 0)
                    return Qt.SizeHorCursor;
                if (axis === 1)
                    return Qt.SizeVerCursor;
                return Qt.ArrowCursor;
            }

            onExited: {
                hoverIndex = -1;
                hoverAxis = -1;
            }

            onPositionChanged: mouse => {
                if (resizing) {
                    Workspace.dragDividerTo(mouse.x, mouse.y);
                    return;
                }
                hoverAxis = Workspace.dividerAxisAtPoint(mouse.x, mouse.y);
                hoverIndex = Workspace.dividerIndexAtPoint(mouse.x, mouse.y);
            }

            onPressed: mouse => {
                activeAxis = Workspace.dividerAxisAtPoint(mouse.x, mouse.y);
                resizing = Workspace.beginDividerDrag(mouse.x, mouse.y);
            }

            onReleased: {
                if (resizing) {
                    resizing = false;
                    Workspace.endDividerDrag();
                }
            }

            onCanceled: {
                if (resizing) {
                    resizing = false;
                    Workspace.endDividerDrag();
                }
            }
        }

        // The dividers themselves, drawn but not interactive.
        Repeater {
            model: Workspace.dividers

            delegate: Rectangle {
                id: dividerVisual

                required property int index
                required property bool horizontal
                required property real rectX
                required property real rectY
                required property real rectW
                required property real rectH

                readonly property bool hovered: resizeArea.hoverIndex === index

                x: rectX
                y: rectY
                width: rectW
                height: rectH
                color: hovered ? Theme.c.dividerHover : Theme.c.divider

                Behavior on color {
                    ColorAnimation {
                        duration: 90
                    }
                }

                // Three dots, the universal "this is draggable".
                Row {
                    visible: dividerVisual.horizontal
                    anchors.centerIn: parent
                    spacing: 0

                    Repeater {
                        model: 3

                        delegate: Item {
                            width: 2
                            height: 10

                            Rectangle {
                                anchors.centerIn: parent
                                width: 2
                                height: 2
                                radius: 1
                                color: Theme.c.textMuted
                                opacity: dividerVisual.hovered ? 0.0 : 0.7
                            }
                        }
                    }
                }

                Column {
                    visible: !dividerVisual.horizontal
                    anchors.centerIn: parent
                    spacing: 0

                    Repeater {
                        model: 3

                        delegate: Item {
                            width: 10
                            height: 2

                            Rectangle {
                                anchors.centerIn: parent
                                width: 2
                                height: 2
                                radius: 1
                                color: Theme.c.textMuted
                                opacity: dividerVisual.hovered ? 0.0 : 0.7
                            }
                        }
                    }
                }
            }
        }

        // The panels. The model updates rows in place when the count has not changed, so a
        // divider drag re-evaluates geometry without tearing down and rebuilding every delegate
        // sixty times a second.
        Repeater {
            model: Workspace.panels

            delegate: PanelFrame {
                surface: workArea
            }

        }

        DropOverlay {
            anchors.fill: parent
        }

        DragGhost {}
    }

    // The node graph, in place of the dock tree rather than inside it.
    Loader {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: topChrome.bottom
        anchors.bottom: statusBar.top
        active: root.encodeActive
        visible: root.encodeActive
        sourceComponent: EncodeView {}
    }

    StatusBar {
        id: statusBar
        visible: mainMenu.statusBarVisible
        height: visible ? 26 : 0

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        notice: root.notice
    }
}
