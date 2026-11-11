// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as Controls
import MunkiCast

Item {
    id: row
    required property var field
    signal committed(var value)
    signal browseRequested()
    implicitHeight: field.visible === false ? 0 : 30
    visible: field.visible !== false
    Loader {
        anchors.fill: parent
        sourceComponent: row.field.kind === "slider" ? sliderRow : ordinaryRow
    }
    Component {
        id: ordinaryRow
        SettingRow {
            field: row.field
            labelWidth: 145
            currentValue: row.field.value
            onCommitted: value => row.committed(value)
            onBrowseRequested: row.browseRequested()
        }
    }
    Component {
        id: sliderRow
        Item {
            Text {
                width: 141; anchors.verticalCenter: parent.verticalCenter
                text: row.field.label; color: Theme.c.textMuted
                font.pixelSize: 11; horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
            }
            Controls.Slider {
                id: slider
                objectName: "filterSlider_" + row.field.key
                height: 28
                x: 149; width: Math.max(40, parent.width - x - 112)
                anchors.verticalCenter: parent.verticalCenter
                from: row.field.minimum; to: row.field.maximum
                value: Number(row.field.value)
                stepSize: Math.pow(10, -(row.field.decimals === undefined ? 3 : row.field.decimals))
                Accessible.name: row.field.label
                onPressedChanged: if (!pressed && value !== Number(row.field.value)) row.committed(value)
                Keys.onReleased: event => {
                    if ([Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down, Qt.Key_Home, Qt.Key_End].indexOf(event.key) >= 0 && value !== Number(row.field.value)) row.committed(value);
                }
                background: Rectangle {
                    x: slider.leftPadding; y: slider.topPadding + slider.availableHeight / 2 - 2
                    width: slider.availableWidth; height: 4; radius: 2; color: Theme.c.border
                    Rectangle { width: slider.visualPosition * parent.width; height: parent.height; radius: 2; color: Theme.c.accent }
                }
                handle: Rectangle {
                    x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
                    y: slider.topPadding + slider.availableHeight / 2 - height / 2
                    width: 12; height: 12; radius: 6; color: Theme.c.text
                    border.color: slider.activeFocus ? Theme.c.accent : Theme.c.border
                }
            }
            NumberField {
                objectName: "settingInput_" + row.field.key
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: 104; label: ""; labelWidth: 0
                value: slider.pressed ? slider.value : Number(row.field.value)
                minimum: row.field.minimum; maximum: row.field.maximum
                decimals: row.field.decimals === undefined ? 3 : row.field.decimals; suffix: row.field.suffix || ""
                onCommitted: value => row.committed(value)
            }
        }
    }
}
