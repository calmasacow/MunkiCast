// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// How far a cache build has got: one bar for the file being copied, one for the whole build,
/// and the name of the file under them.
///
/// Two bars rather than one because they answer different questions. A show of 27 assets where
/// one is 384 MiB spends a quarter of the build on that one file, and a single overall bar
/// looks stuck there. The per-item bar is what shows it is still moving.
///
/// Opens itself when a build starts, from wherever it was started -- the menu, the state
/// dialog, Preferences -- so no caller has to remember to show it.
Item {
    id: progress

    anchors.fill: parent
    visible: AssetCache.building
    focus: visible

    // Cancel is a request, not a stop: the file in flight is abandoned at its next chunk and
    // its .part removed. Escape does the same thing as the button, because a modal overlay
    // that cannot be dismissed by Escape is one the operator fights.
    Keys.onEscapePressed: AssetCache.cancel()

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        // Deliberately swallowed without dismissing. A click outside cannot cancel 31 seconds
        // of copying by accident, and the overlay has nothing behind it worth clicking.
        onClicked: {}
        onWheel: (wheel) => { wheel.accepted = true; }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
        opacity: 0.55
    }

    Rectangle {
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        objectName: "cacheProgressFrame"
        DialogGeometry { resizable: false }
        width: Math.min(parent.width - 80, 420)
        height: body.height + bar.height + 28
        color: Theme.c.panelBackground
        border.width: 1
        border.color: Theme.c.accent
        radius: 3

        component Meter: Rectangle {
            required property double fraction

            height: 6
            radius: 2
            color: Theme.c.window
            border.width: 1
            border.color: Theme.c.border

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.margins: 1
                width: Math.max(0, (parent.width - 2) * Math.min(1, Math.max(0, parent.fraction)))
                radius: 1
                color: Theme.c.accent
            }
        }

        Column {
            id: body

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 14
            spacing: 8

            Text {
                width: parent.width
                text: qsTr("Preparing show assets")
                font.pixelSize: 12
                font.bold: true
                color: Theme.c.text
            }

            Text {
                objectName: "cacheProgressItem"
                width: parent.width
                text: AssetCache.currentItem !== "" ? AssetCache.currentItem
                                                    : qsTr("Starting...")
                font.pixelSize: 11
                color: Theme.c.textMuted
                elide: Text.ElideMiddle
            }

            Meter {
                objectName: "cacheItemMeter"
                width: parent.width
                fraction: AssetCache.itemProgress
            }

            Text {
                width: parent.width
                text: qsTr("%1 of %2 files").arg(AssetCache.itemsDone).arg(AssetCache.itemsTotal)
                font.pixelSize: 11
                color: Theme.c.textMuted
            }

            Meter {
                objectName: "cacheOverallMeter"
                width: parent.width
                fraction: AssetCache.overallProgress
            }
        }

        Row {
            id: bar

            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 14

            DialogButton {
                objectName: "cancelCacheBuild"
                text: qsTr("Cancel")
                onClicked: AssetCache.cancel()
            }
        }
    }
}
