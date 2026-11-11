// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// What is in the project, and what is wrong with it.
///
/// This was called "Stats", which it never was: it counts scenes and sources and lists the
/// document's problems. Broadcast statistics -- frame times, dropped frames, encoder and network
/// health -- are a different panel with a different job, and are not shown here even partially.
Item {
    id: panel

    Flickable {
        anchors.fill: parent
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
                text: qsTr("Document")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                bottomPadding: 4
            }

            StatRow {
                width: parent.width
                label: qsTr("Scenes")
                value: Document.scenes.rowCount()
            }

            StatRow {
                width: parent.width
                label: qsTr("Sources here")
                value: Document.nodes.rowCount()
            }

            StatRow {
                width: parent.width
                label: qsTr("Inputs")
                value: Document.sources.rowCount()
            }

            StatRow {
                width: parent.width
                label: qsTr("Edits")
                value: Document.revision
            }

            StatRow {
                width: parent.width
                label: qsTr("Saved")
                value: Document.dirty ? qsTr("no") : qsTr("yes")
                warn: Document.dirty
            }

            Text {
                width: parent.width
                text: qsTr("Problems")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 12
                bottomPadding: 4
            }

            Text {
                width: parent.width
                visible: Document.problems.length === 0
                text: qsTr("None.")
                font.pixelSize: 11
                color: Theme.c.textMuted
            }

            Repeater {
                model: Document.problems

                delegate: Text {
                    required property string modelData

                    width: body.width
                    text: modelData
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    color: modelData.startsWith("error") ? Theme.c.error : Theme.c.warning
                    bottomPadding: 3
                }
            }

        }
    }
}
