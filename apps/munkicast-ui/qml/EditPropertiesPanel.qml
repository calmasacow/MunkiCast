// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The selected clip.
Item {
    id: panel

    readonly property var clip: Edit.selectedClipDetails

    Text {
        anchors.centerIn: parent
        visible: !panel.clip.id
        text: qsTr("Select a clip to see its properties")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 10
        spacing: 2
        visible: !!panel.clip.id

        Text {
            width: parent.width
            text: panel.clip.name ?? ""
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: Theme.c.text
            elide: Text.ElideMiddle
            bottomPadding: 6
        }

        StatRow { width: parent.width; label: qsTr("Track"); value: panel.clip.track ?? "" }
        StatRow {
            width: parent.width
            label: qsTr("Start")
            value: Edit.timecode(panel.clip.start ?? 0)
        }
        StatRow {
            width: parent.width
            label: qsTr("Length")
            value: Edit.timecode(panel.clip.duration ?? 0)
        }
        StatRow {
            width: parent.width
            label: qsTr("Source in")
            value: Edit.timecode(panel.clip.sourceIn ?? 0)
        }

        Item { width: 1; height: 10 }

        NumberField {
            width: parent.width
            label: qsTr("Gain")
            value: panel.clip.gainDb ?? 0
            suffix: " dB"
            decimals: 1
            minimum: -60
            maximum: 12
            steppers: true
            // One undo step per committed value, not per keystroke: the field commits on
            // Enter and on losing focus, which is when the operator has decided.
            onCommitted: newValue => Edit.setClipGain(newValue)
        }

        Item { width: 1; height: 10 }

        Text {
            width: parent.width
            text: panel.clip.source ?? ""
            wrapMode: Text.WrapAnywhere
            font.pixelSize: 10
            font.family: "monospace"
            color: Theme.c.textMuted
        }
    }
}
