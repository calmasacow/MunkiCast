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
/// ponytail: shows the topmost clip only. Add a composite when a second video track needs to be
/// seen over the first.
Item {
    id: panel

    readonly property var clip: Edit.clipUnderPlayhead
    readonly property bool hasClip: !!panel.clip.source

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
    }

    onClipChanged: {
        const wanted = panel.hasClip ? "file://" + panel.clip.source : "";
        if (player.source != wanted)
            player.source = wanted;
    }

    // The playhead moving is a seek, not a reload: reassigning the source on every frame of a
    // scrub would reopen the file sixty times a second.
    Connections {
        target: Edit

        function onChanged() {
            if (!panel.hasClip)
                return;
            const ms = panel.clip.sourceSeconds * 1000;
            if (Math.abs(player.position - ms) > 40)
                player.position = ms;
        }
    }

    MediaPlayer {
        id: player

        videoOutput: output
        audioOutput: AudioOutput {}
    }

    VideoOutput {
        id: output

        anchors.fill: parent
        anchors.margins: 8
        fillMode: VideoOutput.PreserveAspectFit
        visible: panel.hasClip
    }

    Text {
        anchors.centerIn: parent
        visible: !panel.hasClip
        text: Edit.hasTimeline ? qsTr("No clip under the playhead")
                               : qsTr("No timeline open")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Rectangle {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 8
        radius: 2
        visible: Edit.hasTimeline
        color: Theme.c.panelBackground
        opacity: 0.82
        width: stamp.implicitWidth + 10
        height: stamp.implicitHeight + 6

        Text {
            id: stamp

            anchors.centerIn: parent
            text: Edit.hasTimeline ? Edit.timecode(Edit.timeline.playhead) : ""
            font.pixelSize: 11
            font.family: "monospace"
            color: Theme.c.text
        }
    }
}
