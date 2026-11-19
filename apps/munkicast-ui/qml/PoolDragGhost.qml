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

    objectName: "poolDragGhost"

    property string path: ""
    property string label: ""
    /// Set by the panel when a press has moved far enough to be a drag.
    property bool dragging: false

    /// What the drop target sees. The Source viewer reads the uri from here, so it does not
    /// have to reach back into the panel the drag came from.
    Drag.active: ghost.dragging
    // The hot spot is the pointer: the ghost sits just right of and below it, and the
    // drop lands where the pointer is, not 8px away.
    Drag.hotSpot.x: 0
    Drag.hotSpot.y: 0
    // Keys as well as data: for an internal drag a DropArea matches its `keys` against
    // these, not against the mime formats, so without them every target refused the drop.
    Drag.keys: ["text/uri-list"]
    Drag.mimeData: ({"text/uri-list": "file://" + ghost.path})
    Drag.dragType: Drag.Internal
    Drag.supportedActions: Qt.CopyAction

    // A real size, because Qt drags an item by moving it and a one-pixel item has nowhere to
    // be. This is the label that follows the cursor.
    width: caption.implicitWidth + 14
    height: caption.implicitHeight + 8
    visible: ghost.dragging
    z: 95

    Rectangle {
        anchors.fill: parent
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
}
