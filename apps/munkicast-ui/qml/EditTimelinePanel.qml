// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The timeline: tracks down, time across.
///
/// Drawn with a Canvas rather than an item per clip. A sequence is thousands of clips and a
/// zoom is a continuous gesture over all of them; an item tree that size is rebuilt on every
/// wheel notch, which is the thing that makes a timeline feel heavy.
Item {
    id: panel

    /// Pixels per frame. The zoom.
    property real scale: 2.0
    /// The leftmost frame on screen.
    property real offset: 0

    readonly property int headerWidth: 120
    readonly property int rulerHeight: 22
    readonly property int trackHeight: 44

    readonly property var doc: Edit.timeline
    readonly property bool hasDocument: Edit.hasTimeline

    function frameAt(x) {
        return Math.max(0, Math.round(panel.offset + (x - panel.headerWidth) / panel.scale));
    }

    function xOf(frame) {
        return panel.headerWidth + (frame - panel.offset) * panel.scale;
    }

    onDocChanged: body.requestPaint()
    onScaleChanged: body.requestPaint()
    onOffsetChanged: body.requestPaint()

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
    }

    Text {
        anchors.centerIn: parent
        visible: !panel.hasDocument
        text: qsTr("No timeline open. Make one from the Bins panel.")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Canvas {
        id: body

        anchors.fill: parent
        visible: panel.hasDocument
        renderStrategy: Canvas.Cooperative

        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.fillStyle = Theme.c.window;
            ctx.fillRect(0, 0, width, height);

            const doc = panel.doc;
            if (!doc || !doc.tracks)
                return;

            const fps = doc.fps > 0 ? doc.fps : 30;

            // ---- ruler ----
            ctx.fillStyle = Theme.c.panelHeader;
            ctx.fillRect(0, 0, width, panel.rulerHeight);

            // A tick every second until seconds get too close together, then every 5, 10, 30,
            // 60. Chosen from the zoom so the labels never collide, which is what makes the
            // ruler readable at every scale rather than only at the one it was designed at.
            const steps = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600];
            let step = steps[steps.length - 1];
            for (let i = 0; i < steps.length; ++i) {
                if (steps[i] * fps * panel.scale >= 60) {
                    step = steps[i];
                    break;
                }
            }

            ctx.strokeStyle = Theme.c.border;
            ctx.fillStyle = Theme.c.textMuted;
            ctx.font = "10px sans-serif";
            ctx.lineWidth = 1;
            const firstSecond = Math.floor(panel.offset / fps / step) * step;
            for (let s = firstSecond; panel.xOf(s * fps) < width; s += step) {
                const x = Math.round(panel.xOf(s * fps)) + 0.5;
                if (x < panel.headerWidth)
                    continue;
                ctx.beginPath();
                ctx.moveTo(x, 0);
                ctx.lineTo(x, panel.rulerHeight);
                ctx.stroke();
                const m = Math.floor(s / 60);
                const sec = s % 60;
                ctx.fillText(m + ":" + (sec < 10 ? "0" + sec : sec), x + 3, 13);
            }

            // ---- tracks ----
            // Drawn top-down in reverse, so the last track in the document -- the topmost
            // video layer -- is the top lane on screen, which is the way every editor reads.
            for (let t = 0; t < doc.tracks.length; ++t) {
                const track = doc.tracks[doc.tracks.length - 1 - t];
                const y = panel.rulerHeight + t * panel.trackHeight;

                ctx.fillStyle = t % 2 === 0 ? Theme.c.panelBackground : Theme.c.window;
                ctx.fillRect(panel.headerWidth, y, width - panel.headerWidth,
                             panel.trackHeight);

                ctx.fillStyle = Theme.c.panelHeader;
                ctx.fillRect(0, y, panel.headerWidth, panel.trackHeight);
                ctx.fillStyle = track.locked ? Theme.c.textMuted : Theme.c.text;
                ctx.font = "11px sans-serif";
                ctx.fillText(track.name, 10, y + 17);
                ctx.fillStyle = Theme.c.textMuted;
                ctx.font = "9px sans-serif";
                ctx.fillText((track.kind === "audio" ? "A" : "V") +
                             (track.enabled ? "" : " · hidden") +
                             (track.locked ? " · locked" : ""), 10, y + 31);

                ctx.strokeStyle = Theme.c.divider;
                ctx.beginPath();
                ctx.moveTo(0, y + panel.trackHeight + 0.5);
                ctx.lineTo(width, y + panel.trackHeight + 0.5);
                ctx.stroke();

                for (let c = 0; c < track.clips.length; ++c) {
                    const clip = track.clips[c];
                    const x0 = panel.xOf(clip.start);
                    const x1 = panel.xOf(clip.start + clip.duration);
                    if (x1 < panel.headerWidth || x0 > width)
                        continue;
                    const left = Math.max(x0, panel.headerWidth);
                    const w = x1 - left;
                    if (w <= 0)
                        continue;

                    const selected = clip.id === Edit.selectedClip;
                    ctx.fillStyle = track.kind === "audio" ? "#2d6a4f" : "#1d4e89";
                    ctx.fillRect(left, y + 3, w, panel.trackHeight - 7);
                    ctx.strokeStyle = selected ? Theme.c.accent : Theme.c.border;
                    ctx.lineWidth = selected ? 2 : 1;
                    ctx.strokeRect(left + 0.5, y + 3.5, w - 1, panel.trackHeight - 8);

                    // The name is clipped to the clip, so a short clip shows nothing rather
                    // than spilling its label over the one beside it.
                    if (w > 24) {
                        ctx.save();
                        ctx.beginPath();
                        ctx.rect(left + 3, y + 3, w - 6, panel.trackHeight - 7);
                        ctx.clip();
                        ctx.fillStyle = "#ffffff";
                        ctx.font = "10px sans-serif";
                        ctx.fillText(clip.name, left + 5, y + 16);
                        ctx.restore();
                    }
                }
            }

            // ---- markers ----
            for (let m = 0; m < (doc.markers || []).length; ++m) {
                const marker = doc.markers[m];
                const x = Math.round(panel.xOf(marker.at)) + 0.5;
                if (x < panel.headerWidth || x > width)
                    continue;
                ctx.fillStyle = "#" + ("000000" + marker.color.toString(16)).slice(-6);
                ctx.fillRect(x - 3, 0, 6, 6);
            }

            // ---- in/out ----
            if (doc.inPoint >= 0 || doc.outPoint >= 0) {
                ctx.fillStyle = Theme.c.accent;
                ctx.globalAlpha = 0.12;
                const a = doc.inPoint >= 0 ? panel.xOf(doc.inPoint) : panel.headerWidth;
                const b = doc.outPoint >= 0 ? panel.xOf(doc.outPoint) : width;
                ctx.fillRect(Math.max(a, panel.headerWidth), panel.rulerHeight,
                             b - Math.max(a, panel.headerWidth), height - panel.rulerHeight);
                ctx.globalAlpha = 1.0;
            }

            // ---- playhead ----
            const px = Math.round(panel.xOf(doc.playhead)) + 0.5;
            if (px >= panel.headerWidth) {
                ctx.strokeStyle = Theme.c.error;
                ctx.lineWidth = 1;
                ctx.beginPath();
                ctx.moveTo(px, 0);
                ctx.lineTo(px, height);
                ctx.stroke();
            }

            ctx.strokeStyle = Theme.c.divider;
            ctx.beginPath();
            ctx.moveTo(panel.headerWidth + 0.5, 0);
            ctx.lineTo(panel.headerWidth + 0.5, height);
            ctx.stroke();
        }
    }

    MouseArea {
        id: surface

        anchors.fill: parent
        enabled: panel.hasDocument
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton

        /// What the press grabbed: "playhead", "clip", or nothing.
        property string dragging: ""
        property int grabFrame: 0
        property int clipStart: 0

        onPressed: mouse => {
            if (mouse.x < panel.headerWidth)
                return;
            const frame = panel.frameAt(mouse.x);

            // The ruler scrubs; the tracks select and drag.
            if (mouse.y < panel.rulerHeight) {
                dragging = "playhead";
                Edit.setPlayhead(frame);
                return;
            }

            const lane = Math.floor((mouse.y - panel.rulerHeight) / panel.trackHeight);
            const tracks = panel.doc.tracks || [];
            if (lane < 0 || lane >= tracks.length)
                return;
            const track = tracks[tracks.length - 1 - lane];
            const hit = Edit.clipAt(track.id, frame);
            if (hit) {
                Edit.selectClip(track.id, hit.id);
                dragging = "clip";
                grabFrame = frame;
                clipStart = hit.start;
            } else {
                Edit.selectClip("", "");
                dragging = "playhead";
                Edit.setPlayhead(frame);
            }
        }

        onPositionChanged: mouse => {
            if (dragging === "playhead") {
                Edit.setPlayhead(panel.frameAt(mouse.x));
            } else if (dragging === "clip") {
                // One undo step for the whole drag: the gesture name is the clip, so every
                // frame of movement coalesces into the move the operator actually made.
                const delta = panel.frameAt(mouse.x) - grabFrame;
                Edit.moveSelectedClip(clipStart + delta);
            }
        }

        onReleased: {
            if (dragging === "clip")
                Edit.endGesture();
            dragging = "";
        }

        onWheel: wheel => {
            // Ctrl zooms about the pointer, so the frame under the cursor stays under it.
            // Plain wheel scrolls, which is what it does everywhere else in the shell.
            if (wheel.modifiers & Qt.ControlModifier) {
                const at = panel.frameAt(wheel.x);
                const factor = wheel.angleDelta.y > 0 ? 1.2 : 1 / 1.2;
                panel.scale = Math.max(0.02, Math.min(40, panel.scale * factor));
                panel.offset = Math.max(0, at - (wheel.x - panel.headerWidth) / panel.scale);
            } else {
                panel.offset = Math.max(
                    0, panel.offset - wheel.angleDelta.y / panel.scale);
            }
        }
    }

    Rectangle {
        id: footer

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 24
        visible: panel.hasDocument
        color: Theme.c.panelHeader

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: 10
            font.family: "monospace"
            color: Theme.c.textMuted
            text: panel.hasDocument ? Edit.timecode(panel.doc.playhead) + "  ·  " +
                                      panel.doc.name : ""
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            ToolButton {
                label: "⌥"
                tip: qsTr("Split the selected clip at the playhead")
                enabled: Edit.selectedClip !== ""
                onClicked: Edit.splitAtPlayhead()
            }

            ToolButton {
                label: "⌫"
                tip: qsTr("Remove the selected clip")
                enabled: Edit.selectedClip !== ""
                onClicked: Edit.removeSelectedClip()
            }

            ToolButton {
                label: "+V"
                tip: qsTr("Add a video track")
                onClicked: Edit.addTrack("video")
            }

            ToolButton {
                label: "+A"
                tip: qsTr("Add an audio track")
                onClicked: Edit.addTrack("audio")
            }
        }
    }
}
