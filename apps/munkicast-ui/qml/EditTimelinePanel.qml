// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The timeline: tracks down, time across.
///
/// The lanes are drawn with a Canvas rather than an item per clip. A sequence is thousands of
/// clips and a zoom is a continuous gesture over all of them; an item tree that size is rebuilt
/// on every wheel notch, which is what makes a timeline feel heavy.
///
/// The track headers are real items, because they are buttons: six of them per track, each
/// wanting hover, a tooltip and a hit area. Drawing those into a Canvas would mean hit-testing
/// them by hand, which is work the scene graph already does.
Item {
    id: panel

    /// Pixels per frame. The zoom.
    property real scale: 2.0
    /// The leftmost frame on screen.
    property real offset: 0
    /// Which tool the pointer is: "select", "ripple", "slip", "razor", "hand".
    property string tool: "select"
    property bool snapping: true

    readonly property int headerWidth: 168
    readonly property int rulerHeight: 22
    readonly property int trackHeight: 48

    readonly property var doc: Edit.timeline
    readonly property bool hasDocument: Edit.hasTimeline
    readonly property var tracks: panel.hasDocument ? panel.doc.tracks : []

    function frameAt(x) {
        return Math.max(0, Math.round(panel.offset + (x - panel.headerWidth) / panel.scale));
    }

    function xOf(frame) {
        return panel.headerWidth + (frame - panel.offset) * panel.scale;
    }

    /// The nearest edit point within a few pixels, or the frame itself.
    ///
    /// Snapping is to *cuts*, not to a grid: what an editor lines a clip up against is another
    /// clip, the playhead or a marker, and a grid of frames at this zoom is every pixel.
    function snap(frame) {
        if (!panel.snapping)
            return frame;
        const slack = Math.max(2, 8 / panel.scale);
        let best = frame;
        let distance = slack;
        const consider = at => {
            const d = Math.abs(at - frame);
            if (d < distance) {
                distance = d;
                best = at;
            }
        };
        consider(panel.doc.playhead);
        for (const marker of panel.doc.markers ?? [])
            consider(marker.at);
        for (const track of panel.tracks) {
            for (const clip of track.clips) {
                consider(clip.start);
                consider(clip.start + clip.duration);
            }
        }
        return best;
    }

    /// Which lane a y falls in, as an index into `tracks`, or -1.
    function laneAt(y) {
        const lane = Math.floor((y - panel.rulerHeight) / panel.trackHeight);
        return lane >= 0 && lane < panel.tracks.length ? lane : -1;
    }

    /// Lanes are drawn top-down in reverse, so the last track in the document -- the topmost
    /// video layer -- is the top lane on screen, the way every editor reads.
    function trackOfLane(lane) {
        return panel.tracks[panel.tracks.length - 1 - lane];
    }

    onDocChanged: lanes.requestPaint()
    onScaleChanged: lanes.requestPaint()
    onOffsetChanged: lanes.requestPaint()
    onToolChanged: lanes.requestPaint()

    // Artwork arrives over the moments after a clip appears, and each arrival is a repaint.
    Connections {
        target: ClipArt

        function onReady() {
            lanes.requestPaint();
        }
    }

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

    // ---- tools ----
    //
    // In the ruler band, left of the first track. That strip is otherwise empty, and a tool
    // column beside the headers took width from the one place a name has to be readable.
    Row {
        id: toolbar

        anchors.left: parent.left
        anchors.leftMargin: 4
        anchors.top: parent.top
        height: panel.rulerHeight
        spacing: 1
        visible: panel.hasDocument
        z: 3

        Repeater {
            model: [
                {tool: "select", glyph: "▶", tip: qsTr("Select and move")},
                {tool: "ripple", glyph: "↔", tip: qsTr("Ripple trim: close the gap behind")},
                {tool: "slip", glyph: "↕", tip: qsTr("Slip: change what a clip shows")},
                {tool: "razor", glyph: "✂", tip: qsTr("Razor: cut at the pointer")},
                {tool: "hand", glyph: "✋", tip: qsTr("Hand: drag the view")}
            ]

            delegate: Rectangle {
                id: toolButton

                required property var modelData

                width: 22
                height: panel.rulerHeight - 4
                anchors.verticalCenter: parent.verticalCenter
                color: panel.tool === toolButton.modelData.tool ? Theme.c.accent
                     : toolMouse.containsMouse ? Theme.c.panelHeaderActive
                     : "transparent"
                opacity: panel.tool === toolButton.modelData.tool ? 0.3 : 1.0
                radius: 2

                Text {
                    anchors.centerIn: parent
                    text: toolButton.modelData.glyph
                    font.pixelSize: 11
                    color: panel.tool === toolButton.modelData.tool ? Theme.c.text
                                                                   : Theme.c.textMuted
                }

                MouseArea {
                    id: toolMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: panel.tool = toolButton.modelData.tool
                }

                ToolTipText {
                    text: toolButton.modelData.tip
                    visible: toolMouse.containsMouse
                    hoverArea: toolMouse
                }
            }
        }
    }

    // ---- track headers ----
    Column {
        id: headers

        anchors.left: parent.left
        anchors.top: parent.top
        anchors.topMargin: panel.rulerHeight
        width: panel.headerWidth
        visible: panel.hasDocument
        z: 2

        Repeater {
            model: panel.tracks.length

            delegate: Rectangle {
                id: header

                required property int index

                readonly property var track: panel.trackOfLane(header.index)
                readonly property bool audio: header.track.kind === "audio"

                width: parent.width
                height: panel.trackHeight
                color: Theme.c.panelHeader
                border.width: 1
                border.color: Theme.c.divider

                Text {
                    id: trackName

                    anchors.left: parent.left
                    anchors.leftMargin: 6
                    anchors.top: parent.top
                    anchors.topMargin: 4
                    text: header.track.name
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    color: header.track.enabled ? Theme.c.text : Theme.c.textMuted
                }

                // Lock, eye, mute, solo: the four an editor reaches for without looking.
                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: 6
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    spacing: 3

                    Repeater {
                        model: header.audio
                               ? [{flag: "locked", glyph: "◯", on: header.track.locked,
                                   tip: qsTr("Lock this track")},
                                  {flag: "muted", glyph: "M", on: header.track.muted,
                                   tip: qsTr("Mute")},
                                  {flag: "solo", glyph: "S", on: header.track.solo,
                                   tip: qsTr("Solo")}]
                               : [{flag: "locked", glyph: "◯", on: header.track.locked,
                                   tip: qsTr("Lock this track")},
                                  {flag: "enabled", glyph: "◉", on: !header.track.enabled,
                                   tip: qsTr("Hide this track's picture")}]

                        delegate: Rectangle {
                            id: flagButton

                            required property var modelData

                            width: 16
                            height: 14
                            radius: 2
                            // Solo is yellow wherever it appears, because it is the one that
                            // silences everything else and has to be noticed.
                            color: flagButton.modelData.on
                                   ? (flagButton.modelData.flag === "solo" ? Theme.c.warning
                                                                           : Theme.c.accent)
                                   : flagMouse.containsMouse ? Theme.c.panelHeaderActive
                                   : "transparent"
                            opacity: flagButton.modelData.on ? 0.75 : 1.0
                            border.width: 1
                            border.color: Theme.c.border

                            Text {
                                anchors.centerIn: parent
                                text: flagButton.modelData.glyph
                                font.pixelSize: 8
                                color: flagButton.modelData.on ? Theme.c.text : Theme.c.textMuted
                            }

                            MouseArea {
                                id: flagMouse

                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: Edit.toggleTrack(header.track.id,
                                                            flagButton.modelData.flag)
                            }

                            ToolTipText {
                                text: flagButton.modelData.tip
                                visible: flagMouse.containsMouse
                                hoverArea: flagMouse
                            }
                        }
                    }
                }

                // Track gain, on audio tracks only: a drag across the header is the fastest
                // way to balance two tracks against each other.
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 5
                    visible: header.audio
                    text: (header.track.gainDb > 0 ? "+" : "") +
                          header.track.gainDb.toFixed(1)
                    font.pixelSize: 9
                    font.family: "monospace"
                    color: gainMouse.containsMouse ? Theme.c.accent : Theme.c.textMuted

                    MouseArea {
                        id: gainMouse

                        anchors.fill: parent
                        anchors.margins: -6
                        hoverEnabled: true
                        enabled: header.audio
                        cursorShape: Qt.SizeVerCursor

                        property real grabY: 0
                        property real grabGain: 0

                        onPressed: mouse => {
                            grabY = mouse.y;
                            grabGain = header.track.gainDb;
                        }

                        onPositionChanged: mouse => {
                            if (!pressed)
                                return;
                            // Up is louder. A tenth of a dB per pixel, so the whole usable
                            // range is a comfortable drag rather than a twitch.
                            const db = Math.max(-60, Math.min(12,
                                grabGain + (grabY - mouse.y) * 0.1));
                            Edit.setTrackGain(header.track.id, db);
                        }

                        onReleased: Edit.endGesture()
                    }

                    ToolTipText {
                        text: qsTr("Drag to set this track's level")
                        visible: gainMouse.containsMouse
                        hoverArea: gainMouse
                    }
                }
            }
        }
    }

    // ---- lanes ----
    Canvas {
        id: lanes

        anchors.fill: parent
        visible: panel.hasDocument
        renderStrategy: Canvas.Cooperative

        // `loadImage` is asynchronous. Without this the paint that asked for the picture
        // finishes before it arrives, and nothing ever asks again: the clips stay flat.
        onImageLoaded: lanes.requestPaint()

        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.fillStyle = Theme.c.window;
            ctx.fillRect(0, 0, width, height);
            if (!panel.hasDocument)
                return;

            const doc = panel.doc;
            const fps = doc.fps > 0 ? doc.fps : 30;

            // ---- ruler ----
            ctx.fillStyle = Theme.c.panelHeader;
            ctx.fillRect(0, 0, width, panel.rulerHeight);

            // A tick every second until seconds get too close together, then every 2, 5, 10,
            // and so on. Chosen from the zoom so labels never collide, which is what keeps the
            // ruler readable at every scale rather than only the one it was designed at.
            const steps = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600];
            let step = steps[steps.length - 1];
            for (const candidate of steps) {
                if (candidate * fps * panel.scale >= 60) {
                    step = candidate;
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

            // ---- the review range ----
            if (doc.inPoint >= 0 || doc.outPoint >= 0) {
                const a = Math.max(doc.inPoint >= 0 ? panel.xOf(doc.inPoint) : panel.headerWidth,
                                   panel.headerWidth);
                const b = doc.outPoint >= 0 ? panel.xOf(doc.outPoint) : width;
                ctx.fillStyle = Theme.c.accent;
                ctx.globalAlpha = 0.10;
                ctx.fillRect(a, panel.rulerHeight, b - a, height - panel.rulerHeight);
                ctx.globalAlpha = 1.0;
            }

            // ---- lanes ----
            for (let lane = 0; lane < panel.tracks.length; ++lane) {
                const track = panel.trackOfLane(lane);
                const y = panel.rulerHeight + lane * panel.trackHeight;

                ctx.fillStyle = lane % 2 === 0 ? Theme.c.panelBackground : Theme.c.window;
                ctx.fillRect(panel.headerWidth, y, width - panel.headerWidth,
                             panel.trackHeight);

                ctx.strokeStyle = Theme.c.divider;
                ctx.beginPath();
                ctx.moveTo(panel.headerWidth, y + panel.trackHeight + 0.5);
                ctx.lineTo(width, y + panel.trackHeight + 0.5);
                ctx.stroke();

                for (const clip of track.clips) {
                    const x0 = panel.xOf(clip.start);
                    const x1 = panel.xOf(clip.start + clip.duration);
                    if (x1 < panel.headerWidth || x0 > width)
                        continue;
                    const left = Math.max(x0, panel.headerWidth);
                    const w = x1 - left;
                    if (w <= 0)
                        continue;

                    const top = y + 3;
                    const boxHeight = panel.trackHeight - 7;
                    const selected = clip.id === Edit.selectedClip;
                    const audio = track.kind === "audio";

                    ctx.fillStyle = audio ? "#204d3b" : "#1d3f6b";
                    ctx.fillRect(left, top, w, boxHeight);

                    // The artwork, clipped to the clip and offset by the trim: a clip trimmed
                    // at the head has to show the part of the file it actually starts at, or
                    // the picture would slide out of step with the cut.
                    const art = Edit.clipArt(clip.source, audio);
                    if (art && w > 4) {
                        ctx.save();
                        ctx.beginPath();
                        ctx.rect(left, top, w, boxHeight);
                        ctx.clip();
                        const image = art;
                        if (lanes.isImageLoaded(image)) {
                            // The whole file spans the clip's *source* length, so the drawn
                            // width is the file scaled to this clip's pixels-per-frame.
                            const known = Edit.sourceFrames(clip.source);
                            const media = known > 0 ? known : clip.duration;
                            const full = media * panel.scale;
                            const originX = x0 - (clip.sourceIn * panel.scale);
                            ctx.globalAlpha = audio ? 0.75 : 1.0;
                            ctx.drawImage(image, originX, top, full, boxHeight);
                            ctx.globalAlpha = 1.0;
                        } else {
                            lanes.loadImage(image);
                        }
                        ctx.restore();
                    }

                    // The transition, drawn across the cut it sits on, as every editor draws
                    // one: a wedge over the overlap rather than a separate block.
                    if (clip.transitionFrames > 0) {
                        const tw = clip.transitionFrames * panel.scale;
                        ctx.save();
                        ctx.beginPath();
                        ctx.rect(Math.max(x0 - tw / 2, panel.headerWidth), top, tw, boxHeight);
                        ctx.clip();
                        ctx.fillStyle = clip.transition === "dip-to-black" ? "#000000"
                                                                           : "#e0e0e0";
                        ctx.globalAlpha = 0.5;
                        ctx.fillRect(x0 - tw / 2, top, tw, boxHeight);
                        ctx.globalAlpha = 1.0;
                        ctx.strokeStyle = "#ffffff";
                        ctx.beginPath();
                        ctx.moveTo(x0 - tw / 2, top + boxHeight);
                        ctx.lineTo(x0 + tw / 2, top);
                        ctx.stroke();
                        ctx.restore();
                    }

                    ctx.strokeStyle = selected ? Theme.c.accent : Theme.c.border;
                    ctx.lineWidth = selected ? 2 : 1;
                    ctx.strokeRect(left + 0.5, top + 0.5, w - 1, boxHeight - 1);
                    ctx.lineWidth = 1;

                    // The name, over a strip dark enough to read it against any picture.
                    if (w > 28) {
                        ctx.save();
                        ctx.beginPath();
                        ctx.rect(left, top, w, boxHeight);
                        ctx.clip();
                        ctx.fillStyle = "rgba(0,0,0,0.55)";
                        ctx.fillRect(left, top, w, 13);
                        ctx.fillStyle = "#ffffff";
                        ctx.font = "10px sans-serif";
                        ctx.fillText(clip.name, left + 4, top + 10);
                        ctx.restore();
                    }

                    // The fx badge: "this shot has been worked on", read at a glance.
                    if (clip.hasEffects && w > 40) {
                        ctx.fillStyle = Theme.c.accent;
                        ctx.fillRect(left + w - 16, top + 2, 14, 10);
                        ctx.fillStyle = "#ffffff";
                        ctx.font = "8px sans-serif";
                        ctx.fillText("fx", left + w - 14, top + 10);
                    }
                }
            }

            // ---- markers ----
            for (const marker of doc.markers ?? []) {
                const x = Math.round(panel.xOf(marker.at)) + 0.5;
                if (x < panel.headerWidth || x > width)
                    continue;
                ctx.fillStyle = "#" + ("000000" + marker.color.toString(16)).slice(-6);
                ctx.beginPath();
                ctx.moveTo(x - 4, 0);
                ctx.lineTo(x + 4, 0);
                ctx.lineTo(x, 8);
                ctx.closePath();
                ctx.fill();
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
                ctx.fillStyle = Theme.c.error;
                ctx.fillRect(px - 4, 0, 8, 5);
            }

            ctx.strokeStyle = Theme.c.divider;
            ctx.beginPath();
            ctx.moveTo(panel.headerWidth + 0.5, 0);
            ctx.lineTo(panel.headerWidth + 0.5, height);
            ctx.stroke();
        }
    }

    // ---- interaction ----
    MouseArea {
        id: surface

        anchors.left: parent.left
        anchors.leftMargin: panel.headerWidth
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
        enabled: panel.hasDocument
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        cursorShape: panel.tool === "hand" ? Qt.OpenHandCursor
                   : panel.tool === "razor" ? Qt.SplitHCursor
                   : Qt.ArrowCursor

        /// What the press grabbed: "playhead", "clip", "head", "tail", "slip", "pan".
        property string dragging: ""
        property int grabFrame: 0
        property int clipStart: 0
        property int clipIn: 0
        property real panFrom: 0

        /// How close to an edge counts as grabbing it rather than the clip.
        readonly property int edgeSlack: 6

        function sceneX(mouse) {
            return mouse.x + panel.headerWidth;
        }

        onPressed: mouse => {
            const x = sceneX(mouse);
            const frame = panel.frameAt(x);

            if (panel.tool === "hand") {
                dragging = "pan";
                grabFrame = frame;
                panFrom = panel.offset;
                return;
            }

            // The ruler always scrubs, whatever tool is up: reaching for the playhead should
            // not mean first putting the razor away.
            if (mouse.y < panel.rulerHeight) {
                dragging = "playhead";
                Edit.pause();
                Edit.setPlayhead(frame);
                return;
            }

            const lane = panel.laneAt(mouse.y);
            if (lane < 0) {
                Edit.selectClip("", "");
                return;
            }
            const track = panel.trackOfLane(lane);
            const hit = Edit.clipAt(track.id, frame);

            if (panel.tool === "razor") {
                if (hit) {
                    Edit.selectClip(track.id, hit.id);
                    Edit.setPlayhead(panel.snap(frame));
                    Edit.razorAtPlayhead();
                }
                return;
            }

            if (!hit) {
                Edit.selectClip("", "");
                dragging = "playhead";
                Edit.pause();
                Edit.setPlayhead(frame);
                return;
            }

            Edit.selectClip(track.id, hit.id);
            grabFrame = frame;
            clipStart = hit.start;
            clipIn = hit.sourceIn;

            if (panel.tool === "slip") {
                dragging = "slip";
                return;
            }

            // Near an edge, trim it; anywhere else, move the clip. The slack is in pixels, so
            // a zoomed-out clip is still grabbable in the middle.
            const fromStart = (frame - hit.start) * panel.scale;
            const fromEnd = (hit.start + hit.duration - frame) * panel.scale;
            if (fromStart <= edgeSlack)
                dragging = "head";
            else if (fromEnd <= edgeSlack)
                dragging = "tail";
            else
                dragging = "clip";
        }

        onPositionChanged: mouse => {
            const x = sceneX(mouse);
            if (dragging === "")
                return;
            const frame = panel.frameAt(x);

            if (dragging === "pan") {
                panel.offset = Math.max(0, panFrom + (grabFrame - frame));
            } else if (dragging === "playhead") {
                Edit.setPlayhead(frame);
            } else if (dragging === "slip") {
                Edit.slipSelectedClip(grabFrame - frame);
                grabFrame = frame;
            } else if (dragging === "clip") {
                // One undo step for the whole drag: the gesture name is the clip, so every
                // frame of movement coalesces into the move the operator actually made.
                Edit.moveSelectedClip(panel.snap(clipStart + (frame - grabFrame)));
            } else if (dragging === "head") {
                Edit.trimSelectedClip(panel.snap(frame), true);
            } else if (dragging === "tail") {
                Edit.trimSelectedClip(panel.snap(frame), false);
            }
        }

        onReleased: {
            if (dragging !== "" && dragging !== "pan" && dragging !== "playhead")
                Edit.endGesture();
            dragging = "";
        }

        onWheel: wheel => {
            // Ctrl zooms about the pointer, so the frame under the cursor stays under it.
            // Plain wheel scrolls, which is what it does everywhere else in the shell.
            if (wheel.modifiers & Qt.ControlModifier) {
                const at = panel.frameAt(sceneX(wheel));
                const factor = wheel.angleDelta.y > 0 ? 1.2 : 1 / 1.2;
                panel.scale = Math.max(0.02, Math.min(40, panel.scale * factor));
                panel.offset = Math.max(0, at - (wheel.x) / panel.scale);
            } else {
                panel.offset = Math.max(0, panel.offset - wheel.angleDelta.y / panel.scale);
            }
        }
    }

    // ---- keys ----
    //
    // The standard set, so an editor's hands work here without being taught: J-K-L to shuttle,
    // space to play, arrows to step, I and O to mark, and the tools on their usual letters.
    focus: panel.hasDocument
    Keys.onPressed: event => {
        switch (event.key) {
        case Qt.Key_Space: Edit.togglePlay(); break;
        case Qt.Key_J: Edit.shuttle(-1); break;
        case Qt.Key_K: Edit.pause(); break;
        case Qt.Key_L: Edit.shuttle(1); break;
        case Qt.Key_Left: Edit.step(event.modifiers & Qt.ShiftModifier ? -10 : -1); break;
        case Qt.Key_Right: Edit.step(event.modifiers & Qt.ShiftModifier ? 10 : 1); break;
        case Qt.Key_Up: Edit.stepEdit(false); break;
        case Qt.Key_Down: Edit.stepEdit(true); break;
        case Qt.Key_Home: Edit.goToStart(); break;
        case Qt.Key_End: Edit.goToEnd(); break;
        case Qt.Key_I: Edit.setInPoint(); break;
        case Qt.Key_O: Edit.setOutPoint(); break;
        case Qt.Key_M: Edit.addMarkerAtPlayhead(); break;
        case Qt.Key_S: panel.snapping = !panel.snapping; break;
        case Qt.Key_V: panel.tool = "select"; break;
        case Qt.Key_B: panel.tool = "ripple"; break;
        case Qt.Key_Y: panel.tool = "slip"; break;
        case Qt.Key_C: panel.tool = "razor"; break;
        case Qt.Key_H: panel.tool = "hand"; break;
        case Qt.Key_Delete:
        case Qt.Key_Backspace:
            // Shift-delete ripples, plain delete leaves the gap: the same split every editor
            // makes, because both are wanted often enough to deserve their own key.
            if (event.modifiers & Qt.ShiftModifier)
                Edit.rippleDeleteSelectedClip();
            else
                Edit.removeSelectedClip();
            break;
        default:
            return;
        }
        event.accepted = true;
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
            text: panel.hasDocument
                  ? Edit.timecode(panel.doc.playhead) + "  ·  " + panel.doc.name +
                    (Edit.dirty ? " •" : "")
                  : ""
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            ToolButton {
                label: panel.snapping ? "⧗" : "✕"
                tip: panel.snapping ? qsTr("Snapping on") : qsTr("Snapping off")
                active: panel.snapping
                onClicked: panel.snapping = !panel.snapping
            }

            ToolButton {
                label: "✂"
                tip: qsTr("Cut every track at the playhead")
                onClicked: Edit.razorAtPlayhead()
            }

            ToolButton {
                label: "⧉"
                tip: qsTr("Add a cross dissolve to the selected clip")
                enabled: Edit.selectedClip !== ""
                onClicked: Edit.setTransition("cross-dissolve",
                                              Math.round((panel.doc.fps ?? 30) * 0.5))
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
