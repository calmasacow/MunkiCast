// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// A small square button for a panel's footer. Drawn rather than taken from QtQuick.Controls,
/// which would bring a style to fight with the theme for the sake of a rectangle and a glyph.
Rectangle {
    id: button

    required property string label
    property string tip: ""
    // `enabled` is Item's own: it already stops children receiving input and it
    // propagates, so declaring another one here would shadow the working one.

    /// A latched button -- a mute, a monitor -- that is currently on. This exists rather than
    /// letting a caller assign `color` directly: an assignment from outside *replaces* the
    /// binding below, so a latched button would keep its colour and stop reacting to hover and
    /// press altogether. `activeColor` is what it latches to.
    property bool active: false
    property color activeColor: Theme.c.accent

    signal clicked

    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: tip || label
    Accessible.onPressAction: if (button.enabled) button.clicked()
    Keys.onReturnPressed: button.clicked()
    Keys.onSpacePressed: button.clicked()

    implicitWidth: 22
    implicitHeight: 20
    radius: 2
    color: !button.enabled ? "transparent"
         : mouse.pressed ? Theme.c.accent
         : button.active ? button.activeColor
         : mouse.containsMouse ? Theme.c.panelHeaderActive
         : "transparent"
    border.width: 1
    border.color: !button.enabled ? "transparent"
                : mouse.containsMouse || button.active || button.activeFocus ? Theme.c.accent
                : Theme.c.border

    Behavior on color {
        ColorAnimation {
            duration: 70
        }
    }

    Text {
        anchors.centerIn: parent
        text: button.label
        font.pixelSize: 13
        color: !button.enabled ? Qt.rgba(Theme.c.textMuted.r, Theme.c.textMuted.g,
                                         Theme.c.textMuted.b, 0.4)
             : mouse.pressed || button.active ? Theme.c.window
             : Theme.c.text
    }

    MouseArea {
        id: mouse

        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }

    ToolTipText {
        text: button.tip
        visible: mouse.containsMouse && button.tip !== ""
        hoverArea: mouse
    }
}
