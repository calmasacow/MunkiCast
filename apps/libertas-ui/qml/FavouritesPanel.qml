// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// The scenes marked with a star, and nothing else.
///
/// This exists so that a live workspace can close the scenes and sources panels entirely. During
/// a show the useful set is small and known in advance, while the full scene list is a building
/// tool -- and every row of it that is not going on air is a row to mis-click under pressure.
///
/// Rows are bigger than the scene list's on purpose: this is the panel that gets clicked while
/// something else has your attention.
Item {
    id: panel

    SceneReorder {
        id: reorder
        view: list
        favourites: true
    }

    ListView {
        id: list

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
        clip: true
        model: Document.favouriteScenes
        boundsBehavior: Flickable.StopAtBounds
        // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
        // because Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

        delegate: Rectangle {
            id: row

            required property int index
            required property string sceneId
            required property string name
            required property bool isProgram
            required property bool isPreview
            required property bool isEmergency

            readonly property bool selected: Document.selectedScene === row.sceneId

            objectName: "favouriteRow"
            width: ListView.view.width
            height: 32
            color: row.isProgram ? Theme.c.panelHeaderActive
                 : row.selected ? Theme.c.accent
                 : mouse.containsMouse ? Theme.c.panelHeaderActive
                 : "transparent"
            border.width: row.activeFocus || row.isProgram ? 1 : 0
            border.color: row.activeFocus ? Theme.c.text : Theme.c.error

            opacity: reorder.dragging && reorder.sceneId === row.sceneId ? 0.45 : 1
            activeFocusOnTab: true
            Accessible.role: Accessible.ListItem
            Accessible.name: row.name
            Accessible.description: qsTr("Drag to reorder, or use Ctrl+Up and Ctrl+Down")
            Keys.onEscapePressed: reorder.cancel()
            Keys.onPressed: event => {
                if ((event.modifiers & Qt.ControlModifier) &&
                    (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
                    reorder.step(row.sceneId, row.index, event.key === Qt.Key_Up ? -1 : 1);
                    event.accepted = true;
                }
            }
            Keys.onReturnPressed: Document.activateScene(row.sceneId)
            Keys.onSpacePressed: Document.activateScene(row.sceneId)

            MouseArea {
                id: mouse

                anchors.fill: parent
                hoverEnabled: true
                property bool suppressClick: false
                cursorShape: reorder.dragging ? Qt.ClosedHandCursor : Qt.ArrowCursor
                onPressed: event => {
                    suppressClick = false;
                    if (event.button === Qt.LeftButton) {
                        row.forceActiveFocus();
                        reorder.begin(row.sceneId, mapToItem(reorder, event.x, event.y));
                    }
                }
                onPositionChanged: event => {
                    if (pressed && (pressedButtons & Qt.LeftButton)) {
                        reorder.move(mapToItem(reorder, event.x, event.y));
                        suppressClick = suppressClick || reorder.dragging;
                    }
                }
                onReleased: event => {
                    if (event.button === Qt.LeftButton) {
                        suppressClick = suppressClick || reorder.dragging;
                        reorder.finish();
                    }
                }
                onCanceled: reorder.cancel()
                onClicked: if (!suppressClick) Document.activateScene(row.sceneId)
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: markers.left
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                text: row.name
                font.pixelSize: 13
                color: row.selected && !row.isProgram ? Theme.c.window : Theme.c.text
                elide: Text.ElideRight
            }

            Row {
                id: markers

                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.isProgram
                    width: liveLabel.implicitWidth + 8
                    height: 14
                    radius: 2
                    color: Theme.c.error

                    Text {
                        id: liveLabel

                        anchors.centerIn: parent
                        text: qsTr("LIVE")
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        color: Theme.c.window
                    }
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.isPreview
                    width: previewLabel.implicitWidth + 8
                    height: 14
                    radius: 2
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.c.good

                    Text {
                        id: previewLabel

                        anchors.centerIn: parent
                        text: qsTr("PVW")
                        font.pixelSize: 9
                        color: Theme.c.good
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.isEmergency
                    text: "⛨"
                    font.pixelSize: 11
                    color: Theme.c.good
                    opacity: 0.8
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: starMouse.containsMouse
                    text: "★"
                    font.pixelSize: 12
                    color: Theme.c.warning

                    MouseArea {
                        id: starMouse

                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        onClicked: Document.setSceneFavourite(row.sceneId, false)
                    }

                    ToolTipText {
                        text: qsTr("Remove from favourites")
                        visible: starMouse.containsMouse
                        hoverArea: starMouse
                    }
                }

                // Keeps the star's hover target alive when the pointer is not over it.
                Item {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !starMouse.containsMouse
                    width: 12
                    height: 12
                }
            }
        }
    }

    Text {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        visible: Document.favouriteScenes.rowCount() === 0
        wrapMode: Text.WordWrap
        text: qsTr("No favourites yet. Click the star beside a scene to put it here — then a live workspace can close the scene and source lists entirely.")
        font.pixelSize: 10
        color: Theme.c.textMuted
    }

    Rectangle {
        id: footer

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 22
        color: Theme.c.panelHeader

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.c.border
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("%n favourite(s)", "", Document.favouriteScenes.rowCount())
            font.pixelSize: 9
            color: Theme.c.textMuted
            opacity: 0.7
        }
    }
}
