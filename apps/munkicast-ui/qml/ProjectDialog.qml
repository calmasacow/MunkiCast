// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import MunkiCast

/// Making a project: its name, and where the folder for it goes.
///
/// Both, because a project is a folder *named after itself*. Asking only for a location and
/// taking the name from the folder picked meant choosing an existing folder -- Videos, say --
/// and having the tree built inside it, scattering Collections and Recordings through
/// somebody's media folder. The project is created at location/name instead, which is also
/// what makes the name a real choice rather than a thing scraped off a path.
Item {
    id: dialog
    objectName: "projectDialog"
    anchors.fill: parent
    z: 300
    visible: false
    focus: visible

    property string projectName: ""
    property string location: ""
    property string errorText: ""
    property bool busy: false

    /// Where the project will be made. Shown before it is, because a path assembled out of
    /// two fields is one the operator should see rather than infer.
    readonly property string target: {
        const clean = dialog.projectName.trim();
        if (dialog.location === "" || clean === "")
            return "";
        return dialog.location.replace(/\/+$/, "") + "/" + clean;
    }

    function open() {
        projectName = "";
        // Beside the last project, which is where the next one usually goes.
        location = Document.openProjectPath
                   ? Document.openProjectPath.replace(/\/[^/]*$/, "")
                   : "";
        errorText = "";
        busy = false;
        visible = true;
        forceActiveFocus();
    }

    function close() { if (!busy) visible = false; }

    function browse() {
        const chosen = FileDialog.directory(qsTr("Where to put the project"), dialog.location);
        if (chosen) dialog.location = chosen;
        else dialog.errorText = FileDialog.lastError();
    }

    function accept() {
        forceActiveFocus(); // Flush field edits before the operation reads them.
        Qt.callLater(function() {
            if (dialog.target === "") {
                dialog.errorText = qsTr("Enter a name and choose where to put it.");
                return;
            }
            dialog.busy = true;
            dialog.errorText = "";
            const ok = Document.createProject(dialog.target, dialog.projectName.trim());
            dialog.busy = false;
            if (ok)
                dialog.visible = false;
        });
    }

    Connections {
        target: Document
        function onNotice(text) { if (dialog.visible) dialog.errorText = text; }
    }

    Keys.onEscapePressed: dialog.close()
    Keys.onReturnPressed: dialog.accept()
    Keys.onEnterPressed: dialog.accept()

    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.45
        MouseArea { anchors.fill: parent; onClicked: {} }
    }

    Rectangle {
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        objectName: "projectFrame"
        DialogGeometry { minimumWidth: 460; minimumHeight: 230 }
        width: Math.min(parent.width - 48, 640)
        // Two fields, a line of explanation and the path: sized to that rather than to a
        // round number, which left a hand's depth of empty panel above the buttons.
        height: Math.min(parent.height - 48, 250)
        color: Theme.c.panelBackground
        border.color: Theme.c.accent
        radius: 3
        MouseArea { anchors.fill: parent; onClicked: {} }

        Text {
            id: title
            anchors { left: parent.left; top: parent.top; margins: 16 }
            text: qsTr("New Project")
            font { pixelSize: 14; bold: true }
            color: Theme.c.text
        }

        Column {
            anchors { left: parent.left; right: parent.right; top: title.bottom; margins: 16 }
            spacing: 10

            Text {
                width: parent.width
                wrapMode: Text.Wrap
                font.pixelSize: 11
                color: Theme.c.textMuted
                text: qsTr("A project is a folder holding its scene collections, timelines, " +
                           "graphics, recordings and exports. A folder is made for it where " +
                           "you choose, named after the project.")
            }

            TextLine {
                objectName: "projectNameField"
                width: parent.width; labelWidth: 110
                label: qsTr("Name")
                value: dialog.projectName
                onCommitted: newValue => dialog.projectName = newValue
            }

            TextLine {
                objectName: "projectLocationField"
                width: parent.width; labelWidth: 110
                label: qsTr("Location")
                value: dialog.location; browsable: true
                onCommitted: newValue => dialog.location = newValue
                onBrowseRequested: dialog.browse()
            }

            Text {
                objectName: "projectTargetLabel"
                width: parent.width
                visible: dialog.target !== ""
                wrapMode: Text.WrapAnywhere
                font { pixelSize: 11; family: "monospace" }
                color: Theme.c.textMuted
                text: qsTr("Will be created at  %1").arg(dialog.target)
            }
        }

        Text {
            anchors { left: parent.left; right: parent.right; bottom: buttons.top; margins: 16 }
            text: dialog.errorText
            color: Theme.c.text
            font.pixelSize: 11
            wrapMode: Text.WrapAnywhere
        }

        Row {
            id: buttons
            anchors { right: parent.right; bottom: parent.bottom; margins: 16 }
            spacing: 8

            DialogButton {
                text: qsTr("Cancel")
                enabled: !dialog.busy
                onClicked: dialog.close()
            }

            DialogButton {
                objectName: "projectAccept"
                text: dialog.busy ? qsTr("Working…") : qsTr("Create")
                primary: true
                // Both fields, because the project is made at one joined to the other.
                enabled: !dialog.busy && dialog.target !== ""
                onClicked: dialog.accept()
            }
        }
    }
}
