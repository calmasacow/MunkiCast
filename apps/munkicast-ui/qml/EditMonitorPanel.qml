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

    /// Where the warmed player is parked, in seconds into its own file.
    property real warmSeconds: 0

    /// Open the file the next cut needs, if it is not the one already playing.
    function warmNext() {
        const next = panel.hasClip && panel.clip.nextSource
                     ? "file://" + panel.clip.nextSource : "";
        if (next === "" || next === String(player.source)) {
            warm.source = "";
            return;
        }
        if (warm.source != next) {
            panel.warmSeconds = panel.clip.nextSourceSeconds ?? 0;
            warm.source = next;
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
    }

    onClipChanged: panel.follow()

    /// Keep the player on the clip under the playhead, at the frame under the playhead.
    ///
    /// Changing the source is a reload and costs a beat, so it happens only when the playhead
    /// crosses into a different clip -- which is exactly what a cut is. Within one clip the
    /// playhead moving is a seek.
    ///
    /// The next shot is opened on a second player while this one is still running, so a cut
    /// is a swap between two loaded decoders rather than a load. That is what took the hitch
    /// out of every edit.
    ///
    /// ponytail: the swap is still a swap, so a cut is not frame-exact. Exactness needs the
    /// renderer, which can decode both and hand over on a known frame.
    function follow() {
        const wanted = panel.hasClip ? "file://" + panel.clip.source : "";
        if (player.source != wanted) {
            // Already warmed? Then the cut is a swap: the decoder for this file is open and
            // sitting at the right frame, so take it and start warming the one after.
            if (wanted !== "" && warm.source == wanted) {
                player.source = wanted;
                player.position = panel.clip.sourceSeconds * 1000;
                panel.pendingSeek = panel.clip.sourceSeconds;
            } else {
                player.source = wanted;
                // A fresh source starts at zero, so the seek has to wait for it to be ready.
                if (panel.hasClip)
                    panel.pendingSeek = panel.clip.sourceSeconds;
            }
            panel.warmNext();
            return;
        }
        panel.warmNext();
        if (!panel.hasClip)
            return;

        const ms = panel.clip.sourceSeconds * 1000;
        if (Edit.playing && Edit.rate === 1) {
            // While playing forward at speed, the file plays itself: seeking on every frame
            // would stutter. Only correct when it has drifted away from the timeline.
            if (player.playbackState !== MediaPlayer.PlayingState)
                player.play();
            if (Math.abs(player.position - ms) > 250)
                player.position = ms;
        } else {
            // Scrubbing, stepping or shuttling: the file is a frame store, not a player.
            if (player.playbackState === MediaPlayer.PlayingState)
                player.pause();
            if (Math.abs(player.position - ms) > 30)
                player.position = ms;
        }
    }

    /// Where to seek once the newly-opened source is ready to be seeked.
    property real pendingSeek: -1

    Connections {
        target: Edit

        function onChanged() { panel.follow(); }
        function onTransportChanged() { panel.follow(); }
    }

    MediaPlayer {
        id: player

        videoOutput: output
        audioOutput: AudioOutput {
            // The mix level, so muting a track or pulling the master down is audible here
            // rather than only in the document.
            volume: Edit.hasTimeline ? Math.pow(10, Edit.masterGain / 20) : 1.0
        }

        onMediaStatusChanged: {
            // A source that has just loaded is at zero whatever was asked for before it was
            // ready; this is the first moment a seek will land.
            if (mediaStatus === MediaPlayer.LoadedMedia && panel.pendingSeek >= 0) {
                player.position = panel.pendingSeek * 1000;
                panel.pendingSeek = -1;
                if (Edit.playing && Edit.rate === 1)
                    player.play();
            }
        }
    }

    /// The next shot, opened and parked at its first frame. Never shown: it exists so that
    /// the decoder is already up when the playhead reaches the cut.
    MediaPlayer {
        id: warm

        onMediaStatusChanged: {
            // Park it where the next clip starts, so the swap lands on the right frame
            // rather than at the head of the file.
            if (warm.mediaStatus === MediaPlayer.LoadedMedia ||
                warm.mediaStatus === MediaPlayer.BufferedMedia) {
                warm.position = panel.warmSeconds * 1000;
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
        fillMode: panel.zoom === "fit" ? VideoOutput.PreserveAspectFit
                                       : VideoOutput.PreserveAspectCrop
        scale: panel.zoom === "fit" ? 1.0 : parseFloat(panel.zoom)
        transformOrigin: Item.Center
        visible: panel.hasClip
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
        anchors.margins: 8
        height: 14
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
            x: Math.max(0, Math.min(scrub.xOf(Edit.hasTimeline ? Edit.timeline.playhead : 0),
                                    bar.width - 2))
            width: 2
            height: parent.height
            color: Theme.c.text
        }

        MouseArea {
            anchors.fill: parent
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
        height: 30
        visible: Edit.hasTimeline
        color: Theme.c.panelHeader

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: Edit.hasTimeline ? Edit.timecode(Edit.timeline.playhead) : ""
            font.pixelSize: 11
            font.family: "monospace"
            color: Theme.c.text
        }

        Row {
            anchors.centerIn: parent
            spacing: 4

            ToolButton {
                label: "⏮"
                tip: qsTr("Go to the start (Home)")
                onClicked: Edit.goToStart()
            }

            ToolButton {
                label: "⮜"
                tip: qsTr("Previous edit (Up)")
                onClicked: Edit.stepEdit(false)
            }

            ToolButton {
                label: "◀◀"
                tip: qsTr("Shuttle backwards (J)")
                active: Edit.rate < 0
                onClicked: Edit.shuttle(-1)
            }

            ToolButton {
                label: Edit.playing && Edit.rate > 0 ? "❚❚" : "▶"
                tip: Edit.playing ? qsTr("Pause (K or space)") : qsTr("Play (space)")
                active: Edit.rate === 1
                onClicked: Edit.togglePlay()
            }

            ToolButton {
                label: "▶▶"
                tip: qsTr("Shuttle forwards (L)")
                active: Edit.rate > 1
                onClicked: Edit.shuttle(1)
            }

            ToolButton {
                label: "⮞"
                tip: qsTr("Next edit (Down)")
                onClicked: Edit.stepEdit(true)
            }

            ToolButton {
                label: "⏭"
                tip: qsTr("Go to the end (End)")
                onClicked: Edit.goToEnd()
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

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

        // The shuttle rate, when it is not simply playing: an editor racing through at 8x
        // wants to see that is what is happening.
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 1
            visible: Math.abs(Edit.rate) > 1
            text: Math.abs(Edit.rate) + "×"
            font.pixelSize: 9
            color: Theme.c.accent
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
