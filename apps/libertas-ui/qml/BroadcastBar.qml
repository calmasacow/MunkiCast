// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

Item {
    id: bar
    objectName: "broadcastBar"
    implicitHeight: info.height + 20
    property string recordTarget: ""
    property var streamTarget: ({})
    property bool replayEnabled: false
    function refresh() {
        bar.recordTarget = Document.recordingEncodeId();
        bar.streamTarget = Document.streamingTarget();
        bar.replayEnabled = !!Document.outputEncoderSettings(Document.replayOutputId()).enabled;
        Telemetry.setDiskPath(Document.recordingDirectory());
    }
    Component.onCompleted: refresh()
    Connections {
        target: Document
        function onChanged() { bar.refresh(); }
    }

    function bytesLabel(n) {
        if (n >= 1024 * 1024 * 1024 * 1024)
            return (n / (1024 * 1024 * 1024 * 1024)).toFixed(1) + " TB";
        if (n >= 1024 * 1024 * 1024)
            return (n / (1024 * 1024 * 1024)).toFixed(1) + " GB";
        return (n / (1024 * 1024)).toFixed(1) + " MB";
    }

    ToolButton {
        objectName: "studioModeButton"
        anchors.right: info.left
        anchors.rightMargin: 20
        anchors.verticalCenter: parent.verticalCenter
        width: 100; height: 28
        label: qsTr("Studio Mode")
        tip: qsTr("Stage Main scenes before taking them to Program")
        active: !!Document.mainDeck.studio
        enabled: !!Document.mainDeck.id
        onClicked: {
            Preview.setManualTransition(-1);
            Document.setDeckStudioMode(Document.mainDeck.id, !Document.mainDeck.studio);
        }
    }

    Row {
        id: info
        anchors.right: actions.left
        anchors.rightMargin: 20
        anchors.verticalCenter: parent.verticalCenter
        spacing: 22

        Column {
            spacing: 1

            Text {
                text: Telemetry.diskAvailable
                      ? bar.bytesLabel(Telemetry.diskFreeBytes)
                      : "—"
                font.pixelSize: 12
                color: "#7AA2F7"
            }

            Text {
                text: qsTr("Disk space")
                font.pixelSize: 9
                color: Theme.c.textMuted
            }
        }

        Column {
            spacing: 1

            Text {
                // Target rate of the first canvas, rather than a measured encoding rate.
                text: canvasFps.value > 0 ? canvasFps.value.toFixed(1) : "—"
                font.pixelSize: 12
                color: "#7AA2F7"
            }

            Text {
                text: qsTr("FPS")
                font.pixelSize: 9
                color: Theme.c.textMuted
            }
        }
    }

    QtObject {
        id: canvasFps

        // An empty id means the first canvas. A call, so the Connections below is what
        // makes it re-read; Document emits `changed` after every command.
        property real value: 0

        function refresh() { canvasFps.value = Document.canvasFpsOf(""); }
        Component.onCompleted: refresh()
    }

    Connections {
        target: Document
        function onChanged() { canvasFps.refresh(); }
    }

    Row {
        id: actions

        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        ToolButton {
            width: 96
            height: 28
            objectName: "goLiveButton"
            label: Preview.streaming ? qsTr("Stop Live")
                   : Preview.streamingPending ? qsTr("Connecting…") : qsTr("Go Live!")
            active: Preview.streaming
            activeColor: Theme.c.error
            enabled: !Preview.streamingPending && !Preview.recordingPending
                     && (Preview.streaming || !!bar.streamTarget.outputId)
            tip: Preview.streaming ? qsTr("Stop the live stream")
                 : bar.streamTarget.reason || ""
            onClicked: {
                if (Preview.streaming) Preview.stopStreaming();
                else Preview.startStreaming(bar.streamTarget.outputId);
            }
        }

        // The Recording node names the bitstreams this action writes.
        ToolButton {
            objectName: "recordButton"
            width: 96
            height: 28
            label: Preview.recording ? qsTr("Stop") : qsTr("Record")
            active: Preview.recording
            activeColor: Theme.c.error
            enabled: !Preview.streamingPending && !Preview.recordingPending
                     && (Preview.recording || bar.recordTarget !== "")
            tip: Preview.recording
                 ? qsTr("Stop recording and close %1").arg(Preview.recordingPath)
                 : bar.recordTarget !== ""
                   ? qsTr("Record to the configured recording folder")
                   : qsTr("Connect a Recording destination to video and audio encoders")
            onClicked: {
                if (Preview.recording) {
                    Preview.stopRecording(bar.recordTarget);
                    return;
                }
                const path = Document.recordingPathFor(bar.recordTarget);
                if (path !== "")
                    Preview.startRecording(bar.recordTarget, path);
            }
        }

        ToolButton {
            width: 96
            height: 28
            objectName: "replayButton"
            label: Preview.replayActive ? qsTr("Stop Replay") : qsTr("Replay")
            enabled: (bar.replayEnabled || Preview.replayActive) && !Preview.replaySaving && !Preview.outputPending
            active: Preview.replayActive
            tip: Preview.replayActive ? qsTr("Stop the replay buffer") : qsTr("Start the replay buffer")
            onClicked: {
                if (Preview.replayActive) Preview.stopReplay();
                else Preview.startReplay();
            }
        }

        // ponytail: manual clip trigger for testing; remove when event triggers replace it.
        ToolButton {
            objectName: "clipButton"
            width: 64
            height: 28
            label: qsTr("Clip")
            enabled: Preview.replayActive && !Preview.replaySaving && !Preview.outputPending && Preview.replayDurationMs > 0
            tip: qsTr("Save the previous buffered seconds as a clip")
            onClicked: Preview.saveReplay(Document.replayPath())
        }
    }

}
