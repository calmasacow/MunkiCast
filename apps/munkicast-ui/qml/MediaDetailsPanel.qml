// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// What the selected file actually is.
Item {
    id: panel

    readonly property var item: Media.selectedItem

    Text {
        anchors.centerIn: parent
        visible: !panel.item.path
        text: qsTr("Select a file to see its details")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 10
        spacing: 2
        visible: !!panel.item.path

        Text {
            width: parent.width
            text: panel.item.name ?? ""
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: Theme.c.text
            elide: Text.ElideMiddle
            bottomPadding: 6
        }

        StatRow { width: parent.width; label: qsTr("Folder"); value: panel.item.folder ?? "" }
        StatRow { width: parent.width; label: qsTr("Size"); value: panel.item.size ?? "" }
        StatRow { width: parent.width; label: qsTr("Modified"); value: panel.item.modified ?? "" }

        Item { width: 1; height: 8 }

        // Until the prober has reached this file there is nothing true to say about its
        // streams, so the section says that rather than showing six empty rows.
        Text {
            width: parent.width
            visible: !panel.item.probed
            text: qsTr("Reading the file…")
            font.pixelSize: 11
            color: Theme.c.textMuted
        }

        Column {
            width: parent.width
            spacing: 2
            visible: !!panel.item.probed

            StatRow { width: parent.width; label: qsTr("Duration"); value: panel.item.duration ?? "" }
            StatRow { width: parent.width; label: qsTr("Resolution"); value: panel.item.resolution ?? "" }
            StatRow { width: parent.width; label: qsTr("Frame rate"); value: panel.item.frameRate ?? "" }
            StatRow { width: parent.width; label: qsTr("Video"); value: panel.item.codec ?? "" }
            StatRow {
                width: parent.width
                label: qsTr("Audio")
                value: panel.item.audioCodec ? panel.item.audioCodec : qsTr("none")
            }
        }

        Item { width: 1; height: 10 }

        Text {
            width: parent.width
            text: panel.item.path ?? ""
            wrapMode: Text.WrapAnywhere
            font.pixelSize: 10
            font.family: "monospace"
            color: Theme.c.textMuted
        }
    }
}
