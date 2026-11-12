// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import MunkiCast

/// The outputs: one encode of one canvas going to one place.
///
/// Its own panel rather than a section of Controls. An operator building a show and an operator
/// running one want different things on screen -- destinations and bitrates are set up once and
/// then wanted out of the way, while program and preview are touched constantly. Keeping them
/// together meant one of the two was always in the way of the other.
///
/// Recording and replay have row actions. The single supported live stream is controlled
/// from the broadcast toolbar; per-row streaming and Start All remain pending. Outputs are
/// born disabled so an unreviewed destination cannot be started accidentally.

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
                    required property string serviceIcon
                    required property string url
                    required property string credentialRef
                    required property bool isEnabled
                    required property bool misconfigured

                    objectName: "outputBlock"
                    width: content.width
                    height: 36

                    // One rule for every row: 8px gutter, 8px between columns, everything on
                    // the row's centre line. Column widths are fixed fractions so the three text
                    // columns line up across rows whatever the names are.
                    readonly property int gutter: 8
                    readonly property bool armable: output.destinationKind !== "file"
                                                    && output.destinationKind !== "replay"

                    Rectangle {
                        anchors.fill: parent
                        anchors.leftMargin: output.gutter
                        anchors.rightMargin: output.gutter
                        anchors.topMargin: 2
                        anchors.bottomMargin: 2
                        radius: 3
                        color: rowMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                        border.width: 1
                        border.color: output.misconfigured ? Theme.c.error : Theme.c.border
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
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

                    // The service icon, borderless; it opens the node in Encoder Flow.
                    Item {
                        id: badge
                        objectName: "outputIcon"
                        x: output.gutter * 2
                        width: 20
                        height: 20
                        anchors.verticalCenter: parent.verticalCenter
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Settings for %1").arg(output.name)
                        Accessible.onPressAction: Workspace.openEncode(output.outputId)
                        Keys.onReturnPressed: Workspace.openEncode(output.outputId)
                        Keys.onSpacePressed: Workspace.openEncode(output.outputId)

                        Image {
                            anchors.fill: parent
                            source: output.serviceIcon
                            sourceSize: Qt.size(40, 40)
                            fillMode: Image.PreserveAspectFit
                            opacity: badgeMouse.containsMouse || badge.activeFocus ? 1 : 0.9
                        }
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -2
                            radius: 3
                            color: "transparent"
                            border.width: 1
                            border.color: Theme.c.accent
                            visible: badge.activeFocus
                        }
                        MouseArea {
                            id: badgeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Workspace.openEncode(output.outputId)
                        }
                        ToolTipText {
                            text: qsTr("Settings for %1").arg(output.name)
                            visible: badgeMouse.containsMouse
                            hoverArea: badgeMouse
                        }
                    }

                    Row {
                        anchors.left: badge.right
                        anchors.leftMargin: output.gutter
                        anchors.right: arm.left
                        anchors.rightMargin: output.gutter
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: output.gutter

                        Text {
                            width: (parent.width - 2 * parent.spacing) * 0.4
                            text: output.name
                            font.pixelSize: 11
                            color: output.misconfigured ? Theme.c.error : Theme.c.text
                            elide: Text.ElideRight
                        }
                        Text {
                            width: (parent.width - 2 * parent.spacing) * 0.25
                            text: output.canvasName
                            font.pixelSize: 11
                            color: Theme.c.textMuted
                            elide: Text.ElideRight
                        }
                        Text {
                            width: (parent.width - 2 * parent.spacing) * 0.35
                            text: output.encodeName
                            font.pixelSize: 11
                            color: Theme.c.text
                            elide: Text.ElideRight
                        }
                    }

                    // Same toggle as the Encoder Flow node. Recording and Backtrack have no arm
                    // state here (their switches are the toolbar's Record and Replay), so the
                    // slot stays reserved to keep the columns aligned.
                    ArmSwitch {
                        id: arm
                        objectName: "outputArmSwitch"
                        anchors.right: parent.right
                        anchors.rightMargin: output.gutter * 2
                        anchors.verticalCenter: parent.verticalCenter
                        visible: output.armable
                        opacity: visible ? 1 : 0
                        checked: output.isEnabled
                        name: output.name
                        onToggled: Document.setOutputEnabled(output.outputId, !output.isEnabled)
                    }
                }
            }

            Repeater {
                model: [qsTr("Built-in virtual camera")]
                delegate: Item {
                    required property string modelData
                    width: content.width
                    height: 36
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 16
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
                        anchors.rightMargin: 16
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

            // Arms or disarms every streaming destination at once. Reads "Disable All" while
            // any is armed, so one press always makes the panel safe.
            ToolButton {
                objectName: "armAllOutputs"
                readonly property bool anyArmed: {
                    for (let i = 0; i < armedProbe.count; ++i)
                        if (armedProbe.itemAt(i) && armedProbe.itemAt(i).armed) return true;
                    return false;
                }
                width: 76
                label: anyArmed ? qsTr("Disable All") : qsTr("Enable All")
                enabled: armedProbe.count > 0
                tip: anyArmed ? qsTr("Disarm every destination") : qsTr("Arm every destination that has somewhere to send")
                onClicked: Document.setAllOutputsEnabled(!anyArmed)

                Repeater {
                    id: armedProbe
                    model: Document.outputs
                    delegate: Item {
                        required property string destinationKind
                        required property bool isEnabled
                        readonly property bool armed: isEnabled && destinationKind !== "file"
                                                      && destinationKind !== "replay"
                    }
                }
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
            text: [Preview.streaming ? qsTr("Live") : "",
                   Preview.recording ? qsTr("Recording") : "",
                   Preview.replayActive ? qsTr("Replay") : ""].filter(label => label !== "").join(" · ")
                  || qsTr("Idle")
            font.pixelSize: 9
            color: Theme.c.textMuted
            opacity: 0.6
            elide: Text.ElideRight
        }
    }
}
