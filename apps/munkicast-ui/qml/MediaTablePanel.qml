// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The files in the folder on screen, as a table.
///
/// Columns are fixed rather than draggable. Six columns at known widths read the same way every
/// time they are opened, and a layout the operator cannot wreck is one they cannot have to
/// rebuild in the middle of a job.
Item {
    id: panel

    /// What the footer's export button sends. Kept on the panel rather than in the menu so the
    /// choice survives the menu closing.
    property string preset: Media.presets.length > 0 ? Media.presets[0] : ""

    readonly property real colName: Math.max(160, panel.width - 460)

    Rectangle {
        id: header

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 22
        color: Theme.c.panelHeader

        Row {
            anchors.fill: parent
            anchors.leftMargin: 10
            spacing: 0

            Text {
                width: panel.colName
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Name")
                font.pixelSize: 10
                color: Theme.c.textMuted
                elide: Text.ElideRight
            }
            Text {
                width: 90
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Duration")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }
            Text {
                width: 100
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Resolution")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }
            Text {
                width: 80
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Codec")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }
            Text {
                width: 90
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Size")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }
            Text {
                width: 110
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Modified")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }
        }
    }

    Text {
        anchors.centerIn: parent
        visible: Media.count === 0
        text: Media.hasProject ? qsTr("Nothing in this folder") : qsTr("No project open")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    ListView {
        id: list

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: footer.top
        clip: true
        model: Media
        boundsBehavior: Flickable.StopAtBounds
        // ADR-0039: nothing here scrolls by being dragged.
        interactive: false

        NoDragScroll {}

        delegate: Item {
            id: row

            required property int index
            required property string path
            required property string name
            required property string size
            required property string modified
            required property string duration
            required property string resolution
            required property string codec
            required property bool probed

            width: ListView.view.width
            height: 22

            readonly property bool current: Media.selected === row.index

            Rectangle {
                anchors.fill: parent
                color: row.current ? Theme.c.accent
                     : mouse.containsMouse ? Theme.c.panelHeaderActive
                     : "transparent"
                opacity: row.current ? 0.25 : 1.0
            }

            Row {
                anchors.fill: parent
                anchors.leftMargin: 10
                spacing: 0

                Text {
                    width: panel.colName
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.name
                    font.pixelSize: 11
                    color: Theme.c.text
                    elide: Text.ElideMiddle
                }
                Text {
                    width: 90
                    anchors.verticalCenter: parent.verticalCenter
                    // A row that has not been probed yet says so, rather than showing a blank
                    // that reads as "this file has no duration".
                    text: row.probed ? row.duration : "…"
                    font.pixelSize: 11
                    font.family: "monospace"
                    color: row.probed ? Theme.c.text : Theme.c.textMuted
                }
                Text {
                    width: 100
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.resolution
                    font.pixelSize: 11
                    font.family: "monospace"
                    color: Theme.c.textMuted
                }
                Text {
                    width: 80
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.codec
                    font.pixelSize: 11
                    color: Theme.c.textMuted
                    elide: Text.ElideRight
                }
                Text {
                    width: 90
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.size
                    font.pixelSize: 11
                    font.family: "monospace"
                    color: Theme.c.textMuted
                }
                Text {
                    width: 110
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modified
                    font.pixelSize: 11
                    font.family: "monospace"
                    color: Theme.c.textMuted
                }
            }

            MouseArea {
                id: mouse

                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                onClicked: Media.selected = row.index
            }
        }
    }

    Rectangle {
        id: footer

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 26
        color: Theme.c.panelHeader

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: Media.count === 1 ? qsTr("1 item") : qsTr("%1 items").arg(Media.count)
            font.pixelSize: 10
            color: Theme.c.textMuted
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            // The preset the export buttons use. A plain cycling button rather than a combo
            // box: there are six presets and no style here to draw a popup list with.
            Rectangle {
                width: presetLabel.implicitWidth + 16
                height: 20
                radius: 2
                color: presetMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                border.width: 1
                border.color: presetMouse.containsMouse ? Theme.c.accent : Theme.c.border

                Text {
                    id: presetLabel

                    anchors.centerIn: parent
                    text: panel.preset
                    font.pixelSize: 10
                    color: Theme.c.text
                }

                MouseArea {
                    id: presetMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        const presets = Media.presets;
                        const at = presets.indexOf(panel.preset);
                        panel.preset = presets[(at + 1) % presets.length];
                    }
                }

                ToolTipText {
                    text: qsTr("Export preset — click to change")
                    visible: presetMouse.containsMouse
                    hoverArea: presetMouse
                }
            }

            ToolButton {
                label: "↻"
                tip: qsTr("List this folder again")
                onClicked: Media.refresh()
            }

            ToolButton {
                label: "→"
                tip: qsTr("Export the selected file")
                enabled: Media.selected >= 0
                onClicked: Media.enqueue(Media.selectedItem.path, panel.preset)
            }

            ToolButton {
                label: "⇉"
                tip: qsTr("Export everything in this folder")
                enabled: Media.count > 0
                onClicked: Media.enqueueAll(panel.preset)
            }
        }
    }
}
