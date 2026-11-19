// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtMultimedia
import MunkiCast

/// The frame under the playhead.
///
/// The clip's own file, seeked to the frame the playhead names. Not a composite: with one
/// video track that is the same picture, and building a real compositor for the Edit tab is the
/// renderer's job rather than something to fake here.
///
/// ponytail: shows the topmost clip only, and cannot blend two. A transition is reported --
/// which shot, how far through -- rather than mixed, so a dissolve can be timed here even
/// though it cannot yet be seen. Real compositing is the renderer's job.
///
/// ponytail: add a composite when a second video track needs to be
/// seen over the first.
Item {
    id: panel

    readonly property var clip: Edit.clipUnderPlayhead
    readonly property bool hasClip: !!panel.clip.source

    /// How the picture is sized: "fit" letterboxes it, a number is that multiple of actual
    /// size. Fit is what a monitor is looked at in; the zooms are for checking an edge.
    property string zoom: "fit"

    /// Which deck is on screen.
    property int front: 0
    readonly property var decks: [deckA, deckB]

    /// Keep a deck on the clip under the playhead, at the frame under the playhead.
    ///
    /// Two decks, each a player with its own picture. A cut loads the next file on the deck
    /// that is not showing, and the screen changes over only once that deck has drawn a frame.
    /// One player, reloaded at every cut, went black for as long as the load took -- which,
    /// dragged across a cut, was the whole drag.
    ///
    /// The deck not showing also opens the next shot ahead of the playhead, so a cut while
    /// playing is usually a change of picture rather than a load.
    ///
    /// ponytail: the changeover is still not frame-exact. Exactness needs the renderer, which
    /// can decode both and hand over on a known frame.
    function follow() {
        const shown = panel.decks[panel.front];
        const spare = panel.decks[1 - panel.front];
        if (!panel.hasClip) {
            shown.hold();
            spare.hold();
            return;
        }
        const wanted = "file://" + panel.clip.source;
        const ms = panel.clip.sourceSeconds * 1000;
        const target = shown.url === wanted ? shown : spare;
        if (target.url !== wanted)
            target.load(wanted);
        target.drive(ms);
        // The other one parks: if it is still on screen it keeps its last frame until the
        // target has one of its own.
        (target === shown ? spare : shown).hold();
        if (target.ready && target !== shown)
            panel.front = 1 - panel.front;
        // Warm the next shot on the deck that is not needed now.
        if (target === panel.decks[panel.front] && panel.clip.nextSource) {
            const other = panel.decks[1 - panel.front];
            const next = "file://" + panel.clip.nextSource;
            if (next !== wanted && other.url !== next) {
                other.load(next);
                other.cue((panel.clip.nextSourceSeconds ?? 0) * 1000);
            }
        }
    }

    onClipChanged: panel.follow()

    Connections {
        target: Edit

        function onChanged() { panel.follow(); }
        function onTransportChanged() { panel.follow(); }
    }

    // The sequence's own J-K-L, so it answers here as it does in the timeline.
    TransportKeys {
        panelId: "edit-program"
        onReverse: Edit.shuttle(-1)
        onStop: Edit.pause()
        onForward: Edit.shuttle(1)
        onToggle: Edit.togglePlay()
    }

    /// One file's player and its picture.
    component Deck: Item {
        id: deck

        property string url: ""
        /// A frame of `url` is on this deck's picture.
        property bool ready: false
        /// Where it should be parked, in ms into its file.
        property real wantMs: 0
        /// A seek is out and its frame not yet drawn, since `sentAt`.
        property bool waiting: false
        property real sentAt: 0
        property bool audible: false
        property alias output: picture
        readonly property bool loaded: player.mediaStatus === MediaPlayer.LoadedMedia
                                       || player.mediaStatus === MediaPlayer.BufferedMedia
                                       || player.mediaStatus === MediaPlayer.BufferingMedia
                                       || player.mediaStatus === MediaPlayer.EndOfMedia

        function load(u) {
            deck.url = u;
            deck.ready = false;
            deck.waiting = false;
            player.source = u;
        }

        /// One seek in flight at a time: a seek over an unfinished one cancels its decode,
        /// and a drag's worth of them drew nothing. The last one asked for is the one that
        /// is sent when the frame in flight lands.
        function park() {
            if (!deck.loaded || player.playbackState === MediaPlayer.PlayingState)
                return;
            if (deck.ready && Math.abs(player.position - deck.wantMs) <= 20)
                return;
            if (deck.waiting && Date.now() - deck.sentAt < 250)
                return;
            deck.waiting = true;
            deck.sentAt = Date.now();
            player.position = deck.wantMs;
        }

        function drive(ms) {
            deck.wantMs = ms;
            if (!deck.loaded)
                return;
            if (Edit.playing && Edit.rate > 0) {
                // Forwards at any speed the file plays itself at the timeline's rate,
                // corrected only when it has drifted, with slack that grows with the speed.
                if (player.playbackRate !== Edit.rate)
                    player.playbackRate = Edit.rate;
                if (player.playbackState !== MediaPlayer.PlayingState)
                    player.play();
                if (Math.abs(player.position - ms) > Math.max(250, 120 * Edit.rate))
                    player.position = ms;
                return;
            }
            // Scrubbing, stepping or running backwards: the file is a frame store.
            if (player.playbackState === MediaPlayer.PlayingState)
                player.pause();
            player.playbackRate = 1;
            deck.park();
        }

        /// Parked at `ms` and left there: the next shot, waiting off screen.
        function cue(ms) {
            deck.wantMs = ms;
            deck.hold();
            deck.park();
        }

        function hold() {
            if (player.playbackState === MediaPlayer.PlayingState)
                player.pause();
        }

        MediaPlayer {
            id: player

            videoOutput: picture
            audioOutput: AudioOutput {
                // The mix level, so muting a track or pulling the master down is audible
                // here rather than only in the document. Only the deck on screen is heard.
                volume: Edit.hasTimeline ? Math.pow(10, Edit.masterGain / 20) : 1.0
                muted: !deck.audible
            }
            // Loaded is the first moment a seek lands. Paused rather than left stopped: a
            // stopped player draws nothing, and the monitor stayed black.
            onMediaStatusChanged: {
                if (player.mediaStatus === MediaPlayer.LoadedMedia
                        && player.playbackState === MediaPlayer.StoppedState) {
                    player.pause();
                    deck.waiting = false;
                    panel.follow();
                    deck.park();
                }
            }
        }

        VideoOutput {
            id: picture
            anchors.fill: parent
            fillMode: panel.zoom === "fit" ? VideoOutput.PreserveAspectFit
                                           : VideoOutput.PreserveAspectCrop
            scale: panel.zoom === "fit" ? 1.0 : parseFloat(panel.zoom)
            transformOrigin: Item.Center
        }

        Connections {
            target: picture.videoSink
            function onVideoFrameChanged() {
                // Ready on a frame from a seek, not the first one the file opens on: changing
                // over to that showed the head of the clip for a beat at every cut. The
                // frame's own time is not readable from here, so it is the order of events
                // that says: a frame that lands while a seek is out is that seek's.
                const settled = deck.waiting || player.playbackState === MediaPlayer.PlayingState;
                deck.waiting = false;
                if (!deck.ready && settled) {
                    deck.ready = true;
                    panel.follow();
                }
                // Moved on while that one decoded: go to where it is now.
                deck.park();
            }
        }
    }

    Item {
        id: output

        objectName: "programOutput"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: scrub.top
        anchors.margins: 8
        clip: panel.zoom !== "fit"
        visible: panel.hasClip

        Deck {
            id: deckA
            objectName: "programDeckA"
            anchors.fill: parent
            visible: panel.front === 0
            audible: panel.front === 0
        }

        Deck {
            id: deckB
            objectName: "programDeckB"
            anchors.fill: parent
            visible: panel.front === 1
            audible: panel.front === 1
        }
    }

    // What the picture cannot show yet: that this frame is inside a dissolve, and how far
    // through. Without it a transition looks exactly like a hard cut here, and an editor
    // cannot tell whether the length they chose is right.
    Rectangle {
        visible: panel.hasClip && !!panel.clip.inTransition
        anchors.horizontalCenter: output.horizontalCenter
        anchors.bottom: output.bottom
        anchors.bottomMargin: 10
        width: mixRow.implicitWidth + 16
        height: 22
        radius: 3
        color: Theme.c.panelBackground
        opacity: 0.85

        Row {
            id: mixRow

            anchors.centerIn: parent
            spacing: 8

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: panel.clip.outgoingName
                      ? qsTr("%1 → %2").arg(panel.clip.outgoingName).arg(panel.clip.name)
                      : qsTr("transition")
                font.pixelSize: 10
                color: Theme.c.text
            }

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 60
                height: 3
                radius: 2
                color: Theme.c.window

                Rectangle {
                    width: parent.width * (panel.clip.transitionProgress ?? 0)
                    height: parent.height
                    radius: 2
                    color: Theme.c.accent
                }
            }
        }
    }

    // The sequence's whole length as one bar, with the markers and the marked range on it.
    // The timeline below shows detail; this shows where in the programme you are, which is a
    // different question and the one a monitor is looked at for.
    Item {
        id: scrub

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: transport.top
        // The Source viewer's bar, to the pixel: the two monitors sit side by side.
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.bottomMargin: 6
        height: 18
        visible: Edit.hasTimeline

        readonly property int total: Edit.hasTimeline ? (Edit.timeline.duration || 0) : 0

        function xOf(frame) {
            return scrub.total > 0 ? bar.width * (frame / scrub.total) : 0;
        }

        Rectangle {
            id: bar

            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 4
            radius: 2
            color: Theme.c.panelHeader
        }

        Rectangle {
            readonly property int from: Edit.hasTimeline && Edit.timeline.inPoint >= 0
                                        ? Edit.timeline.inPoint : 0
            readonly property int to: Edit.hasTimeline && Edit.timeline.outPoint >= 0
                                      ? Edit.timeline.outPoint : scrub.total
            visible: Edit.hasTimeline &&
                     (Edit.timeline.inPoint >= 0 || Edit.timeline.outPoint >= 0)
            anchors.verticalCenter: parent.verticalCenter
            x: scrub.xOf(from)
            width: Math.max(1, scrub.xOf(to) - scrub.xOf(from))
            height: 4
            radius: 2
            color: Theme.c.accent
            opacity: 0.55
        }

        Repeater {
            model: Edit.hasTimeline ? Edit.timeline.markers : []

            delegate: Rectangle {
                required property var modelData

                x: scrub.xOf(modelData.at)
                y: 0
                width: 2
                height: 5
                color: Theme.c.warning
            }
        }

        Rectangle {
            objectName: "programPlayhead"
            x: Math.max(0, Math.min(scrub.xOf(Edit.hasTimeline ? Edit.timeline.playhead : 0),
                                    bar.width)) - width / 2
            anchors.verticalCenter: parent.verticalCenter
            width: 8
            height: parent.height
            radius: 2
            color: programScrub.pressed ? Theme.c.accent : Theme.c.text
        }

        MouseArea {
            id: programScrub
            anchors.fill: parent
            anchors.topMargin: -4
            anchors.bottomMargin: -4
            preventStealing: true
            cursorShape: Qt.PointingHandCursor
            onPressed: (mouse) => {
                if (scrub.total > 0)
                    Edit.setPlayhead(Math.round(mouse.x / bar.width * scrub.total));
            }
            onPositionChanged: (mouse) => {
                if (pressed && scrub.total > 0)
                    Edit.setPlayhead(Math.round(mouse.x / bar.width * scrub.total));
            }
        }
    }

    // The transport. The buttons an editor's hands live on, in the order every deck has had
    // them since tape: start, back, play, forward, end, with the mark keys beside them.
    Rectangle {
        id: transport

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        // The Source viewer's two rows: the deck's buttons centred on top, the clock and the
        // tools under them.
        height: 66
        visible: Edit.hasTimeline
        color: Theme.c.panelHeader

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: programTools.verticalCenter
            text: Edit.hasTimeline ? Edit.timecode(Edit.timeline.playhead) : ""
            font.pixelSize: 11
            font.family: "monospace"
            color: Theme.c.text

            // The shuttle gear, while it is not plain play.
            Text {
                anchors.left: parent.right
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: Edit.playing && Edit.rate !== 1
                text: (Edit.rate < 0 ? "◀ " : "▶ ") + Math.abs(Edit.rate) + "×"
                font.pixelSize: 11
                color: Theme.c.accent
            }
        }

        // Plain triangles: ⏮ and ⏭ fall back to the colour emoji font, drawn as blue tiles.
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 4
            spacing: 6

            ToolButton {
                width: 34; height: 30; glyphSize: 18
                label: "|◀◀"
                tip: qsTr("Go to the start (Home)")
                onClicked: Edit.goToStart()
            }

            ToolButton {
                width: 34; height: 30; glyphSize: 18
                label: "◀◀"
                tip: qsTr("Shuttle backwards (J)")
                active: Edit.rate < 0
                onClicked: Edit.shuttle(-1)
            }

            ToolButton {
                width: 34; height: 30; glyphSize: 18
                label: Edit.playing && Edit.rate > 0 ? "❚❚" : "▶"
                tip: Edit.playing ? qsTr("Pause (K or space)") : qsTr("Play (space)")
                active: Edit.rate === 1
                onClicked: Edit.togglePlay()
            }

            ToolButton {
                width: 34; height: 30; glyphSize: 18
                label: "▶▶"
                tip: qsTr("Shuttle forwards (L)")
                active: Edit.rate > 1
                onClicked: Edit.shuttle(1)
            }

            ToolButton {
                width: 34; height: 30; glyphSize: 18
                label: "▶▶|"
                tip: qsTr("Go to the end (End)")
                onClicked: Edit.goToEnd()
            }
        }

        Row {
            id: programTools
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 5
            spacing: 4

            ToolButton {
                label: "⮜"
                tip: qsTr("Previous edit (Up)")
                onClicked: Edit.stepEdit(false)
            }

            ToolButton {
                label: "⮞"
                tip: qsTr("Next edit (Down)")
                onClicked: Edit.stepEdit(true)
            }

            // How the picture is sized. Cycled rather than a menu: there are four, and the
            // one in use is almost always Fit.
            ToolButton {
                readonly property var steps: ["fit", "0.5", "1", "2"]
                label: panel.zoom === "fit" ? qsTr("Fit")
                     : panel.zoom === "0.5" ? "½"
                     : panel.zoom === "1" ? "1:1" : "2:1"
                tip: qsTr("How the picture is sized — click to change")
                onClicked: {
                    const at = steps.indexOf(panel.zoom);
                    panel.zoom = steps[(at + 1) % steps.length];
                }
            }

            ToolButton {
                label: "{"
                tip: qsTr("Mark in (I)")
                active: Edit.hasTimeline && Edit.timeline.inPoint >= 0
                onClicked: Edit.setInPoint()
            }

            ToolButton {
                label: "}"
                tip: qsTr("Mark out (O)")
                active: Edit.hasTimeline && Edit.timeline.outPoint >= 0
                onClicked: Edit.setOutPoint()
            }

            ToolButton {
                label: "✕"
                tip: qsTr("Clear the range")
                enabled: Edit.hasTimeline &&
                         (Edit.timeline.inPoint >= 0 || Edit.timeline.outPoint >= 0)
                onClicked: Edit.clearRange()
            }

            ToolButton {
                label: "⚑"
                tip: qsTr("Add a marker (M)")
                onClicked: Edit.addMarkerAtPlayhead()
            }
        }
    }

    Text {
        anchors.centerIn: output
        visible: !panel.hasClip
        text: Edit.hasTimeline ? qsTr("No clip under the playhead")
                               : qsTr("No timeline open")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

}
