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
    /// The size of what was picked up -- the tile or the row -- and where in it the press
    /// was. Both set on press. The ghost is that outline, placed so the pointer stays on the
    /// spot that was pressed, as if the item itself had been lifted.
    property size itemSize: Qt.size(120, 24)
    property point grabbed: Qt.point(0, 0)

    // The hot spot is the pointer, so the drop lands where the pointer is. Bound, because a
    // press-time write to `hotSpot.x` changes a copy and is lost.
    Drag.hotSpot.x: ghost.grabbed.x
    Drag.hotSpot.y: ghost.grabbed.y
    Drag.onActiveChanged: Edit.setDragCursor(ghost.Drag.active)
    // Keys as well as data: for an internal drag a DropArea matches its `keys` against
    // these, not against the mime formats, so without them every target refused the drop.
    Drag.keys: ["text/uri-list"]
    Drag.mimeData: ({"text/uri-list": "file://" + ghost.path})
    Drag.dragType: Drag.Internal
    Drag.supportedActions: Qt.CopyAction

    width: ghost.itemSize.width
    height: ghost.itemSize.height
    visible: ghost.dragging
    z: 95

    // Unfilled: what is under it stays readable, which is the point of looking while carrying.
    Rectangle {
        objectName: "poolDragOutline"
        anchors.fill: parent
        radius: 3
        color: "transparent"
        border.width: 2
        border.color: Theme.c.accent
    }
}
