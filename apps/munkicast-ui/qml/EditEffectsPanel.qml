// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The effects on the selected clip, and the catalogue to add from.
///
/// The catalogue is the renderer's, which the show's own Filters dialog draws from: an effect
/// that works on a source works on a clip, and keeping one catalogue is what stops the two
/// tabs offering different pictures of what this build can do.
///
/// ponytail: an effect's parameters are not editable here. The clip carries the whole
/// `Effect`, parameters and all, so the document is ready for it; what is missing is the
/// per-type field UI, which the show's FiltersDialog already has and this should share.
Item {
    id: panel

    readonly property var clip: Edit.selectedClipDetails
    readonly property bool hasClip: !!panel.clip.id
    readonly property var applied: panel.clip.effects ?? []

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
                text: panel.applied.length > 0 ? qsTr("On this clip")
                                               : qsTr("Nothing on this clip yet")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }
        }

        ListView {
            id: appliedList

            width: parent.width
            height: Math.min(contentHeight, panel.height * 0.45)
            clip: true
            model: panel.applied
            boundsBehavior: Flickable.StopAtBounds

            NoDragScroll {}

            delegate: Item {
                id: fx

                required property var modelData

                width: ListView.view.width
                height: 22

                Rectangle {
                    anchors.fill: parent
                    color: fxMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                }

                // On or off without removing it: comparing with and without is most of what
                // judging an effect consists of.
                Rectangle {
                    id: onOff

                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 14
                    height: 14
                    radius: 2
                    color: fx.modelData.enabled ? Theme.c.accent : "transparent"
                    opacity: fx.modelData.enabled ? 0.8 : 1.0
                    border.width: 1
                    border.color: Theme.c.border

                    MouseArea {
                        anchors.fill: parent
                        onClicked: Edit.toggleClipEffect(fx.modelData.id)
                    }
                }

                Text {
                    anchors.left: onOff.right
                    anchors.leftMargin: 8
                    anchors.right: remove.left
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: fx.modelData.name
                    font.pixelSize: 11
                    color: fx.modelData.enabled ? Theme.c.text : Theme.c.textMuted
                    elide: Text.ElideRight
                }

                ToolButton {
                    id: remove

                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    label: "✕"
                    tip: qsTr("Take this effect off the clip")
                    onClicked: Edit.removeClipEffect(fx.modelData.id)
                }

                MouseArea {
                    id: fxMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                }
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
            height: panel.height - 44 - appliedList.height
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
                    text: row.modelData.name
                    font.pixelSize: 11
                    color: Theme.c.text
                    elide: Text.ElideRight
                }

                MouseArea {
                    id: rowMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onDoubleClicked: Edit.addClipEffect(row.modelData.type)
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
