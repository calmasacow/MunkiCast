// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// A numeric field that commits when editing finishes, not while it is being typed.
///
/// Deliberately not a slider. Every committed edit is one undoable command, and a control that
/// emits a value per pixel of travel would put two hundred of them on the undo stack for one
/// drag. Continuous controls need the session to grow a way to coalesce a run of commands into
/// one step; until it has that, the honest control is one that commits once.
Item {
    id: field

    required property string label
    property real value: 0
    property int decimals: 2
    property real minimum: -1e100
    property real maximum: 1e100
    /// The caption column. Wider in a dialog than in a docked panel, where 54 is all there is
    /// to give it, so it is a property rather than the constant it used to be.
    property int labelWidth: 54
    property int labelAlign: Text.AlignLeft
    /// A unit printed inside the box, after the number. Never a value.
    property string suffix: ""
    /// Up/down arrows inside the box, for a field whose value is nudged as often as it is typed
    /// -- the four parts of a countdown's period, which is what a spin box is for. Off by
    /// default, so every field that was here before this looks exactly as it did.
    property bool steppers: false
    /// What one press of an arrow is worth.
    property real step: 1

    signal committed(real newValue)

    /// Clamped and rounded here rather than in each caller, so an arrow can never commit a value
    /// the typed path would have refused.
    function nudge(by) {
        const next = Math.min(field.maximum, Math.max(field.minimum, field.value + by));
        const rounded = field.decimals === 0 ? Math.round(next)
                                             : parseFloat(next.toFixed(field.decimals));
        if (rounded !== field.value)
            field.committed(rounded);
    }

    implicitHeight: 20

    Text {
        id: caption

        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: field.labelWidth
        horizontalAlignment: field.labelAlign
        text: field.label
        font.pixelSize: 11
        color: Theme.c.textMuted
        elide: Text.ElideRight
    }

    Rectangle {
        anchors.left: caption.right
        anchors.leftMargin: 4
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 18
        radius: 2
        color: field.enabled ? Theme.c.window : "transparent"
        border.width: 1
        border.color: input.activeFocus ? Theme.c.accent
                    : hover.containsMouse ? Theme.c.border
                    : "transparent"

        MouseArea {
            id: hover

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: field.enabled ? Qt.IBeamCursor : Qt.ArrowCursor
            acceptedButtons: Qt.NoButton
        }

        Text {
            id: unit

            anchors.right: steps.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            visible: field.suffix !== ""
            text: field.suffix
            font.pixelSize: 10
            color: Theme.c.textMuted
        }

        Column {
            id: steps

            anchors.right: parent.right
            anchors.rightMargin: 1
            anchors.verticalCenter: parent.verticalCenter
            visible: field.steppers && field.enabled
            width: visible ? 13 : 0

            Repeater {
                model: [true, false]

                delegate: Rectangle {
                    id: arrow

                    required property bool modelData

                    width: steps.width
                    height: 8
                    color: arrowMouse.pressed ? Theme.c.accent
                         : arrowMouse.containsMouse ? Theme.c.panelHeaderActive
                         : "transparent"
                    radius: 1

                    Text {
                        anchors.centerIn: parent
                        text: arrow.modelData ? "▴" : "▾"
                        font.pixelSize: 8
                        color: arrowMouse.pressed ? Theme.c.window : Theme.c.textMuted
                    }

                    MouseArea {
                        id: arrowMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        // Held down, it repeats: setting fifty-five seconds one press at a time
                        // is not a thing to ask of anyone.
                        onPressed: field.nudge(arrow.modelData ? field.step : -field.step)
                        onReleased: repeat.stop()
                        onCanceled: repeat.stop()
                        onPressAndHold: repeat.start()
                    }

                    Timer {
                        id: repeat

                        interval: 60
                        repeat: true
                        onTriggered: field.nudge(arrow.modelData ? field.step : -field.step)
                    }
                }
            }
        }

        TextInput {
            id: input
            objectName: "numberInput"
            activeFocusOnTab: true
            onActiveFocusChanged: if (activeFocus) selectAll()
            // Up/Down nudge by `step`, Page Up/Down by ten steps.
            Keys.onUpPressed: field.nudge(field.step)
            Keys.onDownPressed: field.nudge(-field.step)
            Keys.onPressed: event => {
                if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
                    field.nudge((event.key === Qt.Key_PageUp ? 10 : -10) * field.step);
                    event.accepted = true;
                }
            }

            anchors.fill: parent
            anchors.leftMargin: 5
            anchors.rightMargin: (unit.visible ? unit.width + 10 : 5) + steps.width
            verticalAlignment: TextInput.AlignVCenter
            font.pixelSize: 11
            color: field.enabled ? Theme.c.text : Theme.c.textMuted
            selectionColor: Theme.c.accent
            selectByMouse: true
            validator: DoubleValidator {
                bottom: field.minimum
                top: field.maximum
                decimals: field.decimals === 0 ? 0 : 1000
            }

            // Only follow the document while the operator is not typing, or their half-finished
            // number gets replaced the moment anything else changes.
            text: activeFocus ? text : field.value.toFixed(field.decimals)

            onEditingFinished: {
                const parsed = parseFloat(text);
                if (acceptableInput && !isNaN(parsed) && parsed !== field.value)
                    field.committed(parsed);
                else
                    text = field.value.toFixed(field.decimals);
            }
            Keys.onEscapePressed: {
                text = field.value.toFixed(field.decimals);
                focus = false;
            }
        }
    }
}
