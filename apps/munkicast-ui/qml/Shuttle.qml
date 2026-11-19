// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtMultimedia
import MunkiCast

/// J-K-L for a player of one file.
///
/// Forwards, at any speed, the player plays at that rate: its decoder keeps up to 32x on
/// 1080p and every frame it gets to is drawn, sound and all, pitch as it falls. Parking the
/// player on a computed position every tick instead -- what this did first -- froze the
/// picture: on a long-GOP recording each seek decodes from the last keyframe, a new seek
/// cancelled it before it finished, and nothing reached the screen until normal speed.
///
/// Backwards a player cannot play, so it is parked, as Filmcraft parks its source monitor:
/// the position is computed from when the run started and the rate, never accumulated, and
/// the next seek waits for the last one's frame to arrive. That is the frame skipping, and
/// it always shows something.
///
/// ponytail: reverse is silent. Backwards sound needs its own mixer; add when asked.
Item {
    id: shuttle

    required property MediaPlayer player
    /// Where the player draws: its sink says when a parked seek's frame has landed.
    required property VideoOutput output

    /// Times real time: negative backwards, 0 stopped. Read from the player when nothing
    /// here is running, so a play button that talks to the player directly cannot leave it
    /// stale.
    readonly property real rate: clock.running ? shuttle.fast
                                 : shuttle.player.playbackState === MediaPlayer.PlayingState
                                   ? shuttle.player.playbackRate : 0

    property real fast: 0
    property real anchorMs: 0
    property real anchorAt: 0
    /// A seek is out and its frame not yet drawn, since `sentAt`.
    property bool waiting: false
    property real sentAt: 0

    function now() {
        if (!clock.running)
            return shuttle.player.position;
        const at = shuttle.anchorMs + (Date.now() - shuttle.anchorAt) * shuttle.fast;
        return Math.max(0, Math.min(at, shuttle.player.duration));
    }

    function setRate(r) {
        const from = shuttle.now();
        const wasParked = clock.running;
        clock.stop();
        if (r > 0) {
            shuttle.player.playbackRate = r;
            if (wasParked)
                shuttle.player.position = from;
            shuttle.player.play();
            return;
        }
        shuttle.player.pause();
        shuttle.player.playbackRate = 1;
        shuttle.player.position = from;
        if (r < 0) {
            shuttle.fast = r;
            shuttle.anchorMs = from;
            shuttle.anchorAt = Date.now();
            shuttle.waiting = false;
            clock.start();
        }
    }

    function forward() { shuttle.setRate(Edit.shuttleRate(shuttle.rate, 1)); }
    function reverse() { shuttle.setRate(Edit.shuttleRate(shuttle.rate, -1)); }
    function stop() { shuttle.setRate(0); }
    function toggle() { shuttle.setRate(shuttle.rate === 0 ? 1 : 0); }

    /// A jump that keeps whatever speed is running, from the new place.
    function seek(ms) {
        shuttle.anchorMs = ms;
        shuttle.anchorAt = Date.now();
        shuttle.waiting = false;
        shuttle.player.position = ms;
    }

    Connections {
        target: shuttle.output ? shuttle.output.videoSink : null
        function onVideoFrameChanged() { shuttle.waiting = false; }
    }

    Timer {
        id: clock
        interval: 15
        repeat: true
        onTriggered: {
            const at = shuttle.now();
            if (at <= 0) {
                shuttle.player.position = 0;
                clock.stop();
                return;
            }
            // One seek in flight at a time: a seek issued over an unfinished one cancels
            // its decode, and a stream of them draws nothing at all.
            // Not forever, though: a seek that lands on the frame already shown draws
            // nothing new, and the wait would never end.
            if (!shuttle.waiting || Date.now() - shuttle.sentAt > 250) {
                shuttle.waiting = true;
                shuttle.sentAt = Date.now();
                shuttle.player.position = at;
            }
        }
    }
}
