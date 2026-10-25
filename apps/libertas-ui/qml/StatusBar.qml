// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

Rectangle {
    id: bar

    required property string notice

    height: 26
    color: Theme.c.panelHeader

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: Theme.c.border
    }

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: Theme.name
            font.pixelSize: 11
            color: Theme.c.accent
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: Theme.dark ? qsTr("dark") : qsTr("light")
            font.pixelSize: 11
            color: Theme.c.textMuted
        }

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 1
            height: 12
            color: Theme.c.border
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: Document.projectName
            font.pixelSize: 11
            color: Theme.c.text
        }

        // An unsaved-changes dot rather than the word, because this line is read at a glance
        // while something else has the operator's attention.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            visible: Document.dirty
            width: 6
            height: 6
            radius: 3
            color: Theme.c.warning

            ToolTipText {
                text: qsTr("Unsaved changes — recoverable from the journal if this stops")
                visible: dirtyHover.containsMouse
                hoverArea: dirtyHover
            }

            MouseArea {
                id: dirtyHover

                anchors.fill: parent
                anchors.margins: -4
                hoverEnabled: true
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            objectName: "projectProblems"
            visible: Document.problems.length > 0
            text: qsTr("%1 problem(s)").arg(Document.problems.length)
            font.pixelSize: 11
            color: Theme.c.warning

            ToolTipText {
                text: Document.problems.join("\n")
                visible: problemHover.containsMouse
                hoverArea: problemHover
            }

            MouseArea {
                id: problemHover

                anchors.fill: parent
                hoverEnabled: true
                onClicked: Workspace.openPanel("info")
            }
        }
    }

    // The middle says what is happening: what a release would do while dragging, and whatever
    // the last notice was otherwise. Never a dialog -- nothing in a live show should have to be
    // acknowledged before work can continue.
    Text {
        anchors.centerIn: parent
        width: parent.width * 0.55
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        font.pixelSize: 11
        text: Workspace.dragging ? Workspace.dropDescription
              : bar.notice || Document.problems.find(problem => problem.startsWith("error:")) || ""
        color: Workspace.dragging ? (Workspace.dropValid ? Theme.c.text : Theme.c.warning)
                                  : Theme.c.warning
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        // Closed panels, one chip each. This is the direct answer to "losing the main canvas, or
        // having it pop out and go off screen completely": there is no off-screen to go to, and
        // anything closed is one click from coming back -- the program canvas included.
        Repeater {
            model: Workspace.closedPanels

            delegate: Rectangle {
                id: chip

                required property string modelData

                anchors.verticalCenter: parent.verticalCenter
                width: chipLabel.implicitWidth + 16
                height: 18
                radius: 9
                color: chipMouse.containsMouse ? Theme.c.accent : "transparent"
                border.width: 1
                border.color: chipMouse.containsMouse ? Theme.c.accent : Theme.c.border

                Text {
                    id: chipLabel

                    anchors.centerIn: parent
                    text: "+ " + Workspace.title(chip.modelData)
                    font.pixelSize: 10
                    color: chipMouse.containsMouse ? Theme.c.window : Theme.c.textMuted
                }

                MouseArea {
                    id: chipMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: Workspace.openPanel(chip.modelData)
                }
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: Workspace.canUndo || Workspace.canRedo
            text: (Workspace.canUndo ? qsTr("Ctrl+Z undo") : "") +
                  (Workspace.canUndo && Workspace.canRedo ? "  " : "") +
                  (Workspace.canRedo ? qsTr("Ctrl+Shift+Z redo") : "")
            font.pixelSize: 10
            color: Theme.c.textMuted
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: Workspace.layoutSource
            font.pixelSize: 10
            color: Theme.c.textMuted
            elide: Text.ElideLeft
            width: Math.min(implicitWidth, 320)
        }
    }
}
