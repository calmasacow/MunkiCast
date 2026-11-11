// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma Singleton
pragma ComponentBehavior: Bound

import QtQml
import QtQml.Models

/// Everything the shell has said, kept.
///
/// The status line shows one notice for a few seconds and then forgets it, which is right for a
/// line read at a glance while something else has the operator's attention -- and wrong as the
/// only record. A message about a recovered save or a refused drop is worth being able to go back
/// and read, especially the ones that arrive while the operator is mid-show and not looking.
QtObject {
    id: log

    readonly property ListModel entries: ListModel {}
    property int limit: 500

    function add(text) {
        if (text === undefined || text === "")
            return;
        const now = new Date();
        log.entries.insert(0, {
            "time": Qt.formatTime(now, "HH:mm:ss"),
            "text": text
        });
        while (log.entries.count > log.limit)
            log.entries.remove(log.entries.count - 1);
    }

    function clear() {
        log.entries.clear();
    }
}
