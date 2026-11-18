// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
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
    property real position: 0

    /// Which halves a drag out of the picture carries. Both, until told otherwise.
    property bool dragVideo: true
    property bool dragAudio: true

    onSrcChanged: {
        const wanted = panel.hasSource ? "file://" + panel.src.path : "";
        if (player.source != wanted) {
            player.source = wanted;
            panel.position = 0;
        }
    }

    function frameOf(secs) {
        const frames = panel.src.frames || 0;
        if (!(panel.seconds > 0) || frames <= 0)
            return 0;
        return Math.round(secs / panel.seconds * frames);
    }

    function seekTo(secs) {
        panel.position = Math.max(0, Math.min(secs, panel.seconds));
        player.position = panel.position * 1000;
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
    }

    // A clip dragged out of the Media Pool lands here, which is how one is auditioned
    // before any of it is used.
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]

        onDropped: (drop) => {
            const uri = drop.getDataAsString("text/uri-list");
            if (uri.startsWith("file://"))
                Edit.openSource(uri.substring(7));
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
            if (player.playbackState === MediaPlayer.PlayingState)
                panel.position = player.position / 1000;
        }
    }

    /// Dragging out of the picture is how a marked range reaches the timeline. The three
    /// buttons below pick which halves go; dragging the picture itself takes both, which is
    /// what an editor means by dragging a shot.
    Item {
        id: dragOut

        anchors.fill: output
        z: 5
        visible: panel.hasSource

        Drag.active: dragMouse.drag.active
        Drag.hotSpot.x: 0
        Drag.hotSpot.y: 0
        Drag.mimeData: ({"application/x-munkicast-source": panel.src.path})
        Drag.dragType: Drag.Internal

        MouseArea {
            id: dragMouse

            anchors.fill: parent
            drag.target: dragOut
            // A click still scrubs nothing and a drag still starts: the threshold keeps a
            // careless press from becoming an edit.
            drag.threshold: 8
            onPressed: Edit.beginSourceDrag(panel.dragVideo, panel.dragAudio)
            onReleased: {
                if (dragOut.Drag.active)
                    dragOut.Drag.drop();
                else
                    Edit.cancelSourceDrag();
                dragOut.x = 0;
                dragOut.y = 0;
            }
        }
    }

    VideoOutput {
        id: output

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: scrub.top
        anchors.margins: 8
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

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: transport.top
        anchors.margins: 8
        height: 14
        visible: panel.hasSource

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
            width: frames > 0
                   ? Math.max(1, track.width * ((panel.src.out - panel.src.in) / frames))
                   : 0
            height: 4
            radius: 2
            color: Theme.c.accent
            opacity: 0.55
        }

        Rectangle {
            x: panel.seconds > 0
               ? Math.max(0, Math.min(track.width * (panel.position / panel.seconds),
                                      track.width - 2))
               : 0
            width: 2
            height: parent.height
            color: Theme.c.text
        }

        MouseArea {
            anchors.fill: parent
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
        height: 30
        color: Theme.c.panelHeader

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: panel.hasSource ? Edit.timecode(panel.frameOf(panel.position)) : "--:--:--:--"
            font.pixelSize: 11
            font.family: "monospace"
            color: Theme.c.text
        }

        Row {
            anchors.centerIn: parent
            spacing: 4

            ToolButton {
                label: "⏮"
                tip: qsTr("Go to the start of the file")
                enabled: panel.hasSource
                onClicked: panel.seekTo(0)
            }

            ToolButton {
                label: player.playbackState === MediaPlayer.PlayingState ? "❚❚" : "▶"
                tip: qsTr("Play or pause the source")
                enabled: panel.hasSource
                onClicked: player.playbackState === MediaPlayer.PlayingState
                           ? player.pause() : player.play()
            }

            ToolButton {
                label: "⏭"
                tip: qsTr("Go to the end of the file")
                enabled: panel.hasSource
                onClicked: panel.seekTo(panel.seconds)
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
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
