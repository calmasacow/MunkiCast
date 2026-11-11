// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick

/// Wheel scrolling for a Flickable that cannot be dragged.
///
/// Nothing in this application scrolls by being dragged (ADR-0039). A list in a studio tool is
/// a list of things to *grab* -- a source to reorder, a strip to ride, a scene to take -- and a
/// surface that moves under the pointer when you pull on it will eventually move while someone
/// is trying to pull on what is in it. Setting `interactive: false` is what turns that off, and
/// in Qt it turns the wheel off with it: Flickable::wheelEvent returns early when the flickable
/// is not interactive. So the wheel has to be given back, and this is where.
///
/// Declared inside the Flickable with no arguments -- a pointer handler's `parent` is the item
/// it was declared in, which is the thing to scroll.
///
/// A control inside one of these that wants the wheel for itself -- a fader being trimmed --
/// declares its own WheelHandler. Qt offers a wheel to handlers from the innermost item
/// outward, so the deeper one wins and this never sees it. A MouseArea's `onWheel` does *not*
/// win that contest, which is why MixerFader uses a handler rather than its MouseArea.
WheelHandler {
    id: wheel

    /// The thing to scroll. A handler declared inside a Flickable has it as `parent`; inside
    /// a ListView the declaration may land on the content item instead, so walk up until a
    /// Flickable is found rather than assuming which.
    readonly property Flickable view: {
        let at = wheel.parent;
        while (at && !(at instanceof Flickable))
            at = at.parent;
        return at as Flickable;
    }

    // A touchpad sends pixelDelta and a mouse sends angleDelta in eighths of a degree, 120 to
    // the notch. Three rows a notch, which is the usual.
    readonly property real notch: 78

    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
    // acceptedModifiers is left at its default, which is "any". Writing
    // `Qt.NoModifier | Qt.ShiftModifier` looks like "plain or shifted" and is not: NoModifier
    // is 0, so the OR is exactly ShiftModifier and a plain wheel stops working. The branch
    // below reads the modifier itself.

    onWheel: (event) => {
        if (!wheel.view)
            return;
        const overY = wheel.view.contentHeight - wheel.view.height;
        const overX = wheel.view.contentWidth - wheel.view.width;
        // A horizontal strip of faders scrolls sideways from a plain wheel, because there is
        // no other direction for it to go.
        const sideways = overY <= 0 || (event.modifiers & Qt.ShiftModifier) !== 0;

        const pixels = event.pixelDelta.y !== 0 || event.pixelDelta.x !== 0;
        const delta = sideways
            ? (pixels ? (event.pixelDelta.x !== 0 ? event.pixelDelta.x : event.pixelDelta.y)
                      : (event.angleDelta.x !== 0 ? event.angleDelta.x : event.angleDelta.y)
                        / 120 * wheel.notch)
            : (pixels ? event.pixelDelta.y : event.angleDelta.y / 120 * wheel.notch);

        if (sideways) {
            if (overX <= 0)
                return;
            wheel.view.contentX = Math.max(0, Math.min(overX, wheel.view.contentX - delta));
        } else {
            if (overY <= 0)
                return;
            wheel.view.contentY = Math.max(0, Math.min(overY, wheel.view.contentY - delta));
        }
    }
}
