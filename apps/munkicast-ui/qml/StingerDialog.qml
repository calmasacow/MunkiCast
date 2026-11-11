// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import MunkiCast

Window {
    id: dialog
    objectName: "stingerDialog"
    title: sourceId === "" ? qsTr("Add Stinger Transition") : qsTr("Edit Stinger Transition")
    width: 620; height: 317
    minimumWidth: 500; minimumHeight: 317
    color: Theme.c.window
    modality: Qt.ApplicationModal
    flags: Qt.Dialog | Qt.FramelessWindowHint
    DialogGeometry { target: dialog; title: dialog.title }
    property string deckId: ""
    property string sourceId: ""
    property string stingerName: "Stinger"
    property string filePath: ""
    property int durationMs: 1000
    property int cutMs: 500
    function open(id, editing = false) {
        deckId = id;
        const settings = editing ? Document.stingerSettings(id) : ({});
        sourceId = settings.sourceId || "";
        stingerName = settings.name || "Stinger";
        filePath = settings.file || "";
        durationMs = settings.durationMs === undefined ? 1000 : settings.durationMs;
        cutMs = settings.cutMs === undefined ? 500 : settings.cutMs;
        show(); requestActivate();
    }
    Column {
        anchors.fill: parent
        anchors.margins: 16
        anchors.topMargin: 48
        spacing: 12
        TextLine {
            width: parent.width; label: qsTr("Name"); labelWidth: 120
            value: dialog.stingerName
            onCommitted: value => dialog.stingerName = value
        }
        TextLine {
            width: parent.width; label: qsTr("Video file"); labelWidth: 120
            value: dialog.filePath; browsable: true
            onCommitted: value => dialog.filePath = value
            onBrowseRequested: {
                const path = FileDialog.pick(qsTr("Stinger video"), dialog.filePath, false);
                if (path !== "") dialog.filePath = path;
            }
        }
        NumberField {
            width: parent.width; label: qsTr("Duration"); labelWidth: 120
            decimals: 0; minimum: 1; suffix: "ms"
            value: dialog.durationMs
            onCommitted: value => dialog.durationMs = Math.round(value)
        }
        NumberField {
            width: parent.width; label: qsTr("Cut point"); labelWidth: 120
            decimals: 0; minimum: 0; suffix: "ms"
            value: dialog.cutMs
            onCommitted: value => dialog.cutMs = Math.round(value)
        }
        Text {
            width: parent.width
            text: qsTr("Enter the clip duration and time when it covers the scene. Transparent video and its audio play over the scene change. Track mattes are not supported.")
            color: Theme.c.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: 11
        }
        Row {
            spacing: 8
            DialogButton {
                text: dialog.sourceId === "" ? qsTr("Add stinger") : qsTr("Save changes"); primary: true
                objectName: "acceptStinger"
                onClicked: {
                    if (Document.saveStinger(dialog.deckId, dialog.stingerName, dialog.filePath,
                                             dialog.cutMs, dialog.durationMs, dialog.sourceId)) dialog.close();
                }
            }
            DialogButton {
                visible: dialog.sourceId !== ""
                text: qsTr("Delete stinger")
                objectName: "deleteStingerEntry"
                onClicked: {
                    Document.removeSourceAndUses(dialog.sourceId);
                    dialog.close();
                }
            }
            DialogButton { text: qsTr("Cancel"); onClicked: dialog.close() }
        }
    }
}
