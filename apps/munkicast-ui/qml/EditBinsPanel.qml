// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The Media Browser: the project's own folders, the files in them, and its timelines.
///
/// The folders are the Maya-style tree the project was created with, so this panel is a view
/// of the disk rather than a second catalogue to keep in step with it. Going into a folder
/// replaces the tiles with its files; the up arrow comes back.
Item {
    id: panel

    /// Showing a folder's files rather than the folders themselves.
    property bool inFolder: false
    /// Tiles or rows. A grid reads faster for pictures, a list for names and lengths.
    property bool grid: true

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
    }

    // ---- the bar: up, search, count, view toggle ----
    Rectangle {
        id: bar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 26
        color: Theme.c.panelHeader

        ToolButton {
            id: up

            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            label: "↑"
            tip: qsTr("Back to the project's folders")
            enabled: panel.inFolder
            onClicked: panel.inFolder = false
        }

        Rectangle {
            id: searchBox

            anchors.left: up.right
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(60, bar.width - 150)
            height: 18
            radius: 2
            color: Theme.c.window
            border.color: search.activeFocus ? Theme.c.accent : Theme.c.border

            TextInput {
                id: search

                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                verticalAlignment: TextInput.AlignVCenter
                font.pixelSize: 10
                color: Theme.c.text
                selectByMouse: true
                onTextChanged: Media.filter = text

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: search.text === ""
                    text: qsTr("Search")
                    font.pixelSize: 10
                    color: Theme.c.textMuted
                }
            }
        }

        Text {
            id: countLabel

            anchors.right: viewToggle.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: panel.inFolder ? qsTr("%1 items").arg(Media.count) : ""
            font.pixelSize: 10
            color: Theme.c.textMuted
        }

        ToolButton {
            id: viewToggle

            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            label: panel.grid ? "☰" : "▦"
            tip: panel.grid ? qsTr("Show as a list") : qsTr("Show as tiles")
            onClicked: panel.grid = !panel.grid
        }
    }

    // ---- the folders ----
    Flickable {
        id: folders

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: bar.bottom
        anchors.bottom: timelinesHeader.top
        visible: !panel.inFolder
        clip: true
        contentHeight: folderFlow.height
        boundsBehavior: Flickable.StopAtBounds

        NoDragScroll {}

        Flow {
            id: folderFlow

            width: folders.width
            padding: 10
            spacing: 10

            Repeater {
                model: Media.projectFolders

                delegate: Item {
                    id: tile

                    required property var modelData

                    width: 84
                    height: 76

                    Rectangle {
                        anchors.fill: parent
                        radius: 3
                        color: tileMouse.containsMouse ? Theme.c.panelHeaderActive
                                                       : "transparent"
                    }

                    // A folder, drawn rather than shipped as an icon: two rectangles say
                    // "folder" as well as any picture and follow the theme for free.
                    Item {
                        id: glyph

                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        anchors.topMargin: 8
                        width: 44
                        height: 34

                        Rectangle {
                            width: 18
                            height: 6
                            radius: 1
                            color: tile.modelData.exists ? Theme.c.accent : Theme.c.border
                        }

                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 28
                            radius: 2
                            color: tile.modelData.exists ? Theme.c.accent : Theme.c.border
                            opacity: tile.modelData.exists ? 0.75 : 0.4
                        }
                    }

                    Text {
                        anchors.top: glyph.bottom
                        anchors.topMargin: 4
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: parent.width - 4
                        horizontalAlignment: Text.AlignHCenter
                        text: tile.modelData.name
                        font.pixelSize: 10
                        color: Theme.c.text
                        elide: Text.ElideMiddle
                    }

                    Text {
                        anchors.bottom: parent.bottom
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("%1 items").arg(tile.modelData.items)
                        font.pixelSize: 9
                        color: Theme.c.textMuted
                    }

                    MouseArea {
                        id: tileMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            Media.openFolder(tile.modelData.path);
                            panel.inFolder = true;
                        }
                    }
                }
            }
        }
    }

    // ---- a folder's files ----
    Loader {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: bar.bottom
        anchors.bottom: timelinesHeader.top
        visible: panel.inFolder
        active: panel.inFolder
        sourceComponent: panel.grid ? gridView : listView
    }

    Component {
        id: gridView

        GridView {
            clip: true
            model: Media
            cellWidth: 96
            cellHeight: 76
            boundsBehavior: Flickable.StopAtBounds

            NoDragScroll {}

            delegate: Item {
                id: gtile

                required property int index
                required property string path
                required property string name
                required property string duration

                width: 96
                height: 76

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 3
                    radius: 2
                    color: gridMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                    border.color: Theme.c.border
                }

                // The clip's own first frames, which is what makes a grid worth having over
                // a list. Empty until the artwork has been built.
                Image {
                    id: poster

                    anchors.top: parent.top
                    anchors.topMargin: 6
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 84
                    height: 40
                    fillMode: Image.PreserveAspectCrop
                    cache: false
                    source: Edit.clipArt(gtile.path, false)
                }

                Rectangle {
                    anchors.fill: poster
                    visible: poster.status !== Image.Ready
                    color: Theme.c.panelBackground
                }

                Text {
                    anchors.top: poster.bottom
                    anchors.topMargin: 3
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width - 10
                    horizontalAlignment: Text.AlignHCenter
                    text: gtile.name
                    font.pixelSize: 9
                    color: Theme.c.text
                    elide: Text.ElideMiddle
                }

                Text {
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: gtile.duration
                    font.pixelSize: 9
                    font.family: "monospace"
                    color: Theme.c.textMuted
                }

                MouseArea {
                    id: gridMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: Edit.openSource(gtile.path)
                    onDoubleClicked: {
                        if (!Edit.hasTimeline)
                            Edit.createTimeline(qsTr("Timeline 1"));
                        Edit.appendToFirstTrack(gtile.path);
                    }
                }

                ToolTipText {
                    text: qsTr("Click to open in the Source viewer, double-click to append")
                    visible: gridMouse.containsMouse
                    hoverArea: gridMouse
                }
            }
        }
    }

    Component {
        id: listView

        ListView {
            clip: true
            model: Media
            boundsBehavior: Flickable.StopAtBounds

            NoDragScroll {}

            delegate: Item {
                id: file

                required property int index
                required property string path
                required property string name
                required property string duration

                width: ListView.view.width
                height: 22

                Rectangle {
                    anchors.fill: parent
                    color: fileMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.right: length.left
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: file.name
                    font.pixelSize: 11
                    color: Theme.c.text
                    elide: Text.ElideMiddle
                }

                Text {
                    id: length

                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: file.duration
                    font.pixelSize: 10
                    font.family: "monospace"
                    color: Theme.c.textMuted
                }

                MouseArea {
                    id: fileMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: Edit.openSource(file.path)
                    // Double-click appends, rather than drag-and-drop: dropping onto a
                    // Canvas-drawn timeline would need its own hit testing, and appending is
                    // the gesture an assembly edit is actually made of.
                    onDoubleClicked: {
                        if (!Edit.hasTimeline)
                            Edit.createTimeline(qsTr("Timeline 1"));
                        Edit.appendToFirstTrack(file.path);
                    }
                }

                ToolTipText {
                    text: qsTr("Click to open in the Source viewer, double-click to append")
                    visible: fileMouse.containsMouse
                    hoverArea: fileMouse
                }
            }
        }
    }

    // ---- the project's timelines, always in view ----
    Rectangle {
        id: timelinesHeader

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: timelines.top
        height: 22
        color: Theme.c.panelHeader

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Sequences")
            font.pixelSize: 10
            color: Theme.c.textMuted
        }

        ToolButton {
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            label: "+"
            tip: qsTr("New timeline")
            onClicked: Edit.createTimeline(qsTr("Timeline %1").arg(Edit.timelines.length + 1))
        }
    }

    ListView {
        id: timelines

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Math.min(contentHeight, panel.height * 0.3)
        clip: true
        model: Edit.timelines
        boundsBehavior: Flickable.StopAtBounds

        NoDragScroll {}

        delegate: Item {
            id: row

            required property var modelData

            width: ListView.view.width
            height: 22

            Rectangle {
                anchors.fill: parent
                color: row.modelData.open ? Theme.c.accent
                     : mouse.containsMouse ? Theme.c.panelHeaderActive
                     : "transparent"
                opacity: row.modelData.open ? 0.22 : 1.0
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.name
                font.pixelSize: 11
                color: Theme.c.text
                elide: Text.ElideRight
            }

            MouseArea {
                id: mouse

                anchors.fill: parent
                hoverEnabled: true
                onClicked: Edit.openTimeline(row.modelData.id)
            }
        }
    }
}
