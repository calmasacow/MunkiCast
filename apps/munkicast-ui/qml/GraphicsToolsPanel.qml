// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// What can be added to the stage.
///
/// Three kinds, not thirty. A broadcast template is boxes, text and the occasional logo; shape
/// tools beyond that are a drawing application's job, and an SVG imported as an image covers
/// the cases this cannot draw.
Item {
    id: panel

    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 6
        spacing: 4

        ToolButton {
            label: "▭"
            tip: qsTr("Add a box")
            enabled: Graphics.hasGraphic
            onClicked: Graphics.addLayer("rect")
        }

        ToolButton {
            label: "T"
            tip: qsTr("Add text")
            enabled: Graphics.hasGraphic
            onClicked: Graphics.addLayer("text")
        }

        Item { width: 1; height: 8 }

        ToolButton {
            label: "⌫"
            tip: qsTr("Remove the selected layer")
            enabled: Graphics.selectedLayer !== ""
            onClicked: Graphics.removeSelectedLayer()
        }

        Item { width: 1; height: 8 }

        ToolButton {
            label: "↶"
            tip: Graphics.canUndo ? qsTr("Undo in this graphic") : qsTr("Nothing to undo")
            enabled: Graphics.canUndo
            onClicked: Graphics.undo()
        }

        ToolButton {
            label: "↷"
            tip: Graphics.canRedo ? qsTr("Redo in this graphic") : qsTr("Nothing to redo")
            enabled: Graphics.canRedo
            onClicked: Graphics.redo()
        }
    }
}
