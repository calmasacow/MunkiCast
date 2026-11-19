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
/// Selecting an effect opens its parameters, built from the same schema the show's Filters
/// dialog uses, so a blur is the same blur with the same controls on either tab.
Item {
    id: panel

    readonly property var clip: Edit.selectedClipDetails
    readonly property bool hasClip: !!panel.clip.id
    readonly property var applied: panel.clip.effects ?? []
    /// Which applied effect's parameters are open. Cleared when the clip changes, because
    /// an index into another clip's list means nothing.
    property string openEffect: ""

    onClipChanged: panel.openEffect = ""

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
                    onClicked: panel.openEffect =
                        panel.openEffect === fx.modelData.id ? "" : fx.modelData.id
                }

                // The open one is marked, because its parameters are what the block below
                // is showing and there is otherwise nothing saying which.
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 2
                    visible: panel.openEffect === fx.modelData.id
                    color: Theme.c.accent
                }
            }
        }

        // ---- the open effect's parameters ----
        Rectangle {
            width: parent.width
            height: 22
            visible: panel.openEffect !== ""
            color: Theme.c.panelHeader

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Settings")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }
        }

        Flickable {
            id: settings

            width: parent.width
            height: visible ? Math.min(contentHeight, panel.height * 0.35) : 0
            visible: panel.openEffect !== "" && settingsColumn.children.length > 0
            clip: true
            contentHeight: settingsColumn.height
            boundsBehavior: Flickable.StopAtBounds

            NoDragScroll {}

            Column {
                id: settingsColumn

                width: settings.width

                Repeater {
                    model: {
                        for (const e of panel.applied) {
                            if (e.id === panel.openEffect)
                                return Document.clipEffectFields(e.type, e.params);
                        }
                        return [];
                    }

                    delegate: FilterSetting {
                        required property var modelData

                        width: settingsColumn.width
                        field: modelData
                        onCommitted: value => Edit.setClipEffectParam(panel.openEffect,
                                                                      modelData.key, value)
                    }
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
            height: Math.max(44, panel.height - 44 - appliedList.height -
                             (settings.visible ? settings.height + 22 : 0))
            clip: true
            // Picture first, then sound, each under its own heading: in one list the two were
            // mixed, and an audio filter offered between two colour ones reads as a mistake.
            model: {
                const all = Edit.effectCatalogue();
                const video = all.filter(e => !e.audio);
                const audio = all.filter(e => e.audio);
                return [{heading: qsTr("Video")}].concat(video,
                                                         [{heading: qsTr("Audio")}], audio);
            }
            boundsBehavior: Flickable.StopAtBounds

            NoDragScroll {}

            delegate: Item {
                id: row

                required property var modelData
                /// A heading row: what kind the effects under it are, not an effect.
                readonly property bool heading: !!row.modelData.heading

                objectName: row.heading ? "effectsHeading" : "effectRow"
                width: ListView.view.width
                height: row.heading ? 20 : 22

                Rectangle {
                    anchors.fill: parent
                    color: row.heading ? Theme.c.window
                         : rowMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: row.heading ? 8 : 16
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.heading ? row.modelData.heading : row.modelData.name
                    font.pixelSize: row.heading ? 10 : 11
                    font.bold: row.heading
                    color: row.heading ? Theme.c.textMuted : Theme.c.text
                    elide: Text.ElideRight
                }

                MouseArea {
                    id: rowMouse

                    anchors.fill: parent
                    enabled: !row.heading
                    hoverEnabled: true
                    onDoubleClicked: Edit.addClipEffect(row.modelData.type)
                }

                ToolTipText {
                    text: qsTr("Double-click to put this on the clip")
                    visible: rowMouse.containsMouse && !row.heading
                    hoverArea: rowMouse
                }
            }
        }
    }
}
