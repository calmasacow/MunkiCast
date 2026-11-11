// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

Item {
    id: dialog

    property string targetCanvasId: ""
    property string targetSceneId: ""
    property string mainSceneId: ""
    property var canvases: []
    property var mainRows: []
    readonly property var linkedMainIds: mainRows.filter(r => r.ticked).map(r => r.id)
    readonly property bool selectedPairLinked: mainSceneId !== ""
                                              && linkedMainIds.indexOf(mainSceneId) !== -1
    readonly property string movingFrom: {
        for (const row of mainRows)
            if (row.id === mainSceneId)
                return row.takenBy;
        return "";
    }

    signal closed

    anchors.fill: parent
    visible: false
    focus: visible
    z: 50

    function refresh() {
        dialog.canvases = Document.sceneLinkCanvases();
        if (!dialog.canvases.some(c => c.id === dialog.targetCanvasId))
            dialog.setCanvas(dialog.canvases.length ? dialog.canvases[0].id : "");
        dialog.mainRows = Document.mainScenesForLink(dialog.targetSceneId);
        if (dialog.mainSceneId !== ""
                && !dialog.mainRows.some(r => r.id === dialog.mainSceneId)
                && dialog.targetSceneId !== "")
            dialog.mainSceneId = "";
    }

    function setCanvas(id) {
        dialog.targetCanvasId = id;
        dialog.targetSceneId = "";
        dialog.mainSceneId = "";
        dialog.mainRows = [];
    }

    function selectTarget(id) {
        dialog.targetSceneId = id;
        dialog.mainRows = Document.mainScenesForLink(id);
        if (dialog.linkedMainIds.length)
            dialog.mainSceneId = dialog.linkedMainIds[0];
    }

    function open(canvasId, sceneId) {
        dialog.setCanvas(canvasId);
        dialog.refresh();
        if (sceneId !== "")
            dialog.selectTarget(sceneId);
        dialog.visible = true;
        dialog.forceActiveFocus();
    }

    function close() {
        dialog.visible = false;
        dialog.closed();
    }

    function link() {
        if (!dialog.mainSceneId || !dialog.targetSceneId || dialog.selectedPairLinked)
            return;
        Document.commitSceneLinks(dialog.targetSceneId,
                                  dialog.linkedMainIds.concat([dialog.mainSceneId]));
    }

    function unlink() {
        if (dialog.selectedPairLinked)
            Document.commitSceneLinks(dialog.targetSceneId,
                dialog.linkedMainIds.filter(id => id !== dialog.mainSceneId));
    }

    Connections {
        target: Document
        function onChanged() { if (dialog.visible) dialog.refresh(); }
    }

    Keys.onEscapePressed: dialog.close()

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: dialog.close()
        onWheel: wheel => { wheel.accepted = true; }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
        opacity: 0.55
    }

    component SceneList: Rectangle {
        id: sceneList

        property string canvasId: ""
        property string selectedScene: ""
        property bool showLinks: false
        signal selected(string sceneId)

        color: Theme.c.window
        border.color: Theme.c.accent
        radius: 2

        ListView {
            id: list

            anchors.fill: parent
            anchors.margins: 1
            clip: true
            model: dialog.visible && (!sceneList.showLinks || dialog.targetCanvasId !== "")
                   ? Document.scenesFor(sceneList.canvasId) : null
            boundsBehavior: Flickable.StopAtBounds
            interactive: false
            NoDragScroll {}

            delegate: Rectangle {
                id: row

                required property int index
                required property string sceneId
                required property string name
                required property int linkCount
                readonly property bool selected: sceneList.selectedScene === row.sceneId

                objectName: sceneList.showLinks ? "sceneLinkTargetRow" : "sceneLinkRow"
                width: list.width
                height: 30
                color: row.selected ? Theme.c.accent
                     : mouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                activeFocusOnTab: true
                Accessible.role: Accessible.ListItem
                Accessible.name: row.name
                Accessible.selected: row.selected
                Accessible.onPressAction: sceneList.selected(row.sceneId)
                Keys.onReturnPressed: sceneList.selected(row.sceneId)
                Keys.onSpacePressed: sceneList.selected(row.sceneId)
                onSelectedChanged: if (selected) list.positionViewAtIndex(index, ListView.Contain)

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 9
                    anchors.right: chain.left
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.name
                    font.pixelSize: 13
                    color: row.selected ? Theme.c.window : Theme.c.text
                    elide: Text.ElideRight
                }

                ChainIcon {
                    id: chain
                    objectName: "sceneLinkStatus"
                    anchors.right: parent.right
                    anchors.rightMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    visible: sceneList.showLinks && row.linkCount > 0
                    strong: true
                    color: Theme.c.good
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: Theme.c.border
                }

                MouseArea {
                    id: mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: sceneList.selected(row.sceneId)
                }
            }

            Text {
                anchors.centerIn: parent
                visible: list.count === 0
                text: qsTr("No scenes")
                color: Theme.c.textMuted
                font.pixelSize: 11
            }
        }
    }

    Rectangle {
        id: frame

        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        objectName: "sceneLinkFrame"
        DialogGeometry { minimumWidth: 560; minimumHeight: 440; dragWidth: parent.width * 0.48 }
        width: Math.min(parent.width - 48, 736)
        height: Math.min(parent.height - 48, 672)
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
            height: 48
            color: Theme.c.panelHeader

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Scene Linker")
                font.pixelSize: 20
                color: Theme.c.text
            }

            ChoiceField {
                objectName: "sceneLinkCanvas"
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(310, parent.width * 0.48)
                label: qsTr("Target Canvas")
                labelWidth: 85
                choices: dialog.canvases.map(c => c.id)
                choiceLabels: {
                    const labels = {};
                    for (const c of dialog.canvases)
                        labels[c.id] = c.name;
                    return labels;
                }
                value: dialog.targetCanvasId
                onCommitted: id => dialog.setCanvas(id)
            }
        }

        Text {
            id: mainLabel
            anchors.left: mainList.left
            anchors.bottom: mainList.top
            anchors.bottomMargin: 6
            text: qsTr("Main")
            font.pixelSize: 11
            color: Theme.c.textMuted
        }

        Text {
            anchors.left: targetList.left
            anchors.bottom: targetList.top
            anchors.bottomMargin: 6
            text: {
                for (const c of dialog.canvases)
                    if (c.id === dialog.targetCanvasId)
                        return c.name;
                return qsTr("Target canvas");
            }
            font.pixelSize: 11
            color: Theme.c.textMuted
        }

        SceneList {
            id: mainList
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.top: titleBar.bottom
            anchors.topMargin: 28
            anchors.bottom: status.top
            anchors.bottomMargin: 8
            width: Math.max(0, (frame.width - 92) / 2)
            selectedScene: dialog.mainSceneId
            onSelected: id => { dialog.mainSceneId = id; }
        }

        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: mainList.verticalCenter
            spacing: Math.max(24, mainList.height * 0.22)

            ToolButton {
                objectName: "commitLinks"
                width: 36
                height: 36
                label: ""
                tip: dialog.movingFrom !== ""
                    ? qsTr("Link scenes (moves link from %1)").arg(dialog.movingFrom)
                    : qsTr("Link selected scenes")
                enabled: dialog.mainSceneId !== "" && dialog.targetSceneId !== ""
                         && !dialog.selectedPairLinked
                onClicked: dialog.link()
                ChainIcon {
                    anchors.centerIn: parent
                    width: 26
                    height: 26
                    strong: true
                    color: parent.enabled ? Theme.c.text : Theme.c.textMuted
                    opacity: parent.enabled ? 1 : 0.4
                }
            }

            ToolButton {
                objectName: "unlinkScenes"
                width: 36
                height: 36
                label: ""
                tip: qsTr("Unlink selected scenes")
                enabled: dialog.selectedPairLinked
                onClicked: dialog.unlink()
                ChainIcon {
                    anchors.centerIn: parent
                    width: 26
                    height: 26
                    broken: true
                    color: parent.enabled ? Theme.c.text : Theme.c.textMuted
                    opacity: parent.enabled ? 1 : 0.4
                }
            }
        }

        SceneList {
            id: targetList
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.top: mainList.top
            anchors.bottom: mainList.bottom
            width: mainList.width
            canvasId: dialog.targetCanvasId
            selectedScene: dialog.targetSceneId
            showLinks: true
            visible: dialog.targetCanvasId !== ""
            onSelected: id => dialog.selectTarget(id)
        }

        Text {
            id: status
            anchors.left: mainList.left
            anchors.right: targetList.right
            anchors.bottom: exitButton.top
            anchors.bottomMargin: 8
            height: 28
            text: dialog.movingFrom !== "" && !dialog.selectedPairLinked
                ? qsTr("Linking moves the Main scene’s link from “%1”.").arg(dialog.movingFrom)
                : qsTr("Select a Main scene and a target scene, then link or unlink.")
            color: dialog.movingFrom !== "" ? Theme.c.warning : Theme.c.textMuted
            font.pixelSize: 10
            wrapMode: Text.WordWrap
        }

        DialogButton {
            id: exitButton
            objectName: "exitSceneLinker"
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            minimumWidth: 150
            text: qsTr("Exit")
            onClicked: dialog.close()
        }
    }
}
