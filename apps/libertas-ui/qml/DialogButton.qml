// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// A button in a dialog's bottom bar. ToolButton's wider sibling: that one is a glyph in a
/// panel footer, this one is a word in a bottom bar, and the two dialogs in this shell were
/// drawing the same rectangle twice each before it existed.
///
/// `primary` is the one the Return key would press -- it is drawn as such and nothing more;
/// there is no default-button handling behind it, because a dialog here is an Item rather than
/// a window and owns its own keys.
Rectangle {
    id: button

    required property string text
    property bool primary: false
    property int minimumWidth: 80

    signal clicked
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: button.text
    Accessible.onPressAction: if (button.enabled) button.clicked()
    Keys.onReturnPressed: if (button.enabled) button.clicked()
    Keys.onSpacePressed: if (button.enabled) button.clicked()

    implicitWidth: Math.max(button.minimumWidth, label.implicitWidth + 24)
    implicitHeight: 26
    width: implicitWidth
    height: implicitHeight
    radius: 2
    color: !button.enabled ? "transparent"
         : mouse.pressed ? Theme.c.accent
         : mouse.containsMouse ? Theme.c.panelHeaderActive
         : "transparent"
    border.width: 1
    border.color: !button.enabled ? Theme.c.border
                : mouse.containsMouse ? Theme.c.accent
                : button.primary ? Theme.c.accent
                : Theme.c.border

    Behavior on color {
        ColorAnimation {
            duration: 70
        }
    }

    Text {
        id: label

        anchors.centerIn: parent
        width: Math.max(0, button.width - 12)
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        text: button.text
        font.pixelSize: 11
        font.weight: button.primary ? Font.DemiBold : Font.Normal
        color: !button.enabled ? Theme.c.textMuted
             : mouse.pressed ? Theme.c.window
             : Theme.c.text
        opacity: button.enabled ? 1.0 : 0.45
    }

    MouseArea {
        id: mouse

        anchors.fill: parent
        hoverEnabled: true
        enabled: button.enabled
        onClicked: button.clicked()
    }
}
