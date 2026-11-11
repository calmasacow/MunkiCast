// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

Item {
    id: panel

    Text {
        anchors.centerIn: parent
        visible: NoticeLog.entries.count === 0
        text: qsTr("Nothing to report")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    ListView {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
        clip: true
        model: NoticeLog.entries
        boundsBehavior: Flickable.StopAtBounds
        // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
        // because Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

        delegate: Item {
            id: row

            required property string time
            required property string text

            width: ListView.view.width
            implicitHeight: message.implicitHeight + 8

            Text {
                id: stamp

                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.top: parent.top
                anchors.topMargin: 4
                text: row.time
                font.pixelSize: 10
                font.family: "monospace"
                color: Theme.c.textMuted
            }

            Text {
                id: message

                anchors.left: stamp.right
                anchors.leftMargin: 8
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.top: parent.top
                anchors.topMargin: 4
                text: row.text
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.c.text
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

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.c.border
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("%1 message(s)").arg(NoticeLog.entries.count)
            font.pixelSize: 10
            color: Theme.c.textMuted
        }

        ToolButton {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            label: "✕"
            tip: qsTr("Clear")
            enabled: NoticeLog.entries.count > 0
            onClicked: NoticeLog.clear()
        }
    }
}
