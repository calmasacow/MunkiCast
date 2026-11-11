// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// Transition configuration and file-backed stingers for each canvas.
Item {
    id: panel

    StingerDialog { id: stingerSetup }
    property string stingerSourceId: ""
    Loader {
        id: stingerMedia
        parent: panel.Window.contentItem
        anchors.fill: parent
        active: false
        function open(sourceId) { panel.stingerSourceId = sourceId; active = true; }
        sourceComponent: Component { SourceSettingsDialog { onClosed: stingerMedia.active = false } }
        onLoaded: item.open(panel.stingerSourceId)
    }

    Flickable {
        id: scroll

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: banner.top
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
                model: Document.decks

                delegate: Item {
                    id: deck

                    required property string deckId
                    required property string name
                    required property string canvasName
                    required property string programScene
                    required property string previewScene
                    required property string programSceneId
                    required property string previewSceneId
                    required property bool studioMode
                    required property string transition
                    required property int transitionMs
                    readonly property var stinger: Document.revision >= 0 &&
                                                   deck.transition.startsWith("transition.stinger:")
                        ? Document.stingerSettings(deckId) : ({})
                    required property bool hasEmergencyScene

                    objectName: "deckBlock"
                    width: content.width
                    height: deckColumn.height + 12

                    Column {
                        id: deckColumn

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: 6
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 4

                        Text {
                            width: parent.width
                            text: deck.name + " · " + deck.canvasName
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            color: Theme.c.textMuted
                            elide: Text.ElideRight
                        }

                        Flow {
                            width: parent.width
                            spacing: 8
                            DialogButton {
                                text: qsTr("Add stinger…")
                                objectName: "addStinger"
                                onClicked: stingerSetup.open(deck.deckId)
                            }
                            DialogButton {
                                text: qsTr("Edit…"); minimumWidth: 60
                                objectName: "editStinger"
                                enabled: !!deck.stinger.sourceId
                                onClicked: stingerSetup.open(deck.deckId, true)
                            }
                            DialogButton {
                                text: qsTr("Delete"); minimumWidth: 60
                                objectName: "deleteStinger"
                                enabled: !!deck.stinger.sourceId
                                onClicked: Document.removeSourceAndUses(deck.stinger.sourceId)
                            }
                        }

                        ChoiceField {
                            width: parent.width
                            label: qsTr("Transition")
                            objectName: "deckTransition"
                            choices: Document.transitions.map(t => t.id)
                            choiceLabels: {
                                const labels = {};
                                for (const t of Document.transitions)
                                    labels[t.id] = t.implemented ? t.title : t.title + " (no renderer)";
                                return labels;
                            }
                            value: deck.transition
                            onCommitted: value => Document.setDeckTransition(deck.deckId, value, -1)
                        }

                        NumberField {
                            width: parent.width
                            label: qsTr("Duration")
                            decimals: 0
                            value: deck.stinger.durationMs === undefined ? deck.transitionMs : deck.stinger.durationMs
                            onCommitted: (newValue) => {
                                if (deck.stinger.sourceId) Document.setStingerTiming(deck.deckId, deck.stinger.cutMs, Math.round(newValue));
                                else Document.setDeckTransition(deck.deckId, "", Math.max(0, Math.round(newValue)));
                            }
                        }

                        DialogButton {
                            visible: !!deck.stinger.sourceId
                            text: qsTr("Edit stinger media…")
                            onClicked: stingerMedia.open(deck.stinger.sourceId)
                        }

                        NumberField {
                            width: parent.width
                            visible: !!deck.stinger.sourceId
                            label: qsTr("Cut point (ms)")
                            labelWidth: 100
                            decimals: 0
                            minimum: 0
                            value: deck.stinger.cutMs || 0
                            onCommitted: value => Document.setStingerTiming(deck.deckId, Math.round(value), deck.stinger.durationMs)
                        }


                    }
                }
            }

            Text {
                width: content.width
                visible: Document.decks.rowCount() === 0
                padding: 10
                text: qsTr("No deck. A deck is one program path for one canvas.")
                font.pixelSize: 10
                color: Theme.c.textMuted
                wrapMode: Text.WordWrap
            }

        }
    }

    // Where a "Go Live" button would be, if there were anything behind it.
    Rectangle {
        id: banner

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
            anchors.centerIn: parent
            text: qsTr("Transitions apply when selecting a scene or taking Preview")
            font.pixelSize: 9
            color: Theme.c.textMuted
            opacity: 0.6
        }
    }
}
