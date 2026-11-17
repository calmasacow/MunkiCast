// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The project's timelines, and the media available to cut into them.
///
/// Two lists rather than a bin tree. A bin hierarchy is worth its weight once a project has
/// hundreds of clips; until then it is a folder to open before reaching anything, and the
/// project's own folders already give the media somewhere to live.
Item {
    id: panel

    Column {
        anchors.fill: parent

        // ---- timelines ----
        Rectangle {
            width: parent.width
            height: 22
            color: Theme.c.panelHeader

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Timelines")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }

            ToolButton {
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                label: "+"
                tip: qsTr("New timeline")
                onClicked: Edit.createTimeline(qsTr("Timeline %1").arg(Edit.timelines.length + 1))
            }
        }

        ListView {
            width: parent.width
            height: Math.min(contentHeight, panel.height * 0.4)
            clip: true
            model: Edit.timelines
            interactive: false
            boundsBehavior: Flickable.StopAtBounds

            NoDragScroll {}

            delegate: Item {
                id: row

                required property var modelData

                width: ListView.view.width
                height: 22

                Rectangle {
                    anchors.fill: parent
                    color: row.modelData.open ? Theme.c.accent
                         : mouse.containsMouse ? Theme.c.panelHeaderActive
                         : "transparent"
                    opacity: row.modelData.open ? 0.22 : 1.0
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.name
                    font.pixelSize: 11
                    font.weight: row.modelData.open ? Font.DemiBold : Font.Normal
                    color: Theme.c.text
                }

                MouseArea {
                    id: mouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: Edit.openTimeline(row.modelData.id)
                }
            }
        }

        // ---- media ----
        Rectangle {
            width: parent.width
            height: 22
            color: Theme.c.panelHeader

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: Media.folderLabel ? Media.folderLabel : qsTr("Media")
                font.pixelSize: 10
                color: Theme.c.textMuted
                elide: Text.ElideRight
            }
        }

        ListView {
            id: media

            width: parent.width
            height: panel.height - y
            clip: true
            model: Media
            interactive: false
            boundsBehavior: Flickable.StopAtBounds

            NoDragScroll {}

            delegate: Item {
                id: file

                required property int index
                required property string path
                required property string name
                required property string duration

                width: ListView.view.width
                height: 22

                Rectangle {
                    anchors.fill: parent
                    color: fileMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.right: length.left
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: file.name
                    font.pixelSize: 11
                    color: Theme.c.text
                    elide: Text.ElideMiddle
                }

                Text {
                    id: length

                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: file.duration
                    font.pixelSize: 10
                    font.family: "monospace"
                    color: Theme.c.textMuted
                }

                MouseArea {
                    id: fileMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    // Double-click appends, rather than drag-and-drop: dropping onto a
                    // Canvas-drawn timeline would need its own hit testing, and appending is
                    // the gesture an assembly edit is actually made of.
                    onDoubleClicked: {
                        if (!Edit.hasTimeline) {
                            Edit.createTimeline(qsTr("Timeline 1"));
                        }
                        Edit.appendToFirstTrack(file.path);
                    }
                }

                ToolTipText {
                    text: qsTr("Double-click to add to the timeline")
                    visible: fileMouse.containsMouse
                    hoverArea: fileMouse
                }
            }
        }
    }
}
