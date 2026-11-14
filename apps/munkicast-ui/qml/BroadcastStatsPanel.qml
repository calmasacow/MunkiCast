// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// How the broadcast is actually going.
///
/// Render counters measure compositing; output counters measure delivered frames and stalls.
Item {
    id: panel
    objectName: "broadcastStatsPanel"

    function bytesLabel(n) {
        if (n >= 1024 * 1024 * 1024 * 1024)
            return (n / (1024 * 1024 * 1024 * 1024)).toFixed(1) + " TB";
        if (n >= 1024 * 1024 * 1024)
            return (n / (1024 * 1024 * 1024)).toFixed(1) + " GB";
        return (n / (1024 * 1024)).toFixed(1) + " MB";
    }

    property var output: Preview.outputStats()
    property var captures: Preview.captureStats()
    Timer {
        interval: 250
        repeat: true
        running: panel.visible
        onTriggered: panel.output = Preview.outputStats()
    }
    Connections {
        target: Preview
        function onChanged() {
            panel.output = Preview.outputStats();
            panel.captures = Preview.captureStats();
        }
    }

    function frameCount(n) {
        const total = panel.output.frames + panel.output.missed;
        return n + " (" + (total > 0 ? 100 * n / total : 0).toFixed(1) + "%)";
    }

    readonly property string unavailable: "—"
    /// Below this the five-column table does not fit, so each output becomes two lines instead.
    /// The panel folds rather than demanding width, because its minimum size constrains every
    /// panel docked near it, not just itself.
    readonly property bool wideEnough: panel.width >= 340
    readonly property int statsColumns: panel.width >= 640 ? 2 : 1

    Flickable {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
        contentHeight: body.implicitHeight + 20
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
        // because Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

        Column {
            id: body

            x: 10
            y: 8
            width: parent.width - 20
            spacing: 2

            Grid {
                id: machineGrid
                objectName: "machineStatsGrid"
                width: parent.width
                columns: panel.statsColumns
                columnSpacing: 24
                rowSpacing: 10

                Column {
                    width: (machineGrid.width - machineGrid.columnSpacing * (machineGrid.columns - 1)) / machineGrid.columns
                    spacing: 2
                    Text {
                        width: parent.width
                        text: qsTr("This machine")
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                        color: Theme.c.accent
                        bottomPadding: 4
                    }

                    // The bar carries the machine figure; this panel is where the two are told
                    // apart, which is the question once the bar has said the box is busy.
                    StatRow {
                        width: parent.width
                        label: qsTr("CPU usage (machine)")
                        value: Telemetry.systemCpuAvailable
                            ? Telemetry.systemCpuPercent.toFixed(1) + " %" : panel.unavailable
                    }

                    StatRow {
                        width: parent.width
                        label: qsTr("CPU usage (MunkiCast)")
                        value: Telemetry.cpuAvailable
                            ? Telemetry.cpuPercent.toFixed(1) + " %" : panel.unavailable
                    }

                    StatRow {
                        width: parent.width
                        label: qsTr("Memory usage")
                        value: Telemetry.memoryAvailable
                            ? panel.bytesLabel(Telemetry.memoryBytes) : panel.unavailable
                    }

                    StatRow {
                        width: parent.width
                        label: qsTr("Disk space available")
                        value: Telemetry.diskAvailable
                            ? panel.bytesLabel(Telemetry.diskFreeBytes) : panel.unavailable
                    }

                    // Only meaningful while something is being written, and only for a local
                    // disk: the figure divides free space by the rate it is actually filling
                    // at, so with nothing recording there is no rate and no answer.
                    StatRow {
                        width: parent.width
                        label: qsTr("Disk full in (approx.)")
                        value: {
                            if (!Telemetry.diskAvailable || !Preview.recording)
                                return panel.unavailable;
                            const kbps = panel.output.bitrateKbps;
                            if (kbps === undefined || kbps <= 0)
                                return panel.unavailable;
                            const seconds = Telemetry.diskFreeBytes / (kbps * 125);
                            if (seconds < 90)
                                return qsTr("%1 s").arg(Math.round(seconds));
                            if (seconds < 5400)
                                return qsTr("%1 min").arg(Math.round(seconds / 60));
                            return qsTr("%1 h").arg((seconds / 3600).toFixed(1));
                        }
                    }

                }

                Column {
                    width: (machineGrid.width - machineGrid.columnSpacing * (machineGrid.columns - 1)) / machineGrid.columns
                    spacing: 2
                    Text {
                        width: parent.width
                        text: Telemetry.gpuAvailable
                            ? qsTr("Graphics — %1").arg(Telemetry.gpuName)
                            : qsTr("Graphics")
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                        color: Theme.c.accent
                        bottomPadding: 4
                        elide: Text.ElideRight
                    }

                    StatRow {
                        width: parent.width
                        label: qsTr("GPU usage")
                        value: Telemetry.gpuAvailable
                            ? Telemetry.gpuPercent.toFixed(0) + " %" : panel.unavailable
                    }

                    StatRow {
                        width: parent.width
                        label: qsTr("Encoder usage")
                        value: Telemetry.encoderAvailable
                            ? Telemetry.encoderPercent.toFixed(0) + " %" : panel.unavailable
                    }

                    StatRow {
                        width: parent.width
                        label: qsTr("VRAM usage")
                        value: Telemetry.gpuAvailable && Telemetry.vramTotalBytes > 0
                            ? panel.bytesLabel(Telemetry.vramUsedBytes) + " / " +
                              panel.bytesLabel(Telemetry.vramTotalBytes) +
                              "  (" + (Telemetry.vramUsedBytes / Telemetry.vramTotalBytes * 100).toFixed(0) + " %)"
                            : panel.unavailable
                    }

                    Text {
                        width: parent.width
                        visible: !Telemetry.gpuAvailable && Telemetry.gpuNote !== ""
                        text: Telemetry.gpuNote
                        wrapMode: Text.WordWrap
                        font.pixelSize: 9
                        color: Theme.c.textMuted
                        bottomPadding: 2
                    }

                }
            }

            Text {
                width: parent.width
                text: qsTr("The show")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 10
                bottomPadding: 4
            }

            Grid {
                id: canvasGrid
                objectName: "canvasStatsGrid"
                width: parent.width
                columns: panel.statsColumns
                columnSpacing: 24
                rowSpacing: 8

                Repeater {
                    model: Document.canvases
                    delegate: Column {
                        id: canvasStats
                        required property string canvasId
                        required property string name
                        required property double fps
                        property var stats: Preview.renderStats(canvasId)
                        Timer {
                            interval: 250
                            repeat: true
                            running: panel.visible
                            onTriggered: canvasStats.stats = Preview.renderStats(canvasStats.canvasId)
                        }
                        Connections {
                            target: Preview
                            function onChanged() {
                                canvasStats.stats = Preview.renderStats(canvasStats.canvasId);
                            }
                        }
                        objectName: "canvasStatsCard"
                        width: (canvasGrid.width - canvasGrid.columnSpacing * (canvasGrid.columns - 1)) / canvasGrid.columns
                        spacing: 1
                        Text {
                            width: parent.width
                            text: name
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                            color: Theme.c.textMuted
                            topPadding: 5
                        }
                        StatRow {
                            objectName: "canvasTargetFps"
                            width: parent.width
                            label: qsTr("Canvas target FPS")
                            value: canvasStats.fps.toFixed(1)
                        }
                        StatRow {
                            objectName: "canvasRenderTime"
                            width: parent.width
                            label: qsTr("Average time to render frame (500 ms)")
                            value: stats.available
                                ? stats.renderMs.toFixed(2) + " ms" : panel.unavailable
                        }
                        StatRow {
                            objectName: "canvasRenderedFrames"
                            width: parent.width
                            label: qsTr("Frames rendered since reset")
                            value: stats.available ? stats.frames : panel.unavailable
                        }
                        StatRow {
                            objectName: "canvasMissedFrames"
                            width: parent.width
                            label: qsTr("Frames missed due to rendering lag")
                            value: stats.available
                                ? stats.missed + " (" + (stats.frames + stats.missed > 0
                                    ? 100 * stats.missed / (stats.frames + stats.missed) : 0).toFixed(1) + "%)"
                                : panel.unavailable
                        }
                    }
                }

            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: Preview.rendererAttached
                    ? qsTr("Render counters include full-size compositing for active outputs. Encoding lag counts output frames missed while encoding or writing packets.")
                    : qsTr("Waiting for renderer statistics.")
                font.pixelSize: 9
                color: Theme.c.textMuted
                topPadding: 4
                bottomPadding: 6
            }

            // ---- the per-output table ----
            Rectangle {
                width: parent.width
                height: 1
                color: Theme.c.border
            }

            Item {
                width: parent.width
                height: 18
                visible: panel.wideEnough

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.30
                    text: qsTr("Output")
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    color: Theme.c.textMuted
                    elide: Text.ElideRight
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: parent.width * 0.30
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.18
                    text: qsTr("Status")
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    color: Theme.c.textMuted
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: parent.width * 0.48
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.22
                    text: qsTr("Dropped (net)")
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    color: Theme.c.textMuted
                    elide: Text.ElideRight
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: parent.width * 0.70
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.16
                    text: qsTr("Sent")
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    color: Theme.c.textMuted
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: parent.width * 0.86
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.14
                    text: qsTr("Bitrate")
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    color: Theme.c.textMuted
                    elide: Text.ElideRight
                }
            }

            Repeater {
                model: Document.outputs

                delegate: Item {
                    id: row

                    required property string name
                    required property bool isEnabled
                    required property string outputId

                    /// Figures for this row, or empty when this is not the destination that is
                    /// running. One encoder runs at a time (ADR-0029), so at most one row has
                    /// numbers; the others are not at zero, they are unknown.
                    readonly property var stats: panel.output && panel.output.owner !== undefined
                                                 ? Preview.outputStatsFor(row.outputId) : ({})
                    readonly property bool live: row.stats.available === true
                    readonly property string dropped:
                        row.live ? String(row.stats.missed) : panel.unavailable
                    readonly property string sent:
                        row.live ? panel.bytesLabel(row.stats.bytes) : panel.unavailable
                    readonly property string rate:
                        row.live && row.stats.bitrateKbps !== undefined
                        ? Math.round(row.stats.bitrateKbps) + " kbps" : panel.unavailable

                    objectName: "broadcastStatRow"
                    width: body.width
                    height: panel.wideEnough ? 17 : 28

                    Text {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.topMargin: panel.wideEnough ? 0 : 1
                        anchors.verticalCenter: panel.wideEnough
                            ? parent.verticalCenter : undefined
                        width: parent.width * (panel.wideEnough ? 0.30 : 1.0)
                        text: row.name
                        font.pixelSize: 10
                        color: Theme.c.text
                        elide: Text.ElideRight
                    }

                    // The narrow form: everything the columns would have said, on one line
                    // under the name.
                    Text {
                        anchors.left: parent.left
                        anchors.bottom: parent.bottom
                        visible: !panel.wideEnough
                        text: (row.live ? qsTr("Running")
                                        : row.isEnabled ? qsTr("Enabled") : qsTr("Off")) +
                              " · " + qsTr("dropped %1 · sent %2 · %3")
                                  .arg(row.dropped)
                                  .arg(row.sent)
                                  .arg(row.rate)
                        font.pixelSize: 9
                        color: Theme.c.textMuted
                        elide: Text.ElideRight
                    }

                    Text {
                        visible: panel.wideEnough
                        anchors.left: parent.left
                        anchors.leftMargin: parent.width * 0.30
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width * 0.18
                        // "Inactive" would claim this was asked to run and is not. A row that
                        // was never asked is only configured, which is what Enabled says.
                        text: row.live ? qsTr("Running")
                                       : row.isEnabled ? qsTr("Enabled") : qsTr("Off")
                        font.pixelSize: 10
                        color: Theme.c.textMuted
                    }

                    Text {
                        visible: panel.wideEnough
                        anchors.left: parent.left
                        anchors.leftMargin: parent.width * 0.48
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.dropped
                        font.pixelSize: 10
                        color: Theme.c.textMuted
                    }

                    Text {
                        visible: panel.wideEnough
                        anchors.left: parent.left
                        anchors.leftMargin: parent.width * 0.70
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.sent
                        font.pixelSize: 10
                        color: Theme.c.textMuted
                    }

                    Text {
                        visible: panel.wideEnough
                        anchors.left: parent.left
                        anchors.leftMargin: parent.width * 0.86
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.rate
                        font.pixelSize: 10
                        color: Theme.c.textMuted
                    }
                }
            }

            Text {
                width: parent.width
                visible: Document.outputs.rowCount() === 0
                text: qsTr("No outputs configured.")
                font.pixelSize: 10
                color: Theme.c.textMuted
                topPadding: 4
            }

            Text {
                width: parent.width
                text: qsTr("Capture Devices")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 10
                visible: panel.captures.length > 0
            }

            Repeater {
                model: panel.captures
                delegate: Column {
                    required property var modelData
                    width: body.width
                    Text {
                        width: parent.width
                        text: modelData.name + qsTr(" capture")
                        color: Theme.c.accent
                        font.pixelSize: 10
                        topPadding: 6
                    }
                    StatRow {
                        width: parent.width
                        label: qsTr("Requested FPS")
                        value: modelData.requested
                    }
                    StatRow {
                        width: parent.width
                        label: qsTr("Device negotiated FPS")
                        value: modelData.negotiated > 0 ? modelData.negotiated.toFixed(3) : panel.unavailable
                    }
                    StatRow {
                        width: parent.width
                        label: qsTr("Received FPS (1 second)")
                        value: modelData.received.toFixed(3)
                    }
                    Text {
                        width: parent.width
                        text: modelData.error || qsTr("Arrival rate; repeated images still count as frames.")
                        color: modelData.error ? Theme.c.warning : Theme.c.textMuted
                        font.pixelSize: 9
                        wrapMode: Text.WordWrap
                    }
                }
            }

            StatRow {
                width: parent.width
                objectName: "encoderSkippedFrames"
                label: qsTr("Skipped frames due to encoding lag")
                value: panel.output.available ? panel.frameCount(panel.output.lag) : panel.unavailable
            }

            StatRow {
                width: parent.width
                objectName: "outputMissedFrames"
                visible: Preferences.showOutputMissedFrames
                label: qsTr("Output frames missed (all causes)")
                value: panel.output.available ? panel.frameCount(panel.output.missed) : panel.unavailable
            }
        }
    }

    Rectangle {
        id: footer

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 26
        color: Theme.c.panelHeader

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.c.border
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("machine figures live · show figures need the renderer")
            font.pixelSize: 9
            color: Theme.c.textMuted
            opacity: 0.6
            elide: Text.ElideRight
        }

        ToolButton {
            objectName: "resetRenderStats"
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            label: "↺"
            enabled: Preview.rendererAttached
            tip: Preview.rendererAttached
                ? qsTr("Reset per-canvas render counters")
                : qsTr("No renderer is attached")
            onClicked: Preview.resetStats()
        }
    }
}
