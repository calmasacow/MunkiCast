// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import MunkiCast

Column {
    id: settings
    required property string outputId
    property var destination: ({})
    spacing: 6
    function refresh() { destination = Document.outputEncoderSettings(outputId); }
    onOutputIdChanged: refresh()
    Component.onCompleted: refresh()
    Connections { target: Document; function onChanged() { settings.refresh(); } }

    CheckLabel {
        objectName: "enableReplayBuffer"
        width: parent.width
        text: qsTr("Enable Replay Buffer")
        checked: !!settings.destination.enabled
        enabled: !Preview.outputPending && !Preview.replaySaving
        onToggled: Document.setOutputEnabled(settings.outputId, !checked)
    }
    NumberField {
        objectName: "replaySeconds"
        width: parent.width
        label: qsTr("Maximum Replay Time")
        labelWidth: 145
        value: settings.destination.replaySeconds || 20
        decimals: 0
        minimum: 1
        maximum: 86400
        suffix: "s"
        steppers: true
        enabled: !Preview.replayActive && !Preview.outputPending
        onCommitted: value => Document.setReplaySettings(settings.outputId, value,
            settings.destination.replayMemoryMB || 512)
    }
    NumberField {
        objectName: "replayMemoryMB"
        width: parent.width
        label: qsTr("Maximum Memory")
        labelWidth: 145
        value: settings.destination.replayMemoryMB || 512
        decimals: 0
        minimum: 1
        maximum: 1048576
        suffix: "MB"
        steppers: true
        enabled: !Preview.replayActive && !Preview.outputPending
        onCommitted: value => Document.setReplaySettings(settings.outputId,
            settings.destination.replaySeconds || 20, value)
    }
    Text {
        width: parent.width
        text: Preview.replayActive
            ? qsTr("Buffered %1s · %2MB").arg(Math.floor(Preview.replayDurationMs / 1000))
                .arg((Preview.replayBytes / 1048576).toFixed(1))
            : qsTr("RAM buffer. The memory limit may shorten the clip.")
        font.pixelSize: 10
        color: Theme.c.textMuted
        wrapMode: Text.WordWrap
    }
}
