// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast
import QtQuick.Shapes

// The drag overlay of ADR-0016: the hovered panel's five zones, with the one under the cursor
// previewing the region the drop would produce.
//
// Drawn over live panel content -- the program canvas included, which can be any colour at all --
// so it is a translucent fill plus a clear border rather than an opaque block.
Item {
    id: overlay

    visible: Workspace.dragging
    z: 100

    readonly property rect target: Workspace.dropTargetRect
    readonly property rect centre: Workspace.dropCentreRect
    readonly property rect preview: Workspace.dropPreview
    readonly property bool overPanel: target.width > 0 && target.height > 0
    readonly property bool refused: Workspace.dragging && !Workspace.dropValid && overPanel

    // The target's own two diagonals, plus the centre rectangle carved out of the middle. Not a
    // 3x3 grid: with diagonals, every point belongs to the zone it is nearest to in the direction
    // that matters, so the target is as large as it can be and the gesture stays forgiving at
    // speed. The lines follow the panel's aspect ratio, which is why a wide panel shows wide,
    // shallow top and bottom zones.
    Shape {
        visible: overlay.overPanel
        anchors.fill: parent
        antialiasing: true
        opacity: 0.5

        ShapePath {
            strokeColor: Theme.c.dropBorder
            strokeWidth: 1
            fillColor: "transparent"
            startX: overlay.target.x
            startY: overlay.target.y

            PathLine {
                x: overlay.target.x + overlay.target.width
                y: overlay.target.y + overlay.target.height
            }
        }

        ShapePath {
            strokeColor: Theme.c.dropBorder
            strokeWidth: 1
            fillColor: "transparent"
            startX: overlay.target.x + overlay.target.width
            startY: overlay.target.y

            PathLine {
                x: overlay.target.x
                y: overlay.target.y + overlay.target.height
            }
        }
    }

    // The centre rectangle, drawn filled with the panel background so the diagonals appear to
    // stop at its edge. Its corners only lie on the diagonals when neither dimension hit a clamp,
    // so the lines are drawn in full and covered rather than trimmed to a point they might miss.
    Rectangle {
        visible: overlay.overPanel
        x: overlay.centre.x
        y: overlay.centre.y
        width: overlay.centre.width
        height: overlay.centre.height
        color: Theme.c.panelBackground
        opacity: 0.55
        border.width: 1
        border.color: Qt.rgba(Theme.c.dropBorder.r, Theme.c.dropBorder.g, Theme.c.dropBorder.b,
                              0.5)
        radius: 2
    }

    Text {
        visible: overlay.overPanel && overlay.centre.width > 70
        x: overlay.centre.x
        y: overlay.centre.y + overlay.centre.height / 2 - height / 2
        width: overlay.centre.width
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("tab")
        font.pixelSize: 11
        color: Theme.c.textMuted
        opacity: 0.8
    }

    // What releasing here would produce. ADR-0016 rule 2: the highlight previews the *resulting*
    // region, so the outcome is visible before it is committed.
    Rectangle {
        id: previewRect

        // Gliding between zones is what makes the gesture feel like it tracks the pointer rather
        // than snapping between states -- but only *between* zones. On first appearance the
        // animation would grow the highlight out of a zero-size rect at the origin, which reads
        // as a glitch and, for the first seventy milliseconds, shows the operator nothing at all
        // where the panel is going to land. So animations are off until one frame after the
        // highlight appears.
        property bool settled: false

        visible: overlay.preview.width > 0 && overlay.preview.height > 0
        x: overlay.preview.x
        y: overlay.preview.y
        width: overlay.preview.width
        height: overlay.preview.height
        color: Theme.c.dropFill
        border.width: 2
        border.color: Theme.c.dropBorder
        radius: 2

        onVisibleChanged: {
            if (visible)
                settleTimer.restart();
            else
                settled = false;
        }

        Timer {
            id: settleTimer

            interval: 1
            onTriggered: previewRect.settled = true
        }

        Behavior on x {
            enabled: previewRect.settled

            NumberAnimation {
                duration: 70
                easing.type: Easing.OutQuad
            }
        }
        Behavior on y {
            enabled: previewRect.settled

            NumberAnimation {
                duration: 70
                easing.type: Easing.OutQuad
            }
        }
        Behavior on width {
            enabled: previewRect.settled

            NumberAnimation {
                duration: 70
                easing.type: Easing.OutQuad
            }
        }
        Behavior on height {
            enabled: previewRect.settled

            NumberAnimation {
                duration: 70
                easing.type: Easing.OutQuad
            }
        }
    }

    // ADR-0016 rule 8: a drop that would leave a panel below its minimum usable size does not
    // highlight and does nothing. Saying so is better than a zone that simply fails to light up,
    // which reads as the gesture not being recognised.
    Rectangle {
        visible: overlay.refused
        x: overlay.target.x
        y: overlay.target.y
        width: overlay.target.width
        height: overlay.target.height
        color: Qt.rgba(Theme.c.error.r, Theme.c.error.g, Theme.c.error.b, 0.12)
        border.width: 1
        border.color: Theme.c.error
        radius: 2
    }
}
