// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

Item {
    id: stat

    required property string label
    property var value: ""
    property bool warn: false

    implicitHeight: 18

    Text {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        text: stat.label
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Text {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        text: stat.value
        font.pixelSize: 11
        font.family: "monospace"
        color: stat.warn ? Theme.c.warning : Theme.c.text
    }
}
