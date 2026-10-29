// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import Libertas

Window {
    id: dialog
    objectName: "mcpSettingsDialog"
    title: qsTr("MCP Server Settings")
    width: 570; height: 370
    minimumWidth: 480; minimumHeight: 370
    color: Theme.c.window
    modality: Qt.ApplicationModal
    flags: Qt.Dialog | Qt.FramelessWindowHint
    DialogGeometry { target: dialog; title: dialog.title }
    property int serverPort: 4466
    property string serverToken: ""
    function open() {
        enableServer.checked = McpServer.enabled;
        serverPort = McpServer.port;
        serverToken = McpServer.token;
        tokenField.echoMode = TextInput.Password;
        show(); requestActivate();
    }
    onVisibleChanged: if (!visible) { serverToken = ""; tokenField.clear(); }
    Shortcut { sequence: "Escape"; enabled: dialog.visible; onActivated: dialog.close() }
    Column {
        anchors.fill: parent
        anchors.margins: 16
        anchors.topMargin: 48
        spacing: 12
        CheckLabel {
            id: enableServer
            objectName: "mcpEnabled"
            width: parent.width
            text: qsTr("Enable native MCP server")
            onToggled: checked = !checked
            activeFocusOnTab: true
            Keys.onSpacePressed: toggled()
            Accessible.role: Accessible.CheckBox
            Accessible.name: text
            Accessible.checked: checked
            Accessible.onPressAction: toggled()
        }
        NumberField {
            objectName: "mcpPort"
            width: parent.width
            label: qsTr("Port")
            labelWidth: 110
            decimals: 0; minimum: 1024; maximum: 65535; steppers: true
            value: dialog.serverPort
            onCommitted: value => dialog.serverPort = value
        }
        TextLine {
            id: tokenField
            objectName: "mcpToken"
            width: parent.width
            label: qsTr("Access token")
            labelWidth: 110
            echoMode: TextInput.Password
            commitOnEdit: true
            value: dialog.serverToken
            onCommitted: value => dialog.serverToken = value
            Accessible.name: qsTr("MCP access token")
        }
        TextEdit {
            objectName: "mcpEndpoint"
            width: parent.width
            text: "http://127.0.0.1:" + dialog.serverPort + "/mcp"
            color: Theme.c.text
            font.pixelSize: 12
            readOnly: true
            selectByMouse: true
            Accessible.name: qsTr("MCP endpoint")
        }
        Text {
            width: parent.width
            text: qsTr("Independent AI control of the application: scenes, sources, transforms, menus and settings. Connect an MCP client over Streamable HTTP using this endpoint and a Bearer access token. Listens only on this computer. WebSocket settings are separate.")
            font.pixelSize: 11
            color: Theme.c.textMuted
            wrapMode: Text.WordWrap
        }
        Text {
            objectName: "mcpStatus"
            width: parent.width
            text: McpServer.error !== "" ? McpServer.error : McpServer.running ? qsTr("MCP listening at %1").arg(McpServer.endpoint) : qsTr("MCP server stopped")
            color: McpServer.error !== "" ? Theme.c.error : Theme.c.textMuted
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }
        Row {
            spacing: 8
            DialogButton {
                objectName: "applyMcpSettings"
                text: qsTr("Apply")
                primary: true
                onClicked: { dialog.contentItem.forceActiveFocus(); McpServer.apply(enableServer.checked, dialog.serverPort, dialog.serverToken); }
                activeFocusOnTab: true
                Keys.onReturnPressed: clicked()
                Keys.onSpacePressed: clicked()
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.onPressAction: clicked()
            }
            DialogButton {
                text: qsTr("New token")
                onClicked: { dialog.contentItem.forceActiveFocus(); dialog.serverToken = McpServer.newToken(); }
                activeFocusOnTab: true
                Keys.onReturnPressed: clicked()
                Keys.onSpacePressed: clicked()
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.onPressAction: clicked()
            }
            DialogButton {
                text: tokenField.echoMode === TextInput.Password ? qsTr("Show token") : qsTr("Hide token")
                onClicked: tokenField.echoMode = tokenField.echoMode === TextInput.Password ? TextInput.Normal : TextInput.Password
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
