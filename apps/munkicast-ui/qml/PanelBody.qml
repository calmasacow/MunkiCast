// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

// Panel contents.
//
// The program picture is a frame published by munkicast-render when one has arrived (ADR-0040).
// Until then the panel still says no pipeline is attached. The other bodies are this shell's
// own controls; a meter or a live button this build cannot back is not drawn.
Item {
    id: body
    objectName: "panelBody"

    required property string panelId

    /// "scenes" and "scenes:<canvasId>" are the same panel showing different canvases. The
    /// first canvas keeps the bare id -- it always exists and cannot move (ADR-0027) -- so a
    /// layout written before canvases had panels still loads.
    readonly property string panelKind: {
        const at = body.panelId.indexOf(":");
        return at < 0 ? body.panelId : body.panelId.substring(0, at);
    }
    readonly property string panelCanvas: {
        const at = body.panelId.indexOf(":");
        return at < 0 ? "" : body.panelId.substring(at + 1);
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.panelBackground
    }

    // One Loader rather than three panels with two of them hidden.
    //
    // A hidden item is still a built item: the first version instantiated all three document
    // panels in *every* panel frame, so a six-panel workspace held six scene lists and six
    // source lists, each with its own ListView over the same model. Only one was ever visible.
    Loader {
        id: panelLoader
        objectName: "panelContentLoader"

        anchors.fill: parent
        active: sourceComponent !== null
        sourceComponent: {
            switch (body.panelKind) {
            case "scenes":
                return scenesComponent;
            case "sources":
                return sourcesComponent;
            case "properties":
                return propertiesComponent;
            case "shortcuts":
                return shortcutsComponent;
            case "info":
                return infoComponent;
            case "broadcast-stats":
                return broadcastComponent;
            case "log":
                return logComponent;
            case "audio-mixer":
                return mixerComponent;
            case "countdowns":
                return countdownsComponent;
            case "controls":
                return controlsComponent;
            case "outputs":
                return outputsComponent;
            case "canvas":
                return canvasComponent;
            case "favourites":
                return favouritesComponent;
            default:
                return null;
            }
        }
    }

    Component {
        id: countdownsComponent
        CountdownsPanel {}
    }

    Component {
        id: scenesComponent

        ScenesPanel {
            canvasId: body.panelCanvas
        }
    }

    Component {
        id: sourcesComponent

        SourcesPanel {
            canvasId: body.panelCanvas
        }
    }

    Component {
        id: propertiesComponent

        PropertiesPanel {}
    }

    Component {
        id: shortcutsComponent

        ShortcutsPanel {}
    }

    Component {
        id: infoComponent

        InfoPanel {}
    }

    Component {
        id: broadcastComponent

        BroadcastStatsPanel {}
    }

    Component {
        id: logComponent

        LogPanel {}
    }

    Component {
        id: mixerComponent

        AudioMixerPanel {}
    }

    Component {
        id: controlsComponent

        ControlsPanel {}
    }

    Component {
        id: outputsComponent

        OutputsPanel {}
    }

    Component {
        id: canvasComponent

        CanvasPanel {}
    }

    Component {
        id: favouritesComponent

        FavouritesPanel {}
    }

    // The program canvas. ADR-0015 rule 2: it is a region of this surface, never a separate OS
    // window, so it cannot be lost, moved off-screen or closed by accident.
    //
    // The pixels, when there are any, are the render process's frame. This item does not
    // composite. No frame yet — the process has not started, or it has gone — and the centred
    // caption stays the one that says so.
    Item {
        id: programView
        objectName: "programView"

        visible: body.panelKind === "program" && !studioLoader.active
        /// The canvas this picture is of. Read again on every document change, because a
        /// canvas can be resized or renamed while its panel is open.
        readonly property size canvasSize: {
            if (Document.revision < 0) return Qt.size(0, 0);
            return Document.canvasSizeOf(body.panelCanvas);
        }
        readonly property string canvasName: {
            if (Document.revision < 0) return "";
            return Document.canvasNameOf(body.panelCanvas);
        }
        /// `caption` and `hasFrame` are calls. A bare statement is not a dependency — the QML
        /// compiler drops it — so the generation has to be used in the value.
        readonly property int previewGeneration: Preview.generation
        readonly property bool frameReady: programView.visible && programView.previewGeneration >= 0 &&
                                            Preview.hasFrame(body.panelCanvas)
        readonly property string liveCaption: programView.previewGeneration < 0
                                              ? ""
                                              : Preview.caption(body.panelCanvas)
        readonly property string idleCaption: programView.canvasSize.width > 0
                ? qsTr("%1x%2 · no pipeline attached")
                    .arg(programView.canvasSize.width).arg(programView.canvasSize.height)
                : qsTr("no pipeline attached")
        /// Width over height. A 9:16 vertical canvas has to look like one: drawn into the old
        /// fixed 16:9 frame it would read as a second landscape output.
        readonly property real aspect: programView.canvasSize.width > 0 &&
                                       programView.canvasSize.height > 0
                                       ? programView.canvasSize.width / programView.canvasSize.height
                                       : 16 / 9
        anchors.fill: parent
        anchors.margins: 10
        onVisibleChanged: Preview.requestSize(canvasFrame, visible ? canvasFrame.previewEdge : 0)

        Rectangle {
            id: canvasFrame

            anchors.centerIn: parent
            objectName: "programFrame"
            // The canvas's own shape, letterboxed into whatever space the layout gave us.
            readonly property real fit: Math.min(parent.width / programView.aspect, parent.height)
            width: Math.max(fit * programView.aspect, 1)
            height: Math.max(fit, 1)
            readonly property int previewEdge: Math.ceil(Math.max(width, height) * Screen.devicePixelRatio)
            onPreviewEdgeChanged: Preview.requestSize(canvasFrame, programView.visible ? previewEdge : 0)
            Component.onCompleted: Preview.requestSize(canvasFrame, programView.visible ? previewEdge : 0)
            color: Theme.c.window
            border.width: 1
            border.color: Theme.c.border

            // Behind the thirds. Stretch, unsmoothed: a solid colour has to stay that colour
            // when the frame is a different size from the canvas.
            Image {
                anchors.fill: parent
                visible: programView.frameReady
                cache: false
                smooth: false
                asynchronous: false
                fillMode: Image.Stretch
                source: programView.frameReady
                        ? "image://program/" + Preview.canvasKey(body.panelCanvas) + "/" +
                          programView.previewGeneration
                        : ""
            }

            // Thirds, which is what an operator actually frames against.
            Repeater {
                model: 2

                delegate: Rectangle {
                    required property int index
                    x: canvasFrame.width * (index + 1) / 3
                    width: 1
                    height: canvasFrame.height
                    color: Theme.c.border
                    opacity: 0.35
                }
            }

            Repeater {
                model: 2

                delegate: Rectangle {
                    required property int index
                    y: canvasFrame.height * (index + 1) / 3
                    width: canvasFrame.width
                    height: 1
                    color: Theme.c.border
                    opacity: 0.35
                }
            }

            Column {
                visible: !programView.frameReady
                anchors.centerIn: parent
                spacing: 4

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: programView.canvasName === "" ? qsTr("Program") : programView.canvasName
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Theme.c.textMuted
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    objectName: "programStatus"
                    text: programView.idleCaption
                    font.pixelSize: 11
                    color: Theme.c.textMuted
                    opacity: 0.7
                }
            }

            // Once a frame is up, the caption is a corner note. The picture is the content;
            // a title stacked on the middle of it would be covering the thing it describes.
            ProgramOverlay {
                inputSurface: body
                z: 2
                canvasId: body.panelCanvas
                canvasWidth: programView.canvasSize.width
                canvasHeight: programView.canvasSize.height
            }

            Rectangle {
                visible: programView.frameReady
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 6
                radius: 2
                color: Theme.c.panelBackground
                opacity: 0.82
                width: liveStatus.implicitWidth + 10
                height: liveStatus.implicitHeight + 6

                Text {
                    id: liveStatus
                    objectName: "programStatus"
                    anchors.centerIn: parent
                    text: programView.liveCaption
                    font.pixelSize: 11
                    color: Theme.c.text
                }
            }
        }
    }

    Loader {
        id: studioLoader
        anchors.fill: parent
        anchors.margins: 10
        active: body.visible && body.panelKind === "program" && !!Document.mainDeck.studio &&
            (body.panelCanvas === "" || Preview.canvasKey(body.panelCanvas) === Preview.canvasKey(""))
        sourceComponent: Component { StudioModeView {} }
    }

    // A panel this build has no body for. Every panel in the default layout has one now, so
    // this is reached only by a layout file naming something we do not know -- a layout saved by
    // a newer build, or one hand-edited. Asked of the Loader rather than written out as a list
    // of panel ids: the list would be the switch above copied, and the copy would be the one
    // that went stale.
    Column {
        visible: panelLoader.sourceComponent === null && body.panelId !== "" &&
                 body.panelKind !== "program"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        spacing: 6

        Text {
            text: Workspace.title(body.panelId)
            font.pixelSize: 13
            font.weight: Font.DemiBold
            color: Theme.c.text
        }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            color: Theme.c.textMuted
            text: qsTr("This build has nothing to draw in a panel named '%1'.")
                      .arg(body.panelId)
        }
    }

    // An empty group can exist for a moment during a drag; say so rather than draw nothing.
    Text {
        visible: body.panelId === ""
        anchors.centerIn: parent
        text: qsTr("empty")
        font.pixelSize: 11
        color: Theme.c.textMuted
        opacity: 0.5
    }
}
