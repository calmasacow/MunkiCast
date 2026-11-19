// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import MunkiCast

/// The media pool: the bins a project's media is organised into, and what is in them.
///
/// Two kinds of bin, and the difference is worth knowing. Clips, Recordings and Graphics are
/// *system* bins -- each is a window onto the project folder of that name, so a file copied
/// in there by any means at all is simply in the pool, with no import step. A bin the
/// operator makes holds references to files wherever they are, which is what makes it an
/// arrangement rather than a second copy of the disk.
Item {
    id: panel

    /// Showing a folder's files rather than the folders themselves.
    // Held by Media, not here: the panel is rebuilt when the Edit tab is left, and a
    // flag of its own came back false, dropping the operator out of the bin they were in.
    readonly property bool inFolder: Media.openBin !== ""
    /// Tiles or rows. A grid reads faster for pictures, a list for names and lengths.
    property bool grid: true

    /// The one drag in flight, driven by whichever row was pressed.
    ///
    /// A property rather than a component per row: Qt drags an item by moving it, so the
    /// thing being dragged has to be a real item with somewhere to go.
    readonly property var poolDragState: QtObject {
        property string path: ""
        property point from: Qt.point(0, 0)
        property bool active: false
        /// True from the moment a drag passes the threshold until the click that follows the
        /// release has been swallowed, so a drag does not also open the file.
        property bool wasDrag: false

        function press(filePath, label, at, from, local) {
            poolDragState.path = filePath;
            poolDragState.from = at;
            poolDragState.active = false;
            poolDragState.wasDrag = false;
            poolDrag.label = label;
            poolDrag.path = filePath;
            poolDrag.itemSize = Qt.size(from.width, from.height);
            poolDrag.grabbed = local;
        }

        function move(at) {
            if (poolDragState.path === "")
                return;
            if (!poolDragState.active) {
                const dx = at.x - poolDragState.from.x;
                const dy = at.y - poolDragState.from.y;
                if (dx * dx + dy * dy < 36)
                    return; // inside the threshold: still a click
                poolDragState.active = true;
                poolDragState.wasDrag = true;
                poolDrag.dragging = true;
            }
            // The ghost lives on the window, so it is not clipped to the pool.
            const p = panel.mapToItem(poolDrag.parent, at.x, at.y);
            poolDrag.x = p.x - poolDrag.grabbed.x;
            poolDrag.y = p.y - poolDrag.grabbed.y;
        }

        function release() {
            if (poolDragState.active) {
                poolDrag.Drag.drop();
                poolDrag.dragging = false;
                poolDragState.active = false;
            }
            poolDragState.path = "";
        }
    }

    /// Open the pool's menu. `bin` is the one under the pointer, or null for empty space.
    function showMenu(from, ev, bin) {
        const at = from.mapToItem(poolMenu, ev.x, ev.y);
        const rows = [
            {text: qsTr("New Bin…"), action: function() { newBin.open(); }},
            {text: qsTr("Import Media…"),
             enabled: Media.hasProject,
             action: function() { importer.open(bin ? bin.id : Media.openBin); }}
        ];
        if (bin && !bin.system) {
            rows.push({separator: true});
            rows.push({text: qsTr("Rename “%1”…").arg(bin.name),
                       action: function() { renameBin.open(bin.id, bin.name); }});
            rows.push({text: qsTr("Remove “%1”").arg(bin.name),
                       action: function() { Media.removeBin(bin.id); }});
        } else if (bin && bin.system) {
            rows.push({separator: true});
            // Said rather than silently left out: an operator who right-clicks Recordings
            // and finds no Rename should know it is deliberate.
            rows.push({text: qsTr("“%1” is one of the project's own folders").arg(bin.name),
                       enabled: false});
        }
        poolMenu.popup(at.x, at.y, rows);
    }

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
            onClicked: Media.closeBin()
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
                model: Media.bins

                delegate: Item {
                    id: tile

                    objectName: "binTile"
                    required property var modelData

                    width: 84
                    height: 76

                    Rectangle {
                        anchors.fill: parent
                        radius: 3
                        color: tileMouse.containsMouse ? Theme.c.panelHeaderActive
                                                       : "transparent"
                    }

                    // A bin, drawn rather than shipped as an icon: two rectangles say "bin"
                    // as well as any picture and follow the theme for free. A system bin is
                    // drawn in the accent because it is the project's own folder; one the
                    // operator made is plainer.
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
                            color: tile.modelData.system ? Theme.c.accent : Theme.c.textMuted
                        }

                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 28
                            radius: 2
                            color: tile.modelData.system ? Theme.c.accent : Theme.c.textMuted
                            opacity: tile.modelData.current ? 0.95 : 0.7
                            border.width: tile.modelData.current ? 1 : 0
                            border.color: Theme.c.text
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
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        // Not an arrow function: `this` inside one is not the MouseArea, so
                        // the menu could not work out where it had been opened and never
                        // appeared.
                        onClicked: function(ev) {
                            if (ev.button === Qt.RightButton) {
                                panel.showMenu(tileMouse, ev, tile.modelData);
                                return;
                            }
                            Media.openBinById(tile.modelData.id);
                        }
                    }

                    ToolTipText {
                        text: tile.modelData.system
                              ? qsTr("%1 — the project's own folder; anything copied into it appears here")
                                    .arg(tile.modelData.name)
                              : qsTr("%1 — right-click to rename or remove")
                                    .arg(tile.modelData.name)
                        visible: tileMouse.containsMouse
                        hoverArea: tileMouse
                    }
                }
            }
        }

        // Right-clicking the empty space below the bins is the other way to reach the menu,
        // and the only one when a project has no bins at all.
    }

    // Right-clicking anywhere in the pool opens its menu, not only within a few pixels of a
    // bin. This sits over the whole panel rather than inside the Flow, which is only as tall
    // as its rows -- so every empty pixel below the last row was dead, which is most of the
    // panel. The bins take their own right-clicks first; this one gets the rest.
    MouseArea {
        id: emptyMouse

        objectName: "poolEmptySpace"

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: bar.bottom
        anchors.bottom: timelinesHeader.top
        z: -1
        acceptedButtons: Qt.RightButton
        onClicked: function(ev) { panel.showMenu(emptyMouse, ev, null); }
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
            // Not flicked: a flick takes the press a drag needs (ADR-0039). The wheel
            // comes back through NoDragScroll.
            interactive: false
            cellWidth: 96
            cellHeight: 76
            boundsBehavior: Flickable.StopAtBounds

            NoDragScroll {}

            delegate: Item {
                id: gtile

                objectName: "poolFileRow"

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

                    // Kept from the panel stack's own gestures once a drag is under way.
                    preventStealing: true

                    anchors.fill: parent
                    hoverEnabled: true
                    // Dragged onto the Source viewer, which is how a clip is auditioned
                    // before any of it is used. The payload is the file; the Source viewer
                    // is what decides which part of it goes anywhere.
                    // The drag is run by the panel: one ghost, moved in panel
                    // coordinates. A ghost per row was tried and cannot work -- Qt drags an
                    // item by moving it, and a one-pixel item inside a delegate has nowhere
                    // to be moved to.
                    onPressed: (ev) => poolDragState.press(gtile.path, gtile.name,
                                                      gridMouse.mapToItem(panel, ev.x, ev.y),
                                                      gridMouse, Qt.point(ev.x, ev.y))
                    onPositionChanged: (ev) => poolDragState.move(
                        gridMouse.mapToItem(panel, ev.x, ev.y))
                    onReleased: poolDragState.release()
                    onClicked: { if (!poolDragState.wasDrag) Edit.openSource(gtile.path); }
                    // Into the Source viewer, not onto the timeline. A clip is looked at
                    // and marked up before any of it is used; appending the whole of it
                    // because someone double-clicked is an edit they did not ask for.
                    onDoubleClicked: Edit.openSource(gtile.path)
                }

                ToolTipText {
                    text: qsTr("Open in the Source viewer, or drag it there")
                    visible: gridMouse.containsMouse && !poolDragState.active
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
            interactive: false
            boundsBehavior: Flickable.StopAtBounds

            NoDragScroll {}

            delegate: Item {
                id: file

                objectName: "poolFileRow"
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

                    // Kept from the panel stack's own gestures once a drag is under way.
                    preventStealing: true

                    anchors.fill: parent
                    hoverEnabled: true
                    onPressed: (ev) => poolDragState.press(file.path, file.name,
                                                      fileMouse.mapToItem(panel, ev.x, ev.y),
                                                      fileMouse, Qt.point(ev.x, ev.y))
                    onPositionChanged: (ev) => poolDragState.move(
                        fileMouse.mapToItem(panel, ev.x, ev.y))
                    onReleased: poolDragState.release()
                    onClicked: { if (!poolDragState.wasDrag) Edit.openSource(file.path); }
                    onDoubleClicked: Edit.openSource(file.path)
                }

                ToolTipText {
                    text: qsTr("Open in the Source viewer, or drag it there")
                    visible: fileMouse.containsMouse && !poolDragState.active
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
            objectName: "newTimelineButton"
            onClicked: newTimeline.open()
        }
    }

    TimelineDialog {
        id: newTimeline
        parent: panel.Window.contentItem
    }

    PoolDragGhost {
        id: poolDrag

        parent: panel.Window.contentItem

        objectName: "poolDragGhost"
    }

    StripMenu {
        id: poolMenu

        objectName: "poolMenu"
        parent: panel.Window.contentItem
        z: 40
    }

    /// Asking for one line of text. Small enough to live here rather than be a component:
    /// naming a bin is the only thing it does.
    Item {
        id: newBin

        objectName: "newBinPrompt"
        anchors.fill: parent
        z: 30
        visible: false

        property string value: ""

        function open() {
            newBin.value = "";
            newBin.visible = true;
            newBinField.forceActiveFocus();
        }

        function accept() {
            const made = Media.createBin(newBin.value);
            if (made !== "") {
                newBin.visible = false;
                Media.openBinById(made);
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "#000000"
            opacity: 0.4
            MouseArea { anchors.fill: parent; onClicked: newBin.visible = false }
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 24, 300)
            height: 96
            radius: 3
            color: Theme.c.panelBackground
            border.color: Theme.c.accent
            MouseArea { anchors.fill: parent; onClicked: {} }

            Text {
                id: newBinTitle
                anchors { left: parent.left; top: parent.top; margins: 12 }
                text: qsTr("New Bin")
                font { pixelSize: 12; bold: true }
                color: Theme.c.text
            }

            TextLine {
                id: newBinField
                objectName: "newBinField"
                anchors { left: parent.left; right: parent.right; top: newBinTitle.bottom
                          margins: 12 }
                labelWidth: 50
                label: qsTr("Name")
                value: newBin.value
                commitOnEdit: true
                onCommitted: newValue => newBin.value = newValue
            }

            Row {
                anchors { right: parent.right; bottom: parent.bottom; margins: 10 }
                spacing: 6
                DialogButton { text: qsTr("Cancel"); onClicked: newBin.visible = false }
                DialogButton {
                    objectName: "newBinAccept"
                    text: qsTr("Create")
                    primary: true
                    enabled: newBin.value.trim() !== ""
                    onClicked: newBin.accept()
                }
            }
        }
    }

    Item {
        id: renameBin

        anchors.fill: parent
        z: 30
        visible: false

        property string binId: ""
        property string value: ""

        function open(id, name) {
            renameBin.binId = id;
            renameBin.value = name;
            renameBin.visible = true;
            renameField.forceActiveFocus();
        }

        Rectangle {
            anchors.fill: parent
            color: "#000000"
            opacity: 0.4
            MouseArea { anchors.fill: parent; onClicked: renameBin.visible = false }
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 24, 300)
            height: 96
            radius: 3
            color: Theme.c.panelBackground
            border.color: Theme.c.accent
            MouseArea { anchors.fill: parent; onClicked: {} }

            Text {
                id: renameTitle
                anchors { left: parent.left; top: parent.top; margins: 12 }
                text: qsTr("Rename Bin")
                font { pixelSize: 12; bold: true }
                color: Theme.c.text
            }

            TextLine {
                id: renameField
                anchors { left: parent.left; right: parent.right; top: renameTitle.bottom
                          margins: 12 }
                labelWidth: 50
                label: qsTr("Name")
                value: renameBin.value
                commitOnEdit: true
                onCommitted: newValue => renameBin.value = newValue
            }

            Row {
                anchors { right: parent.right; bottom: parent.bottom; margins: 10 }
                spacing: 6
                DialogButton { text: qsTr("Cancel"); onClicked: renameBin.visible = false }
                DialogButton {
                    text: qsTr("Rename")
                    primary: true
                    enabled: renameBin.value.trim() !== ""
                    onClicked: {
                        if (Media.renameBin(renameBin.binId, renameBin.value))
                            renameBin.visible = false;
                    }
                }
            }
        }
    }

    MediaImportDialog { id: importer }

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
