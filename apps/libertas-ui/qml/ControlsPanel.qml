// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// Program/preview controls and file-backed stinger setup for each canvas.
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

                        // Program first and larger: it is the one that is already out there.
                        Rectangle {
                            width: parent.width
                            height: 30
                            radius: 2
                            color: Theme.c.panelHeader
                            border.width: 1
                            border.color: deck.programSceneId !== ""
                                ? Theme.c.error : Theme.c.border

                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("PGM")
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                                color: deck.programSceneId !== ""
                                    ? Theme.c.error : Theme.c.textMuted
                            }

                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 38
                                anchors.right: pgmSet.left
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                text: deck.programSceneId === ""
                                    ? qsTr("nothing on air") : deck.programScene
                                font.pixelSize: 12
                                color: deck.programSceneId === ""
                                    ? Theme.c.textMuted : Theme.c.text
                                elide: Text.ElideRight
                            }

                            ToolButton {
                                id: pgmSet

                                anchors.right: parent.right
                                anchors.rightMargin: 5
                                anchors.verticalCenter: parent.verticalCenter
                                label: "◉"
                                tip: qsTr("Put the selected scene on program")
                                enabled: Document.selectedScene !== "" &&
                                         Document.selectedScene !== deck.programSceneId
                                onClicked: Document.setDeckProgram(deck.deckId,
                                                                   Document.selectedScene)
                            }
                        }

                        Rectangle {
                            width: parent.width
                            height: 26
                            radius: 2
                            color: Theme.c.panelHeader
                            border.width: 1
                            border.color: Theme.c.border

                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("PVW")
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                                color: Theme.c.good
                            }

                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 38
                                anchors.right: pvwSet.left
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                text: deck.previewSceneId === ""
                                    ? qsTr("nothing staged") : deck.previewScene
                                font.pixelSize: 11
                                color: deck.previewSceneId === ""
                                    ? Theme.c.textMuted : Theme.c.text
                                elide: Text.ElideRight
                            }

                            ToolButton {
                                id: pvwSet

                                anchors.right: parent.right
                                anchors.rightMargin: 5
                                anchors.verticalCenter: parent.verticalCenter
                                label: "○"
                                tip: qsTr("Stage the selected scene on preview")
                                enabled: Document.selectedScene !== "" &&
                                         Document.selectedScene !== deck.previewSceneId
                                onClicked: Document.setDeckPreview(deck.deckId,
                                                                   Document.selectedScene)
                            }
                        }

                        Row {
                            width: parent.width
                            spacing: 6

                            Rectangle {
                                objectName: "takeButton"
                                width: 58
                                height: 22
                                radius: 2
                                enabled: deck.previewSceneId !== "" &&
                                         deck.previewSceneId !== deck.programSceneId
                                color: !enabled ? "transparent"
                                     : takeMouse.pressed ? Theme.c.error
                                     : takeMouse.containsMouse ? Theme.c.panelHeaderActive
                                     : Theme.c.panelHeader
                                border.width: 1
                                border.color: !enabled ? Theme.c.border
                                            : takeMouse.containsMouse ? Theme.c.error
                                            : Theme.c.border

                                Text {
                                    anchors.centerIn: parent
                                    text: qsTr("TAKE")
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                    color: parent.enabled ? Theme.c.text : Theme.c.textMuted
                                    opacity: parent.enabled ? 1.0 : 0.4
                                }

                                MouseArea {
                                    id: takeMouse

                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: Document.takeDeck(deck.deckId)
                                }

                                ToolTipText {
                                    text: qsTr("Put what is on preview onto program")
                                    visible: takeMouse.containsMouse
                                    hoverArea: takeMouse
                                }
                            }

                            CheckLabel {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Studio")
                                checked: deck.studioMode
                                onToggled: Document.setDeckStudioMode(deck.deckId,
                                                                      !deck.studioMode)
                            }
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

                        // ADR-0025: a canvas with no safety net is not a broken document, it is
                        // a pre-flight question. This is where the answer belongs.
                        Text {
                            width: parent.width
                            visible: !deck.hasEmergencyScene
                            text: qsTr("⚠ %1 has no emergency scene").arg(deck.canvasName)
                            font.pixelSize: 9
                            color: Theme.c.warning
                            wrapMode: Text.WordWrap
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
