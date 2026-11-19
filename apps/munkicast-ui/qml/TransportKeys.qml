// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import MunkiCast

/// J, K, L and space for a panel with a player, live only while that panel is the focused one.
///
/// Shortcuts rather than `Keys`: `Keys` needs the panel to hold keyboard focus, and clicking a
/// player's picture gives focus to nothing. Gated on the focused panel so two players on screen
/// never both answer -- two live shortcuts on one key is an ambiguity Qt answers with neither.
Item {
    id: keys

    required property string panelId
    readonly property bool live: Workspace.focusedPanel === keys.panelId && keys.visible && keys.enabled

    signal reverse()
    signal stop()
    signal forward()
    signal toggle()

    Shortcut { sequence: "J"; enabled: keys.live; onActivated: keys.reverse() }
    Shortcut { sequence: "K"; enabled: keys.live; onActivated: keys.stop() }
    Shortcut { sequence: "L"; enabled: keys.live; onActivated: keys.forward() }
    Shortcut { sequence: "Space"; enabled: keys.live; onActivated: keys.toggle() }
}
