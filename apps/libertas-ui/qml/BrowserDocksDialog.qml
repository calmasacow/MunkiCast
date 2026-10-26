// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import Libertas

Controls.Dialog {
    id: dialog
    objectName: "browserDocksDialog"
    title: qsTr("Custom Browser Docks")
    anchors.centerIn: parent
    width: Math.min(700, parent.width - 40)
    height: Math.min(500, parent.height - 40)
    modal: true
    standardButtons: Controls.Dialog.Close
    property string editingId: ""

    function clearEditor() {
        editingId = "";
        nameField.clear();
        urlField.clear();
    }
    onOpened: clearEditor()

    contentItem: ColumnLayout {
        spacing: 10
        Controls.Label {
            text: qsTr("Add a named web page, then arrange its tab like any other dock.")
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Controls.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ColumnLayout {
                width: parent.width
                Repeater {
                    model: Workspace.browserDocks
                    delegate: RowLayout {
                        id: row
                        required property var modelData
                        Layout.fillWidth: true
                        Controls.Label {
                            text: row.modelData.name
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Controls.Button {
                            text: qsTr("Show")
                            onClicked: {
                                Workspace.openPanel(row.modelData.id);
                                Workspace.activate(row.modelData.id);
                                dialog.close();
                            }
                        }
                        Controls.Button {
                            text: qsTr("Edit")
                            onClicked: {
                                dialog.editingId = row.modelData.id;
                                nameField.text = row.modelData.name;
                                urlField.text = row.modelData.url;
                            }
                        }
                        Controls.Button {
                            text: qsTr("Remove")
                            onClicked: {
                                if (Workspace.removeBrowserDock(row.modelData.id))
                                    dialog.clearEditor();
                            }
                        }
                    }
                }
            }
        }
        Controls.Label {
            visible: Workspace.browserDocks.length === 0
            text: qsTr("No browser docks yet.")
        }
        Controls.TextField {
            id: nameField
            objectName: "browserDockName"
            placeholderText: qsTr("Dock name")
            Accessible.name: qsTr("Dock name")
            Layout.fillWidth: true
            onAccepted: urlField.forceActiveFocus()
        }
        Controls.TextField {
            id: urlField
            objectName: "browserDockUrl"
            placeholderText: "https://…"
            Accessible.name: qsTr("Dock URL")
            Layout.fillWidth: true
            selectByMouse: true
            onAccepted: saveButton.clicked()
        }
        Controls.Label {
            text: Workspace.browserDockError
            visible: text !== ""
            color: Theme.c.warning
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        RowLayout {
            Controls.Button {
                id: saveButton
                objectName: "saveBrowserDock"
                text: dialog.editingId === "" ? qsTr("Add dock") : qsTr("Save changes")
                onClicked: {
                    if (Workspace.saveBrowserDock(dialog.editingId, nameField.text, urlField.text))
                        dialog.clearEditor();
                }
            }
            Controls.Button {
                text: qsTr("Cancel edit")
                visible: dialog.editingId !== ""
                onClicked: dialog.clearEditor()
            }
        }
    }
}
