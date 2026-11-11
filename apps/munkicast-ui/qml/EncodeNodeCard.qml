// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// One node on the encode graph: a titled card with a body and ports on its edges.
///
/// One component for all five kinds -- canvas, audio track, encoder, audio encoder,
/// destination -- because they differ only in what goes in the body. Five near-identical
/// cards would drift apart the first time the selection outline changed.
///
/// The graph owns wire gestures; the card supplies its identity and typed landing points.
Item {
    id: card

    required property string title
    property string nodeKind: ""
    property string nodeId: ""
    property Item connectionGraph: null
    property string acceptedSourceKind: ""
    /// Drawn after the title in the muted colour: a resolution, a bitrate, a codec.
    property string subtitle: ""
    property bool selected: false
    property bool missingConnections: false
    readonly property color outlineColor: missingConnections ? Theme.c.error
                                          : selected ? Theme.c.accent : Theme.c.border
    /// Whether to draw a port on the left edge, the right edge, or both. A canvas has no
    /// input and a destination has no output, and drawing one anyway would promise an edge
    /// that cannot be made.
    property bool hasInput: false
    property bool hasOutput: false
    property var inputPorts: hasInput ? [{key: "input", y: 30, label: qsTr("Input"),
                                        accepts: acceptedSourceKind}] : []
    readonly property real bodyTop: bodySlot.y
    /// Where edges attach, in the graph's coordinates. Read by GraphEdges.
    readonly property point inPort: Qt.point(card.x, card.y + 30)
    readonly property point outPort: Qt.point(card.x + card.width, card.y + 30)
    /// Filled by the node, drawn at the right of the title row.
    property alias status: statusSlot.data
    default property alias body: bodySlot.data

    signal pressed(real mouseX, real mouseY)
    signal dragged(real mouseX, real mouseY)
    signal released
    signal contextRequested(real mouseX, real mouseY)
    signal disconnectRequested(string port)

    implicitWidth: 260
    implicitHeight: header.height + bodySlot.implicitHeight + 10

    Rectangle {
        anchors.fill: parent
        radius: 2
        color: Theme.c.panelBackground
        border.width: 1
        border.color: card.outlineColor
    }

    // Ports sit on the border. Each destination track has its own labelled landing point.
    Repeater {
        model: card.inputPorts.concat(card.hasOutput
               ? [{key: "output", y: 30, label: qsTr("Output")}] : [])

        delegate: Rectangle {
            id: port
            required property var modelData

            objectName: "encodePort_" + modelData.key
            x: (modelData.key === "output" ? card.width : 0) - width / 2
            y: modelData.y - height / 2
            width: 12
            height: 12
            radius: 6
            z: 2
            color: dropZone.containsDrag ? Theme.c.good
                 : card.selected ? Theme.c.accent : Theme.c.textMuted
            border.width: activeFocus ? 1 : 0
            border.color: Theme.c.text
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Disconnect %1").arg(modelData.label)
            Accessible.onPressAction: card.disconnectRequested(modelData.key)
            Keys.onPressed: event => {
                if ((event.modifiers & Qt.AltModifier) &&
                    [Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space].includes(event.key)) {
                    card.disconnectRequested(modelData.key);
                    event.accepted = true;
                }
            }
            MouseArea {
                id: portMouse
                anchors.fill: parent
                anchors.margins: -4
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.PointingHandCursor
                property bool disconnecting: false
                onPressed: mouse => {
                    disconnecting = (mouse.modifiers & Qt.AltModifier) !== 0;
                    if (disconnecting)
                        return;
                    if (port.modelData.key === "output" && card.connectionGraph)
                        card.connectionGraph.beginWire(card, port);
                    else
                        mouse.accepted = false;
                }
                onPositionChanged: mouse => {
                    if (pressed && !disconnecting && port.modelData.key === "output" && card.connectionGraph) {
                        const at = portMouse.mapToItem(card.connectionGraph, mouse.x, mouse.y);
                        card.connectionGraph.moveWire(at.x, at.y);
                    }
                }
                onReleased: {
                    if (!disconnecting && port.modelData.key === "output" && card.connectionGraph)
                        card.connectionGraph.endWire();
                }
                onCanceled: {
                    if (card.connectionGraph)
                        card.connectionGraph.cancelWire();
                }
                onClicked: {
                    if (disconnecting)
                        card.disconnectRequested(port.modelData.key);
                }
            }
            DropArea {
                id: dropZone
                anchors.fill: parent
                anchors.margins: -4
                enabled: port.modelData.key !== "output"
                keys: [port.modelData.accepts || ""]
                onEntered: drag => {
                    if (!card.connectionGraph || drag.source !== card.connectionGraph.dragItem)
                        drag.accepted = false;
                }
                onDropped: drop => {
                    if (card.connectionGraph && card.connectionGraph.dropWire(
                            drop.source, card.nodeKind, card.nodeId, port.modelData.key))
                        drop.acceptProposedAction();
                }
            }
        }
    }

    Item {
        id: header

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 24

        Text {
            id: titleText

            anchors.left: parent.left
            anchors.leftMargin: 9
            anchors.verticalCenter: parent.verticalCenter
            text: card.title
            font.pixelSize: 11
            font.weight: Font.DemiBold
            color: Theme.c.text
            elide: Text.ElideRight
            width: Math.min(implicitWidth, parent.width - statusSlot.width - 18)
        }

        Text {
            anchors.left: titleText.right
            anchors.leftMargin: 6
            anchors.right: statusSlot.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            visible: card.subtitle !== ""
            text: card.subtitle
            font.pixelSize: 10
            color: Theme.c.textMuted
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignRight
        }

        Item {
            id: statusSlot

            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: childrenRect.width
            implicitHeight: childrenRect.height
            width: implicitWidth
            height: implicitHeight
        }
    }

    Item {
        id: bodySlot

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 1
        implicitHeight: childrenRect.height + 8
    }

    // Above the card, below its body: a press anywhere that is not a control selects and
    // starts a drag, and a control inside the body takes its own events first.
    MouseArea {
        anchors.fill: parent
        z: -1
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: mouse => {
            if (mouse.button === Qt.RightButton)
                card.contextRequested(mouse.x, mouse.y);
            else
                card.pressed(mouse.x, mouse.y);
        }
        onPositionChanged: mouse => {
            if (pressedButtons & Qt.LeftButton)
                card.dragged(mouse.x, mouse.y);
        }
        onReleased: mouse => {
            if (mouse.button === Qt.LeftButton)
                card.released();
        }
        onCanceled: card.released
    }
}
