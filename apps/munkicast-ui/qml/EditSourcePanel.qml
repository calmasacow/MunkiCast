// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import QtMultimedia
import MunkiCast

/// The Source viewer: a file being looked at before any of it is used.
///
/// Distinct from the Program viewer, which shows the sequence. This one shows one file, with
/// its own playhead and its own in and out marks, and nothing done here touches the document
/// until the marked range is inserted. That separation is what three-point editing is: decide
/// which part of the file you want here, decide where it goes there.
Item {
    id: panel

    readonly property var src: Edit.source
    readonly property bool hasSource: !!panel.src.path
    readonly property real seconds: panel.src.seconds || 0

    /// Where the viewer's own playhead sits, in seconds into the file. Not the sequence's.
    /// Follows the player always -- playing, shuttling or parked -- except while the scrub bar
    /// is held, where the hand is the authority and the player catches up.
    property real position: 0

    /// The file now in the player. Compared by path rather than on every `src` change,
    /// because marking in or out changes `src` too and must not reload the file.
    property string loadedPath: ""

    /// Which halves a drag out of the picture carries. Both, until told otherwise.
    property bool dragVideo: true
    property bool dragAudio: true

    onSrcChanged: panel.load()
    // Built again on every return to the tab: the file is still open in Edit, and the
    // fresh player has to be given it.
    Component.onCompleted: panel.load()

    function load() {
        const path = panel.src.path || "";
        if (path === panel.loadedPath)
            return;
        panel.loadedPath = path;
        shuttle.stop();
        player.stop();
        panel.position = 0;
        player.source = path ? "file://" + path : "";
    }

    function frameOf(secs) {
        const frames = panel.src.frames || 0;
        if (!(panel.seconds > 0) || frames <= 0)
            return 0;
        return Math.round(secs / panel.seconds * frames);
    }

    /// A jump that keeps whatever is running running, from the new place.
    function seekTo(secs) {
        panel.position = Math.max(0, Math.min(secs, panel.seconds));
        shuttle.seek(panel.position * 1000);
    }

    Shuttle {
        id: shuttle
        objectName: "sourceShuttle"
        player: player
        output: output
    }

    TransportKeys {
        panelId: "edit-source"
        onReverse: shuttle.reverse()
        onStop: shuttle.stop()
        onForward: shuttle.forward()
        onToggle: shuttle.toggle()
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
    }

    // A clip dragged out of the Media Pool lands here, which is how one is auditioned
    // before any of it is used.
    DropArea {
        objectName: "sourceDrop"
        anchors.fill: parent
        keys: ["text/uri-list"]
        // The panel a clip is being carried into is the one being worked in: focused the
        // moment the drag crosses its edge, so its border says where the drop will go and
        // J-K-L land here once it has.
        onEntered: Workspace.focusPanel("edit-source")

        onDropped: (drop) => {
            // From the pool the drag is internal, and the file is on the ghost that carried
            // it; from outside the application it is only in the data.
            const carried = drop.source && drop.source.path ? drop.source.path : "";
            const uri = carried ? "" : drop.getDataAsString("text/uri-list");
            if (carried)
                Edit.openSource(carried);
            else if (uri.startsWith("file://"))
                Edit.openSource(decodeURIComponent(uri.split(/\r?\n/)[0].substring(7)));
            drop.accept();
        }

        Rectangle {
            anchors.fill: parent
            visible: parent.containsDrag
            color: "transparent"
            border.width: 2
            border.color: Theme.c.accent
            opacity: 0.8
        }
    }

    MediaPlayer {
        id: player

        videoOutput: output
        audioOutput: AudioOutput {}
        onPositionChanged: {
            if (!scrubMouse.pressed)
                panel.position = player.position / 1000;
        }
        // A freshly set source draws nothing until it is played: the old picture stayed up and
        // a new clip looked like no clip. Pausing a loaded file decodes and shows its first
        // frame, which is the refresh that says the drop took.
        onMediaStatusChanged: {
            if (player.mediaStatus === MediaPlayer.LoadedMedia
                    && player.playbackState === MediaPlayer.StoppedState) {
                player.pause();
                player.position = 0;
            }
        }
    }

    /// Dragging out of the picture is how a marked range reaches the timeline. The three
    /// buttons below pick which halves go; dragging the picture itself takes both, which is
    /// what an editor means by dragging a shot.
    Item {
        id: dragOut

        objectName: "sourceDragOut"

        // Sized, not anchored: an anchored item cannot be moved, and a drag is the item
        // moving. At rest it sits exactly over `output`, and goes back there on release.
        x: output.x
        y: output.y
        width: output.width
        height: output.height
        z: 5
        visible: panel.hasSource

        Drag.active: dragMouse.drag.active
        // Where the press was, inside the picture. The drop lands at the hot spot, so it
        // has to be the point under the pointer; at the item's corner a press in the
        // middle of the picture dropped half a viewer up and left of the cursor -- usually
        // off the timeline altogether, so letting go did nothing.
        property point grabbed: Qt.point(0, 0)
        Drag.hotSpot.x: dragOut.grabbed.x
        Drag.hotSpot.y: dragOut.grabbed.y
        // DropArea matches these, not the mime formats, for an internal drag.
        Drag.keys: ["application/x-munkicast-source"]
        Drag.mimeData: ({"application/x-munkicast-source": panel.src.path})
        Drag.dragType: Drag.Internal
        Drag.onActiveChanged: {
            Edit.setDragCursor(dragOut.Drag.active);
            if (dragOut.Drag.active)
                outline.home = output.mapToItem(outline.parent, output.contentRect.x,
                                                output.contentRect.y);
        }

        MouseArea {
            id: dragMouse

            anchors.fill: parent
            drag.target: dragOut
            // A click still scrubs nothing and a drag still starts: the threshold keeps a
            // careless press from becoming an edit.
            drag.threshold: 8
            // Not smoothed: smoothing holds the item back by the threshold so it does not jump,
            // which left the drop point that far behind the pointer -- in the ruler when the
            // pointer was on the top track, so the drop found no track.
            drag.smoothed: false
            onPressed: (mouse) => {
                Workspace.focusPanel("edit-source");
                dragOut.grabbed = Qt.point(mouse.x, mouse.y);
                Edit.beginSourceDrag(panel.dragVideo, panel.dragAudio);
            }
            onReleased: {
                if (dragOut.Drag.active)
                    dragOut.Drag.drop();
                else
                    Edit.cancelSourceDrag();
                dragOut.x = Qt.binding(() => output.x);
                dragOut.y = Qt.binding(() => output.y);
            }
        }
    }

    // What is being carried: the picture's outline, following the pointer until it lets go.
    // On the window, not in the panel: the panel frame clips, and the outline vanished at
    // its edge -- exactly where it starts to matter.
    Rectangle {
        id: outline
        objectName: "sourceDragOutline"
        parent: panel.Window.contentItem
        z: 95
        visible: dragOut.Drag.active
        /// Where the picture sits on the window, taken when the drag starts. The drag only
        /// moves `dragOut`, so its offset from rest is all the outline has to add.
        property point home: Qt.point(0, 0)
        x: home.x + dragOut.x - output.x
        y: home.y + dragOut.y - output.y
        width: output.contentRect.width
        height: output.contentRect.height
        color: "transparent"
        border.width: 2
        border.color: Theme.c.accent
    }

    // Which file this is. The picture alone cannot say when two takes look alike, and a
    // drop that replaced one with the other otherwise changed nothing anyone could see.
    Text {
        id: fileName

        objectName: "sourceName"
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.top: parent.top
        anchors.topMargin: 5
        visible: panel.hasSource
        text: panel.src.name || ""
        font.pixelSize: 11
        color: Theme.c.text
        elide: Text.ElideMiddle
    }

    VideoOutput {
        id: output
        objectName: "sourceOutput"

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: fileName.bottom
        anchors.bottom: scrub.top
        anchors.margins: 8
        anchors.topMargin: 4
        fillMode: VideoOutput.PreserveAspectFit
        visible: panel.hasSource
    }

    Text {
        anchors.centerIn: output
        visible: !panel.hasSource
        text: qsTr("Double-click a recording in the Media Browser to open it here")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    // The scrub bar, with the marked range shaded on it. An editor judges a range by looking
    // at where it sits in the whole file, so the marks belong on the bar rather than only in
    // a pair of numbers.
    Item {
        id: scrub

        objectName: "sourceScrub"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: transport.top
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.bottomMargin: 6
        height: 18
        visible: panel.hasSource

        readonly property real headX: panel.seconds > 0
            ? Math.max(0, Math.min(track.width * (panel.position / panel.seconds), track.width)) : 0

        Rectangle {
            id: track
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 4
            radius: 2
            color: Theme.c.panelHeader
        }

        Rectangle {
            readonly property real frames: panel.src.frames || 0
            visible: frames > 0
            anchors.verticalCenter: parent.verticalCenter
            x: frames > 0 ? track.width * (panel.src.in / frames) : 0
            width: frames > 0 ? Math.max(1, track.width * ((panel.src.out - panel.src.in) / frames)) : 0
            height: 4
            radius: 2
            color: Theme.c.accent
            opacity: 0.55
        }

        // The handle: big enough to take hold of. The line it rides is all one target, so
        // a press anywhere on it jumps there and dragging scrubs -- playing or not.
        Rectangle {
            objectName: "sourcePlayhead"
            x: scrub.headX - width / 2
            anchors.verticalCenter: parent.verticalCenter
            width: 8
            height: parent.height
            radius: 2
            color: scrubMouse.pressed ? Theme.c.accent : Theme.c.text
        }

        MouseArea {
            id: scrubMouse
            anchors.fill: parent
            anchors.topMargin: -4
            anchors.bottomMargin: -4
            preventStealing: true
            cursorShape: Qt.PointingHandCursor
            onPressed: (mouse) => panel.seekTo(mouse.x / track.width * panel.seconds)
            onPositionChanged: (mouse) => {
                if (pressed)
                    panel.seekTo(mouse.x / track.width * panel.seconds);
            }
        }
    }

    Rectangle {
        id: transport

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        // Two rows: the deck's buttons on their own, centred, and the clock beside the
        // marking and drag tools under them. One row overlapped as soon as the bigger
        // transport met the right-hand tools in a panel of ordinary width.
        height: 66
        color: Theme.c.panelHeader

        Text {
            objectName: "sourceTimecode"
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: tools.verticalCenter
            text: panel.hasSource ? Edit.timecode(panel.frameOf(panel.position)) : "--:--:--:--"
            font.pixelSize: 11
            font.family: "monospace"
            color: Theme.c.text

            // The shuttle gear, while it is not plain play.
            Text {
                anchors.left: parent.right
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: shuttle.rate !== 0 && shuttle.rate !== 1
                text: (shuttle.rate < 0 ? "◀ " : "▶ ") + Math.abs(shuttle.rate) + "×"
                font.pixelSize: 11
                color: Theme.c.accent
            }
        }

        // |◀◀ ◀◀ ▶ ▶▶ ▶▶|, the order every deck has. Drawn from plain triangles: the ⏮ ⏭
        // code points fall back to the colour emoji font, which paints them as blue tiles.
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 4
            spacing: 6

            ToolButton {
                width: 34; height: 30; glyphSize: 18
                label: "|◀◀"
                tip: qsTr("Go to the start of the file")
                enabled: panel.hasSource
                onClicked: { shuttle.stop(); panel.seekTo(0); }
            }

            ToolButton {
                width: 34; height: 30; glyphSize: 18
                label: "◀◀"
                tip: qsTr("Rewind — again for faster (J)")
                enabled: panel.hasSource
                onClicked: shuttle.reverse()
            }

            ToolButton {
                width: 34; height: 30; glyphSize: 18
                label: shuttle.rate !== 0 ? "❚❚" : "▶"
                tip: qsTr("Play or stop (space)")
                enabled: panel.hasSource
                onClicked: shuttle.toggle()
            }

            ToolButton {
                width: 34; height: 30; glyphSize: 18
                label: "▶▶"
                tip: qsTr("Fast forward — again for faster (L)")
                enabled: panel.hasSource
                onClicked: shuttle.forward()
            }

            ToolButton {
                width: 34; height: 30; glyphSize: 18
                label: "▶▶|"
                tip: qsTr("Go to the end of the file")
                enabled: panel.hasSource
                onClicked: { shuttle.stop(); panel.seekTo(panel.seconds); }
            }
        }

        Row {
            id: tools
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 5
            spacing: 4

            ToolButton {
                label: "{"
                tip: qsTr("Mark in on the source")
                enabled: panel.hasSource
                onClicked: Edit.setSourceIn(panel.frameOf(panel.position))
            }

            ToolButton {
                label: "}"
                tip: qsTr("Mark out on the source")
                enabled: panel.hasSource
                onClicked: Edit.setSourceOut(panel.frameOf(panel.position))
            }

            ToolButton {
                label: "✕"
                tip: qsTr("Clear the marked range")
                enabled: panel.hasSource
                onClicked: Edit.clearSourceRange()
            }

            // The two ways a marked range reaches the sequence. Insert pushes what is there
            // along; overwrite replaces it. Every NLE has both, and which one is wanted is
            // never guessable from the gesture.
            // Which halves a drag out of the picture carries. A mode rather than a gesture:
            // every source monitor has these three, because laying picture over an existing
            // soundtrack is an ordinary edit, and the choice has to survive the press that
            // starts the drag.
            ToolButton {
                label: "V"
                tip: qsTr("Drag video only")
                enabled: panel.hasSource
                active: panel.dragVideo && !panel.dragAudio
                onClicked: { panel.dragVideo = true; panel.dragAudio = false; }
            }

            ToolButton {
                label: "A"
                tip: qsTr("Drag audio only")
                enabled: panel.hasSource
                active: panel.dragAudio && !panel.dragVideo
                onClicked: { panel.dragVideo = false; panel.dragAudio = true; }
            }

            ToolButton {
                label: "V+A"
                tip: qsTr("Drag video and audio")
                enabled: panel.hasSource
                active: panel.dragVideo && panel.dragAudio
                onClicked: { panel.dragVideo = true; panel.dragAudio = true; }
            }

            ToolButton {
                label: "⇥"
                tip: qsTr("Insert the marked range at the playhead")
                enabled: panel.hasSource && Edit.hasTimeline
                onClicked: Edit.insertSourceRange(false)
            }

            ToolButton {
                label: "⇥̶"
                tip: qsTr("Overwrite at the playhead with the marked range")
                enabled: panel.hasSource && Edit.hasTimeline
                onClicked: Edit.insertSourceRange(true)
            }
        }
    }
}
