// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// One row of a source's settings panel, drawn from the field descriptor the document handed
/// over (`Document.sourceSettingsSchema`).
///
/// The schema decides what the control is, so adding a setting to a source type is a line in a
/// table in C++ and nothing here. The alternative -- a hand-built panel per type -- is ten
/// panels to keep in step with one list of keys, and the one that goes stale is always the type
/// nobody is currently using.
Item {
    id: row

    required property var field
    property int labelWidth: 150
    /// The pending value, which is the dialog's rather than the document's: nothing is
    /// committed until the dialog is accepted.
    property var currentValue

    signal committed(var newValue)
    signal browseRequested

    implicitHeight: loader.implicitHeight

    Loader {
        id: loader

        width: row.width
        sourceComponent: {
            switch (row.field.kind) {
            case "note":   return noteRow;
            case "bool":   return boolRow;
            case "int":    return intRow;
            case "real":   return intRow;
            case "choice": return choiceRow;
            case "colour": return colourRow;
            case "multiline": return multilineRow;
            default:       return textRow;
            }
        }
    }

    // A note is prose, not a control: it sits in the field column so it reads as belonging to
    // the field under it rather than to the panel as a whole.
    Component {
        id: noteRow

        Item {
            implicitHeight: note.implicitHeight + 10

            Text {
                id: note

                x: row.labelWidth + 4
                width: Math.max(40, parent.width - x)
                y: 6
                text: row.field.label
                font.pixelSize: 10
                color: Theme.c.textMuted
                wrapMode: Text.WordWrap
                opacity: 0.9
            }
        }
    }

    Component {
        id: boolRow

        Item {
            implicitHeight: 20

            CheckLabel {
                objectName: "settingInput_" + row.field.key
                x: row.labelWidth + 4
                width: Math.max(0, parent.width - x)
                height: parent.height
                anchors.verticalCenter: parent.verticalCenter
                text: row.field.label
                checked: row.currentValue === true
                onToggled: row.committed(!(row.currentValue === true))
            }
        }
    }

    Component {
        id: intRow

        NumberField {
            objectName: "settingInput_" + row.field.key
            label: row.field.label
            labelWidth: row.labelWidth
            labelAlign: Text.AlignRight
            suffix: row.field.suffix
            decimals: row.field.kind === "real" ? 3 : 0
            minimum: row.field.minimum === undefined ? -1e100 : row.field.minimum
            maximum: row.field.maximum === undefined ? 1e100 : row.field.maximum
            value: row.currentValue === undefined ? 0 : Number(row.currentValue)
            onCommitted: newValue => row.committed(row.field.kind === "real" ? newValue : Math.round(newValue))
        }
    }

    Component {
        id: choiceRow

        ChoiceField {
            objectName: "settingInput_" + row.field.key
            label: row.field.label
            labelWidth: row.labelWidth
            labelAlign: Text.AlignRight
            choices: row.field.choices
            choiceLabels: row.field.choiceLabels || ({})
            value: row.currentValue === undefined ? "" : String(row.currentValue)
            onCommitted: newValue => row.committed(newValue)
        }
    }

    Component {
        id: colourRow

        ColourField {
            label: row.field.label
            labelWidth: row.labelWidth
            labelAlign: Text.AlignRight
            value: row.currentValue === undefined ? "#000000" : String(row.currentValue)
            onCommitted: newValue => row.committed(newValue)
        }
    }

    Component {
        id: multilineRow
        Item {
            implicitHeight: 100
            Text {
                width: row.labelWidth - 4
                text: row.field.label
                color: Theme.c.textMuted
                font.pixelSize: 11
                horizontalAlignment: Text.AlignRight
                wrapMode: Text.WordWrap
            }
            Rectangle {
                x: row.labelWidth + 4
                width: Math.max(40, parent.width - x)
                height: parent.height - 4
                color: "transparent"
                border.color: Theme.c.textMuted
                clip: true
                TextEdit {
                    anchors.fill: parent
                    anchors.margins: 4
                    text: row.currentValue === undefined ? "" : String(row.currentValue)
                    color: Theme.c.text
                    font.pixelSize: 12
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    textFormat: TextEdit.PlainText
                    Accessible.name: row.field.label
                    onTextChanged: if (activeFocus) row.committed(text)
                }
            }
        }
    }

    Component {
        id: textRow

        TextLine {
            label: row.field.label
            labelWidth: row.labelWidth
            labelAlign: Text.AlignRight
            browsable: row.field.kind === "path"
            placeholder: row.field.kind === "path" ? qsTr("no file chosen") : ""
            value: row.currentValue === undefined ? "" : String(row.currentValue)
            onCommitted: newValue => row.committed(newValue)
            onBrowseRequested: row.browseRequested()
        }
    }
}
