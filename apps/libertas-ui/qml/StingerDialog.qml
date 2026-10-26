// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import Libertas

Window {
    id: dialog
    objectName: "stingerDialog"
    title: qsTr("Add Stinger Transition")
    width: 620; height: 317
    minimumWidth: 500; minimumHeight: 317
    color: Theme.c.window
    modality: Qt.ApplicationModal
    flags: Qt.Dialog | Qt.FramelessWindowHint
    DialogGeometry { target: dialog; title: dialog.title }
    property string deckId: ""
    property string stingerName: "Stinger"
    property string filePath: ""
    property int durationMs: 1000
    property int cutMs: 500
    function open(id) { deckId = id; show(); requestActivate(); }
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
                text: qsTr("Add stinger"); primary: true
                objectName: "acceptStinger"
                onClicked: {
                    if (Document.addStinger(dialog.deckId, dialog.stingerName, dialog.filePath,
                                            dialog.cutMs, dialog.durationMs)) dialog.close();
                }
            }
            DialogButton { text: qsTr("Cancel"); onClicked: dialog.close() }
        }
    }
}
