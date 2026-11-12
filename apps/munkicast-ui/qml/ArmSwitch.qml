// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The arming toggle for one output: green and right when armed, red and left when not.
/// Shared by the Outputs panel and the Encoder Flow node so the two always look the same.
Rectangle {
    id: toggle

    property bool checked: false
    property string name: ""
    signal toggled

    width: 26
    height: 14
    radius: height / 2
    color: Theme.c.panelHeader
    border.width: 1
    border.color: activeFocus ? Theme.c.accent : Theme.c.border
    activeFocusOnTab: visible
    Accessible.role: Accessible.CheckBox
    Accessible.name: qsTr("Arm %1").arg(toggle.name)
    Accessible.checkable: true
    Accessible.checked: toggle.checked
    Accessible.onToggleAction: toggle.toggled()
    Keys.onSpacePressed: toggle.toggled()
    Keys.onReturnPressed: toggle.toggled()

    Rectangle {
        objectName: "encodeArmingIndicator"
        width: toggle.height - 4
        height: width
        radius: width / 2
        anchors.verticalCenter: parent.verticalCenter
        x: toggle.checked ? toggle.width - width - 2 : 2
        color: toggle.checked ? Theme.c.good : Theme.c.error
        Behavior on x { NumberAnimation { duration: 90 } }
    }
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: toggle.toggled()
    }
}
