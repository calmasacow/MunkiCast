// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

// One tabbed panel group, at the rect the layout engine computed for it. This item never decides
// where it goes; it only draws where it was told.
Rectangle {
    id: frame

    // The self-check looks for these by name to prove the layout actually reached the screen.
    objectName: "panelFrame"

    // Filled in from the model's roles.
    required property real rectX
    required property real rectY
    required property real rectW
    required property real rectH
    required property var tabs
    required property var titles
    required property int activeTab
    required property string activeId

    /// The coordinate space drag positions are reported in -- the whole workspace, so a drag
    /// that leaves this panel is still measured against the same origin the engine hit-tests in.
    required property Item surface

    x: rectX
    y: rectY
    width: rectW
    height: rectH
    readonly property real bodyTop: strip.y + strip.height + 1
    readonly property bool focused: frame.tabs !== undefined &&
                                    frame.tabs.indexOf(Workspace.focusedPanel) >= 0

    color: Theme.c.panelBackground
    border.width: 1
    // The keyboard acts on one panel at a time, so which one has to be visible without hunting.
    border.color: frame.focused ? Theme.c.accent : Theme.c.border
    radius: 2
    clip: true

    Behavior on border.color {
        ColorAnimation {
            duration: 90
        }
    }

    TabStrip {
        id: strip

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 1
        tabs: frame.tabs
        titles: frame.titles
        activeTab: frame.activeTab
        surface: frame.surface
    }

    PanelBody {
        visible: !frame.activeId.startsWith("browser:")
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: strip.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 1
        panelId: frame.activeId
    }
}
