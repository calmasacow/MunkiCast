// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import QtQuick.Templates as T
import Libertas

Item {
    id: studio
    objectName: "studioModeView"
    readonly property var deck: Document.mainDeck
    readonly property size canvasSize: {
        void Document.revision;
        return Document.canvasSizeOf("");
    }
    readonly property real aspect: canvasSize.height > 0 ? canvasSize.width / canvasSize.height : 16 / 9
    readonly property bool canTake: !!deck.canTake
    property string manualProgram: ""
    property string manualPreview: ""

    function take() {
        Document.takeDeck(studio.deck.id);
        Preview.setManualTransition(-1);
        manual.value = 0;
    }
    function titleFor(id) {
        for (const entry of Document.transitions)
            if (entry.id === id) return entry.title;
        return qsTr("Missing transition");
    }
    function editQuick(index, item) {
        const entries = Document.transitions.filter(t => t.implemented).map(t => ({
            text: t.title,
            action: () => Document.setQuickTransition(index, t.id,
                t.id === "transition.cut" ? 0 : item.durationMs || 300)
        }));
        if (item.transition !== "transition.cut" && !item.transition.startsWith("transition.stinger:")) {
            for (const ms of [100, 300, 500, 1000, 1500, 2000])
                entries.push({text: qsTr("Duration: %1 ms").arg(ms),
                    action: () => Document.setQuickTransition(index, item.transition, ms)});
        }
        entries.push({text: qsTr("Remove"), action: () => Document.removeQuickTransition(index)});
        return entries;
    }
    Connections {
        target: Document
        function onChanged() {
            if (manual.value > 0 && (studio.manualProgram !== studio.deck.program ||
                                     studio.manualPreview !== studio.deck.preview)) {
                Preview.setManualTransition(-1);
                manual.value = 0;
            }
        }
    }
    Component.onDestruction: Preview.setManualTransition(-1)
    StripMenu { id: quickMenu; parent: studio.Window.contentItem; z: 100 }

    component Monitor: Item {
        id: monitor
        required property string frameKey
        required property string title
        property bool editable: false
        readonly property bool ready: Preview.generation >= 0 && Preview.hasFrame(frameKey)
        Text {
            anchors.top: parent.top
            text: monitor.title
            font.pixelSize: 11
            color: monitor.editable ? Theme.c.good : Theme.c.error
        }
        Rectangle {
            id: picture
            objectName: monitor.editable ? "studioPreviewFrame" : "studioProgramFrame"
            anchors.centerIn: parent
            width: Math.max(1, Math.min(parent.width, (parent.height - 24) * studio.aspect))
            height: width / studio.aspect
            color: Theme.c.window
            border.width: 1
            border.color: monitor.editable ? Theme.c.good : Theme.c.error
            readonly property int edge: Math.ceil(Math.max(width, height) * Screen.devicePixelRatio)
            onEdgeChanged: Preview.requestSize(picture, edge)
            Component.onCompleted: Preview.requestSize(picture, edge)
            Image {
                anchors.fill: parent
                anchors.margins: 1
                cache: false
                smooth: false
                visible: monitor.ready
                source: monitor.ready
                    ? "image://program/" + Preview.canvasKey(monitor.frameKey) + "/" + Preview.generation : ""
            }
            Text {
                anchors.centerIn: parent
                visible: !monitor.ready
                text: qsTr("Waiting for %1").arg(monitor.title)
                font.pixelSize: 11
                color: Theme.c.textMuted
            }
            ProgramOverlay {
                visible: monitor.editable
                enabled: monitor.editable
                inputSurface: monitor
                canvasId: ""
                canvasWidth: studio.canvasSize.width
                canvasHeight: studio.canvasSize.height
            }
        }
    }

    Monitor {
        anchors.left: parent.left
        anchors.right: controls.left
        anchors.rightMargin: 10
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        frameKey: Preview.studioPreviewKey()
        title: qsTr("Preview")
        editable: true
    }
    Column {
        id: controls
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(176, studio.width * 0.23)
        spacing: 6
        DialogButton {
            objectName: "takeButton"
            width: parent.width
            minimumWidth: 0
            text: qsTr("Take")
            primary: true
            enabled: studio.canTake
            onClicked: studio.take()
        }
        Row {
            width: parent.width
            spacing: 2
            Text {
                width: parent.width - 24
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Quick Transitions")
                font.pixelSize: 10
                color: Theme.c.textMuted
                elide: Text.ElideRight
            }
            ToolButton {
                objectName: "addQuickTransition"
                label: "+"
                tip: qsTr("Add a quick transition")
                enabled: Document.quickTransitions.length < 32
                onClicked: {
                    const at = mapToItem(quickMenu, 0, height);
                    quickMenu.popup(at.x, at.y, Document.transitions.filter(t => t.implemented).map(t => ({
                        text: t.title,
                        action: () => Document.setQuickTransition(Document.quickTransitions.length, t.id, 300)
                    })));
                }
            }
        }
        Repeater {
            model: Document.quickTransitions
            delegate: Row {
                id: quick
                required property var modelData
                required property int index
                width: controls.width
                spacing: 2
                DialogButton {
                    objectName: "quickTransitionButton"
                    width: parent.width - 24
                    minimumWidth: 0
                    text: studio.titleFor(quick.modelData.transition) +
                        (quick.modelData.transition === "transition.cut" || quick.modelData.transition.startsWith("transition.stinger:")
                            ? "" : " (" + quick.modelData.durationMs + " ms)")
                    enabled: studio.canTake && Document.transitions.some(t =>
                        t.id === quick.modelData.transition && t.implemented)
                    onClicked: {
                        Preview.setManualTransition(-1);
                        manual.value = 0;
                        Document.triggerQuickTransition(quick.index);
                    }
                }
                ToolButton {
                    label: "▾"
                    tip: qsTr("Configure quick transition")
                    onClicked: {
                        const at = mapToItem(quickMenu, 0, height);
                        quickMenu.popup(at.x, at.y, studio.editQuick(quick.index, quick.modelData));
                    }
                }
            }
        }
        T.Slider {
            id: manual
            objectName: "manualTransitionSlider"
            width: parent.width
            implicitHeight: 22
            from: 0; to: 1
            stepSize: 0.001
            enabled: studio.canTake && ["transition.cut", "transition.fade", "transition.fade-to-black"]
                .includes(studio.deck.transition)
            opacity: enabled ? 1 : 0.4
            Accessible.name: qsTr("Manual transition")
            onMoved: {
                studio.manualProgram = studio.deck.program;
                studio.manualPreview = studio.deck.preview;
                Preview.setManualTransition(value);
                if (value >= 1) studio.take();
            }
            onPressedChanged: {
                if (!pressed && value <= 0) Preview.setManualTransition(-1);
            }
            background: Rectangle {
                x: manual.leftPadding
                y: (manual.height - height) / 2
                width: manual.availableWidth
                height: 3
                color: Theme.c.border
            }
            handle: Rectangle {
                x: manual.leftPadding + manual.visualPosition * (manual.availableWidth - width)
                y: (manual.height - height) / 2
                width: 8; height: 18
                color: manual.activeFocus || manual.pressed ? Theme.c.accent : Theme.c.textMuted
            }
        }
    }
    Monitor {
        anchors.left: controls.right
        anchors.leftMargin: 10
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        frameKey: ""
        title: qsTr("Program")
    }
}
