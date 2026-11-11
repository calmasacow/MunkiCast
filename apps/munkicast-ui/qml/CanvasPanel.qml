// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import MunkiCast

/// The canvases, and which one the rest of the shell is working on.
///
/// A tab beside Outputs by default, and the two share a footer shape on purpose: `+ − gear`,
/// where the gear opens a manager window rather than editing in place. Selecting a row here is
/// what the scenes panel lists, where a new scene lands, and which canvas the emergency button
/// designates on -- a canvas is an independent pipeline (ADR-0020) and scenes belong to one
/// (ADR-0027), so "which canvas" has to be a thing the operator can see and change.
///
/// Main is the first canvas and cannot be removed, renamed, or displaced. The command layer
/// refuses all three; this panel greys the buttons to match, rather than offering a press that
/// will only be refused.

Item {
    id: panel
    StripMenu { id: canvasAddMenu; parent: panel.Window.contentItem; z: 100 }

    // Reparented to the window, like every other dialog in the shell.
    CanvasManagerDialog {
        id: managerDialog

        objectName: "canvasManagerDialog"
        parent: panel.Window.contentItem
    }

    Flickable {
        id: scroll

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
        clip: true
        contentHeight: content.height
        contentWidth: width
        boundsBehavior: Flickable.StopAtBounds
        // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
        // because Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

        Column {
            id: content

            width: scroll.width
            spacing: 0

            Repeater {
                model: Document.canvases

                delegate: Rectangle {
                    id: row

                    required property string canvasId
                    required property string name
                    required property int pixelWidth
                    required property int pixelHeight
                    required property double fps
                    required property bool isMain
                    required property int sceneCount
                    required property bool hasEmergencyScene

                    readonly property bool current: Document.currentCanvas === row.canvasId
                        || (Document.currentCanvas === "" && row.isMain)

                    objectName: "canvasRow"
                    width: content.width
                    height: 34
                    color: row.current ? Theme.c.panelHeaderActive
                         : mouse.containsMouse ? Theme.c.panelHeader
                         : "transparent"

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 2
                        color: row.current ? Theme.c.accent : "transparent"
                    }

                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 9
                        anchors.right: badges.left
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 1

                        Text {
                            width: parent.width
                            text: row.name
                            font.pixelSize: 11
                            font.bold: row.current
                            color: Theme.c.text
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            text: row.pixelWidth + "x" + row.pixelHeight + " · " + row.fps.toFixed(2)
                                  + " fps · " + row.sceneCount
                                  + (row.sceneCount === 1 ? qsTr(" scene") : qsTr(" scenes"))
                            font.pixelSize: 9
                            color: Theme.c.textMuted
                            elide: Text.ElideRight
                        }
                    }

                    Row {
                        id: badges

                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3

                        // Main says so on its face. Half the rules about canvases are rules
                        // about this one, and "it is the first row" is not something a panel
                        // should expect to be read as a rule.
                        Rectangle {
                            visible: row.isMain
                            width: mainLabel.width + 8
                            height: 13
                            radius: 2
                            color: "transparent"
                            border.width: 1
                            border.color: Theme.c.border

                            Text {
                                id: mainLabel

                                anchors.centerIn: parent
                                text: qsTr("MAIN")
                                font.pixelSize: 8
                                color: Theme.c.textMuted
                            }
                        }

                        // No safety net designated. A pre-flight question (ADR-0025), and this
                        // is the only list where every canvas is visible at once to ask it of.
                        Rectangle {
                            visible: !row.hasEmergencyScene
                            width: netLabel.width + 8
                            height: 13
                            radius: 2
                            color: "transparent"
                            border.width: 1
                            border.color: Theme.c.warning

                            Text {
                                id: netLabel

                                anchors.centerIn: parent
                                text: qsTr("NO NET")
                                font.pixelSize: 8
                                color: Theme.c.warning
                            }
                        }
                    }

                    MouseArea {
                        id: mouse

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: Document.selectCanvas(row.canvasId)
                        onDoubleClicked: managerDialog.open()
                    }
                }
            }
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

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            ToolButton {
                label: "+"
                tip: qsTr("Add a canvas")
                onClicked: canvasAddMenu.popup(mapToItem(canvasAddMenu, 0, 0).x,
                    mapToItem(canvasAddMenu, 0, 0).y, [
                        {text: qsTr("Horizontal 16:9"), action: () => Document.addCanvas(qsTr("Canvas"), false)},
                        {text: qsTr("Vertical 9:16"), action: () => Document.addCanvas(qsTr("Vertical"), true)}
                    ])
            }

            ToolButton {
                objectName: "removeCanvas"
                label: "−"
                // Main can never go, and a canvas still holding scenes cannot go either --
                // the command says "move or delete them first" rather than taking the scenes
                // with it, because a canvas is a pipeline and its scenes are not scrap.
                enabled: !Document.currentCanvasIsMain && Document.currentCanvasSceneCount === 0
                tip: Document.currentCanvasIsMain ? qsTr("Main cannot be removed")
                   : Document.currentCanvasSceneCount > 0
                     ? qsTr("Move or delete this canvas's scenes first")
                     : qsTr("Remove this canvas")
                onClicked: Document.removeCanvas(Document.currentCanvas)
            }

            ToolButton {
                objectName: "manageCanvases"
                label: "⚙"
                tip: qsTr("Canvas settings")
                onClicked: managerDialog.open()
            }
        }

        Text {
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: Document.currentCanvasName
            font.pixelSize: 9
            color: Theme.c.textMuted
            opacity: 0.7
        }
    }
}
