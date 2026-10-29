// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import Libertas

Window {
    id: dialog
    objectName: "websocketSettingsDialog"
    title: qsTr("WebSocket Server Settings")
    width: 500; height: 315
    minimumWidth: 420; minimumHeight: 315
    color: Theme.c.window
    modality: Qt.ApplicationModal
    flags: Qt.Dialog | Qt.FramelessWindowHint
    DialogGeometry { target: dialog; title: dialog.title }

    property int serverPort: 4455
    property string serverPassword: ""
    property string serverMode: "libertas"
    function open() {
        enableServer.checked = WebsocketServer.enabled;
        serverPort = WebsocketServer.port;
        serverPassword = WebsocketServer.password;
        serverMode = WebsocketServer.mode;
        show();
        requestActivate();
    }
    onVisibleChanged: if (!visible) {
        serverPassword = "";
        passwordField.clear();
    }
    Shortcut { sequence: "Escape"; enabled: dialog.visible; onActivated: dialog.close() }

    Column {
        anchors.fill: parent
        anchors.margins: 16
        anchors.topMargin: 48
        spacing: 12
        CheckLabel {
            id: enableServer
            objectName: "websocketEnabled"
            width: parent.width
            text: qsTr("Enable WebSocket server")
            onToggled: checked = !checked
            activeFocusOnTab: true
            Keys.onSpacePressed: toggled()
            Accessible.role: Accessible.CheckBox
            Accessible.name: text
            Accessible.checked: checked
            Accessible.onPressAction: toggled()
        }
        NumberField {
            objectName: "websocketPort"
            width: parent.width
            label: qsTr("Port")
            labelWidth: 100
            decimals: 0
            minimum: 1024
            maximum: 65535
            steppers: true
            value: dialog.serverPort
            onCommitted: value => dialog.serverPort = value
        }
        TextLine {
            id: passwordField
            objectName: "websocketPassword"
            width: parent.width
            label: qsTr("Password")
            labelWidth: 100
            echoMode: TextInput.Password
            commitOnEdit: true
            value: dialog.serverPassword
            onCommitted: value => dialog.serverPassword = value
            Accessible.name: qsTr("WebSocket password")
        }
        ChoiceField {
            id: modeField
            objectName: "websocketMode"
            width: parent.width
            label: qsTr("Mode")
            labelWidth: 100
            choices: ["libertas", "obs"]
            choiceLabels: ({"libertas": qsTr("Libertas Studio"), "obs": qsTr("OBS")})
            value: dialog.serverMode
            onCommitted: value => dialog.serverMode = value
            activeFocusOnTab: true
            Keys.onSpacePressed: committed(value === "libertas" ? "obs" : "libertas")
            Keys.onDownPressed: committed("obs")
            Keys.onUpPressed: committed("libertas")
            Accessible.onPressAction: committed(value === "libertas" ? "obs" : "libertas")
            Accessible.role: Accessible.ComboBox
            Accessible.name: qsTr("WebSocket mode")
            Accessible.description: choiceLabels[value]
        }
        Text {
            width: parent.width
            text: qsTr("Accepts connections on all IPv4 interfaces. OBS mode supports Companion’s OBS connection. Libertas Studio mode adds native controls.")
            font.pixelSize: 11
            color: Theme.c.textMuted
            wrapMode: Text.WordWrap
        }
        Text {
            objectName: "websocketStatus"
            width: parent.width
            text: WebsocketServer.error !== "" ? WebsocketServer.error
                : WebsocketServer.running ? qsTr("Listening on 0.0.0.0:%1").arg(WebsocketServer.port)
                : qsTr("Server stopped")
            font.pixelSize: 11
            color: WebsocketServer.error !== "" ? Theme.c.error : Theme.c.textMuted
            wrapMode: Text.WordWrap
        }
        Row {
            spacing: 8
            DialogButton {
                objectName: "applyWebsocketSettings"
                text: qsTr("Apply")
                primary: true
                onClicked: {
                    dialog.contentItem.forceActiveFocus();
                    WebsocketServer.applyMode(enableServer.checked, dialog.serverPort, dialog.serverPassword, dialog.serverMode);
                }
                activeFocusOnTab: true
                Keys.onReturnPressed: clicked()
                Keys.onSpacePressed: clicked()
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.onPressAction: clicked()
            }
            DialogButton {
                text: qsTr("Close")
                onClicked: dialog.close()
                activeFocusOnTab: true
                Keys.onReturnPressed: clicked()
                Keys.onSpacePressed: clicked()
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.onPressAction: clicked()
            }
        }
    }
}
