// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import MunkiCast

/// Bringing media into the pool.
///
/// The question this asks that a plain file chooser cannot is whether to copy. A project that
/// carries its own media still opens when the drive the footage came from is unplugged, and
/// can be handed to someone else whole; one that references media in place does not duplicate
/// a hundred gigabytes of footage to put it in a bin. Both are right, at different times, so
/// it is asked rather than decided here.
Item {
    id: dialog

    objectName: "mediaImportDialog"
    anchors.fill: parent
    z: 30
    visible: false

    property string binId: ""
    property var files: []
    /// Copying is the safer default: a project that carries its own media cannot be broken by
    /// someone tidying up the folder the footage came from.
    property bool copyIn: true

    readonly property var bin: {
        for (const b of Media.bins) {
            if (b.id === dialog.binId)
                return b;
        }
        return null;
    }
    /// Into a system bin a copy must land in that bin's own folder, or it would not appear in
    /// the bin it was imported into. So the choice is not the operator's there.
    readonly property bool forced: !!dialog.bin && !!dialog.bin.system

    function open(binId) {
        dialog.binId = binId;
        dialog.files = [];
        dialog.copyIn = true;
        dialog.visible = true;
        dialog.browse();
    }

    function browse() {
        const chosen = FileDialog.pickMany(qsTr("Import Media"), "", Media.mediaSuffixes());
        if (chosen.length > 0) {
            dialog.files = chosen;
        } else if (dialog.files.length === 0) {
            // Cancelled with nothing chosen: there is no decision left to make.
            dialog.visible = false;
        }
    }

    function accept() {
        Media.importMedia(dialog.files, dialog.binId, dialog.copyIn || dialog.forced);
        dialog.visible = false;
    }

    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.4
        MouseArea { anchors.fill: parent; onClicked: dialog.visible = false }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 24, 420)
        height: Math.min(parent.height - 24, 260)
        radius: 3
        color: Theme.c.panelBackground
        border.color: Theme.c.accent
        MouseArea { anchors.fill: parent; onClicked: {} }

        Text {
            id: title
            anchors { left: parent.left; top: parent.top; margins: 12 }
            text: dialog.bin ? qsTr("Import into “%1”").arg(dialog.bin.name)
                             : qsTr("Import Media")
            font { pixelSize: 12; bold: true }
            color: Theme.c.text
        }

        Text {
            id: count
            anchors { left: parent.left; right: parent.right; top: title.bottom; margins: 12 }
            text: dialog.files.length === 0
                  ? qsTr("No files chosen.")
                  : qsTr("%n file(s) chosen.", "", dialog.files.length)
            font.pixelSize: 11
            color: Theme.c.textMuted
        }

        // The files themselves, because importing the wrong take is easy and noticing it
        // afterwards is not.
        Flickable {
            id: list
            anchors { left: parent.left; right: parent.right; top: count.bottom
                      margins: 12 }
            height: 92
            clip: true
            contentHeight: names.height
            boundsBehavior: Flickable.StopAtBounds

            NoDragScroll {}

            Column {
                id: names
                width: list.width

                Repeater {
                    model: dialog.files

                    delegate: Text {
                        required property string modelData
                        width: names.width
                        text: modelData.split("/").pop()
                        font.pixelSize: 10
                        color: Theme.c.text
                        elide: Text.ElideMiddle
                    }
                }
            }
        }

        CheckLabel {
            id: copyCheck
            anchors { left: parent.left; right: parent.right; top: list.bottom; margins: 12 }
            // Forced for a system bin, and shown as forced rather than quietly ignored.
            checked: dialog.copyIn || dialog.forced
            enabled: !dialog.forced
            // Dimmed as well as inert: an unexplained tick that will not clear looks broken.
            opacity: dialog.forced ? 0.6 : 1.0
            text: dialog.forced
                  ? qsTr("Copied into the project's %1 folder").arg(dialog.bin.name)
                  : qsTr("Copy into the project's SourceMedia folder")
            onToggled: dialog.copyIn = !dialog.copyIn
        }

        Row {
            anchors { right: parent.right; bottom: parent.bottom; margins: 12 }
            spacing: 6

            DialogButton { text: qsTr("Choose Files…"); onClicked: dialog.browse() }
            DialogButton { text: qsTr("Cancel"); onClicked: dialog.visible = false }
            DialogButton {
                objectName: "mediaImportAccept"
                text: qsTr("Import")
                primary: true
                enabled: dialog.files.length > 0
                onClicked: dialog.accept()
            }
        }
    }
}
