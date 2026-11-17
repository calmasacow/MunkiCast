// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The stage: the graphic as it stands at the playhead.
///
/// Items rather than a Canvas, which is the opposite of the timeline's choice and for the
/// opposite reason. A graphic is a handful of layers that are dragged, selected and typed into;
/// real items give hit testing, text rendering and transforms for free, and there are never
/// enough of them for the item count to matter.
Item {
    id: panel

    readonly property var doc: Graphics.graphic
    readonly property bool ready: Graphics.hasGraphic

    /// Stage pixels to screen pixels.
    readonly property real zoom: panel.ready
        ? Math.min((panel.width - 40) / panel.doc.width,
                   (panel.height - 40) / panel.doc.height)
        : 1

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
    }

    Text {
        anchors.centerIn: parent
        visible: !panel.ready
        text: qsTr("No graphic open. Make one from the Library panel.")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    // The stage itself. Checkerboard, because a broadcast graphic is transparent everywhere it
    // has not been drawn on and a flat background would hide exactly that.
    Rectangle {
        id: stage

        anchors.centerIn: parent
        visible: panel.ready
        width: panel.ready ? panel.doc.width * panel.zoom : 0
        height: panel.ready ? panel.doc.height * panel.zoom : 0
        color: "#1a1a1a"
        border.width: 1
        border.color: Theme.c.border

        Canvas {
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();
                const s = 10;
                for (let y = 0; y < height; y += s) {
                    for (let x = 0; x < width; x += s) {
                        ctx.fillStyle = ((x / s + y / s) % 2 === 0) ? "#1f1f1f" : "#262626";
                        ctx.fillRect(x, y, s, s);
                    }
                }
            }
        }

        Repeater {
            model: Graphics.frame

            delegate: Item {
                id: layer

                required property var modelData

                x: layer.modelData.x * panel.zoom
                y: layer.modelData.y * panel.zoom
                width: layer.modelData.width * panel.zoom
                height: layer.modelData.height * panel.zoom
                rotation: layer.modelData.rotation
                opacity: layer.modelData.visible ? layer.modelData.opacity : 0
                transformOrigin: Item.TopLeft

                Rectangle {
                    anchors.fill: parent
                    visible: layer.modelData.kind === "rect"
                    color: layer.modelData.color
                }

                Text {
                    anchors.fill: parent
                    visible: layer.modelData.kind === "text"
                    // The bound field shows its own name in a preview, so a designer can see
                    // which layer is a slot and which is fixed copy.
                    text: layer.modelData.dataField
                          ? layer.modelData.text + " {" + layer.modelData.dataField + "}"
                          : layer.modelData.text
                    color: layer.modelData.color
                    font.pixelSize: Math.max(1, layer.modelData.fontSize * panel.zoom)
                    verticalAlignment: Text.AlignVCenter
                }

                Image {
                    anchors.fill: parent
                    visible: layer.modelData.kind === "image"
                    source: layer.modelData.source ? "file://" + layer.modelData.source : ""
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                }

                // The selection box is outside the layer's own opacity, so a layer faded to
                // nothing can still be found and dragged.
                Rectangle {
                    anchors.fill: parent
                    visible: layer.modelData.selected
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.c.accent
                    opacity: 1.0
                }

                MouseArea {
                    id: drag

                    anchors.fill: parent
                    enabled: !layer.modelData.locked
                    acceptedButtons: Qt.LeftButton

                    property real grabX: 0
                    property real grabY: 0

                    onPressed: mouse => {
                        Graphics.selectLayer(layer.modelData.id);
                        grabX = mouse.x;
                        grabY = mouse.y;
                    }

                    onPositionChanged: mouse => {
                        if (!pressed)
                            return;
                        // In stage coordinates, not screen: the document is authored at its own
                        // size whatever the panel is scaled to.
                        const dx = (mouse.x - grabX) / panel.zoom;
                        const dy = (mouse.y - grabY) / panel.zoom;
                        Graphics.moveSelectedLayer(layer.modelData.x + dx,
                                                   layer.modelData.y + dy);
                    }

                    onReleased: Graphics.endGesture()
                }
            }
        }
    }

    // The safe area, which is the one overlay a broadcast designer actually needs: text outside
    // it gets cropped on someone's screen.
    Rectangle {
        anchors.centerIn: stage
        width: stage.width * 0.9
        height: stage.height * 0.9
        visible: panel.ready
        color: "transparent"
        border.width: 1
        border.color: Theme.c.textMuted
        opacity: 0.25
    }

    Text {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 8
        visible: panel.ready
        text: panel.ready ? panel.doc.width + "×" + panel.doc.height + " · " +
                            Math.round(panel.zoom * 100) + "%" : ""
        font.pixelSize: 10
        font.family: "monospace"
        color: Theme.c.textMuted
    }
}
