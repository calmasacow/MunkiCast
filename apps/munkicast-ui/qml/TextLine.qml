// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// A one-line text field, the sibling of NumberField and ChoiceField.
///
/// Commits when editing finishes rather than on every keystroke, for the same reason the
/// numeric one does: a command per character would be four hundred undo steps for one URL.
/// Escape puts the stored value back, so a half-typed path can always be abandoned.
///
/// `browsable` adds the Browse button a path wants. The field itself opens nothing -- it asks,
/// and whoever owns it decides what a file chooser means there.
Item {
    id: field

    required property string label
    property int labelWidth: 54
    property int labelAlign: Text.AlignLeft
    property string value: ""
    property string placeholder: ""
    property bool browsable: false
    property bool commitOnEdit: false
    property int echoMode: TextInput.Normal

    signal committed(string newValue)
    signal browseRequested
    signal editingFinished
    function clear() { input.clear(); input.dirty = false; }

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
        id: browse

        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        visible: field.browsable
        width: visible ? 62 : 0
        height: 18
        radius: 2
        color: browseMouse.pressed ? Theme.c.accent
             : browseMouse.containsMouse ? Theme.c.panelHeaderActive
             : "transparent"
        border.width: 1
        border.color: browseMouse.containsMouse ? Theme.c.accent : Theme.c.border

        Text {
            anchors.centerIn: parent
            text: qsTr("Browse")
            font.pixelSize: 10
            color: browseMouse.pressed ? Theme.c.window : Theme.c.text
        }

        MouseArea {
            id: browseMouse

            anchors.fill: parent
            hoverEnabled: true
            onClicked: field.browseRequested()
        }
    }

    Rectangle {
        anchors.left: caption.right
        anchors.leftMargin: 4
        anchors.right: browse.left
        anchors.rightMargin: field.browsable ? 6 : 0
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
            anchors.fill: parent
            anchors.leftMargin: 5
            anchors.rightMargin: 5
            verticalAlignment: Text.AlignVCenter
            visible: input.text === "" && !input.activeFocus && field.placeholder !== ""
            text: field.placeholder
            font.pixelSize: 11
            color: Theme.c.textMuted
            opacity: 0.6
            elide: Text.ElideRight
        }

        TextInput {
            echoMode: field.echoMode
            id: input
            objectName: "textLineInput"
            activeFocusOnTab: true

            anchors.fill: parent
            anchors.leftMargin: 5
            anchors.rightMargin: 5
            verticalAlignment: TextInput.AlignVCenter
            font.pixelSize: 11
            color: field.enabled ? Theme.c.text : Theme.c.textMuted
            selectionColor: Theme.c.accent
            selectByMouse: true
            clip: true

            // Only follow the stored value while nothing is being typed, or a half-written
            // path is replaced the moment anything else in the document changes.
            text: activeFocus ? text : field.value

            property bool dirty: false
            property string draft: ""

            // A half-typed draft is only ever committed over the value it was started from. If the
            // stored value moves underneath -- a collection switch, an undo, a remote command --
            // the draft is incomplete input about a value that no longer exists, so it is dropped
            // and the field shows what is actually stored. Keeping it would write a stale edit over
            // a known-good value on the next focus change.
            property string base: ""
            onDirtyChanged: if (dirty) base = field.value
            Connections {
                target: field
                function onValueChanged() {
                    if (!input.dirty || field.value === input.base) return;
                    input.dirty = false;
                    input.text = field.value;
                }
            }
            function finishEdit() {
                if (!dirty) return;
                const value = draft;
                dirty = false;
                if (value !== field.value) field.committed(value);
                field.editingFinished();
            }
            onEditingFinished: finishEdit()
            onTextChanged: {
                if (activeFocus) { draft = text; dirty = true; }
            }
            onActiveFocusChanged: if (!activeFocus) finishEdit()
            onTextEdited: {
                draft = text;
                dirty = true;
                if (field.commitOnEdit) field.committed(text);
            }
            Keys.onEscapePressed: {
                text = field.value;
                dirty = false;
                focus = false;
            }
        }
    }
}
