// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The project's graphics, and what to do with the open one.
Item {
    id: panel

    Column {
        anchors.fill: parent

        Rectangle {
            width: parent.width
            height: 22
            color: Theme.c.panelHeader

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Graphics")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }

            ToolButton {
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                label: "+"
                tip: qsTr("New graphic")
                onClicked: Graphics.createGraphic(
                    qsTr("Graphic %1").arg(Graphics.graphics.length + 1))
            }
        }

        ListView {
            width: parent.width
            height: Math.min(contentHeight, panel.height - 150)
            clip: true
            model: Graphics.graphics
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
                    onClicked: Graphics.openGraphic(row.modelData.id)
                }
            }
        }

        Item { width: 1; height: 10 }

        Column {
            width: parent.width
            spacing: 6
            visible: Graphics.hasGraphic

            Text {
                x: 10
                text: qsTr("Play-out")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }

            // The whole point of the tab: a page a browser source can play. Written into the
            // project's Graphics folder, so it travels with the show.
            Rectangle {
                x: 10
                width: parent.width - 20
                height: 24
                radius: 2
                color: exportMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                border.width: 1
                border.color: exportMouse.containsMouse ? Theme.c.accent : Theme.c.border

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Export HTML overlay")
                    font.pixelSize: 11
                    color: Theme.c.text
                }

                MouseArea {
                    id: exportMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: Graphics.exportHtml()
                }
            }

            Rectangle {
                x: 10
                width: parent.width - 20
                height: 24
                radius: 2
                color: saveMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                border.width: 1
                border.color: Graphics.dirty ? Theme.c.accent : Theme.c.border

                Text {
                    anchors.centerIn: parent
                    text: Graphics.dirty ? qsTr("Save graphic •") : qsTr("Save graphic")
                    font.pixelSize: 11
                    color: Theme.c.text
                }

                MouseArea {
                    id: saveMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: Graphics.save()
                }
            }
        }
    }
}
