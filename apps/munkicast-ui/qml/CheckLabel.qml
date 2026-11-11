// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

Item {
    id: check

    property alias text: label.text
    property bool checked: false

    signal toggled

    activeFocusOnTab: true
    Accessible.role: Accessible.CheckBox
    Accessible.name: text
    Accessible.checked: checked
    Accessible.onToggleAction: check.toggled()
    Keys.onSpacePressed: check.toggled()
    Keys.onReturnPressed: check.toggled()

    implicitWidth: 19 + label.implicitWidth
    implicitHeight: Math.max(13, label.implicitHeight)

    Rectangle {
        id: box

        objectName: "checkboxSquare"
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 13
        height: 13
        radius: 2
        color: check.checked ? Theme.c.accent : "transparent"
        border.width: 1
        border.color: mouse.containsMouse || check.activeFocus ? Theme.c.accent : Theme.c.border

        Text {
            anchors.centerIn: parent
            visible: check.checked
            text: "✓"
            font.pixelSize: 10
            color: Theme.c.window
        }
    }

    Text {
        id: label

        objectName: "checkboxLabel"
        anchors.left: box.right
        anchors.leftMargin: 6
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        elide: Text.ElideRight
        font.pixelSize: 11
        color: Theme.c.text
    }

    MouseArea {
        id: mouse

        anchors.fill: parent
        hoverEnabled: true
        onClicked: check.toggled()
    }
}
