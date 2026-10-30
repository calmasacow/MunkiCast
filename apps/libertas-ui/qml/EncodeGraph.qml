// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import Libertas

/// The node graph: sources on the left, encoders in the middle, destinations on the right.
///
/// The left column is not placeable. Canvases and audio tracks are what the project *has*
/// rather than what the operator arranged, so they stack in order and stay put; only encoders
/// and destinations carry a position. If that ever changes, they need a position in the
/// document too -- see the ponytail note on Canvas in document.hpp.
///
/// Panning is middle-drag or space-drag, never a Flickable: ADR-0039 says nothing in this
/// program scrolls by being dragged, because a drag that scrolls is a drag that cannot also
/// move a node.
Item {
    id: graph
    objectName: "encodeGraph"

    required property string selectedKind
    required property string selectedId

    signal nodeSelected(string kind, string id)
    signal removeRequested(string kind, string id, string name)

    function revealOutput(id) {
        for (let i = 0; i < outputRepeater.count; ++i) {
            const card = outputRepeater.itemAt(i);
            if (card && card.outputId === id) {
                graph.panX = (graph.width - card.width) / 2 - card.positionX;
                graph.panY = (graph.height - card.height) / 2 - card.positionY;
                return;
            }
        }
    }

    function nodeMenu(card, kind, id, name, mx, my) {
        graph.nodeSelected(kind, id);
        const at = card.mapToItem(graph, mx, my);
        contextMenu.popup(at.x, at.y, [{text: qsTr("Remove…"), action: function() {
            graph.removeRequested(kind, id, name);
        }}]);
    }

    /// Graph-space offset of the view. Pan moves this; nodes are drawn at position + offset.
    property real panX: 0
    property real panY: 0

    property string wireSourceKind: ""
    property string wireSourceId: ""
    readonly property bool wireActive: wireSourceId !== ""
    property point wireStart: Qt.point(0, 0)
    property point wireEnd: Qt.point(0, 0)
    readonly property Item dragItem: wireCursor

    function beginWire(card, port) {
        graph.cancelWire();
        graph.nodeSelected(card.nodeKind, card.nodeId);
        graph.forceActiveFocus();
        graph.wireStart = port.mapToItem(graph, port.width / 2, port.height / 2);
        graph.wireEnd = graph.wireStart;
        graph.wireSourceKind = card.nodeKind;
        graph.wireSourceId = card.nodeId;
    }
    function moveWire(x, y) {
        if (graph.wireActive)
            graph.wireEnd = Qt.point(x, y);
    }
    function dropWire(source, kind, id, port) {
        return graph.wireActive && source === wireCursor &&
               Document.connectGraphPorts(graph.wireSourceKind, graph.wireSourceId, kind, id, port);
    }
    function endWire() {
        if (graph.wireActive)
            wireCursor.Drag.drop();
        graph.wireSourceId = "";
        graph.wireSourceKind = "";
    }
    function cancelWire() {
        wireCursor.Drag.cancel();
        graph.wireSourceId = "";
        graph.wireSourceKind = "";
    }
    Keys.onEscapePressed: graph.cancelWire()

    Item {
        id: wireCursor
        x: graph.wireEnd.x
        y: graph.wireEnd.y
        width: 1
        height: 1
        Drag.active: graph.wireActive
        Drag.source: wireCursor
        Drag.keys: [graph.wireSourceKind]
        Drag.supportedActions: Qt.LinkAction
        Drag.proposedAction: Qt.LinkAction
    }

    readonly property real railX: 30
    readonly property real railTop: 66
    readonly property real railSpacing: 14

    function isSelected(kind, id) {
        return graph.selectedKind === kind && graph.selectedId === id;
    }

    clip: true

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
    }

    // Reserve the watermark's corner visually and for pointer input while the graph pans.
    Rectangle {
        objectName: "encodeWatermark"
        width: watermark.implicitWidth + 32
        height: 46
        color: Theme.c.window
        z: 2

        Text {
            id: watermark
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Encoder Flow")
            font.pixelSize: 19
            font.weight: Font.DemiBold
            color: Theme.c.accent
            opacity: 0.45
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: wheel => { wheel.accepted = true; }
        }
    }

    // The dotted ground, so panning is visible even over empty space. Drawn from the pan
    // offset rather than scrolled, which keeps it exact at any offset.
    Repeater {
        model: 400

        delegate: Rectangle {
            required property int index

            readonly property int columns: 20
            readonly property real step: 60

            width: 1
            height: 1
            radius: 0
            color: Theme.c.border
            opacity: 0.35
            x: ((index % columns) * step + graph.panX % step + step) % (columns * step)
            y: (Math.floor(index / columns) * step + graph.panY % step + step) % (20 * step)
            visible: x < graph.width && y < graph.height
        }
    }

    // --- the edges, under the nodes ---------------------------------------------------------
    //
    // Rebuilt whenever anything they attach to moves. Each node reports its own port position
    // into `ports`, keyed by kind and id, and the links are read back out of that -- so an
    // edge cannot be drawn to a card that is not on screen, and nothing needs to know the card
    // geometry twice.
    property var ports: ({})
    property int portRevision: 0

    function setPort(key, inX, inY, outX, outY) {
        const previous = graph.ports[key];
        if (previous && previous.inX === inX && previous.inY === inY &&
            previous.outX === outX && previous.outY === outY) return;
        graph.ports[key] = { "inX": inX, "inY": inY, "outX": outX, "outY": outY };
        graph.portRevision++;
    }

    /// The edges, read back out of the ports the cards reported.
    ///
    /// Built from the models rather than by walking the Repeaters' delegates: `itemAt()` is a
    /// call with no binding dependency, so a graph built that way draws whatever existed the
    /// first time it ran and never gains an edge again. The models are properties, and
    /// `portRevision` covers the cards moving.
    property var links: []
    // A layout pass reports several ports per card. Coalesce those reports so opening or
    // panning the graph scans the document once after geometry settles, not once per port.
    onPortRevisionChanged: Qt.callLater(graph.refreshLinks)
    onSelectedKindChanged: Qt.callLater(graph.refreshLinks)
    onSelectedIdChanged: Qt.callLater(graph.refreshLinks)
    Component.onCompleted: Qt.callLater(graph.refreshLinks)
    Connections {
        target: Document
        function onChanged() { Qt.callLater(graph.refreshLinks); }
    }

    function refreshLinks() {
        const out = [];
        const add = (a, b, lit) => {
            const s = graph.ports[a];
            const e = graph.ports[b];
            if (s && e)
                out.push({ "x1": s.outX, "y1": s.outY, "x2": e.inX, "y2": e.inY, "lit": lit });
        };
        const live = (kind, id) => graph.isSelected(kind, id);

        for (const e of Document.encodeChoices()) {
            const canvas = Document.encodeCanvasOf(e.id);
            add("canvas:" + canvas, "encode:" + e.id,
                live("encode", e.id) || live("canvas", canvas));
            for (const d of e.destinations)
                add("encode:" + e.id, "output-video:" + d.id,
                    live("output", d.id) || live("encode", e.id));
        }
        for (const a of Document.audioEncodeChoices()) {
            const bus = Document.audioEncodeSettings(a.id).busId || "";
            add("bus:" + bus, "audio-encode:" + a.id,
                live("audio-encode", a.id) || live("bus", bus));
        }
        for (const o of Document.outputsWithAudio()) {
            add("audio-encode:" + o.audioEncode, "output-audio:" + o.id,
                live("output", o.id) || live("audio-encode", o.audioEncode));
        }
        graph.links = out;
    }

    GraphEdges {
        objectName: "encodeGraphEdges"
        anchors.fill: parent
        links: graph.links
    }

    GraphEdges {
        objectName: "encodePendingWire"
        anchors.fill: parent
        visible: graph.wireActive
        links: graph.wireActive ? [{x1: graph.wireStart.x, y1: graph.wireStart.y,
                                   x2: graph.wireEnd.x, y2: graph.wireEnd.y, lit: true}] : []
    }

    // --- the left rail: canvases, then audio tracks ------------------------------------------

    Column {
        id: rail

        x: graph.railX + graph.panX
        y: graph.railTop + graph.panY
        spacing: graph.railSpacing

        Repeater {
            id: canvasRepeater

            model: Document.canvases

            delegate: EncodeNodeCard {
                id: canvasCard
                objectName: "encodeCanvasNode_" + canvasId
                nodeKind: "canvas"
                nodeId: canvasId
                connectionGraph: graph

                required property string canvasId
                required property string name
                required property int pixelWidth
                required property int pixelHeight
                required property double fps

                width: 250
                title: qsTr("CANVAS: %1").arg(canvasCard.name)
                subtitle: canvasCard.pixelWidth + " x " + canvasCard.pixelHeight + " " +
                          canvasCard.fps.toFixed(0) + "fps"
                hasOutput: true
                selected: graph.isSelected("canvas", canvasCard.canvasId)
                onPressed: graph.nodeSelected("canvas", canvasCard.canvasId)
                onDisconnectRequested: port => Document.disconnectGraphPort("canvas", canvasCard.canvasId, port)

                function report() {
                    graph.setPort("canvas:" + canvasCard.canvasId,
                                  rail.x, rail.y + y + 30,
                                  rail.x + width, rail.y + y + 30);
                }
                Component.onCompleted: report()
                onYChanged: report()
                Connections {
                    target: rail
                    function onXChanged() { canvasCard.report(); }
                    function onYChanged() { canvasCard.report(); }
                }

                // The canvas's own picture, from the preview the renderer already publishes.
                Item {
                    width: canvasCard.width - 18
                    height: 108
                    x: 9

                    Rectangle {
                        anchors.fill: parent
                        color: "black"
                        border.width: 1
                        border.color: Theme.c.border
                    }

                    Image {
                        id: thumbnail
                        objectName: "encodeCanvasPreview"
                        readonly property int previewEdge: Math.ceil(Math.max(width, height) * Screen.devicePixelRatio)
                        onPreviewEdgeChanged: Preview.requestSize(thumbnail, visible ? previewEdge : 0)
                        onVisibleChanged: Preview.requestSize(thumbnail, visible ? previewEdge : 0)
                        Component.onCompleted: Preview.requestSize(thumbnail, visible ? previewEdge : 0)
                        anchors.fill: parent
                        anchors.margins: 1
                        fillMode: Image.PreserveAspectFit
                        cache: false
                        // The provider returns an already-held image. An asynchronous reload
                        // clears it between frames; match the Program panel's atomic swap.
                        asynchronous: false
                        // `hasFrame` is a call, so the generation binding is what makes this
                        // re-evaluate; a bare call would never update.
                        source: thumbnail.visible && Preview.generation >= 0 &&
                                Preview.hasFrame(canvasCard.canvasId)
                                ? "image://program/" +
                                  Preview.canvasKey(canvasCard.canvasId) + "/" +
                                  Preview.generation
                                : ""
                    }
                }
            }
        }

        Repeater {
            id: busRepeater

            model: Document.buses

            delegate: EncodeNodeCard {
                id: busCard
                objectName: "encodeBusNode_" + busId
                nodeKind: "bus"
                nodeId: busId
                connectionGraph: graph

                required property string busId
                required property string name
                required property string kind

                // Only the mixes that can be encoded. An input bus is where one source
                // arrives, not something a destination can carry.
                readonly property bool encodable: busCard.kind === "program" ||
                                                  busCard.kind === "vod" ||
                                                  busCard.kind === "track"

                property var meter: ({})

                visible: busCard.encodable
                // A Column lays out by height, so an invisible card still has to measure zero
                // or it leaves a gap where it would have been.
                height: busCard.encodable ? busCard.implicitHeight : 0
                width: 250
                title: qsTr("AUDIO TRACK: %1").arg(busCard.name)
                hasOutput: true
                selected: graph.isSelected("bus", busCard.busId)
                onPressed: graph.nodeSelected("bus", busCard.busId)
                onDisconnectRequested: port => Document.disconnectGraphPort("bus", busCard.busId, port)

                function report() {
                    if (!busCard.encodable)
                        return;
                    graph.setPort("bus:" + busCard.busId,
                                  rail.x, rail.y + busCard.y + 30,
                                  rail.x + busCard.width, rail.y + busCard.y + 30);
                }
                Component.onCompleted: report()
                onYChanged: report()
                Connections {
                    target: rail
                    function onXChanged() { busCard.report(); }
                    function onYChanged() { busCard.report(); }
                }
                // Nothing makes a sound yet, so this reads whatever the renderer publishes and
                // sits at nothing when that is nothing. A meter that moved on a timer would
                // look finished while being a decoration.
                Connections {
                    target: Preview
                    function onChanged() { busCard.meter = Preview.audioLevel(busCard.busId); }
                }

                Item {
                    width: busCard.width - 18
                    height: 26
                    x: 9

                    Rectangle {
                        anchors.fill: parent
                        color: Theme.c.panelHeader
                        border.width: 1
                        border.color: Theme.c.border
                    }

                    Repeater {
                        model: 2

                        delegate: Rectangle {
                            required property int index

                            readonly property real peak: index === 0
                                                         ? (busCard.meter.left || 0)
                                                         : (busCard.meter.right || 0)

                            x: 2
                            y: 2 + index * 11
                            height: 9
                            // The same dB mapping the mixer faders use: -60 dB to 0 across the
                            // width, so a strip here and a strip there agree.
                            width: (parent.width - 4) *
                                   Math.max(0, Math.min(1,
                                       (20 * Math.log(Math.max(peak, 0.001)) / Math.LN10 + 60) / 60))
                            color: peak >= 0.99 ? Theme.c.warning : Theme.c.good
                        }
                    }
                }
            }
        }
    }

    // --- the placed nodes --------------------------------------------------------------------

    Repeater {
        id: encodeRepeater

        model: Document.encodes

        delegate: EncodeNodeCard {
            id: encodeCard
            objectName: "encodeNode_" + encodeId
            nodeKind: "encode"
            nodeId: encodeId
            connectionGraph: graph
            acceptedSourceKind: "canvas"

            required property string encodeId
            required property string name
            required property string canvasId
            required property string resolution
            required property string codec
            required property string implementation
            required property int bitrateKbps
            required property double positionX
            required property double positionY

            property var settings: ({})
            function refreshSettings() {
                settings = Document.encodeSettings(encodeId);
            }
            readonly property var extra: settings.extra || ({})
            Connections {
                target: Document
                function onChanged() { encodeCard.refreshSettings(); }
            }

            property real dragFromX: 0
            property real dragFromY: 0

            x: encodeCard.positionX + graph.panX
            y: encodeCard.positionY + graph.panY
            width: 270
            title: encodeCard.implementation.toUpperCase() + ": " +
                   encodeCard.codec.toUpperCase()
            subtitle: qsTr("Rescale: %1").arg(encodeCard.settings.width > 0
                      ? (encodeCard.extra.rescale_filter || "bicubic") : qsTr("Off"))
            hasInput: true
            hasOutput: true
            selected: graph.isSelected("encode", encodeCard.encodeId)
            missingConnections: encodeCard.canvasId === ""
            onDisconnectRequested: port => Document.disconnectGraphPort("encode", encodeCard.encodeId, port)

            onPressed: (mx, my) => {
                graph.nodeSelected("encode", encodeCard.encodeId);
                encodeCard.dragFromX = mx;
                encodeCard.dragFromY = my;
            }
            onDragged: (mx, my) => Document.setGraphPosition(
                           "encode", encodeCard.encodeId,
                           encodeCard.x - graph.panX + (mx - encodeCard.dragFromX),
                           encodeCard.y - graph.panY + (my - encodeCard.dragFromY), true)
            // One drag is one undo step, and it lands back where it started.
            onReleased: Document.endGesture()
            onContextRequested: (mx, my) => graph.nodeMenu(
                encodeCard, "encode", encodeCard.encodeId, encodeCard.name, mx, my)

            function report() {
                graph.setPort("encode:" + encodeCard.encodeId,
                              x, y + 30, x + width, y + 30);
            }
            Component.onCompleted: { refreshSettings(); report(); }
            onXChanged: report()
            onYChanged: report()

            Column {
                x: 9
                width: encodeCard.width - 18
                spacing: 4

                Text {
                    text: qsTr("Bitrate")
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                }

                Rectangle {
                    width: parent.width
                    height: 22
                    color: Theme.c.panelHeader
                    border.width: 1
                    border.color: Theme.c.border

                    Text {
                        anchors.centerIn: parent
                        objectName: "encodeNodeRate"
                        text: {
                            const rate = (encodeCard.settings.rateControl || "cbr").toUpperCase();
                            if (rate === "CQP" || rate === "CRF")
                                return rate + " " + (encodeCard.extra.quality ?? 23);
                            return qsTr("%1 %2 Kbps").arg(rate).arg(encodeCard.bitrateKbps);
                        }
                        font.pixelSize: 11
                        color: Theme.c.text
                    }
                }

                Text {
                    text: encodeCard.name + " · " + encodeCard.resolution
                    width: parent.width
                    elide: Text.ElideRight
                    font.pixelSize: 10
                    color: Theme.c.textMuted
                }

                Row {
                    width: parent.width
                    spacing: 4
                    Repeater {
                        model: [
                            {label: qsTr("Mode"), value: encodeCard.implementation === "nvenc"
                                ? ((encodeCard.extra["nvenc.multipass"] ?? "quarter") === "disabled"
                                   ? qsTr("1 Pass") : qsTr("2 Pass"))
                                : (encodeCard.settings.preset || qsTr("Default"))},
                            {label: qsTr("Tuning"), value: encodeCard.implementation === "nvenc"
                                ? (encodeCard.extra["nvenc.tuning"] || "hq").toUpperCase()
                                : (encodeCard.extra["x264.tune"] || qsTr("None"))},
                            {label: qsTr("Profile"), value: encodeCard.extra[
                                encodeCard.implementation + ".profile"] || qsTr("Auto")}
                        ]
                        delegate: Column {
                            required property var modelData
                            width: (encodeCard.width - 26) / 3
                            spacing: 3
                            Text {
                                text: modelData.label
                                font.pixelSize: 9
                                color: Theme.c.textMuted
                            }
                            Rectangle {
                                width: parent.width
                                height: 22
                                color: Theme.c.panelHeader
                                border.width: 1
                                border.color: Theme.c.border
                                Text {
                                    anchors.fill: parent
                                    anchors.margins: 3
                                    text: modelData.value
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                    font.pixelSize: 10
                                    color: Theme.c.text
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Repeater {
        id: audioRepeater

        model: Document.audioEncodes

        delegate: EncodeNodeCard {
            id: audioCard
            objectName: "audioEncodeNode_" + audioEncodeId
            nodeKind: "audio-encode"
            nodeId: audioEncodeId
            connectionGraph: graph
            acceptedSourceKind: "bus"

            required property string audioEncodeId
            required property string name
            required property string busId
            required property string codec
            required property int bitrateKbps
            required property double positionX
            required property double positionY

            property real dragFromX: 0
            property real dragFromY: 0

            x: audioCard.positionX + graph.panX
            y: audioCard.positionY + graph.panY
            width: 270
            title: "FFmpeg: " + audioCard.codec.toUpperCase()
            hasInput: true
            hasOutput: true
            selected: graph.isSelected("audio-encode", audioCard.audioEncodeId)
            missingConnections: audioCard.busId === ""
            onDisconnectRequested: port => Document.disconnectGraphPort("audio-encode", audioCard.audioEncodeId, port)
            onContextRequested: (mx, my) => graph.nodeMenu(
                audioCard, "audio-encode", audioCard.audioEncodeId, audioCard.name, mx, my)

            onPressed: (mx, my) => {
                graph.nodeSelected("audio-encode", audioCard.audioEncodeId);
                audioCard.dragFromX = mx;
                audioCard.dragFromY = my;
            }
            onDragged: (mx, my) => Document.setGraphPosition(
                           "audio-encode", audioCard.audioEncodeId,
                           audioCard.x - graph.panX + (mx - audioCard.dragFromX),
                           audioCard.y - graph.panY + (my - audioCard.dragFromY), true)
            onReleased: Document.endGesture()

            function report() {
                graph.setPort("audio-encode:" + audioCard.audioEncodeId,
                              x, y + 30, x + width, y + 30);
            }
            Component.onCompleted: report()
            onXChanged: report()
            onYChanged: report()

            Column {
                x: 9
                width: audioCard.width - 18
                spacing: 4

                Text {
                    text: qsTr("Bitrate")
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                }

                Rectangle {
                    width: parent.width
                    height: 22
                    color: Theme.c.panelHeader
                    border.width: 1
                    border.color: Theme.c.border

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("%1 Kbps").arg(audioCard.bitrateKbps)
                        font.pixelSize: 11
                        color: Theme.c.text
                    }
                }
            }
        }
    }

    Repeater {
        id: outputRepeater

        model: Document.outputs

        delegate: EncodeNodeCard {
            id: outputCard
            objectName: "outputNode_" + outputId
            nodeKind: "output"
            nodeId: outputId
            connectionGraph: graph

            required property string outputId
            required property string name
            required property string encodeId
            required property string audioEncodeId
            required property string audioEncodeName
            required property string encodeName
            required property string destinationKind
            required property bool isEnabled
            required property bool misconfigured
            required property double positionX
            required property double positionY

            property real dragFromX: 0
            property real dragFromY: 0

            x: outputCard.positionX + graph.panX
            y: outputCard.positionY + graph.panY
            width: 270
            title: outputCard.destinationKind === "file" ? outputCard.name.toUpperCase()
                 : outputCard.destinationKind === "replay" ? outputCard.name.toUpperCase()
                 : qsTr("STREAM: %1").arg(outputCard.name)
            hasInput: true
            readonly property real videoInputY: bodyTop + outputDetails.y + videoTrack.y + videoTrack.height / 2
            readonly property real audioInputY: bodyTop + outputDetails.y + audioTrack.y + audioTrack.height / 2
            inputPorts: [{key: "video", y: videoInputY, label: qsTr("Video 0"), accepts: "encode"},
                         {key: "audio", y: audioInputY, label: qsTr("Audio 0"), accepts: "audio-encode"}]
            missingConnections: outputCard.encodeId === "" || outputCard.audioEncodeId === ""
            onDisconnectRequested: port => Document.disconnectGraphPort("output", outputCard.outputId, port)
            selected: graph.isSelected("output", outputCard.outputId)
            onContextRequested: (mx, my) => {
                if (outputCard.destinationKind !== "file" && outputCard.destinationKind !== "replay")
                    graph.nodeMenu(outputCard, "output", outputCard.outputId, outputCard.name, mx, my);
            }

            onPressed: (mx, my) => {
                graph.nodeSelected("output", outputCard.outputId);
                outputCard.dragFromX = mx;
                outputCard.dragFromY = my;
            }
            onDragged: (mx, my) => Document.setGraphPosition(
                           "output", outputCard.outputId,
                           outputCard.x - graph.panX + (mx - outputCard.dragFromX),
                           outputCard.y - graph.panY + (my - outputCard.dragFromY), true)
            onReleased: Document.endGesture()

            function report() {
                graph.setPort("output-video:" + outputCard.outputId,
                              x, y + videoInputY, x + width, y + videoInputY);
                graph.setPort("output-audio:" + outputCard.outputId,
                              x, y + audioInputY, x + width, y + audioInputY);
            }
            Component.onCompleted: report()
            onXChanged: report()
            onYChanged: report()
            onVideoInputYChanged: report()
            onAudioInputYChanged: report()

            status: Rectangle {
                objectName: "encodeArmingSwitch"
                visible: outputCard.destinationKind !== "file" && outputCard.destinationKind !== "replay"
                width: 26
                height: 13
                radius: 7
                color: Theme.c.panelHeader
                border.width: 1
                border.color: activeFocus ? Theme.c.accent : Theme.c.border
                activeFocusOnTab: visible
                Accessible.role: Accessible.CheckBox
                Accessible.name: qsTr("Arm %1").arg(outputCard.name)
                Accessible.checkable: true
                Accessible.checked: outputCard.isEnabled
                Accessible.onToggleAction: toggle()

                function toggle() {
                    Document.setOutputEnabled(outputCard.outputId, !outputCard.isEnabled);
                }
                Keys.onSpacePressed: toggle()
                Keys.onReturnPressed: toggle()

                Rectangle {
                    width: 9
                    height: 9
                    radius: 5
                    y: 2
                    x: outputCard.isEnabled ? parent.width - 11 : 2
                    objectName: "encodeArmingIndicator"
                    color: outputCard.isEnabled ? Theme.c.good : Theme.c.error
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: parent.toggle()
                }
            }

            Column {
                id: outputDetails
                x: 9
                width: outputCard.width - 18
                spacing: 2

                Text {
                    text: qsTr("Video Tracks")
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                    bottomPadding: 1
                }

                Text {
                    id: videoTrack
                    width: parent.width
                    text: outputCard.encodeName === ""
                          ? qsTr("Video 0: Not connected")
                          : qsTr("Video track 0 (%1)").arg(outputCard.encodeName)
                    font.pixelSize: 10
                    color: Theme.c.text
                    elide: Text.ElideRight
                }

                Text {
                    text: qsTr("Audio Tracks")
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                    topPadding: 5
                    bottomPadding: 1
                }

                // Said rather than left blank: a destination with no audio goes to air silent,
                // which is a thing someone can mean and a thing nobody should discover live.
                Text {
                    id: audioTrack
                    width: parent.width
                    text: outputCard.audioEncodeId === ""
                          ? qsTr("Audio 0: Not connected")
                          : qsTr("Audio track 0 (%1)").arg(outputCard.audioEncodeName)
                    font.pixelSize: 10
                    color: outputCard.audioEncodeId === "" ? Theme.c.warning : Theme.c.text
                    elide: Text.ElideRight
                }

            }
        }
    }

    // --- panning -----------------------------------------------------------------------------
    StripMenu {
        id: contextMenu
        objectName: "encodeNodeMenu"
        z: 10
    }
    //
    // Middle-drag, or left-drag on empty space. Below the cards, so a press that lands on a
    // node moves the node instead.
    MouseArea {
        anchors.fill: parent
        z: -2
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.ArrowCursor

        property real fromX: 0
        property real fromY: 0

        onPressed: mouse => {
            fromX = mouse.x - graph.panX;
            fromY = mouse.y - graph.panY;
            // A press on nothing is also how a selection is let go.
            if (mouse.button === Qt.LeftButton)
                graph.nodeSelected("", "");
        }
        onPositionChanged: mouse => {
            graph.panX = mouse.x - fromX;
            graph.panY = mouse.y - fromY;
        }
    }
}
