// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// A label that follows the pointer. Deliberately not a popup window: ADR-0015 says the
/// workspace is one surface, and a tooltip that is its own toplevel is one more thing a
/// compositor can place somewhere surprising.
///
/// It takes the hovering MouseArea rather than the item being described, and that is the whole
/// fix for how this used to behave. The first version positioned itself from
/// `anchorItem.mapToItem(parent, 0, 0)` -- which reads like a binding and is not one. `mapToItem`
/// is a function call over values QML does not watch, so it was evaluated once, when the panel
/// was built and before anything had been laid out, and never again. Every tooltip in the
/// program therefore sat wherever its button had been at construction time, which was the
/// top-left corner; and for panels that genuinely live near the top, `y - height - 4` went
/// negative and the tooltip rendered off the window entirely. That is why the scenes panel
/// appeared to have no tooltips at all while other panels merely had misplaced ones.
///
/// `mouseX`/`mouseY` are notifying properties, so reading them here is a real binding that
/// re-evaluates as the pointer moves.
Rectangle {
    id: tip

    /// The hovering MouseArea. Its `containsMouse` is normally what drives `visible` too.
    required property MouseArea hoverArea
    property alias text: label.text
    /// Clear of the pointer itself, and of the usual cursor hotspot size.
    property int offsetX: 14
    property int offsetY: 18

    parent: tip.hoverArea ? tip.hoverArea.Window.contentItem : null

    /// Where the pointer is, in the overlay's coordinates.
    readonly property point cursor: (tip.hoverArea && tip.parent)
        ? tip.hoverArea.mapToItem(tip.parent, tip.hoverArea.mouseX, tip.hoverArea.mouseY)
        : Qt.point(0, 0)

    // Kept inside the window on both axes. A tooltip that explains a button in the bottom-right
    // corner is no use rendered past the edge of the surface.
    x: tip.parent
        ? Math.max(4, Math.min(tip.cursor.x + tip.offsetX, tip.parent.width - width - 4))
        : 0
    // Below the pointer by default, flipped above when there is no room below.
    y: tip.parent
        ? (tip.cursor.y + tip.offsetY + height + 4 > tip.parent.height
               ? Math.max(4, tip.cursor.y - tip.offsetY - height)
               : tip.cursor.y + tip.offsetY)
        : 0
    z: 500
    width: label.implicitWidth + 12
    height: label.implicitHeight + 8
    radius: 3
    color: Theme.c.panelHeaderActive
    border.width: 1
    border.color: Theme.c.border
    opacity: visible ? 1 : 0

    Behavior on opacity {
        NumberAnimation {
            duration: 90
        }
    }

    Text {
        id: label

        anchors.centerIn: parent
        font.pixelSize: 11
        color: Theme.c.text
    }
}
