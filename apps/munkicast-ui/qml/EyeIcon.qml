// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes

/// An eye, open or lashed shut, for the mixer's "show hidden" toggle.
///
/// Drawn rather than set in type for the reason ChainIcon.qml gives: 👁 resolves to Noto Color
/// Emoji on this system and a colour-emoji glyph cannot be tinted, so a latched toggle would
/// look exactly like an unlatched one -- which is the whole job of this control.
Item {
    id: icon

    /// Open: hidden strips are being shown. Closed: they are not.
    property bool open: true
    property color color: "white"

    implicitWidth: 14
    implicitHeight: 14

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        // Open: the almond, both halves. Closed: only the lower half, which is the lid come
        // down -- the same width and position, so the two states do not jump.
        ShapePath {
            strokeColor: icon.color
            strokeWidth: 1.2
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap

            startX: 1
            startY: icon.height / 2
            PathQuad {
                x: icon.width - 1
                y: icon.height / 2
                controlX: icon.width / 2
                controlY: icon.open ? 1 : icon.height / 2
            }
            PathQuad { x: 1; y: icon.height / 2
                       controlX: icon.width / 2; controlY: icon.height - 1 }
        }
    }

    // Open: a pupil. Closed: the almond stays and lashes go below it, which reads as shut
    // while keeping the two states the same shape and size -- a toggle that changes outline
    // is a toggle that moves the things beside it.
    Rectangle {
        anchors.centerIn: parent
        visible: icon.open
        width: Math.round(icon.height * 0.36)
        height: width
        radius: width / 2
        color: icon.color
    }

    // Lashes, below the closed lid and pointing down and out, which is how a shut eye is
    // drawn everywhere. Inward-pointing strokes read as a scribble at 14 pixels.
    Repeater {
        model: icon.open ? 0 : 3

        delegate: Rectangle {
            required property int index

            readonly property real spread: (index - 1) * 20

            x: icon.width * (0.5 + (index - 1) * 0.26) - width / 2
            y: icon.height * 0.62
            width: 1.2
            height: Math.round(icon.height * 0.22)
            radius: 0.6
            color: icon.color
            transformOrigin: Item.Top
            rotation: spread
        }
    }
}
