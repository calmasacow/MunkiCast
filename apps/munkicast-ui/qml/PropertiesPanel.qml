// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

Item {
    id: panel

    readonly property var props: Document.nodeProperties
    /// With several selected this panel shows nothing (ADR-0037). Every field here edits one
    /// node, and showing the leader's values while three are selected invites an edit that
    /// looks like it applies to all of them and does not.
    readonly property int selectedCount: Document.selectedNodes.length
    /// Text draws at exactly the size of its laid-out glyphs, so a fit mode and a box have
    /// nothing to act on: Natural is the only answer and an empty box is the only box. The
    /// controls are hidden rather than disabled, because a row that can never be used is
    /// clutter, not information. Where the type sits is Anchor, which is on the source's own
    /// settings page as well as here.
    readonly property bool boxable: panel.hasNode &&
        panel.props.sourceType !== "source.text"
    readonly property bool hasNode: Object.keys(panel.props).length > 0 &&
                                    panel.selectedCount <= 1

    Text {
        anchors.centerIn: parent
        visible: !panel.hasNode
        text: panel.selectedCount > 1
              ? qsTr("%n sources selected").arg(panel.selectedCount)
              : qsTr("No source selected")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    // Said under the count, because "why has my inspector gone" is the question this panel
    // would otherwise leave unanswered.
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.verticalCenter
        anchors.topMargin: 16
        visible: panel.selectedCount > 1
        text: qsTr("Pick one to edit its properties.")
        font.pixelSize: 10
        color: Theme.c.textMuted
        opacity: 0.7
    }

    Flickable {
        anchors.fill: parent
        visible: panel.hasNode
        contentHeight: body.implicitHeight + 20
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
        // because Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

        Column {
            id: body

            x: 10
            y: 10
            width: parent.width - 20
            spacing: 2

            Text {
                width: parent.width
                text: panel.hasNode ? panel.props.name : ""
                font.pixelSize: 13
                font.weight: Font.DemiBold
                color: Theme.c.text
                elide: Text.ElideRight
            }

            Text {
                width: parent.width
                text: panel.hasNode ? panel.props.kind : ""
                font.pixelSize: 10
                color: Theme.c.textMuted
                bottomPadding: 8
            }

            // --- Transform. Separate from Layout on purpose (ADR-0021): where it sits, versus
            // how the image fills its box. Conflating them is why OBS bounding boxes confuse.
            Text {
                width: parent.width
                text: qsTr("Transform")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 4
                bottomPadding: 4
            }

            NumberField {
                width: parent.width
                label: qsTr("X")
                value: panel.hasNode ? panel.props.positionX : 0
                onCommitted: newValue => Document.setNodePosition(newValue, panel.props.positionY)
            }

            NumberField {
                width: parent.width
                label: qsTr("Y")
                value: panel.hasNode ? panel.props.positionY : 0
                onCommitted: newValue => Document.setNodePosition(panel.props.positionX, newValue)
            }

            // Which point of the node the X and Y above locate. Two dropdowns rather than nine
            // buttons: the horizontal and the vertical are independent choices, and a grid of
            // nine would still have to say which cell means what.
            ChoiceField {
                width: parent.width
                objectName: "anchorH"
                label: qsTr("Anchor H")
                choices: ["0", "0.5", "1"]
                choiceLabels: ({"0": qsTr("Left"), "0.5": qsTr("Centre"), "1": qsTr("Right")})
                value: panel.hasNode ? String(panel.props.anchorX) : "0"
                onCommitted: newValue =>
                    Document.setNodeAnchor(Number(newValue), panel.props.anchorY)
            }

            ChoiceField {
                width: parent.width
                objectName: "anchorV"
                label: qsTr("Anchor V")
                choices: ["0", "0.5", "1"]
                choiceLabels: ({"0": qsTr("Top"), "0.5": qsTr("Middle"), "1": qsTr("Bottom")})
                value: panel.hasNode ? String(panel.props.anchorY) : "0"
                onCommitted: newValue =>
                    Document.setNodeAnchor(panel.props.anchorX, Number(newValue))
            }

            NumberField {
                width: parent.width
                label: qsTr("Scale X")
                decimals: 3
                value: panel.hasNode ? panel.props.scaleX : 1
                onCommitted: newValue => Document.setNodeScale(newValue, panel.props.scaleY)
            }

            NumberField {
                width: parent.width
                label: qsTr("Scale Y")
                decimals: 3
                value: panel.hasNode ? panel.props.scaleY : 1
                onCommitted: newValue => Document.setNodeScale(panel.props.scaleX, newValue)
            }

            NumberField {
                width: parent.width
                label: qsTr("Rotation")
                decimals: 1
                value: panel.hasNode ? panel.props.rotation : 0
                onCommitted: newValue => Document.setNodeRotation(newValue)
            }

            // --- Layout. Separate from Transform, and the separation is the point: where the
            // box sits versus how the content fills it. Conflating them is why OBS bounding
            // boxes confuse people -- scaling a source and changing its fit mode look like the
            // same operation until they disagree.
            Text {
                width: parent.width
                text: qsTr("Layout")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 10
                bottomPadding: 4
            }

            ChoiceField {
                width: parent.width
                visible: panel.boxable
                label: qsTr("Fit")
                choices: Document.fitModes
                value: panel.hasNode ? panel.props.fit : ""
                onCommitted: newValue => Document.setNodeFit(newValue)
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                visible: panel.boxable && panel.props.fit === "natural"
                text: qsTr("Natural size: the source is drawn at its own dimensions and placed in the box by the alignment below. A box of 0 means the source's own size.")
                font.pixelSize: 9
                color: Theme.c.textMuted
                topPadding: 2
                bottomPadding: 2
            }

            NumberField {
                width: parent.width
                visible: panel.boxable
                label: qsTr("Box W")
                decimals: 0
                // Enabled for Natural as well. Natural no longer means "no box": it draws the
                // content at its own size and aligns it inside whatever box there is, so a box
                // is how a caption gets room to sit against an edge. Zero still means "the
                // content's own size", which is the no-box case.
                enabled: panel.hasNode
                value: panel.hasNode ? panel.props.boxWidth : 0
                onCommitted: newValue => Document.setNodeBoxSize(newValue, panel.props.boxHeight)
            }

            NumberField {
                width: parent.width
                visible: panel.boxable
                label: qsTr("Box H")
                decimals: 0
                enabled: panel.hasNode
                value: panel.hasNode ? panel.props.boxHeight : 0
                onCommitted: newValue => Document.setNodeBoxSize(panel.props.boxWidth, newValue)
            }

            // Which edge of the content meets which edge of the box: left to left, centre to
            // centre, right to right. Only does anything when the box is bigger than the
            // content, which is what the note above says.
            ChoiceField {
                width: parent.width
                objectName: "alignH"
                visible: panel.boxable
                label: qsTr("Align H")
                choices: ["0", "0.5", "1"]
                choiceLabels: ({"0": qsTr("Left"), "0.5": qsTr("Centre"), "1": qsTr("Right")})
                value: panel.hasNode ? String(panel.props.alignX) : "0.5"
                onCommitted: newValue =>
                    Document.setNodeAlignment(Number(newValue), panel.props.alignY)
            }

            ChoiceField {
                width: parent.width
                objectName: "alignV"
                visible: panel.boxable
                label: qsTr("Align V")
                choices: ["0", "0.5", "1"]
                choiceLabels: ({"0": qsTr("Top"), "0.5": qsTr("Middle"), "1": qsTr("Bottom")})
                value: panel.hasNode ? String(panel.props.alignY) : "0.5"
                onCommitted: newValue =>
                    Document.setNodeAlignment(panel.props.alignX, Number(newValue))
            }

            NumberField {
                width: parent.width
                label: qsTr("Crop L")
                decimals: 0
                value: panel.hasNode ? panel.props.cropLeft : 0
                onCommitted: newValue => Document.setNodeCrop(newValue, panel.props.cropTop,
                                                              panel.props.cropRight,
                                                              panel.props.cropBottom)
            }

            NumberField {
                width: parent.width
                label: qsTr("Crop T")
                decimals: 0
                value: panel.hasNode ? panel.props.cropTop : 0
                onCommitted: newValue => Document.setNodeCrop(panel.props.cropLeft, newValue,
                                                              panel.props.cropRight,
                                                              panel.props.cropBottom)
            }

            NumberField {
                width: parent.width
                label: qsTr("Crop R")
                decimals: 0
                value: panel.hasNode ? panel.props.cropRight : 0
                onCommitted: newValue => Document.setNodeCrop(panel.props.cropLeft,
                                                              panel.props.cropTop, newValue,
                                                              panel.props.cropBottom)
            }

            NumberField {
                width: parent.width
                label: qsTr("Crop B")
                decimals: 0
                value: panel.hasNode ? panel.props.cropBottom : 0
                onCommitted: newValue => Document.setNodeCrop(panel.props.cropLeft,
                                                              panel.props.cropTop,
                                                              panel.props.cropRight, newValue)
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                visible: panel.hasNode &&
                         (panel.props.cropLeft > 0 || panel.props.cropTop > 0 ||
                          panel.props.cropRight > 0 || panel.props.cropBottom > 0)
                text: qsTr("Crop is in source pixels and is taken off before fitting.")
                font.pixelSize: 9
                color: Theme.c.textMuted
                topPadding: 2
            }

            // --- Blending
            Text {
                width: parent.width
                text: qsTr("Blending")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 10
                bottomPadding: 4
            }

            NumberField {
                width: parent.width
                label: qsTr("Opacity")
                decimals: 3
                value: panel.hasNode ? panel.props.opacity : 1
                onCommitted: newValue => Document.setNodeOpacity(newValue)
            }

            ChoiceField {
                width: parent.width
                label: qsTr("Mode")
                objectName: "blendModeField"
                choices: Document.blendModes
                choiceLabels: ({ "normal": qsTr("Normal"), "linear-dodge": qsTr("Add"),
                                 "subtract": qsTr("Subtract"), "screen": qsTr("Screen"),
                                 "multiply": qsTr("Multiply"), "lighten": qsTr("Lighten"),
                                 "darken": qsTr("Darken") })
                value: panel.hasNode ? panel.props.blendMode : ""
                onCommitted: newValue => Document.setNodeBlendMode(newValue)
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                visible: panel.hasNode &&
                         ["hue", "saturation", "color", "luminosity"].indexOf(panel.props.blendMode) >= 0
                text: qsTr("This mode needs the backdrop, so the group containing it is flattened to an intermediate before compositing.")
                font.pixelSize: 9
                color: Theme.c.textMuted
                topPadding: 4
            }

            // --- State
            Text {
                width: parent.width
                text: qsTr("State")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 10
                bottomPadding: 4
            }

            Row {
                spacing: 14

                CheckLabel {
                    text: qsTr("Visible")
                    checked: panel.hasNode && panel.props.visible
                    onToggled: Document.setNodeVisible(Document.selectedNode,
                                                       !panel.props.visible)
                }

                CheckLabel {
                    text: qsTr("Locked")
                    checked: panel.hasNode && panel.props.locked
                    onToggled: Document.setNodeLocked(Document.selectedNode,
                                                      !panel.props.locked)
                }
            }

            Text {
                width: parent.width
                visible: panel.hasNode && panel.props.effectCount > 0
                text: qsTr("%1 effect(s)").arg(panel.hasNode ? panel.props.effectCount : 0)
                font.pixelSize: 10
                color: Theme.c.textMuted
                topPadding: 10
            }
        }
    }
}
