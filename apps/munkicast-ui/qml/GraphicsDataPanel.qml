// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The fields this template takes.
///
/// Read from the layers rather than declared separately: a field exists because some layer is
/// bound to it, so there is no way for this list and the graphic to disagree.
Item {
    id: panel

    Text {
        anchors.centerIn: parent
        width: parent.width - 24
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        visible: Graphics.dataFields.length === 0
        text: Graphics.hasGraphic
              ? qsTr("No data fields. Give a text layer a field name to make it a slot the show fills in.")
              : qsTr("No graphic open")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 10
        spacing: 4
        visible: Graphics.dataFields.length > 0

        Text {
            width: parent.width
            text: qsTr("This template takes:")
            font.pixelSize: 10
            color: Theme.c.textMuted
            bottomPadding: 4
        }

        Repeater {
            model: Graphics.dataFields

            delegate: Text {
                required property string modelData

                width: parent.width
                text: modelData
                font.pixelSize: 11
                font.family: "monospace"
                color: Theme.c.text
            }
        }

        Item { width: 1; height: 10 }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("The exported page takes these through setFields({…}), so a browser source can be given new values without reloading it.")
            font.pixelSize: 10
            color: Theme.c.textMuted
        }
    }
}
