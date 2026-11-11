// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// A drawn menu. Not QtQuick.Controls' Menu: a styled popup brings a platform style to fight
/// with the theme, for the sake of a rectangle and some rows of text (ADR-0015, ADR-0032).
///
/// A row is {text, checked, action, shortcut, enabled, separator, children}. Every field but
/// `text` may be left out, which is what the mixer and the sources list pass. `children` opens
/// a flyout. `enabled: false` draws the row and does not run `action`.
///
/// Placed by its caller in the panel's coordinates and kept inside it: a menu opened on the
/// rightmost strip must not hang off the edge of a docked panel, where there is no window to
/// spill into.
Item {
    id: menu

    property var items: []
    property var flyoutRows: []
    property bool flyoutOpen: false
    property real flyoutX: 0
    property real flyoutY: 0
    property real flyoutWidth: 150

    anchors.fill: parent
    visible: false

    FontMetrics {
        id: metrics
        font.pixelSize: 11
    }

    function rowBlockHeight(rows) {
        let h = 0;
        const list = rows || [];
        for (let i = 0; i < list.length; ++i)
            h += list[i] && list[i].separator ? 9 : 24;
        return h;
    }

    function rowBlockWidth(rows) {
        let wide = false;
        let w = 150;
        const list = rows || [];
        for (let i = 0; i < list.length; ++i) {
            const row = list[i];
            if (!row || row.separator)
                continue;
            const cut = row.shortcut ? row.shortcut : "";
            const kids = row.children && row.children.length > 0;
            if (cut !== "" || kids)
                wide = true;
            const need = 28 + metrics.advanceWidth(row.text || "") +
                         (cut !== "" ? 16 + metrics.advanceWidth(cut) : 0) +
                         (kids ? 18 : 0);
            w = Math.max(w, need);
        }
        return wide ? Math.min(Math.ceil(w), 360) : 150;
    }

    function popup(x, y, rows) {
        menu.items = rows;
        menu.flyoutOpen = false;
        const w = menu.rowBlockWidth(rows);
        const h = menu.rowBlockHeight(rows) + 8;
        frame.x = Math.max(4, Math.min(x, menu.width - w - 4));
        frame.y = Math.max(4, Math.min(y, menu.height - h - 4));
        menu.visible = true;
        menu.forceActiveFocus();
    }

    function openFlyout(rowItem, children) {
        const w = menu.rowBlockWidth(children);
        const h = menu.rowBlockHeight(children) + 8;
        menu.flyoutRows = children;
        menu.flyoutWidth = w;
        const at = rowItem.mapToItem(menu, 0, 0);
        const right = at.x + rowItem.width - 2;
        const left = at.x - w + 2;
        menu.flyoutX = (right + w <= menu.width - 4) ? right : Math.max(4, left);
        menu.flyoutY = Math.max(4, Math.min(at.y, menu.height - h - 4));
        menu.flyoutOpen = true;
    }

    function close() {
        menu.flyoutOpen = false;
        menu.visible = false;
    }

    Keys.onEscapePressed: menu.close()

    // Catches the click that dismisses it, including a right-click somewhere else.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: menu.close()
        onWheel: (wheel) => { wheel.accepted = true; }
    }

    Rectangle {
        id: frame

        objectName: "stripMenu"
        width: menu.rowBlockWidth(menu.items)
        height: menu.rowBlockHeight(menu.items) + 8
        color: Theme.c.panelBackground
        border.width: 1
        border.color: Theme.c.accent
        radius: 3
        z: 1

        Column {
            id: rows

            y: 4
            width: parent.width

            Repeater {
                model: menu.items

                delegate: Rectangle {
                    id: row

                    required property var modelData
                    required property int index

                    readonly property bool separator: !!row.modelData.separator
                    readonly property bool rowEnabled: row.modelData.enabled !== false
                    readonly property bool hasChildren: !!(row.modelData.children &&
                                                           row.modelData.children.length > 0)
                    property string label: row.modelData.text || ""

                    objectName: row.separator ? "stripMenuSeparator" : "stripMenuItem"
                    width: rows.width
                    height: row.separator ? 9 : 24
                    color: !row.separator && row.rowEnabled && rowMouse.containsMouse
                           ? Theme.c.panelHeaderActive : "transparent"

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        height: 1
                        visible: row.separator
                        color: Theme.c.border
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12
                        visible: !row.separator
                        text: row.modelData.checked ? "✓" : ""
                        font.pixelSize: 10
                        color: Theme.c.accent
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 24
                        anchors.right: side.left
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !row.separator
                        text: row.label
                        font.pixelSize: 11
                        color: row.rowEnabled ? Theme.c.text : Theme.c.textMuted
                        elide: Text.ElideRight
                    }

                    Text {
                        id: side

                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !row.separator && (row.modelData.shortcut || row.hasChildren)
                        text: row.hasChildren ? "▸" : (row.modelData.shortcut || "")
                        font.pixelSize: 11
                        color: Theme.c.textMuted
                    }

                    MouseArea {
                        id: rowMouse

                        anchors.fill: parent
                        enabled: !row.separator
                        hoverEnabled: true
                        onContainsMouseChanged: {
                            if (!rowMouse.containsMouse)
                                return;
                            if (row.hasChildren)
                                menu.openFlyout(row, row.modelData.children);
                            else
                                menu.flyoutOpen = false;
                        }
                        onClicked: {
                            if (!row.rowEnabled)
                                return;
                            if (row.hasChildren) {
                                menu.openFlyout(row, row.modelData.children);
                                return;
                            }
                            if (row.modelData.action)
                                row.modelData.action();
                            menu.close();
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        id: fly

        objectName: "stripMenuFlyout"
        visible: menu.visible && menu.flyoutOpen && menu.flyoutRows.length > 0
        x: menu.flyoutX
        y: menu.flyoutY
        width: menu.flyoutWidth
        height: menu.rowBlockHeight(menu.flyoutRows) + 8
        color: Theme.c.panelBackground
        border.width: 1
        border.color: Theme.c.accent
        radius: 3
        z: 2

        Column {
            id: flyRows

            y: 4
            width: parent.width

            Repeater {
                model: menu.flyoutRows

                delegate: Rectangle {
                    id: flyRow

                    required property var modelData

                    readonly property bool separator: !!flyRow.modelData.separator
                    readonly property bool rowEnabled: flyRow.modelData.enabled !== false
                    property string label: flyRow.modelData.text || ""

                    objectName: flyRow.separator ? "stripMenuSeparator" : "stripMenuItem"
                    width: flyRows.width
                    height: flyRow.separator ? 9 : 24
                    color: !flyRow.separator && flyRow.rowEnabled && flyMouse.containsMouse
                           ? Theme.c.panelHeaderActive : "transparent"

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        height: 1
                        visible: flyRow.separator
                        color: Theme.c.border
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12
                        visible: !flyRow.separator
                        text: flyRow.modelData.checked ? "✓" : ""
                        font.pixelSize: 10
                        color: Theme.c.accent
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 24
                        anchors.right: flySide.left
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !flyRow.separator
                        text: flyRow.label
                        font.pixelSize: 11
                        color: flyRow.rowEnabled ? Theme.c.text : Theme.c.textMuted
                        elide: Text.ElideRight
                    }

                    Text {
                        id: flySide

                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !flyRow.separator && !!flyRow.modelData.shortcut
                        text: flyRow.modelData.shortcut || ""
                        font.pixelSize: 11
                        color: Theme.c.textMuted
                    }

                    MouseArea {
                        id: flyMouse

                        anchors.fill: parent
                        enabled: !flyRow.separator
                        hoverEnabled: true
                        onClicked: {
                            if (!flyRow.rowEnabled)
                                return;
                            if (flyRow.modelData.action)
                                flyRow.modelData.action();
                            menu.close();
                        }
                    }
                }
            }
        }
    }
}
