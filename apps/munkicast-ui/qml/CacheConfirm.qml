// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// "This throws away prepared copies — go on?"
///
/// Two of the three Caching entries destroy work: Delete Cache throws away every prepared
/// file, and Rebuild throws away the ones it is about to replace. Both can be 31 seconds of
/// copying on the reference collection, and both are one click from a menu, so both are asked
/// about first.
///
/// Self-contained in the shape PasteWarning established: its own scrim, its own key handling,
/// no state outside it. That is what lets it be hosted inside the Preferences window as well
/// as over the workspace.
Item {
    id: confirm

    /// "clear" or "rebuild". Empty when nothing is being asked.
    property string pendingAction: ""

    anchors.fill: parent
    visible: confirm.pendingAction !== ""
    focus: visible

    function ask(action) {
        confirm.pendingAction = action;
        confirm.forceActiveFocus();
    }

    function dismiss() {
        confirm.pendingAction = "";
    }

    function go() {
        const action = confirm.pendingAction;
        confirm.dismiss();
        if (action === "clear")
            AssetCache.clear();
        else if (action === "rebuild")
            AssetCache.build(true);
    }

    Keys.onEscapePressed: confirm.dismiss()

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: confirm.dismiss()
        onWheel: (wheel) => { wheel.accepted = true; }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
        opacity: 0.55
    }

    Rectangle {
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        objectName: "cacheConfirmFrame"
        DialogGeometry { resizable: false }
        width: Math.min(parent.width - 80, 400)
        height: body.height + bar.height + 28
        color: Theme.c.panelBackground
        border.width: 1
        border.color: Theme.c.warning
        radius: 3

        MouseArea {
            anchors.fill: parent
            onClicked: {}
        }

        Column {
            id: body

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 14
            spacing: 6

            Text {
                width: parent.width
                text: confirm.pendingAction === "clear" ? qsTr("Delete every prepared copy?")
                                                        : qsTr("Prepare every file again?")
                font.pixelSize: 12
                font.bold: true
                color: Theme.c.text
                wrapMode: Text.WordWrap
            }

            Text {
                width: parent.width
                text: confirm.pendingAction === "clear"
                    ? qsTr("Every source goes back to reading the file where it lives. Nothing in the show is lost, but it has to be prepared again before it plays from local disk.")
                    : qsTr("Copies that are already good are thrown away and made again. On a large show this takes a while, and the show reads its originals until each one is back.")
                font.pixelSize: 11
                color: Theme.c.textMuted
                wrapMode: Text.WordWrap
            }
        }

        Row {
            id: bar

            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 14
            spacing: 8

            DialogButton {
                objectName: "cancelCacheAction"
                text: qsTr("Cancel")
                onClicked: confirm.dismiss()
            }

            DialogButton {
                objectName: "confirmCacheAction"
                text: confirm.pendingAction === "clear" ? qsTr("Delete") : qsTr("Rebuild")
                primary: true
                onClicked: confirm.go()
            }
        }
    }
}
