// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick

/// Two links of a chain, drawn rather than set in type.
///
/// The obvious spellings do not work. U+26D3 and U+1F517 both resolve to Noto Color Emoji on
/// this system, and a colour-emoji glyph cannot be tinted -- so a linked row and an unlinked one
/// would be the same shade of nothing-in-particular, which is the one thing this icon exists to
/// distinguish. Two rounded rectangles cost nothing and take `color` like everything else.
///
/// 16px is the floor. At 14 the two links smear into one blob at this stroke width; if the row
/// gets tighter, drop the icon rather than the size.
Item {
    id: chain

    property color color: "white"
    /// Drawn heavier when something is actually linked, so the difference reads at a glance and
    /// not only by hue -- the panel is used by people looking at six other things at once.
    property bool strong: false
    property bool broken: false

    implicitWidth: 16
    implicitHeight: 16

    Repeater {
        model: 2

        delegate: Rectangle {
            required property int index

            // Two ovals on a 45-degree axis, overlapping in the middle so they read as
            // interlocked rather than as two separate pills.
            width: chain.width * 0.60
            height: chain.height * 0.36
            radius: height / 2
            color: "transparent"
            border.width: chain.strong ? 1.6 : 1.2
            border.color: chain.color
            rotation: -45
            antialiasing: true
            x: (chain.width - width) / 2 + (index === 0 ? -chain.width * (chain.broken ? 0.26 : 0.14)
                                                        : chain.width * (chain.broken ? 0.26 : 0.14))
            y: (chain.height - height) / 2 + (index === 0 ? -chain.height * (chain.broken ? 0.26 : 0.14)
                                                          : chain.height * (chain.broken ? 0.26 : 0.14))
        }
    }
}
