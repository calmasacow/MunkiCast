// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The project's own folders, which is the whole of the tree.
///
/// Not a file browser. A project has a known shape -- Recordings, SourceMedia, Exports,
/// CacheMedia -- and those four are where its media is. Browsing the rest of the disk is what
/// the Add Source dialog is for; offering it here would make the common errand, "play back what
/// I just recorded", one click further away than it needs to be.
Item {
    id: panel

    Text {
        anchors.centerIn: parent
        width: parent.width - 24
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        visible: !Media.hasProject
        text: qsTr("Open a project to see its media folders.")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 6
        visible: Media.hasProject

        Repeater {
            model: Media.projectFolders

            delegate: Item {
                id: row

                required property var modelData

                width: parent.width
                height: 26

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 4
                    anchors.rightMargin: 4
                    radius: 2
                    color: row.modelData.current ? Theme.c.accent
                         : mouse.containsMouse ? Theme.c.panelHeaderActive
                         : "transparent"
                    opacity: row.modelData.current ? 0.22 : 1.0
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.name
                    font.pixelSize: 11
                    font.weight: row.modelData.current ? Font.DemiBold : Font.Normal
                    // A folder that is not there yet is still listed, greyed: it is part of the
                    // project's shape, and saying so is more use than hiding it.
                    color: !row.modelData.exists ? Theme.c.textMuted
                         : row.modelData.current ? Theme.c.text
                         : Theme.c.text
                }

                MouseArea {
                    id: mouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: Media.openFolder(row.modelData.path)
                }

                ToolTipText {
                    text: row.modelData.exists
                        ? row.modelData.path
                        : qsTr("%1 — not created yet").arg(row.modelData.path)
                    visible: mouse.containsMouse
                    hoverArea: mouse
                }
            }
        }
    }
}
