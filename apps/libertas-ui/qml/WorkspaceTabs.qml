// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// The saved arrangements, across the top: Build, Live, whatever has been added, and a + .
///
/// These are not compositor workspaces. Nothing here is an OS window or a virtual desktop — they
/// are saved arrangements of this one surface (ADR-0015), one per job. Building a show wants the
/// scene and source lists and the properties panel; running one wants none of those and wants the
/// statistics and the outputs instead.
///
/// Build and Live cannot be renamed or removed. They can be rearranged like any other and reset
/// to how they shipped, so there is always a known arrangement to get back to — which is the
/// whole point of a layout that someone has just wrecked mid-show.
Item {
    id: strip

    implicitHeight: 26

    Rectangle {
        anchors.fill: parent
        color: Theme.c.panelHeader
    }

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 6
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        spacing: 0

        Repeater {
            model: Workspace.workspaces

            delegate: Item {
                id: tab

                required property int index
                required property string name
                required property bool builtin
                required property bool isActive

                objectName: "workspaceTab"
                width: Math.max(label.implicitWidth + (tab.builtin ? 26 : 38), 64)
                height: strip.height

                Rectangle {
                    anchors.fill: parent
                    anchors.topMargin: 2
                    anchors.bottomMargin: 0
                    color: tab.isActive ? Theme.c.panelBackground
                         : tabMouse.containsMouse ? Theme.c.panelHeaderActive
                         : "transparent"
                    radius: 2
                }

                // The active tab joins the workspace below it rather than sitting in a box.
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: 2
                    height: 2
                    visible: tab.isActive
                    color: Theme.c.accent
                }

                MouseArea {
                    id: tabMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton
                    onClicked: Workspace.selectWorkspace(tab.index)
                    onDoubleClicked: {
                        // Built-in tabs are not renameable; double-clicking one resets it
                        // instead, which is the only thing that *can* be done to it.
                        if (tab.builtin) {
                            Workspace.resetWorkspace(tab.index);
                            return;
                        }
                        rename.text = tab.name;
                        rename.visible = true;
                        rename.forceActiveFocus();
                        rename.selectAll();
                    }
                }

                Text {
                    id: label

                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !rename.visible
                    text: tab.name === "Encode" ? qsTr("Encoder Flow") : tab.name
                    font.pixelSize: 11
                    font.weight: tab.isActive ? Font.DemiBold : Font.Normal
                    color: tab.isActive ? Theme.c.text : Theme.c.textMuted
                    elide: Text.ElideRight
                }

                TextInput {
                    id: rename

                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    anchors.rightMargin: 22
                    anchors.verticalCenter: parent.verticalCenter
                    visible: false
                    font.pixelSize: 11
                    color: Theme.c.text
                    selectionColor: Theme.c.accent
                    selectByMouse: true

                    onAccepted: {
                        visible = false;
                        if (text !== tab.name)
                            Workspace.renameWorkspace(tab.index, text);
                    }
                    onActiveFocusChanged: {
                        if (!activeFocus)
                            visible = false;
                    }
                    Keys.onEscapePressed: visible = false
                }

                // Only on the ones that can actually go, so there is no button that exists to
                // refuse. Built-in tabs show nothing here.
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !tab.builtin && (tabMouse.containsMouse || closeMouse.containsMouse)
                    text: "×"
                    font.pixelSize: 13
                    color: closeMouse.containsMouse ? Theme.c.error : Theme.c.textMuted

                    MouseArea {
                        id: closeMouse

                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        onClicked: Workspace.removeWorkspace(tab.index)
                    }

                    ToolTipText {
                        text: qsTr("Remove this workspace")
                        visible: closeMouse.containsMouse
                        hoverArea: closeMouse
                    }
                }

                ToolTipText {
                    text: tab.builtin
                        ? qsTr("%1 — built in; double-click to reset it").arg(tab.name)
                        : qsTr("%1 — double-click to rename").arg(tab.name)
                    visible: tabMouse.containsMouse && !rename.visible
                    hoverArea: tabMouse
                }
            }
        }

        // The rightmost tab: a new arrangement, starting from whatever is on screen.
        Item {
            id: addTab

            objectName: "workspaceAddTab"
            width: 26
            height: strip.height

            Rectangle {
                anchors.fill: parent
                anchors.topMargin: 2
                color: addMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                radius: 2
            }

            Text {
                anchors.centerIn: parent
                text: "+"
                font.pixelSize: 14
                color: addMouse.containsMouse ? Theme.c.accent : Theme.c.textMuted
            }

            MouseArea {
                id: addMouse

                anchors.fill: parent
                hoverEnabled: true
                onClicked: {
                    // Named for what it is until it is renamed, and numbered so that adding two
                    // in a row does not collide.
                    let n = Workspace.workspaceCount() + 1;
                    Workspace.addWorkspace(qsTr("Workspace %1").arg(n));
                }
            }

            ToolTipText {
                text: qsTr("Save the current arrangement as a new workspace")
                visible: addMouse.containsMouse
                hoverArea: addMouse
            }
        }
    }
}
