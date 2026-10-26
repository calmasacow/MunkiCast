// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// What a canvas is: a name, a size, a rate, and whether it has a safety net.
///
/// Behind the Canvas panel's gear, matching the Outputs panel's gear. The panel is a list you
/// pick from; this is where the picked thing is edited, because a canvas is an independent
/// pipeline (ADR-0020) and resizing one mid-show is not a thing to make easy to do by accident
/// in a row that is one click from the selection.
///
/// Main's name is read-only here and the field says why rather than silently refusing the
/// edit. The command layer is the thing that actually refuses it (ADR-0027); this dialog agrees
/// with it on screen so the operator is never told "no" after the fact.
Item {
    id: dialog
    StripMenu { id: canvasAddMenu; parent: dialog.Window.contentItem; z: 100 }

    signal closed

    anchors.fill: parent
    visible: false
    focus: visible

    function open() {
        dialog.visible = true;
        dialog.forceActiveFocus();
    }

    function close() {
        dialog.visible = false;
        dialog.closed();
    }

    Keys.onEscapePressed: dialog.close()

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: dialog.close()
        onWheel: (wheel) => { wheel.accepted = true; }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
        opacity: 0.55
    }

    Rectangle {
        id: frame

        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        objectName: "canvasManagerFrame"
        DialogGeometry { minimumWidth: 440; minimumHeight: 360 }
        width: Math.min(parent.width - 48, 560)
        height: Math.min(parent.height - 48, 520)
        color: Theme.c.panelBackground
        border.width: 1
        border.color: Theme.c.accent
        radius: 3

        MouseArea {
            anchors.fill: parent
            onClicked: {}
        }

        Rectangle {
            id: titleBar

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: 34
            color: Theme.c.panelHeader

            Column {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0

                Text {
                    width: parent.width
                    text: qsTr("Canvas Manager")
                    font.pixelSize: 11
                    font.bold: true
                    color: Theme.c.text
                }

                Text {
                    width: parent.width
                    text: qsTr("Each canvas is its own pipeline. Scenes belong to one.")
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                }
            }
        }

        Flickable {
            id: scroll

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: titleBar.bottom
            anchors.bottom: bottomBar.top
            anchors.margins: 1
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

                    delegate: Column {
                        id: block

                        required property string canvasId
                        required property string name
                        required property int pixelWidth
                        required property int pixelHeight
                        required property double fps
                        required property string colorSpace
                        required property bool isMain
                        required property int sceneCount
                        required property bool hasEmergencyScene

                        objectName: "canvasBlock"
                        width: content.width
                        spacing: 0
                        padding: 0

                        Rectangle {
                            width: parent.width
                            height: 1
                            color: Theme.c.border
                        }

                        Item {
                            width: parent.width
                            height: 26

                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: block.isMain ? qsTr("%1 · MAIN").arg(block.name)
                                                   : block.name
                                font.pixelSize: 11
                                font.bold: true
                                color: Theme.c.text
                            }

                            Text {
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: block.sceneCount === 1 ? qsTr("1 scene")
                                                             : qsTr("%1 scenes").arg(
                                                                   block.sceneCount)
                                font.pixelSize: 9
                                color: Theme.c.textMuted
                            }
                        }

                        Item {
                            width: parent.width
                            height: 24

                            TextLine {
                                id: nameField

                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.right: block.isMain ? mainReason.left : parent.right
                                anchors.rightMargin: block.isMain ? 6 : 10
                                anchors.verticalCenter: parent.verticalCenter
                                label: qsTr("Name")
                                labelWidth: 56
                                // Main cannot be renamed and the command refuses it, so the
                                // field refuses it too rather than letting the operator type
                                // into something that will snap back.
                                enabled: !block.isMain
                                value: block.name
                                onCommitted: (v) => Document.renameCanvas(block.canvasId, v)
                            }

                            // A disabled field is a "no" with no reason attached. Say the
                            // reason beside it: half the rules about canvases are rules about
                            // this one, and none of them are guessable from a greyed box.
                            Text {
                                id: mainReason

                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                visible: block.isMain
                                text: qsTr("the first canvas — cannot be renamed or removed")
                                font.pixelSize: 9
                                color: Theme.c.textMuted
                            }
                        }

                        Item {
                            width: parent.width
                            height: 24

                            Row {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 8

                                NumberField {
                                    width: 140
                                    label: qsTr("Size")
                                    labelWidth: 56
                                    value: block.pixelWidth
                                    decimals: 0
                                    onCommitted: (v) => Document.setCanvasSize(
                                        block.canvasId, Math.round(v), block.pixelHeight)
                                }

                                NumberField {
                                    width: 100
                                    label: "×"
                                    labelWidth: 10
                                    value: block.pixelHeight
                                    decimals: 0
                                    onCommitted: (v) => Document.setCanvasSize(
                                        block.canvasId, block.pixelWidth, Math.round(v))
                                }
                            }
                        }

                        Item {
                            width: parent.width
                            height: 24

                            Row {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 8

                                // 60000/1001 and 60/1 are different clocks, and the first is
                                // not "59.94" rounded. Offered as the fractions they are,
                                // rather than as a decimal the model would have to guess back.
                                ChoiceField {
                                    width: 200
                                    label: qsTr("Rate")
                                    labelWidth: 56
                                    choices: ["60000/1001", "60/1", "30000/1001", "30/1",
                                              "50/1", "25/1", "24000/1001", "24/1"]
                                    value: {
                                        const r = block.fps;
                                        if (Math.abs(r - 59.94) < 0.01) return "60000/1001";
                                        if (Math.abs(r - 60) < 0.01) return "60/1";
                                        if (Math.abs(r - 29.97) < 0.01) return "30000/1001";
                                        if (Math.abs(r - 30) < 0.01) return "30/1";
                                        if (Math.abs(r - 50) < 0.01) return "50/1";
                                        if (Math.abs(r - 25) < 0.01) return "25/1";
                                        if (Math.abs(r - 23.976) < 0.01) return "24000/1001";
                                        if (Math.abs(r - 24) < 0.01) return "24/1";
                                        return "60/1";
                                    }
                                    onCommitted: (v) => {
                                        const parts = v.split("/");
                                        Document.setCanvasFps(block.canvasId,
                                                              parseInt(parts[0], 10),
                                                              parseInt(parts[1], 10));
                                    }
                                }

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: block.fps.toFixed(3) + qsTr(" fps · ")
                                          + block.colorSpace
                                    font.pixelSize: 9
                                    color: Theme.c.textMuted
                                }
                            }
                        }

                        // ADR-0025. Shown per canvas because the net is per canvas: a vertical
                        // show wants a separately authored vertical card, not a letterboxed
                        // landscape one.
                        Text {
                            width: parent.width
                            leftPadding: 10
                            bottomPadding: 6
                            visible: !block.hasEmergencyScene
                            text: qsTr("⚠ No emergency scene. Designate one from the scenes "
                                       + "panel while this canvas is selected.")
                            font.pixelSize: 9
                            color: Theme.c.warning
                            wrapMode: Text.WordWrap
                        }

                        Item {
                            width: parent.width
                            height: 6
                        }
                    }
                }
            }
        }

        Rectangle {
            id: bottomBar

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 1
            height: 40
            color: Theme.c.panelHeader

            DialogButton {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                objectName: "addCanvasFromManager"
                minimumWidth: 100
                text: qsTr("+ new canvas")
                onClicked: canvasAddMenu.popup(mapToItem(canvasAddMenu, 0, 0).x,
                    mapToItem(canvasAddMenu, 0, 0).y, [
                        {text: qsTr("Horizontal 16:9"), action: () => Document.addCanvas(qsTr("Canvas"), false)},
                        {text: qsTr("Vertical 9:16"), action: () => Document.addCanvas(qsTr("Vertical"), true)}
                    ])
            }

            Text {
                anchors.centerIn: parent
                text: qsTr("no render process — nothing is rendering")
                font.pixelSize: 9
                color: Theme.c.textMuted
                opacity: 0.6
            }

            DialogButton {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Close")
                primary: true
                onClicked: dialog.close()
            }
        }
    }
}
