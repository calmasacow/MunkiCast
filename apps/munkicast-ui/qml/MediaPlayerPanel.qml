// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtMultimedia
import MunkiCast

/// Playback for the selected file.
///
/// QtMultimedia rather than the render process. This is review, not air: a file being looked at
/// in a browser has none of the frame-deadline requirements a program feed has, and routing it
/// through the compositor would make a preview depend on a pipeline that may not be attached.
Item {
    id: panel

    readonly property var item: Media.selectedItem
    readonly property bool hasFile: !!panel.item.path

    // A still has no duration and no playback; the player would sit at 0:00 forever, so the
    // picture is shown and the transport is not.
    readonly property bool playable: panel.hasFile && panel.item.seconds > 0

    onItemChanged: {
        player.stop();
        player.source = panel.hasFile ? Media.selectedUrl() : "";
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
    }

    MediaPlayer {
        id: player

        videoOutput: output
        audioOutput: AudioOutput {}
    }

    VideoOutput {
        id: output

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: transport.top
        anchors.margins: 8
        fillMode: VideoOutput.PreserveAspectFit
        visible: panel.playable
    }

    // A still is drawn directly: the player cannot open one, and showing a picture is the
    // whole of what is being asked for.
    Image {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: transport.top
        anchors.margins: 8
        visible: panel.hasFile && !panel.playable
        source: panel.hasFile && !panel.playable ? Media.selectedUrl() : ""
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: false
    }

    Text {
        anchors.centerIn: parent
        visible: !panel.hasFile
        text: qsTr("Select a file to play it")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Item {
        id: transport

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 30
        visible: panel.playable

        // The scrub bar. Click or drag anywhere on it to seek, which is what a review player is
        // for; a thin handle that has to be hit exactly is not.
        Rectangle {
            id: track

            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.top: parent.top
            height: 4
            radius: 2
            color: Theme.c.border

            Rectangle {
                width: player.duration > 0
                       ? track.width * (player.position / player.duration)
                       : 0
                height: parent.height
                radius: 2
                color: Theme.c.accent
            }

            MouseArea {
                anchors.fill: parent
                anchors.topMargin: -8
                anchors.bottomMargin: -8
                onPressed: mouse => seek(mouse.x)
                onPositionChanged: mouse => { if (pressed) seek(mouse.x); }

                function seek(x) {
                    if (player.duration <= 0)
                        return;
                    const fraction = Math.max(0, Math.min(1, x / track.width));
                    player.position = fraction * player.duration;
                }
            }
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 4
            spacing: 6

            ToolButton {
                label: player.playing ? "❚❚" : "▶"
                tip: player.playing ? qsTr("Pause") : qsTr("Play")
                onClicked: player.playing ? player.pause() : player.play()
            }

            ToolButton {
                label: "■"
                tip: qsTr("Stop")
                onClicked: player.stop()
            }
        }

        Text {
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 7
            font.pixelSize: 10
            font.family: "monospace"
            color: Theme.c.textMuted
            text: panel.clock(player.position) + " / " + panel.clock(player.duration)
        }
    }

    /// Milliseconds as m:ss, or h:mm:ss once it needs an hours field.
    function clock(ms) {
        const total = Math.floor(Math.max(0, ms) / 1000);
        const h = Math.floor(total / 3600);
        const m = Math.floor((total % 3600) / 60);
        const s = total % 60;
        const ss = s < 10 ? "0" + s : "" + s;
        if (h <= 0)
            return m + ":" + ss;
        const mm = m < 10 ? "0" + m : "" + m;
        return h + ":" + mm + ":" + ss;
    }
}
