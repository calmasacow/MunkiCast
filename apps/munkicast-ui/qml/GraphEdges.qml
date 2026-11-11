// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import MunkiCast

/// The curves between nodes.
///
/// One Shape holding every edge rather than one Shape each: a Shape is a scene-graph node with
/// its own render pass, and thirty of them to draw thirty curves costs far more than thirty
/// paths in one.
///
/// Cubic rather than straight, with horizontal control handles, so an edge leaves a port
/// sideways and arrives sideways. That is not decoration -- it is what makes two edges that
/// cross distinguishable from two that meet.
Item {
    id: edges

    /// Each entry is {x1, y1, x2, y2, lit}. `lit` draws the edge in the accent colour, which
    /// is how the graph says which connections the selected node is part of.
    required property var links

    // One Shape per edge rather than one holding them all.
    //
    // A ShapePath is not an Item, so a Repeater cannot parent it and an Instantiator pushing
    // into `Shape.data` does not attach it either -- both leave a Shape with no paths and no
    // complaint. A Repeater of Shapes is the shape of this that actually draws, and each one
    // is a stroked curve over nothing, so the extra nodes cost little.
    Repeater {
        model: edges.links

        delegate: Shape {
            id: edge

            required property var modelData

            readonly property real dx: Math.max(40, Math.abs(modelData.x2 - modelData.x1) * 0.5)

            anchors.fill: parent
            antialiasing: true
            preferredRendererType: Shape.CurveRenderer
            // On the Shape: a ShapePath has no opacity of its own.
            opacity: modelData.lit ? 1.0 : 0.55

            ShapePath {
                // `divider` is the gap between two panels and is meant to disappear against
                // the window; on this surface an edge drawn in it is invisible. `textMuted`
                // is the quietest token that still reads against the background, which is
                // what a connection nobody has selected should be.
                strokeColor: edge.modelData.lit ? Theme.c.accent : Theme.c.textMuted
                strokeWidth: edge.modelData.lit ? 2 : 1.5
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                startX: edge.modelData.x1
                startY: edge.modelData.y1

                PathCubic {
                    x: edge.modelData.x2
                    y: edge.modelData.y2
                    control1X: edge.modelData.x1 + edge.dx
                    control1Y: edge.modelData.y1
                    control2X: edge.modelData.x2 - edge.dx
                    control2Y: edge.modelData.y2
                }
            }
        }
    }
}
