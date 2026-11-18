// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// What a file dragged out of the Media Pool looks like while it is in the air.
///
/// Qt's own drag machinery rather than the shell's: this drag starts and ends inside one
/// workspace, so it has none of the docking gesture's business with panels and drop zones
/// (ADR-0016), and borrowing that would be a lot of apparatus for a label following a cursor.
Item {
    id: ghost

    property string path: ""
    property string label: ""

    /// Told to the drop target through the drag's own mime data, so the Source viewer does
    /// not have to reach back into the panel the drag came from.
    Drag.active: dragArea.drag.active
    Drag.hotSpot.x: width / 2
    Drag.hotSpot.y: height / 2
    Drag.mimeData: ({"text/uri-list": "file://" + ghost.path})
    Drag.dragType: Drag.Internal

    width: 1
    height: 1

    function drop() {
        if (ghost.Drag.active)
            ghost.Drag.drop();
        ghost.x = 0;
        ghost.y = 0;
    }

    // The label that follows the cursor. Parented to the window so it is not clipped by the
    // panel the drag started in.
    Rectangle {
        parent: ghost.Window.contentItem
        visible: ghost.Drag.active
        z: 90
        x: ghost.mapToItem(parent, 0, 0).x + 12
        y: ghost.mapToItem(parent, 0, 0).y + 12
        width: caption.implicitWidth + 14
        height: caption.implicitHeight + 8
        radius: 3
        color: Theme.c.panelBackground
        border.color: Theme.c.accent
        opacity: 0.92

        Text {
            id: caption

            anchors.centerIn: parent
            text: ghost.label
            font.pixelSize: 10
            color: Theme.c.text
        }
    }

    // `drag.target` on the caller's MouseArea needs a target with a drag of its own for
    // `Drag.active` to mean anything.
    MouseArea {
        id: dragArea

        anchors.fill: parent
        enabled: false
        drag.target: ghost
    }
}
