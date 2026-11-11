// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

// ADR-0016 rule 1: a panel is dragged by its handle -- its tab -- not by its body. The body
// belongs to the panel's own content, which will have its own gestures.
Item {
    id: strip

    required property var tabs
    required property var titles
    required property int activeTab
    required property Item surface

    readonly property int tabCount: tabs ? tabs.length : 0

    height: 28

    Rectangle {
        anchors.fill: parent
        color: Theme.c.panelHeader
    }

    // A hairline under the strip, so a tab group reads as a header over content rather than two
    // unrelated bands of colour.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.c.border
    }

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 3
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        Repeater {
            model: strip.tabCount

            delegate: Rectangle {
                id: tab
                objectName: "workspaceTab"

                required property int index

                readonly property string panelId: strip.tabs[index]
                readonly property string label: strip.titles[index]
                readonly property bool isActive: index === strip.activeTab

                width: labelText.implicitWidth + (isActive ? 36 : 18)
                height: 22
                radius: 2
                color: isActive ? Theme.c.panelHeaderActive
                     : tabMouse.containsMouse ? Qt.rgba(Theme.c.panelHeaderActive.r,
                                                        Theme.c.panelHeaderActive.g,
                                                        Theme.c.panelHeaderActive.b, 0.45)
                     : "transparent"

                Behavior on color {
                    ColorAnimation {
                        duration: 80
                    }
                }

                Text {
                    id: labelText

                    anchors.left: parent.left
                    anchors.leftMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    text: tab.label
                    font.pixelSize: 12
                    font.weight: tab.isActive ? Font.DemiBold : Font.Normal
                    color: tab.isActive ? Theme.c.text : Theme.c.textMuted
                }

                MouseArea {
                    id: tabMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton
                    cursorShape: Workspace.dragging ? Qt.ClosedHandCursor : Qt.ArrowCursor

                    property real pressX: 0
                    property real pressY: 0
                    property bool armed: false

                    onPressed: mouse => {
                        pressX = mouse.x;
                        pressY = mouse.y;
                        armed = true;
                        // Clicking a panel is also how the keyboard's idea of "here" moves.
                        Workspace.focusPanel(tab.panelId);
                    }

                    onPositionChanged: mouse => {
                        if (!armed)
                            return;
                        if (!Workspace.dragging) {
                            // A few pixels of slack, so clicking a tab to select it does not
                            // start a drag and a drag does not need a deliberate pause first.
                            if (Math.abs(mouse.x - pressX) + Math.abs(mouse.y - pressY) < 6)
                                return;
                            Workspace.beginDrag(tab.panelId);
                        }
                        // Wayland gives us an implicit pointer grab for the duration of the
                        // press, so these keep arriving after the cursor has left this tab, this
                        // panel, and eventually the window.
                        const p = mapToItem(strip.surface, mouse.x, mouse.y);
                        Workspace.updateDrag(p.x, p.y);
                    }

                    onReleased: {
                        // Clear our own state before the call that may rebuild this delegate:
                        // anything after a layout change is running on an item being deleted.
                        armed = false;
                        if (Workspace.dragging)
                            Workspace.endDrag();
                        else
                            Workspace.activate(tab.panelId);
                    }

                    onCanceled: {
                        armed = false;
                        Workspace.cancelDrag();
                    }
                }

                // Close affordance, on the active tab only, so a strip of six tabs is not a row
                // of six things to mis-click. A closed panel is always recoverable from the
                // status bar.
                Item {
                    visible: tab.isActive
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 16
                    height: 16

                    Rectangle {
                        anchors.fill: parent
                        radius: 2
                        color: closeMouse.containsMouse ? Theme.c.error : "transparent"
                        opacity: closeMouse.containsMouse ? 0.85 : 1.0
                    }

                    Text {
                        anchors.centerIn: parent
                        text: "×"
                        font.pixelSize: 13
                        color: closeMouse.containsMouse ? Theme.c.window : Theme.c.textMuted
                    }

                    MouseArea {
                        id: closeMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: Workspace.closePanel(tab.panelId)
                    }
                }
            }
        }
    }
}
