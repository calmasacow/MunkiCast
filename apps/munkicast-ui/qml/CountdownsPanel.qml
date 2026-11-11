// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import MunkiCast

/// The countdown dock, laid out as the OBS plugin's is: per timer a title row, the DD:HH:MM:SS
/// readout, the four period fields, and reset/pause/play.
///
/// A timer is a binding onto an existing text source, never a source of its own. The timer
/// writes the time into that source, which is what puts a counting clock on air.
Item {
    id: panel
    objectName: "countdownsPanel"
    /// Bumped on a cadence so the readout follows the clock; the document does not change
    /// while a timer runs, so nothing else would repaint it.
    property int clockRevision: 0
    property string bindingId: ""

    // Only the dock's readout uses this timer; source workers keep the on-air clock running.
    Timer {
        interval: 200
        running: panel.visible && Document.countdowns.length > 0
        repeat: true
        triggeredOnStart: true
        onTriggered: panel.clockRevision++
    }

    CountdownSettingsDialog { id: settings }

    Flickable {
        anchors.fill: parent
        anchors.margins: 8
        contentWidth: width
        contentHeight: content.height
        clip: true
        interactive: false
        NoDragScroll {}

        Column {
            id: content
            width: parent.width
            spacing: 8

            // Binding row. No "new countdown text" button: making a text source is the sources
            // panel's job, and doing it here used to drop an empty element into the scene
            // before the timer that was going to fill it had been set up.
            Row {
                width: parent.width
                spacing: 6
                ChoiceField {
                    id: bindChoice
                    objectName: "countdownTextChoice"
                    width: parent.width - addBinding.width - 6
                    anchors.verticalCenter: parent.verticalCenter
                    label: qsTr("Bind text"); labelWidth: 60
                    choices: {
                        if (Document.revision < 0) return [];
                        return Document.countdownTextSources().filter(s => !s.bound).map(s => s.id);
                    }
                    choiceLabels: {
                        if (Document.revision < 0) return ({});
                        const names = {};
                        for (const s of Document.countdownTextSources()) names[s.id] = s.name;
                        return names;
                    }
                    value: panel.bindingId
                    onCommitted: value => panel.bindingId = value
                }
                DialogButton {
                    id: addBinding
                    objectName: "newCountdown"
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Add timer")
                    minimumWidth: 72
                    enabled: panel.bindingId !== ""
                    onClicked: {
                        Document.addCountdown(panel.bindingId);
                        panel.bindingId = "";
                    }
                }
            }

            Row {
                width: parent.width
                spacing: 6
                DialogButton {
                    text: qsTr("Import OBS timers…")
                    onClicked: {
                        const path = FileDialog.pick(qsTr("OBS countdown config.json"), "", false);
                        if (path !== "") Document.importCountdowns(path);
                    }
                }
                // Play-all and stop-all appear only with more than one timer, as the plugin's do:
                // with one, they are the buttons already on the card.
                DialogButton {
                    visible: Document.countdowns.length > 1
                    text: qsTr("▶ All")
                    minimumWidth: 56
                    onClicked: Document.controlAllCountdowns("start")
                }
                DialogButton {
                    visible: Document.countdowns.length > 1
                    text: qsTr("■ All")
                    minimumWidth: 56
                    onClicked: Document.controlAllCountdowns("reset")
                }
            }

            Text {
                width: parent.width
                visible: Document.countdowns.length === 0
                text: Document.revision >= 0 && Document.countdownTextSources().length === 0
                      ? qsTr("No text sources yet. Add a Text source to a scene, then bind a timer to it here.")
                      : qsTr("Pick a text source and add a timer. The timer writes the time into that source, so add it to each scene that should show the countdown.")
                wrapMode: Text.WordWrap
                color: Theme.c.textMuted
                font.pixelSize: 11
            }

            Repeater {
                model: Document.countdowns
                delegate: Rectangle {
                    id: timer
                    objectName: "countdownCard"

                    required property var modelData
                    required property int index
                    readonly property string sourceId: modelData.id
                    readonly property var status: {
                        // The worker clock does not emit document changes. Keep this tick
                        // dependency in the returned expression so compiled QML retains it.
                        if (panel.clockRevision < 0) return ({});
                        return Document.countdownStatus(sourceId);
                    }
                    readonly property bool datetime: modelData.type === "datetime"

                    function save(key, value) {
                        const settings = {};
                        settings[key] = value;
                        Document.configureCountdown(sourceId, settings);
                    }

                    width: content.width
                    height: form.height + 16
                    color: Theme.c.panelHeader
                    radius: 3

                    Column {
                        id: form
                        x: 8; y: 8
                        width: parent.width - 16
                        spacing: 6

                        // Title row: delete on the left, the timer's id in the middle, settings
                        // on the right -- the plugin's arrangement.
                        Item {
                            width: parent.width
                            height: 20

                            ToolButton {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                label: "🗑"
                                tip: qsTr("Remove this timer binding")
                                onClicked: Document.removeCountdown(timer.sourceId)
                            }
                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 60
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                                text: qsTr("Timer: %1").arg(timer.modelData.timerId
                                                            || timer.modelData.name)
                                font.pixelSize: 11
                                color: Theme.c.text
                            }
                            ToolButton {
                                objectName: "countdownSettingsButton"
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                label: "⚙"
                                tip: qsTr("Timer settings")
                                onClicked: settings.open(timer.sourceId)
                            }
                        }

                        // The readout, with the reorder arrows beside it.
                        Item {
                            width: parent.width
                            height: 30

                            Text {
                                anchors.left: parent.left
                                anchors.right: reorder.left
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                horizontalAlignment: Text.AlignHCenter
                                objectName: "countdownReadout"
                                text: timer.status.display || "00:00:00:00"
                                font.pixelSize: 22
                                font.family: "monospace"
                                color: timer.status.colour && timer.status.colour !== ""
                                       ? timer.status.colour : Theme.c.text
                                elide: Text.ElideRight
                            }
                            Column {
                                id: reorder
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                visible: Document.countdowns.length > 1
                                spacing: 1

                                ToolButton {
                                    label: "▴"
                                    tip: qsTr("Move this timer up")
                                    implicitHeight: 14
                                    enabled: timer.index > 0
                                    onClicked: Document.moveCountdown(timer.sourceId, true)
                                }
                                ToolButton {
                                    label: "▾"
                                    tip: qsTr("Move this timer down")
                                    implicitHeight: 14
                                    enabled: timer.index < Document.countdowns.length - 1
                                    onClicked: Document.moveCountdown(timer.sourceId, false)
                                }
                            }
                        }

                        // The period, as four fields with steppers. Disabled while running, as
                        // the plugin disables them: retiming a timer mid-run is how an operator
                        // loses track of what it is counting to.
                        Row {
                            width: parent.width
                            spacing: 2
                            visible: !timer.datetime

                            Repeater {
                                model: [
                                    {key: "periodDays", label: qsTr("D"), max: 999},
                                    {key: "periodHours", label: qsTr("H"), max: 23},
                                    {key: "periodMinutes", label: qsTr("M"), max: 59},
                                    {key: "periodSeconds", label: qsTr("S"), max: 59}
                                ]
                                delegate: Row {
                                    id: unit
                                    required property var modelData
                                    required property int index
                                    spacing: 2

                                    NumberField {
                                        objectName: "countdownPeriod_" + unit.modelData.key
                                        width: (form.width - 30) / 4
                                        anchors.verticalCenter: parent.verticalCenter
                                        label: unit.modelData.label; labelWidth: 10
                                        decimals: 0
                                        minimum: 0
                                        maximum: unit.modelData.max
                                        steppers: true
                                        enabled: !timer.status.running
                                        value: timer.modelData[unit.modelData.key] === undefined
                                               ? 0 : timer.modelData[unit.modelData.key]
                                        onCommitted: value => timer.save(unit.modelData.key,
                                                                         Math.round(value))
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: unit.index < 3
                                        text: ":"
                                        font.pixelSize: 11
                                        color: Theme.c.textMuted
                                    }
                                }
                            }
                        }

                        TextLine {
                            width: parent.width
                            label: qsTr("To time"); labelWidth: 60
                            visible: timer.datetime
                            enabled: !timer.status.running
                            value: timer.modelData.targetAtMs
                                   ? new Date(Number(timer.modelData.targetAtMs)).toISOString()
                                   : ""
                            placeholder: "2026-10-23T20:00:00-05:00"
                            onCommitted: value => {
                                const date = Date.parse(value);
                                if (!isNaN(date)) timer.save("targetAtMs", date);
                                else NoticeLog.add(qsTr("Enter a valid ISO date and time with timezone."));
                            }
                        }

                        // Period/datetime toggle and the transport, in one row as the plugin
                        // has them.
                        Row {
                            width: parent.width
                            spacing: 4

                            DialogButton {
                                objectName: "countdownReset"
                                text: qsTr("↻")
                                minimumWidth: 36
                                enabled: !timer.status.running
                                onClicked: Document.controlCountdown(timer.sourceId, "reset")
                            }
                            DialogButton {
                                objectName: "countdownStartPause"
                                text: timer.status.running ? qsTr("❚❚") : qsTr("▶")
                                minimumWidth: form.width - 36 - 36 - 36 - 16
                                primary: !timer.status.running
                                onClicked: Document.controlCountdown(
                                    timer.sourceId, timer.status.running ? "pause" : "start")
                            }
                            DialogButton {
                                text: qsTr("P")
                                minimumWidth: 30
                                primary: !timer.datetime
                                enabled: !timer.status.running
                                onClicked: timer.save("type", "period")
                            }
                            DialogButton {
                                text: qsTr("D")
                                minimumWidth: 30
                                primary: timer.datetime
                                enabled: !timer.status.running
                                onClicked: timer.save("type", "datetime")
                            }
                        }

                        // What the bound source will actually show, which is not the readout
                        // above it once a format or an end message is set.
                        Text {
                            width: parent.width
                            text: qsTr("Shows: %1").arg(timer.status.text || "")
                            font.pixelSize: 10
                            color: Theme.c.textMuted
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }
    }
}
