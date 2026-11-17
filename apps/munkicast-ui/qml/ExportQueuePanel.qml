// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// What is being written, and what is waiting to be.
///
/// One job runs at a time. Two ffmpeg processes competing for the same encoder finish a pair of
/// exports later than running them in order does, and while a show is on the machine has work
/// to do that matters more than this.
Item {
    id: panel

    Text {
        anchors.centerIn: parent
        width: parent.width - 24
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        visible: Media.queue.length === 0
        text: qsTr("Nothing queued. Export from the media list.")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    ListView {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
        clip: true
        model: Media.queue
        boundsBehavior: Flickable.StopAtBounds
        interactive: false

        NoDragScroll {}

        delegate: Item {
            id: row

            required property var modelData

            width: ListView.view.width
            height: 40

            readonly property bool running: row.modelData.state === "Running"
            readonly property bool failed: row.modelData.state === "Failed"

            Text {
                id: name

                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: cancel.left
                anchors.rightMargin: 6
                anchors.top: parent.top
                anchors.topMargin: 5
                text: row.modelData.name
                font.pixelSize: 11
                color: Theme.c.text
                elide: Text.ElideMiddle
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: cancel.left
                anchors.rightMargin: 6
                anchors.top: name.bottom
                anchors.topMargin: 1
                text: row.failed && row.modelData.error
                      ? row.modelData.error
                      : row.modelData.preset + " · " + row.modelData.state
                font.pixelSize: 10
                color: row.failed ? Theme.c.error : Theme.c.textMuted
                elide: Text.ElideRight
            }

            // Only while it is running: a bar at zero on a queued job reads as stalled, and a
            // full bar on a finished one is noise once the state already says "Done".
            Rectangle {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 4
                height: 3
                radius: 1.5
                visible: row.running
                color: Theme.c.border

                Rectangle {
                    width: parent.width * Math.max(0, Math.min(1, row.modelData.progress))
                    height: parent.height
                    radius: parent.radius
                    color: Theme.c.accent

                    Behavior on width {
                        NumberAnimation { duration: 120 }
                    }
                }
            }

            ToolButton {
                id: cancel

                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.top: parent.top
                anchors.topMargin: 4
                label: "×"
                tip: qsTr("Cancel this export")
                visible: row.modelData.state === "Queued" || row.running
                onClicked: Media.cancelJob(row.modelData.id)
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
            text: Media.exporting ? qsTr("Exporting…") : qsTr("Idle")
            font.pixelSize: 10
            color: Media.exporting ? Theme.c.accent : Theme.c.textMuted
        }

        ToolButton {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            label: "⌫"
            tip: qsTr("Clear finished exports")
            onClicked: Media.clearFinished()
        }
    }
}
