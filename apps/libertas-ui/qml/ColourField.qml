// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// A colour, drawn as itself with its hex written on it.
///
/// Clicking it opens Qt's colour dialog. That dialog is the picker, and the custom colours
/// saved in it are there the next time it is opened, whichever source the field belongs to.
/// The hex stays on the bar so the colour can be read without opening anything.
///
/// Values are sRGB hex, which is what a human reads. Everything the document composites in is
/// scene-linear, and the one conversion between them lives in `ldoc::Rgba` (ADR: types.hpp).
Item {
    id: field

    required property string label
    property int labelWidth: 54
    property int labelAlign: Text.AlignLeft
    /// "#rrggbb".
    property string value: "#000000"

    signal committed(string newValue)

    function pick() {
        const chosen = ColourDialog.pick(field.parses ? field.value : "#ffffff");
        if (chosen !== "")
            field.committed(chosen);
    }

    implicitHeight: 22

    readonly property bool parses: /^#[0-9a-fA-F]{6}$/.test(field.value)
    readonly property color shown: field.parses ? field.value : "transparent"

    Text {
        id: caption

        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: field.labelWidth
        horizontalAlignment: field.labelAlign
        text: field.label
        font.pixelSize: 11
        color: Theme.c.textMuted
        elide: Text.ElideRight
    }

    Rectangle {
        anchors.left: caption.right
        anchors.leftMargin: 4
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 20
        radius: 2
        color: field.parses ? field.shown : Theme.c.window
        border.width: 1
        // Space/Enter opens the picker, same as a click.
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: field.label
        Keys.onSpacePressed: field.pick()
        Keys.onReturnPressed: field.pick()

        border.color: mouse.containsMouse || activeFocus ? Theme.c.accent
                    : !field.parses ? Theme.c.warning
                    : Theme.c.border

        // Checked against the swatch rather than the theme: white text on a pale yellow card
        // is unreadable, and the swatch is whatever the colour is.
        readonly property color ink: !field.parses ? Theme.c.warning
            : (0.299 * field.shown.r + 0.587 * field.shown.g + 0.114 * field.shown.b) > 0.55
              ? "#101010" : "#f0f0f0"

        Text {
            anchors.centerIn: parent
            width: parent.width - 12
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            font.pixelSize: 11
            font.family: "monospace"
            color: parent.ink
            text: field.value
        }

        MouseArea {
            id: mouse

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: field.pick()
        }

        ToolTipText {
            hoverArea: mouse
            text: qsTr("Choose a colour")
            visible: mouse.containsMouse
        }
    }
}
