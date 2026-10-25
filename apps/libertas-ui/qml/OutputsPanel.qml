// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import Libertas

/// The outputs: one encode of one canvas going to one place.
///
/// Its own panel rather than a section of Controls. An operator building a show and an operator
/// running one want different things on screen -- destinations and bitrates are set up once and
/// then wanted out of the way, while program and preview are touched constantly. Keeping them
/// together meant one of the two was always in the way of the other.
///
/// Output transport is not connected yet; the footer says so. An
/// output is born disabled, because one that arrived enabled and pointed somewhere would be a
/// keystroke from going to a destination nobody chose.

Item {
    id: panel
    property string recordTarget: Document.recordingEncodeId()
    Connections {
        target: Document
        function onChanged() { panel.recordTarget = Document.recordingEncodeId(); }
    }

    StripMenu {
        id: outputMenu
        parent: panel.Window.contentItem
        z: 100
    }

    Flickable {
        id: scroll

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
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
                model: Document.outputs

                delegate: Item {
                    id: output

                    required property string encodeId
                    required property string outputId
                    required property string name
                    required property string canvasName
                    required property string encodeName
                    required property string resolution
                    required property var sharedWith
                    required property string codec
                    required property string implementation
                    required property int bitrateKbps
                    required property string destinationKind
                    required property string url
                    required property string credentialRef
                    required property bool isEnabled
                    required property bool misconfigured

                    objectName: "outputBlock"
                    width: content.width
                    height: outColumn.height + 8

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 4
                        radius: 2
                        color: "transparent"
                        border.width: 1
                        border.color: output.misconfigured ? Theme.c.error
                                    : Theme.c.border
                    }

                    Column {
                        id: outColumn

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: 8
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 3

                        Item {
                            width: parent.width
                            height: 32

                            MouseArea {
                                id: rowMouse
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onDoubleClicked: Workspace.openEncode(output.outputId)
                                onClicked: mouse => {
                                    if (mouse.button !== Qt.RightButton) return;
                                    const at = mapToItem(outputMenu, mouse.x, mouse.y);
                                    outputMenu.popup(at.x, at.y, [
                                        {text: qsTr("Settings"), action: () => { Workspace.openEncode(output.outputId); }},
                                        {text: qsTr("Include in output configuration"), checked: output.isEnabled,
                                         action: () => Document.setOutputEnabled(output.outputId, !output.isEnabled)},
                                        {separator: true},
                                        {text: qsTr("Remove output"), action: () => Document.removeOutput(output.outputId)}
                                    ]);
                                }
                            }

                            ToolButton {
                                id: badge
                                objectName: "outputIcon"
                                width: 36
                                height: 24
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                label: output.destinationKind === "file" ? "REC" : "LIVE"
                                tip: qsTr("Settings for %1").arg(output.name)
                                onClicked: Workspace.openEncode(output.outputId)
                            }
                            Row {
                                anchors.left: badge.right
                                anchors.leftMargin: 6
                                anchors.right: outputAction.left
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                Text {
                                    width: parent.width * 0.38
                                    text: output.name
                                    font.pixelSize: 11
                                    color: output.misconfigured ? Theme.c.error : Theme.c.text
                                    elide: Text.ElideRight
                                }
                                Text {
                                    width: parent.width * 0.29
                                    text: output.canvasName
                                    font.pixelSize: 11
                                    color: Theme.c.textMuted
                                    elide: Text.ElideRight
                                }
                                Text {
                                    width: parent.width * 0.33
                                    text: output.encodeName
                                    font.pixelSize: 11
                                    color: Theme.c.text
                                    elide: Text.ElideRight
                                }
                            }
                            ToolButton {
                                id: outputAction
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                width: 26
                                height: 26
                                label: output.destinationKind === "file"
                                     ? (Preview.recording ? "\u25a0" : "\u25cf") : "\u25b6"
                                enabled: output.destinationKind === "file" &&
                                         !Preview.recordingPending &&
                                         (Preview.recording || output.encodeId === panel.recordTarget)
                                tip: output.destinationKind === "file"
                                     ? (Preview.recording ? qsTr("Stop recording") : qsTr("Start recording"))
                                     : qsTr("Streaming is not connected yet")
                                onClicked: {
                                    if (Preview.recording) Preview.stopRecording(output.encodeId);
                                    else Preview.startRecording(output.encodeId,
                                        Document.recordingPathFor(output.encodeId));
                                }
                            }
                        }


                    }
                }
            }

            Repeater {
                model: [qsTr("Built-in backtrack"), qsTr("Built-in virtual camera")]
                delegate: Item {
                    required property string modelData
                    width: content.width
                    height: 38
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.right: pending.left
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData
                        color: Theme.c.textMuted
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                    Text {
                        id: pending
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Not available")
                        color: Theme.c.textMuted
                        font.pixelSize: 10
                    }
                }
            }

            Text {
                width: content.width
                visible: Document.outputs.rowCount() === 0
                padding: 10
                text: qsTr("No destinations. Use + to add an output.")
                font.pixelSize: 10
                color: Theme.c.textMuted
                wrapMode: Text.WordWrap
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
            id: footerButtons

            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            ToolButton {
                label: "+"
                tip: qsTr("Add an output")
                onClicked: Document.addOutput(qsTr("Output"))
            }

            ToolButton {
                objectName: "manageOutputs"
                label: "⚙"
                // Where reuse is made. A row answers "what does this destination use", one row
                // at a time; the manager is encode-centred and shows what the GPU is actually
                // being asked for (ADR-0029 amendment).
                tip: qsTr("Open Encoder Flow")
                onClicked: Workspace.openEncode()
            }

            ToolButton {
                objectName: "startAllOutputs"
                width: 72
                label: qsTr("Start All")
                enabled: false
                tip: qsTr("Output engine is not connected yet")
            }
            ToolButton {
                label: "\u25a0"
                enabled: false
                tip: qsTr("Stop all outputs - not available yet")
            }

        }

        // Beside the buttons and elided, not centred on the footer. Centring put it underneath
        // them the moment the panel was narrow enough -- which, docked in the right-hand
        // column, is always.
        Text {
            anchors.left: footerButtons.right
            anchors.leftMargin: 8
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            horizontalAlignment: Text.AlignRight
            text: Preview.recording ? qsTr("Recording") : qsTr("Idle")
            font.pixelSize: 9
            color: Theme.c.textMuted
            opacity: 0.6
            elide: Text.ElideRight
        }
    }
}
