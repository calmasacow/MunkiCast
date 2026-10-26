// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// "Some of this lands off the canvas — still paste?"
///
/// Pasting keeps every source's coordinates rather than scaling them, because no scaling turns
/// a 16:9 layout into a 9:16 one and a paste that silently edits the framing is worse than one
/// that lands somewhere visible (ADR-0036). What is owed in exchange is a warning, and the
/// operator was specific about its shape: say that something is off, do not list what.
Item {
    id: warning

    property int offCanvas: 0

    anchors.fill: parent
    visible: false
    focus: visible

    function ask(count) {
        warning.offCanvas = count;
        warning.visible = true;
        warning.forceActiveFocus();
    }

    function dismiss() {
        warning.visible = false;
    }

    Keys.onEscapePressed: warning.dismiss()

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: warning.dismiss()
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
        objectName: "pasteWarningFrame"
        DialogGeometry { resizable: false }
        width: Math.min(parent.width - 80, 380)
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
                text: qsTr("Some of this is off the canvas")
                font.pixelSize: 12
                font.bold: true
                color: Theme.c.text
                wrapMode: Text.WordWrap
            }

            Text {
                width: parent.width
                text: qsTr("%n source(s) would land outside this canvas. Paste anyway?", "",
                           warning.offCanvas)
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
                objectName: "cancelPaste"
                text: qsTr("Cancel")
                onClicked: warning.dismiss()
            }

            DialogButton {
                objectName: "confirmPaste"
                text: qsTr("Paste")
                primary: true
                onClicked: {
                    Document.paste();
                    warning.dismiss();
                }
            }
        }
    }
}
