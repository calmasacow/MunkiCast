// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

// The panel being carried. ADR-0016 rule 1: grabbing a handle puts the drag into a distinct
// movable state, and that state has to be visible or the gesture feels like nothing happened.
Rectangle {
    id: ghost

    visible: Workspace.dragging
    z: 200
    x: Workspace.dragPos.x + 14
    y: Workspace.dragPos.y + 12
    width: ghostLabel.implicitWidth + 22
    height: 26
    radius: 3
    opacity: 0.94
    color: Theme.c.panelHeaderActive
    border.width: 1
    border.color: Workspace.dropValid ? Theme.c.accent : Theme.c.error

    Text {
        id: ghostLabel

        anchors.centerIn: parent
        text: Workspace.dragTitle
        font.pixelSize: 12
        font.weight: Font.DemiBold
        color: Theme.c.text
    }
}
