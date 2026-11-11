// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import MunkiCast

Item {
    id: panel

    /// Which canvas this list belongs to. Empty means the first canvas, which is what the
    /// plain "scenes" panel id means (ADR-0027: Main always exists and cannot move, so it is
    /// the one canvas whose panel id can be a constant).
    ///
    /// Every canvas gets its own scene list, titled "Scenes: Vertical" in the tab. Filtering
    /// one shared list by a current canvas was tried first and is not the same thing: two
    /// canvases cannot then be worked on side by side, which is the whole point of having
    /// more than one.
    property string canvasId: ""

    /// Which scene this canvas's list is on.
    ///
    /// Pushed from a signal rather than bound to an invokable: a binding on a Q_INVOKABLE has
    /// nothing to re-evaluate on, so it would be read once and then quietly stop being true.
    property string selectedScene: ""

    function refreshSelection() {
        panel.selectedScene = Document.selectedSceneOn(panel.canvasId);
    }

    Component.onCompleted: panel.refreshSelection()
    onCanvasIdChanged: panel.refreshSelection()

    Connections {
        target: Document

        function onSelectionChanged() { panel.refreshSelection(); }
        function onChanged() { panel.refreshSelection(); }
    }

    // Parented to the window's content item rather than drawn in the panel, which would clip
    // it -- the same arrangement the sources panel uses for its two dialogs, and still one
    // surface (ADR-0015).
    SceneLinkDialog {
        id: linkDialog

        objectName: "sceneLinkDialog"
        parent: panel.Window.contentItem
    }

    // Copy and paste live on the scene row's right-click, parented the same way. A scene
    // pasted onto another canvas keeps its name, because matching names across canvases are
    // what make a scene link readable (ADR-0036).
    StripMenu {
        id: sceneMenu

        objectName: "sceneMenu"
        parent: panel.Window.contentItem
        z: 40
    }

    PasteWarning {
        id: pasteWarning

        objectName: "pasteWarning"
        parent: panel.Window.contentItem
        z: 45
    }

    SceneReorder {
        id: reorder
        view: list
        favourites: false
    }

    ListView {
        id: list

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
        clip: true
        // Main's canvas ID changes between collections. Use revision in the result so the
        // QML compiler retains this dependency; an unused read can be optimized away.
        model: Document.revision >= 0 ? Document.scenesFor(panel.canvasId) : null
        currentIndex: -1
        boundsBehavior: Flickable.StopAtBounds
        // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
        // because Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

        delegate: Rectangle {
            id: row

            // The self-check looks for these by name, so that "the model has
            // three scenes" and "three rows reached the screen" stay different
            // claims.
            objectName: "sceneRow"

            required property int index
            required property string sceneId
            required property string name
            required property int nodeCount
            required property bool isProgram
            required property bool isPreview
            required property bool isEmergency
            required property bool isFavourite
            required property int linkCount
            required property bool canLink

            readonly property bool selected: panel.selectedScene === row.sceneId

            width: ListView.view.width
            height: 24
            color: row.selected ? Theme.c.accent
                 : mouse.containsMouse ? Theme.c.panelHeaderActive
                 : "transparent"

            border.width: row.activeFocus ? 1 : 0
            border.color: Theme.c.text
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
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: (ev) => {
                    if (mouse.suppressClick) return;
                    if (ev.button !== Qt.RightButton) {
                        Document.activateScene(row.sceneId);
                        return;
                    }
                    Document.selectSceneOn(panel.canvasId, row.sceneId);
                    const at = mapToItem(sceneMenu, ev.x, ev.y);
                    const rows = [{
                        text: qsTr("Copy scene"),
                        checked: false,
                        action: () => Document.copyScene(row.sceneId)
                    }];
                    if (row.canLink) {
                        rows.push({
                            text: qsTr("Link scene"),
                            checked: false,
                            action: () => linkDialog.open(panel.canvasId, row.sceneId)
                        });
                    }
                    if (Document.pasteDescription() !== "") {
                        rows.push({
                            text: Document.pasteDescription(),
                            checked: false,
                            action: () => {
                                const off = Document.pasteWouldMissCanvas();
                                if (off > 0)
                                    pasteWarning.ask(off);
                                else
                                    Document.paste();
                            }
                        });
                    }
                    sceneMenu.popup(at.x, at.y, rows);
                }
                onDoubleClicked: {
                    if (mouse.suppressClick) return;
                    rename.text = row.name;
                    rename.visible = true;
                    rename.forceActiveFocus();
                    rename.selectAll();
                }
            }

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.right: markers.left
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !rename.visible
                    text: row.name
                    font.pixelSize: 12
                    color: row.selected ? Theme.c.window : Theme.c.text
                    elide: Text.ElideRight
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !rename.visible
                    text: row.nodeCount === 0 ? qsTr("empty") : row.nodeCount
                    font.pixelSize: 10
                    color: row.selected ? Theme.c.window : Theme.c.textMuted
                    opacity: 0.7
                }
            }

            TextInput {
                id: rename

                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.right: markers.left
                anchors.verticalCenter: parent.verticalCenter
                visible: false
                font.pixelSize: 12
                color: row.selected ? Theme.c.window : Theme.c.text
                selectionColor: Theme.c.accent
                selectByMouse: true

                onAccepted: {
                    visible = false;
                    if (text !== row.name)
                        Document.renameScene(row.sceneId, text);
                }
                onActiveFocusChanged: {
                    if (!activeFocus)
                        visible = false;
                }
                Keys.onEscapePressed: visible = false
            }

            Row {
                id: markers

                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                // Shown filled when it is a favourite and faintly when it is not, so the way to
                // make one is discoverable by hovering the row rather than by being told.
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.isFavourite || mouse.containsMouse
                    text: row.isFavourite ? "★" : "☆"
                    font.pixelSize: 12
                    color: row.isFavourite
                        ? (row.selected ? Theme.c.window : Theme.c.warning)
                        : (row.selected ? Theme.c.window : Theme.c.textMuted)
                    opacity: row.isFavourite ? 1.0 : (starMouse.containsMouse ? 0.9 : 0.45)

                    MouseArea {
                        id: starMouse

                        anchors.fill: parent
                        anchors.margins: -3
                        hoverEnabled: true
                        onClicked: Document.setSceneFavourite(row.sceneId, !row.isFavourite)
                    }

                    ToolTipText {
                        text: row.isFavourite
                            ? qsTr("Remove from favourites")
                            : qsTr("Add to favourites")
                        visible: starMouse.containsMouse
                        hoverArea: starMouse
                    }
                }

                // Two links of a chain: which Main scenes bring this one up (ADR-0027).
                //
                // Only on a secondary canvas. Linking is purely a secondary canvas's function
                // and a link always runs *from* Main, so Main's own rows carry no chain at all
                // -- not a greyed one, none. A control that is always disabled is a question
                // the operator keeps re-asking.
                ChainIcon {
                    objectName: "sceneLink"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.canLink
                    strong: row.linkCount > 0
                    color: row.linkCount > 0
                        ? (row.selected ? Theme.c.window : Theme.c.good)
                        : (row.selected ? Theme.c.window : Theme.c.textMuted)
                    opacity: row.linkCount > 0 ? 1.0 : (chainMouse.containsMouse ? 0.9 : 0.45)

                    MouseArea {
                        id: chainMouse

                        anchors.fill: parent
                        anchors.margins: -3
                        hoverEnabled: true
                        onClicked: linkDialog.open(panel.canvasId, row.sceneId)
                    }

                    ToolTipText {
                        text: row.linkCount === 0
                            ? qsTr("Nothing on Main brings this scene up")
                            : row.linkCount === 1
                              ? qsTr("1 Main scene brings this scene up")
                              : qsTr("%1 Main scenes bring this scene up").arg(row.linkCount)
                        visible: chainMouse.containsMouse
                        hoverArea: chainMouse
                    }
                }

                // On air. Named rather than a coloured dot, because which dot means what is
                // exactly the thing nobody remembers at the moment it matters.
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
                    color: Theme.c.good

                    Text {
                        id: previewLabel

                        anchors.centerIn: parent
                        text: qsTr("PREV")
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        color: Theme.c.window
                    }
                }

                // ADR-0025: the safety net is marked, because losing it unnoticed is that
                // decision's own failure arriving by another route.
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.isEmergency
                    width: safeLabel.implicitWidth + 8
                    height: 14
                    radius: 2
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.c.warning

                    Text {
                        id: safeLabel

                        anchors.centerIn: parent
                        text: qsTr("SAFE")
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        color: Theme.c.warning
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
                tip: qsTr("Add scene")
                onClicked: Document.addSceneOn(panel.canvasId, "")
            }

            ToolButton {
                label: "−"
                tip: qsTr("Remove scene")
                enabled: panel.selectedScene !== ""
                onClicked: Document.removeScene(panel.selectedScene)
            }

            ToolButton {
                objectName: "openSceneLinker"
                visible: panel.canvasId !== ""
                label: ""
                tip: qsTr("Link scenes")
                onClicked: linkDialog.open(panel.canvasId, panel.selectedScene)

                ChainIcon {
                    anchors.centerIn: parent
                    color: Theme.c.text
                }
            }

            ToolButton {
                label: "⧉"
                tip: qsTr("Duplicate scene")
                enabled: panel.selectedScene !== ""
                onClicked: Document.duplicateScene(panel.selectedScene)
            }
        }

        ToolButton {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            label: "⚠"
            tip: qsTr("Use as the emergency scene for this canvas")
            enabled: panel.selectedScene !== ""
            onClicked: Document.setEmergencyScene(panel.selectedScene)
        }
    }
}
