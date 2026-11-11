// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The bindings table.
///
/// Application and global actions in one list with the scope visible, rather than OBS's split
/// between a hotkeys page and menu accelerators that live somewhere else entirely. The operator
/// can see at a glance which ones follow them out of the application.
Item {
    id: panel

    /// The row currently capturing a keystroke, or empty.
    property string capturing: ""

    Rectangle {
        id: header

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 26
        color: Theme.c.panelHeader

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: panel.capturing !== "" ? qsTr("Press a key — Esc to cancel, Backspace to unbind")
                                         : qsTr("Click a shortcut to change it")
            font.pixelSize: 11
            color: panel.capturing !== "" ? Theme.c.accent : Theme.c.textMuted
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.c.border
        }
    }

    ListView {
        id: list

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: footer.top
        clip: true
        model: Shortcuts.bindings
        boundsBehavior: Flickable.StopAtBounds
        // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
        // because Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

        section.property: "category"
        section.delegate: Rectangle {
            required property string section

            width: ListView.view.width
            height: 20
            color: Theme.c.panelHeader

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: parent.section
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
            }
        }

        delegate: Rectangle {
            id: row

            required property string ref
            required property string title
            required property string scope
            required property string trigger
            required property bool isDefault
            required property bool available
            required property string conflict

            readonly property bool capturing: panel.capturing === row.ref

            objectName: "bindingRow"
            width: ListView.view.width
            height: 24
            color: row.capturing ? Theme.c.panelHeaderActive
                 : rowMouse.containsMouse ? Qt.rgba(Theme.c.panelHeaderActive.r,
                                                    Theme.c.panelHeaderActive.g,
                                                    Theme.c.panelHeaderActive.b, 0.5)
                 : "transparent"

            MouseArea {
                id: rowMouse

                anchors.fill: parent
                hoverEnabled: true
                onClicked: {
                    panel.capturing = row.ref;
                    capture.forceActiveFocus();
                }
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.right: scopeTag.left
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                text: row.title
                font.pixelSize: 11
                color: row.available ? Theme.c.text : Theme.c.textMuted
                elide: Text.ElideRight
            }

            // "global" is the fact worth seeing at a glance: it is the one that follows you out
            // of the application.
            Rectangle {
                id: scopeTag

                anchors.right: triggerBox.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: row.scope === "global"
                width: scopeLabel.implicitWidth + 10
                height: 14
                radius: 2
                color: "transparent"
                border.width: 1
                border.color: Theme.c.accent

                Text {
                    id: scopeLabel

                    anchors.centerIn: parent
                    text: qsTr("global")
                    font.pixelSize: 9
                    color: Theme.c.accent
                }
            }

            Rectangle {
                id: triggerBox

                anchors.right: resetButton.left
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                width: 132
                height: 18
                radius: 2
                color: row.capturing ? Theme.c.accent : Theme.c.window
                border.width: 1
                border.color: row.conflict !== "" ? Theme.c.error
                            : row.capturing ? Theme.c.accent
                            : "transparent"

                Text {
                    anchors.centerIn: parent
                    text: row.capturing ? qsTr("…")
                        : row.trigger === "" ? qsTr("unbound")
                        : row.trigger
                    font.pixelSize: 10
                    color: row.capturing ? Theme.c.window
                         : row.trigger === "" ? Theme.c.textMuted
                         : Theme.c.text
                }

                ToolTipText {
                    text: row.conflict !== ""
                        ? qsTr("Also used by: %1").arg(row.conflict)
                        : (!row.available ? qsTr("Nothing performs this yet; the binding is kept for when it arrives") : "")
                    visible: conflictHover.containsMouse &&
                             (row.conflict !== "" || !row.available)
                    hoverArea: conflictHover
                }

                MouseArea {
                    id: conflictHover

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        panel.capturing = row.ref;
                        capture.forceActiveFocus();
                    }
                }
            }

            ToolButton {
                id: resetButton

                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                label: "↺"
                tip: qsTr("Back to the default")
                enabled: !row.isDefault
                onClicked: Shortcuts.resetTrigger(row.ref)
            }
        }
    }

    // One capture target for the whole table, focused while a row is listening.
    Item {
        id: capture

        focus: panel.capturing !== ""

        Keys.onPressed: event => {
            event.accepted = true;
            if (event.key === Qt.Key_Escape) {
                panel.capturing = "";
                return;
            }
            if (event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete) {
                Shortcuts.setTrigger(panel.capturing, "");
                panel.capturing = "";
                return;
            }
            const text = Shortcuts.formatKey(event.key, event.modifiers);
            if (text === "")
                return; // a bare modifier is not a shortcut
            Shortcuts.setTrigger(panel.capturing, text);
            panel.capturing = "";
        }
    }

    Rectangle {
        id: footer

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 26
        color: Theme.c.panelHeader

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.c.border
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: Shortcuts.unboundGlobalCount > 0
                ? qsTr("%1 global action(s) unbound").arg(Shortcuts.unboundGlobalCount)
                : ""
            font.pixelSize: 10
            color: Theme.c.textMuted
        }

        Rectangle {
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: resetAllLabel.implicitWidth + 14
            height: 18
            radius: 2
            color: resetAllMouse.containsMouse ? Theme.c.error : "transparent"
            border.width: 1
            border.color: resetAllMouse.containsMouse ? Theme.c.error : Theme.c.border

            Text {
                id: resetAllLabel

                anchors.centerIn: parent
                text: qsTr("Reset all")
                font.pixelSize: 10
                color: resetAllMouse.containsMouse ? Theme.c.window : Theme.c.textMuted
            }

            MouseArea {
                id: resetAllMouse

                anchors.fill: parent
                hoverEnabled: true
                onClicked: Shortcuts.resetAll()
            }
        }
    }
}
