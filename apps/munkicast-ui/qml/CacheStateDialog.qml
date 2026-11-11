// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// What the CACHE box on a source row means, and what to do about it.
///
/// A coloured box with no way to ask about it is a box the operator learns to ignore. The
/// wording comes from `AssetCache::stateOf` rather than from here: the state and the sentence
/// that explains it are decided together, in one switch, so a new state cannot ship with the
/// explanation of the old one.
Item {
    id: dialog

    property string sourceName: ""
    property var state: ({})

    readonly property string stateName: dialog.state.state !== undefined ? dialog.state.state : "off"
    readonly property color stateColour: dialog.stateName === "ok" ? Theme.c.good
                                       : dialog.stateName === "stale" ? "#DCA70C"
                                       : dialog.stateName === "error" ? Theme.c.error
                                       : Theme.c.textMuted

    anchors.fill: parent
    visible: false
    focus: visible

    function open(name, found) {
        dialog.sourceName = name;
        dialog.state = found;
        dialog.visible = true;
        dialog.forceActiveFocus();
    }

    function dismiss() {
        dialog.visible = false;
    }

    Keys.onEscapePressed: dialog.dismiss()

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: dialog.dismiss()
        onWheel: (wheel) => { wheel.accepted = true; }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
        opacity: 0.55
    }

    Rectangle {
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        objectName: "cacheStateFrame"
        DialogGeometry { resizable: false }
        width: Math.min(parent.width - 80, 420)
        height: body.height + bar.height + 28
        color: Theme.c.panelBackground
        border.width: 1
        border.color: dialog.stateColour
        radius: 3

        MouseArea {
            anchors.fill: parent
            onClicked: {}
        }

        Column {
            id: body

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 14
            spacing: 6

            Row {
                spacing: 8

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 42; height: 14
                    radius: 2
                    color: "transparent"
                    border.width: 1
                    border.color: dialog.stateColour

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("CACHE")
                        font.pixelSize: 8
                        color: dialog.stateColour
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: dialog.sourceName
                    font.pixelSize: 12
                    font.bold: true
                    color: Theme.c.text
                }
            }

            Text {
                width: parent.width
                text: dialog.state.reason !== undefined ? dialog.state.reason : ""
                font.pixelSize: 11
                color: Theme.c.text
                wrapMode: Text.WordWrap
            }

            Text {
                width: parent.width
                visible: text !== ""
                text: dialog.state.remedy !== undefined ? dialog.state.remedy : ""
                font.pixelSize: 11
                color: Theme.c.textMuted
                wrapMode: Text.WordWrap
            }
        }

        Row {
            id: bar

            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 14
            spacing: 8

            DialogButton {
                objectName: "buildFromCacheState"
                text: qsTr("Build Cache")
                visible: dialog.stateName === "stale" || dialog.stateName === "error"
                enabled: !AssetCache.building
                onClicked: {
                    AssetCache.build(false);
                    dialog.dismiss();
                }
            }

            DialogButton {
                objectName: "closeCacheState"
                text: qsTr("Close")
                primary: true
                onClicked: dialog.dismiss()
            }
        }
    }
}
