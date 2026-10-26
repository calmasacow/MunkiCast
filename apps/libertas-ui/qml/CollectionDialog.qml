// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as Controls
import Libertas

Item {
    id: dialog
    objectName: "collectionDialog"
    anchors.fill: parent
    z: 300
    visible: false
    focus: visible
    property string mode: "new"
    property string collectionName: ""
    property string path: ""
    property string format: "OBS JSON"
    property int canvasWidth: 1920
    property int canvasHeight: 1080
    property int fpsNum: 60
    property int fpsDen: 1
    property string errorText: ""
    property string reportText: ""
    property bool busy: false
    readonly property bool isImport: mode === "import"
    readonly property bool isExport: mode === "export"
    readonly property string heading: ({new: qsTr("New Scene Collection"), duplicate: qsTr("Duplicate Scene Collection"),
        rename: qsTr("Rename Scene Collection"), remove: qsTr("Remove Scene Collection"),
        import: qsTr("Import Scene Collection"), export: qsTr("Export Scene Collection"), report: qsTr("Conversion Report")})[mode] || ""

    function open(action) {
        mode = action;
        collectionName = action === "duplicate" ? Document.projectName + qsTr(" Copy")
            : action === "new" || action === "import" ? "" : Document.projectName;
        path = "";
        errorText = "";
        reportText = "";
        format = action === "export" ? "Libertas native" : "OBS JSON";
        visible = true;
        forceActiveFocus();
    }
    function close() { if (!busy) visible = false; }
    function browse() {
        const chosen = isExport ? FileDialog.saveFile(qsTr("Export Scene Collection"), path ||
                            collectionName.replace(/[/\\]/g, "_") + (format === "OBS JSON" ? ".json" : ".libertas"))
            : format === "OBS JSON" ? FileDialog.pick(qsTr("Import OBS collection JSON"), path, false)
            : FileDialog.directory(qsTr("Select Libertas collection directory"), path);
        if (chosen) path = chosen;
        else errorText = FileDialog.lastError();
    }
    function accept() {
        forceActiveFocus(); // Flush field edits before the operation reads them.
        Qt.callLater(function() {
            if (dialog.mode === "report") { dialog.close(); return; }
            dialog.busy = true;
            dialog.errorText = "";
            let ok = false;
            if (dialog.mode === "new" || dialog.mode === "duplicate")
                ok = Document.createCollection(dialog.collectionName, dialog.mode === "duplicate");
            else if (dialog.mode === "rename") ok = Document.renameCollection(dialog.collectionName);
            else if (dialog.mode === "remove") ok = Document.removeCollection();
            else if (dialog.isImport) ok = Document.importCollection(dialog.path, dialog.format === "OBS JSON",
                dialog.collectionName, dialog.canvasWidth, dialog.canvasHeight, dialog.fpsNum, dialog.fpsDen);
            else if (dialog.isExport) ok = Document.exportCollection(dialog.path, dialog.format === "OBS JSON");
            dialog.busy = false;
            if (ok && (dialog.isImport || dialog.isExport)) {
                dialog.reportText = Document.collectionReport;
                dialog.mode = "report";
            } else if (ok) dialog.close();
        });
    }
    Keys.onEscapePressed: close()
    Connections {
        target: Document
        function onNotice(text) { if (dialog.visible && dialog.mode !== "report") dialog.errorText = text; }
    }
    Rectangle { anchors.fill: parent; color: Theme.c.window; opacity: 0.65 }
    MouseArea { anchors.fill: parent; onClicked: dialog.close(); onWheel: wheel => wheel.accepted = true }
    Rectangle {
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        objectName: "collectionFrame"
        DialogGeometry { minimumWidth: 480; minimumHeight: 400 }
        width: Math.min(parent.width - 48, 680)
        height: Math.min(parent.height - 48, dialog.mode === "report" ? 540 : dialog.isImport ? 500 : 440)
        color: Theme.c.panelBackground
        border.color: Theme.c.accent
        radius: 3
        MouseArea { anchors.fill: parent; onClicked: {} }
        Text {
            id: title
            anchors { left: parent.left; top: parent.top; margins: 16 }
            text: dialog.heading
            font { pixelSize: 14; bold: true }
            color: Theme.c.text
        }
        Column {
            id: fields
            visible: dialog.mode !== "report"
            anchors { left: parent.left; right: parent.right; top: title.bottom; margins: 16 }
            spacing: 10
            Text {
                width: parent.width
                text: dialog.mode === "remove"
                    ? qsTr('Move "%1" to Trash?\n%2\nRestore it from Trash if needed. A blank collection will open.').arg(Document.projectName).arg(Document.projectPath)
                    : qsTr("Current collection is saved before switching. Existing export paths are never overwritten.")
                wrapMode: Text.WrapAnywhere
                color: Theme.c.textMuted
                font.pixelSize: 11
            }
            TextLine {
                objectName: "collectionNameField"
                visible: !dialog.isExport && dialog.mode !== "remove"
                width: parent.width; labelWidth: 110; label: qsTr("Name")
                value: dialog.collectionName
                onCommitted: newValue => dialog.collectionName = newValue
            }
            ChoiceField {
                visible: dialog.isImport || dialog.isExport
                width: parent.width; labelWidth: 110; label: qsTr("Format")
                choices: ["Libertas native", "OBS JSON"]; value: dialog.format
                onCommitted: newValue => { dialog.format = newValue; dialog.path = ""; }
            }
            TextLine {
                objectName: "collectionPathField"
                visible: dialog.isImport || dialog.isExport
                width: parent.width; labelWidth: 110
                label: dialog.isExport ? qsTr("New export path") : qsTr("Import path")
                value: dialog.path; browsable: true
                onCommitted: newValue => dialog.path = newValue
                onBrowseRequested: dialog.browse()
            }
            Text {
                visible: dialog.isImport && dialog.format === "OBS JSON"
                width: parent.width; wrapMode: Text.Wrap; font.pixelSize: 11; color: Theme.c.textMuted
                text: qsTr("OBS JSON does not include profile video settings. Enter its canvas size and exact frame rate.")
            }
            Row {
                visible: dialog.isImport && dialog.format === "OBS JSON"
                width: parent.width; spacing: 16
                NumberField { width: (parent.width - 16) / 2; labelWidth: 110; label: qsTr("Canvas width"); decimals: 0; minimum: 1; maximum: 8192; value: dialog.canvasWidth; onCommitted: v => dialog.canvasWidth = v }
                NumberField { width: (parent.width - 16) / 2; labelWidth: 110; label: qsTr("Canvas height"); decimals: 0; minimum: 1; maximum: 8192; value: dialog.canvasHeight; onCommitted: v => dialog.canvasHeight = v }
            }
            Row {
                visible: dialog.isImport && dialog.format === "OBS JSON"
                width: parent.width; spacing: 16
                NumberField { width: (parent.width - 16) / 2; labelWidth: 110; label: qsTr("FPS numerator"); decimals: 0; minimum: 1; maximum: 240000; value: dialog.fpsNum; onCommitted: v => dialog.fpsNum = v }
                NumberField { width: (parent.width - 16) / 2; labelWidth: 110; label: qsTr("FPS denominator"); decimals: 0; minimum: 1; maximum: 10000; value: dialog.fpsDen; onCommitted: v => dialog.fpsDen = v }
            }
            Text {
                visible: dialog.isExport; width: parent.width; wrapMode: Text.Wrap; font.pixelSize: 11; color: Theme.c.textMuted
                text: dialog.format === "OBS JSON"
                    ? qsTr("OBS export includes a compatibility report. Set OBS profile canvas/FPS separately; outputs and audio routing require review.")
                    : qsTr("Native export creates a .libertas directory. Companion files copy with it; absolute external media references stay external.")
            }
        }
        Flickable {
            interactive: false
            clip: true
            contentHeight: reportView.height
            contentWidth: width
            boundsBehavior: Flickable.StopAtBounds
            NoDragScroll {}
            visible: dialog.mode === "report"
            anchors { left: parent.left; right: parent.right; top: title.bottom; bottom: buttons.top; margins: 16 }
            Controls.TextArea {
                id: reportView
                width: parent.width
                height: implicitHeight
                text: dialog.reportText; readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
                color: Theme.c.text; font.pixelSize: 12
                textFormat: TextEdit.PlainText
                background: Rectangle { color: Theme.c.window }
            }
        }
        Text {
            anchors { left: parent.left; right: parent.right; bottom: buttons.top; margins: 16 }
            visible: dialog.mode !== "report"; text: dialog.errorText
            color: Theme.c.text; font.pixelSize: 11; wrapMode: Text.WrapAnywhere
        }
        Row {
            id: buttons
            anchors { right: parent.right; bottom: parent.bottom; margins: 16 }
            spacing: 8
            DialogButton { text: qsTr("Cancel"); visible: dialog.mode !== "report"; enabled: !dialog.busy; onClicked: dialog.close() }
            DialogButton {
                objectName: "collectionAccept"
                text: dialog.busy ? qsTr("Working…") : ({new: qsTr("Create"), duplicate: qsTr("Duplicate"), rename: qsTr("Rename"), remove: qsTr("Move to Trash"), import: qsTr("Import"), export: qsTr("Export"), report: qsTr("Done")})[dialog.mode]
                primary: true
                enabled: !dialog.busy
                onClicked: dialog.accept()
            }
        }
    }
}
