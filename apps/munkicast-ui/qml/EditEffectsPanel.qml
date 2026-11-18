// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The effects available to put on a clip, and the ones already on the selected one.
///
/// The list is the renderer's filter catalogue, which the show's own Filters dialog draws
/// from: an effect that works on a source works on a clip, and keeping one catalogue is what
/// stops the two tabs offering different pictures of what this build can do.
///
/// ponytail: an effect is recorded on the clip as a flag, not as parameters. The clip model
/// carries `has_effects` and nothing else, so this panel can add and clear but not yet tune.
/// It becomes real when the Edit tab renders through the compositor.
Item {
    id: panel

    readonly property var clip: Edit.selectedClipDetails
    readonly property bool hasClip: !!panel.clip.id

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
    }

    Text {
        anchors.centerIn: parent
        width: parent.width - 20
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        visible: !panel.hasClip
        text: qsTr("Select a clip to put an effect on it")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Column {
        anchors.fill: parent
        visible: panel.hasClip

        Rectangle {
            width: parent.width
            height: 22
            color: Theme.c.panelHeader

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: panel.clip.hasEffects ? qsTr("On this clip") : qsTr("No effects yet")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }

            ToolButton {
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                label: "✕"
                tip: qsTr("Clear this clip's effects")
                enabled: !!panel.clip.hasEffects
                onClicked: Edit.setClipEffects(false)
            }
        }

        Rectangle {
            width: parent.width
            height: 22
            color: Theme.c.panelHeader

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Available")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }
        }

        ListView {
            width: parent.width
            height: parent.height - 44
            clip: true
            model: Edit.effectCatalogue()
            boundsBehavior: Flickable.StopAtBounds

            NoDragScroll {}

            delegate: Item {
                id: row

                required property var modelData

                width: ListView.view.width
                height: 22

                Rectangle {
                    anchors.fill: parent
                    color: rowMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.name ?? row.modelData.type ?? ""
                    font.pixelSize: 11
                    color: Theme.c.text
                    elide: Text.ElideRight
                }

                MouseArea {
                    id: rowMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onDoubleClicked: Edit.setClipEffects(true)
                }

                ToolTipText {
                    text: qsTr("Double-click to put this on the clip")
                    visible: rowMouse.containsMouse
                    hoverArea: rowMouse
                }
            }
        }
    }
}
