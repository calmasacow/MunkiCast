// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// One vertical fader: track, unity mark, fill and knob.
///
/// Extracted when the mixer grew source strips beside its bus strips. The two carry different
/// things -- a source strip is a place audio comes from, a bus strip where it goes -- but the
/// fader itself is the same control, and two copies of a drag-and-collapse gesture is two
/// places for it to be subtly wrong.
///
/// The value under the finger is kept here, in `shownGain`, and only follows the document while
/// nothing is being dragged. Without that the knob fights the model on every move. The owner
/// wires `moved`, `released` and `reset` to whichever command the strip stands for; this item
/// runs none of them itself, so it cannot know -- or get wrong -- which object it is editing.
Item {
    id: fader

    /// What the document holds. Taken up immediately except mid-drag.
    property real gainDb: 0
    property bool muted: false
    property bool meterAvailable: false
    property real peakLeft: 0
    property real peakRight: 0
    /// dB at the top and bottom of the travel.
    property real gainMin: -60
    property real gainMax: 12

    /// The fader will not move (ADR-0032). Refused here rather than by not wiring the signals,
    /// so the knob can say so before the refusal is discovered by pushing it.
    property bool locked: false

    property bool dragging: false
    property real shownGain: fader.gainDb

    /// Dragged to `db`, which is a step within one gesture rather than a finished edit.
    signal moved(real db)
    /// Let go: the owner closes the gesture, which is what separates one drag from the next.
    signal released
    /// Double-clicked, which everywhere in audio means unity.
    signal reset

    function fraction(db) {
        return (Math.max(fader.gainMin, Math.min(fader.gainMax, db)) - fader.gainMin) /
               (fader.gainMax - fader.gainMin);
    }

    function dbAt(y) {
        const usable = fader.height - knob.height;
        if (usable <= 0)
            return fader.shownGain;
        const f = 1 - (y - knob.height / 2) / usable;
        const db = fader.gainMin + Math.max(0, Math.min(1, f)) * (fader.gainMax - fader.gainMin);
        // Quarter-dB steps: fine enough to ride a fader, coarse enough to nudge.
        return Math.round(db * 4) / 4;
    }

    onGainDbChanged: {
        if (!fader.dragging)
            fader.shownGain = fader.gainDb;
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 4
        color: Theme.c.window
        visible: fader.meterAvailable
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: parent.height * Math.max(0, Math.min(1, (20 * Math.log(Math.max(fader.peakLeft, 0.001)) / Math.LN10 + 60) / 60))
            color: fader.peakLeft >= 0.99 ? Theme.c.warning : Theme.c.accent
        }
    }
    Rectangle {
        anchors.left: parent.left
        anchors.leftMargin: 6
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 4
        color: Theme.c.window
        visible: fader.meterAvailable
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: parent.height * Math.max(0, Math.min(1, (20 * Math.log(Math.max(fader.peakRight, 0.001)) / Math.LN10 + 60) / 60))
            color: fader.peakRight >= 0.99 ? Theme.c.warning : Theme.c.accent
        }
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 6
        radius: 3
        color: Theme.c.window
        border.width: 1
        border.color: Theme.c.border
    }

    // Unity, so the fader has somewhere to be read against.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: (fader.height - knob.height) * (1 - fader.fraction(0)) + knob.height / 2
        width: 16
        height: 1
        color: Theme.c.border
    }

    // Filled from the bottom up to where the fader sits.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        width: 6
        radius: 3
        height: fader.height * fader.fraction(fader.shownGain)
        color: fader.muted ? Theme.c.textMuted : Theme.c.accent
        opacity: fader.muted ? 0.4 : 0.75
    }

    Rectangle {
        id: knob
        objectName: "mixerFaderHandle"

        anchors.horizontalCenter: parent.horizontalCenter
        y: (fader.height - height) * (1 - fader.fraction(fader.shownGain))
        width: 24
        height: 10
        radius: 2
        color: fader.locked ? Theme.c.panelHeader
             : mouse.pressed ? Theme.c.accent
             : Theme.c.panelHeaderActive
        border.width: 1
        border.color: fader.locked ? Theme.c.textMuted
                    : mouse.containsMouse || mouse.pressed ? Theme.c.accent
                    : Theme.c.border
    }

    MouseArea {
        id: mouse

        objectName: "mixerFader"
        anchors.fill: parent
        hoverEnabled: true

        onPressed: (ev) => {
            if (fader.locked)
                return;
            fader.dragging = true;
            fader.shownGain = fader.dbAt(ev.y);
            fader.moved(fader.shownGain);
        }
        onPositionChanged: (ev) => {
            if (!fader.dragging)
                return;
            fader.shownGain = fader.dbAt(ev.y);
            fader.moved(fader.shownGain);
        }
        onReleased: {
            if (!fader.dragging)
                return;
            fader.dragging = false;
            fader.moved(fader.shownGain);
            fader.released();
        }
        onCanceled: {
            // Grab stolen mid-drag: close the gesture and go back to following the document,
            // which already holds wherever the drag got to.
            fader.dragging = false;
            fader.released();
            fader.shownGain = fader.gainDb;
        }
        onDoubleClicked: {
            if (fader.locked)
                return;
            fader.dragging = false;
            fader.shownGain = 0;
            fader.released();
            fader.reset();
        }
    }

    /// A wheel is the keyboard-adjacent way to trim a fader, and one notch is one command, so
    /// it does not need the drag treatment.
    ///
    /// A handler rather than the MouseArea's `onWheel`, because the mixer's strip list is a
    /// Flickable with a NoDragScroll on it (ADR-0039) and a WheelHandler outranks any
    /// MouseArea beneath it. Qt offers the event innermost-first, so this one -- being deeper
    /// -- takes it and the list does not scroll under the pointer.
    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        enabled: !fader.locked

        onWheel: (event) => {
            const step = (event.modifiers & Qt.ShiftModifier) ? 0.25 : 1.0;
            const up = event.angleDelta.y !== 0 ? event.angleDelta.y > 0
                                                : event.pixelDelta.y > 0;
            fader.moved(Math.max(fader.gainMin,
                                 Math.min(fader.gainMax,
                                          fader.gainDb + (up ? step : -step))));
        }
    }
}
