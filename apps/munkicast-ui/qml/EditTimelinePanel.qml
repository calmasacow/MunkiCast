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

    /// The whole left gutter: the tool rail plus the track headers beside it.
    readonly property int headerWidth: 208
    readonly property int railWidth: 26
    readonly property int rulerHeight: 22
    readonly property int trackHeight: 48

    readonly property var doc: Edit.timeline
    readonly property bool hasDocument: Edit.hasTimeline
    // `|| []`: a timeline just made is announced before its tracks are filled in.
    readonly property var tracks: panel.hasDocument ? (panel.doc.tracks || []) : []

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
    /// The name an operator reads on a transition box.
    function transitionLabel(kind) {
        switch (kind) {
        case "cross-dissolve": return qsTr("Cross Dissolve");
        case "dip-to-black":   return qsTr("Dip to Black");
        case "push":           return qsTr("Push");
        default:               return "";
        }
    }

    /// Put the whole sequence on screen, with a little air after the last clip.
    function zoomToFit() {
        const total = panel.hasDocument ? (panel.doc.duration || 0) : 0;
        const room = lanes.width - panel.headerWidth;
        if (total <= 0 || room <= 0)
            return;
        panel.offset = 0;
        panel.scale = Math.max(0.02, Math.min(40, room / (total * 1.03)));
    }

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

    /// The tracks in the order their lanes are drawn, top to bottom.
    ///
    /// Video above audio, always: an editor reads the picture at the top and the sound beneath
    /// it. Within each half the conventions differ and both are kept -- video counts *up* the
    /// screen, so V2 sits over V1 because it composites over it, while audio counts *down*,
    /// A1 first, because nothing stacks and reading order wins.
    readonly property var lanes: {
        const video = [];
        const audio = [];
        for (const track of panel.tracks)
            (track.kind === "audio" ? audio : video).push(track);
        return video.reverse().concat(audio);
    }

    function trackOfLane(lane) {
        return panel.lanes[lane];
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
    // A rail down the left edge, its own column rather than something laid over the headers.
    // `headerWidth` counts it, so the headers and the canvas both start clear of it and a
    // track name keeps the width it needs.
    Column {
        id: toolbar

        anchors.left: parent.left
        anchors.top: parent.top
        anchors.topMargin: panel.rulerHeight + 2
        width: panel.railWidth
        spacing: 2
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
                height: 22
                anchors.horizontalCenter: parent.horizontalCenter
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
        anchors.leftMargin: panel.railWidth
        anchors.top: parent.top
        anchors.topMargin: panel.rulerHeight
        width: panel.headerWidth - panel.railWidth
        visible: panel.hasDocument
        z: 2

        Repeater {
            model: panel.tracks.length

            delegate: Rectangle {
                id: header

                required property int index

                // `|| ({})`: for a moment after tracks change, a delegate outlives its lane.
                readonly property var track: panel.trackOfLane(header.index) || ({})
                readonly property bool audio: header.track.kind === "audio"

                width: parent.width
                height: panel.trackHeight
                color: Theme.c.panelHeader
                border.width: 1
                border.color: Theme.c.divider

                // The source patch: which track an insert from the Source viewer lands on.
                // Every NLE puts this at the head of the track as a short badge, because it
                // is read far more often than it is changed.
                Rectangle {
                    id: patch

                    anchors.left: parent.left
                    anchors.leftMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    width: 22
                    height: 20
                    radius: 2
                    color: header.track.patched ? Theme.c.accent : Theme.c.panelBackground
                    opacity: header.track.patched ? 0.8 : 1.0
                    border.width: 1
                    border.color: Theme.c.border

                    Text {
                        anchors.centerIn: parent
                        text: header.track.shortName
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        color: header.track.patched ? Theme.c.text : Theme.c.textMuted
                    }

                    MouseArea {
                        id: patchMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: Edit.toggleTrack(header.track.id, "patched")
                    }

                    ToolTipText {
                        text: qsTr("Patch the source to this track")
                        visible: patchMouse.containsMouse
                        hoverArea: patchMouse
                    }
                }

                Text {
                    id: trackName

                    anchors.left: patch.right
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
                    anchors.left: patch.right
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
                                   tip: qsTr("Solo")},
                                  {flag: "armed", glyph: "●", on: header.track.armed,
                                   tip: qsTr("Arm this track for recording")}]
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
                          (header.track.gainDb || 0).toFixed(1)
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

                    // The transition: a named box over the cut, hatched like every NLE draws
                    // one, placed by its own alignment rather than always centred. `lead` is
                    // how far it reaches back before the cut, worked out by the document.
                    if (clip.transitionFrames > 0) {
                        const tw = clip.transitionFrames * panel.scale;
                        const tx = x0 - (clip.transitionLead * panel.scale);
                        ctx.save();
                        ctx.beginPath();
                        ctx.rect(Math.max(tx, panel.headerWidth), top, tw, boxHeight);
                        ctx.clip();

                        ctx.fillStyle = clip.transition === "dip-to-black" ? "#101010"
                                                                          : "#6d5a8c";
                        ctx.fillRect(tx, top, tw, boxHeight);

                        // The hatching, which is what makes a transition readable as one at a
                        // glance rather than as a very short clip.
                        ctx.strokeStyle = "#ffffff";
                        ctx.globalAlpha = 0.35;
                        ctx.beginPath();
                        for (let hx = tx - boxHeight; hx < tx + tw; hx += 7) {
                            ctx.moveTo(hx, top + boxHeight);
                            ctx.lineTo(hx + boxHeight, top);
                        }
                        ctx.stroke();
                        ctx.globalAlpha = 1.0;

                        if (tw > 34) {
                            ctx.fillStyle = "#ffffff";
                            ctx.font = "9px sans-serif";
                            ctx.fillText(panel.transitionLabel(clip.transition), tx + 3,
                                         top + 10);
                        }

                        ctx.strokeStyle = Theme.c.border;
                        ctx.strokeRect(tx + 0.5, top + 0.5, tw - 1, boxHeight - 1);
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
    // J-K-L and space, as shortcuts rather than key presses: `Keys` needs this item to hold
    // keyboard focus, and a click on a clip or a button took it away, so they went dead.
    TransportKeys {
        panelId: "edit-timeline"
        onReverse: Edit.shuttle(-1)
        onStop: Edit.pause()
        onForward: Edit.shuttle(1)
        onToggle: Edit.togglePlay()
    }

    // The rest of the standard set, so an editor's hands work here without being taught:
    // arrows to step, I and O to mark, and the tools on their usual letters.
    focus: panel.hasDocument
    Keys.onPressed: event => {
        switch (event.key) {
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
            id: timecodeLabel

            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: 10
            font.family: "monospace"
            color: Theme.c.textMuted
            text: panel.hasDocument
                  ? Edit.timecode(panel.doc.playhead) + "  ·  " + panel.doc.name +
                    (Edit.dirty ? " •" : "")
                  : ""
        }

        // The zoom, as a bar rather than only a modifier-wheel gesture: a control you can see
        // is one you can find, and "fit" is the thing an editor reaches for most.
        Row {
            anchors.left: timecodeLabel.right
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            visible: panel.hasDocument

            ToolButton {
                label: "−"
                tip: qsTr("Zoom out")
                onClicked: panel.scale = Math.max(0.02, panel.scale / 1.4)
            }

            Rectangle {
                id: zoomWell

                anchors.verticalCenter: parent.verticalCenter
                width: 80
                height: 4
                radius: 2
                color: Theme.c.panelBackground

                // The bar is logarithmic because the zoom is: half the useful range sits
                // under one pixel per frame, and a linear bar would hide all of it in the
                // first two millimetres.
                readonly property real fraction: {
                    const low = Math.log(0.02);
                    const high = Math.log(40);
                    return (Math.log(panel.scale) - low) / (high - low);
                }

                Rectangle {
                    x: Math.max(0, Math.min(zoomWell.width - 6, zoomWell.fraction * zoomWell.width))
                    y: -3
                    width: 6
                    height: 10
                    radius: 2
                    color: Theme.c.accent
                }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    onPressed: mouse => zoomWell.setFrom(mouse.x)
                    onPositionChanged: mouse => {
                        if (pressed)
                            zoomWell.setFrom(mouse.x);
                    }
                }

                function setFrom(x) {
                    const f = Math.max(0, Math.min(1, x / zoomWell.width));
                    const low = Math.log(0.02);
                    const high = Math.log(40);
                    panel.scale = Math.exp(low + f * (high - low));
                }
            }

            ToolButton {
                label: "+"
                tip: qsTr("Zoom in")
                onClicked: panel.scale = Math.min(40, panel.scale * 1.4)
            }

            ToolButton {
                label: qsTr("Fit")
                tip: qsTr("Fit the whole sequence on screen")
                onClicked: panel.zoomToFit()
            }
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

    // A range dragged out of the Source viewer lands on the track under the pointer, at the
    // frame under it. Holding Alt records over what is there instead of pushing it along --
    // the same two edits the Source viewer's own buttons make, reached by dropping.
    DropArea {
        id: timelineDrop

        objectName: "timelineDrop"
        anchors.fill: parent
        // The Source viewer's marked range, or a whole file carried straight from the pool.
        keys: ["application/x-munkicast-source", "text/uri-list"]

        property int dropFrame: 0
        property string dropTrack: ""

        function aim(x, y) {
            timelineDrop.dropFrame = panel.snap(panel.frameAt(x));
            const lane = panel.laneAt(y);
            const track = lane >= 0 ? panel.trackOfLane(lane) : null;
            timelineDrop.dropTrack = track ? track.id : "";
        }

        onPositionChanged: (drag) => timelineDrop.aim(drag.x, drag.y)
        /// The drag over this came from the pool, and its payload was made on the way in.
        property bool fromPool: false

        onEntered: (drag) => {
            // From the pool the file rides on the ghost; nothing has been marked, so the
            // whole of it goes.
            timelineDrop.fromPool = !!drag.source && drag.source.objectName === "poolDragGhost";
            if (timelineDrop.fromPool)
                Edit.beginFileDrag(drag.source.path);
            timelineDrop.aim(drag.x, drag.y);
        }
        onExited: {
            timelineDrop.dropTrack = "";
            // Carried on past, to the Source viewer say: nothing is on its way here.
            if (timelineDrop.fromPool)
                Edit.cancelSourceDrag();
            timelineDrop.fromPool = false;
        }

        onDropped: (drop) => {
            if (timelineDrop.dropTrack !== "") {
                Edit.dropOnTimeline(timelineDrop.dropTrack, timelineDrop.dropFrame,
                                    (drop.modifiers & Qt.AltModifier) !== 0);
                drop.accept();
            } else if (timelineDrop.fromPool) {
                Edit.cancelSourceDrag();
            }
            timelineDrop.dropTrack = "";
            timelineDrop.fromPool = false;
        }

        // Where it would land, drawn on the track it would land on: dropping a shot without
        // being shown where it goes is guesswork.
        Rectangle {
            visible: timelineDrop.containsDrag && timelineDrop.dropTrack !== ""
            x: panel.xOf(timelineDrop.dropFrame)
            y: {
                for (let lane = 0; lane < panel.lanes.length; ++lane) {
                    if (panel.lanes[lane].id === timelineDrop.dropTrack)
                        return panel.rulerHeight + lane * panel.trackHeight + 3;
                }
                return 0;
            }
            width: Math.max(2, (Edit.dragPayload.frames || 0) * panel.scale)
            height: panel.trackHeight - 7
            color: Theme.c.accent
            opacity: 0.35
            border.width: 1
            border.color: Theme.c.text
        }
    }
}
