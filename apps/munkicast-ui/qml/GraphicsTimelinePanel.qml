// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// Layers down, frames across, with a keyframe diamond per channel.
Item {
    id: panel

    readonly property var doc: Graphics.graphic
    readonly property bool ready: Graphics.hasGraphic
    readonly property int headerWidth: 150
    readonly property int rowHeight: 20

    readonly property real scale: panel.ready && panel.doc.duration > 0
        ? Math.max(1, (panel.width - panel.headerWidth - 20) / panel.doc.duration)
        : 1

    function xOf(frame) {
        return panel.headerWidth + frame * panel.scale;
    }

    function frameAt(x) {
        return Math.max(0, Math.min(panel.doc.duration,
                                    Math.round((x - panel.headerWidth) / panel.scale)));
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
    }

    Text {
        anchors.centerIn: parent
        visible: !panel.ready
        text: qsTr("No graphic open")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    // ---- ruler ----
    Rectangle {
        id: ruler

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 20
        visible: panel.ready
        color: Theme.c.panelHeader

        Repeater {
            // A tick every second. A graphic is seconds long, so seconds are the only unit
            // that needs labelling.
            model: panel.ready ? Math.floor(panel.doc.duration / panel.doc.fps) + 1 : 0

            delegate: Item {
                required property int index

                x: panel.xOf(index * panel.doc.fps)
                height: ruler.height

                Rectangle {
                    width: 1
                    height: parent.height
                    color: Theme.c.border
                }

                Text {
                    x: 3
                    anchors.verticalCenter: parent.verticalCenter
                    text: parent.index + "s"
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            onPressed: mouse => Graphics.setPlayhead(panel.frameAt(mouse.x))
            onPositionChanged: mouse => { if (pressed) Graphics.setPlayhead(panel.frameAt(mouse.x)); }
        }
    }

    // ---- layers ----
    ListView {
        id: rows

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: ruler.bottom
        anchors.bottom: footer.top
        visible: panel.ready
        clip: true
        model: panel.ready ? panel.doc.layers : []
        interactive: false
        boundsBehavior: Flickable.StopAtBounds

        NoDragScroll {}

        delegate: Item {
            id: layerRow

            required property var modelData
            required property int index

            width: ListView.view.width
            height: panel.rowHeight

            readonly property bool current: layerRow.modelData.id === Graphics.selectedLayer

            Rectangle {
                anchors.fill: parent
                color: layerRow.current ? Theme.c.accent
                     : layerRow.index % 2 === 0 ? Theme.c.panelBackground
                     : "transparent"
                opacity: layerRow.current ? 0.2 : 1.0
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                width: panel.headerWidth - 20
                text: layerRow.modelData.name
                font.pixelSize: 11
                color: layerRow.modelData.visible ? Theme.c.text : Theme.c.textMuted
                elide: Text.ElideRight
            }

            MouseArea {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: panel.headerWidth
                onClicked: Graphics.selectLayer(layerRow.modelData.id)
            }

            // Every key of every channel on one lane. A row per channel would be truer to the
            // document and six times as tall; a designer scrubbing a lower third wants to see
            // where the motion is, and that is what this shows.
            Repeater {
                model: {
                    const out = [];
                    const channels = layerRow.modelData.channels;
                    for (const name in channels) {
                        for (const key of channels[name].keys)
                            out.push(key.at);
                    }
                    return out;
                }

                delegate: Rectangle {
                    required property int modelData

                    x: panel.xOf(modelData) - 3
                    y: (panel.rowHeight - 6) / 2
                    width: 6
                    height: 6
                    rotation: 45
                    color: layerRow.current ? Theme.c.accent : Theme.c.textMuted
                }
            }
        }
    }

    // ---- playhead ----
    Rectangle {
        visible: panel.ready
        x: panel.xOf(panel.ready ? panel.doc.playhead : 0)
        y: 0
        width: 1
        height: footer.y
        color: Theme.c.error
    }

    Rectangle {
        id: footer

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 24
        visible: panel.ready
        color: Theme.c.panelHeader

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: panel.ready
                  ? qsTr("frame %1 of %2").arg(panel.doc.playhead).arg(panel.doc.duration)
                  : ""
            font.pixelSize: 10
            font.family: "monospace"
            color: Theme.c.textMuted
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            // One diamond per channel: lit when the selected layer is keyed at this frame,
            // which is the control every keyframe animator reaches for first.
            Repeater {
                model: ["x", "y", "width", "height", "rotation", "opacity"]

                delegate: Rectangle {
                    id: diamond

                    required property string modelData

                    /// Whether the selected layer is keyed on this channel at the playhead.
                    ///
                    /// A plain property refreshed on `changed`, not a binding: `hasKeyframe`
                    /// is a function call, and a binding on one would never be re-evaluated
                    /// when the document behind it moves.
                    property bool keyed: false

                    function refresh() {
                        diamond.keyed = Graphics.selectedLayer !== "" &&
                                        Graphics.hasKeyframe(diamond.modelData);
                    }

                    Component.onCompleted: diamond.refresh()

                    Connections {
                        target: Graphics

                        // `diamond`, not `parent`: inside Connections the parent is the Row.
                        function onChanged() { diamond.refresh(); }
                        function onSelectionChanged() { diamond.refresh(); }
                    }

                    width: 20
                    height: 20
                    radius: 2
                    color: keyMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                    border.width: 1
                    border.color: Theme.c.border
                    opacity: Graphics.selectedLayer ? 1.0 : 0.35

                    Rectangle {
                        anchors.centerIn: parent
                        width: 7
                        height: 7
                        rotation: 45
                        color: diamond.keyed ? Theme.c.accent : "transparent"
                        border.width: 1
                        border.color: diamond.keyed ? Theme.c.accent : Theme.c.textMuted
                    }

                    MouseArea {
                        id: keyMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: Graphics.selectedLayer !== ""
                        onClicked: Graphics.setKeyframe(diamond.modelData, !diamond.keyed)
                    }

                    ToolTipText {
                        text: diamond.keyed
                              ? qsTr("Remove the %1 keyframe here").arg(diamond.modelData)
                              : qsTr("Key %1 at this frame").arg(diamond.modelData)
                        visible: keyMouse.containsMouse
                        hoverArea: keyMouse
                    }
                }
            }
        }
    }
}
