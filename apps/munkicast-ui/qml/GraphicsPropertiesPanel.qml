// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The selected layer.
Item {
    id: panel

    // Not `layer`: every Item already has a FINAL `layer` grouped property, and shadowing it
    // makes the whole component unavailable.
    readonly property var current: Graphics.selectedLayerDetails

    Text {
        anchors.centerIn: parent
        visible: !panel.current.id
        text: qsTr("Select a layer to see its properties")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 10
        spacing: 6
        visible: !!panel.current.id

        TextLine {
            width: parent.width
            label: qsTr("Name")
            value: panel.current.name ?? ""
            onCommitted: newValue => Graphics.setLayerProperty("name", newValue)
        }

        TextLine {
            width: parent.width
            visible: panel.current.kind === "text"
            label: qsTr("Text")
            value: panel.current.text ?? ""
            onCommitted: newValue => Graphics.setLayerProperty("text", newValue)
        }

        TextLine {
            width: parent.width
            visible: panel.current.kind === "text"
            label: qsTr("Field")
            value: panel.current.dataField ?? ""
            placeholder: qsTr("none")
            onCommitted: newValue => Graphics.setLayerProperty("dataField", newValue)
        }

        NumberField {
            width: parent.width
            visible: panel.current.kind === "text"
            label: qsTr("Size")
            value: panel.current.fontSize ?? 48
            decimals: 0
            minimum: 1
            maximum: 512
            steppers: true
            onCommitted: newValue => Graphics.setLayerProperty("fontSize", newValue)
        }

        ColourField {
            width: parent.width
            label: qsTr("Colour")
            value: panel.current.color ?? "#ffffff"
            onCommitted: newValue => Graphics.setLayerProperty("color", newValue)
        }

        Row {
            spacing: 12

            CheckLabel {
                text: qsTr("Visible")
                checked: panel.current.visible ?? true
                onToggled: Graphics.setLayerProperty("visible", !panel.current.visible)
            }

            CheckLabel {
                text: qsTr("Locked")
                checked: panel.current.locked ?? false
                onToggled: Graphics.setLayerProperty("locked", !panel.current.locked)
            }
        }

        Item { width: 1; height: 4 }

        // Which channels are animated, so a designer can see at a glance what this layer does
        // over time without reading the timeline row.
        Text {
            width: parent.width
            text: {
                const channels = panel.current.channels ?? {};
                const animated = [];
                for (const name in channels) {
                    if (channels[name].animated)
                        animated.push(name);
                }
                return animated.length === 0
                    ? qsTr("Not animated")
                    : qsTr("Animated: %1").arg(animated.join(", "));
            }
            font.pixelSize: 10
            color: Theme.c.textMuted
            wrapMode: Text.WordWrap
        }
    }
}
