// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import MunkiCast

// The rows keep their own click/menu handlers; only a threshold-crossing drag reorders.
Item {
    id: reorder
    required property ListView view
    property bool favourites: false
    property string sceneId: ""
    property bool dragging: false
    property point origin
    property point pointer
    property string targetId: ""
    property bool after: false
    property real lineY: 0
    anchors.fill: view
    z: 5

    function begin(id, at) {
        cancel();
        sceneId = id;
        origin = pointer = at;
    }
    function move(at) {
        if (sceneId === "") return;
        pointer = at;
        if (!dragging && Math.hypot(at.x - origin.x, at.y - origin.y) >= Qt.styleHints.startDragDistance)
            dragging = true;
        updateTarget();
    }
    function updateTarget() {
        targetId = "";
        if (!dragging || pointer.x < 0 || pointer.x > width || pointer.y < 0 || pointer.y > height)
            return;
        const y = pointer.y + view.contentY;
        let index = view.indexAt(pointer.x + view.contentX, y);
        if (index < 0 && view.count > 0) index = view.count - 1;
        const item = view.itemAtIndex(index);
        if (!item) return;
        targetId = item.sceneId;
        after = y >= item.y + item.height / 2;
        lineY = item.y + (after ? item.height : 0) - view.contentY;
    }
    function step(id, index, delta) {
        const next = index + delta;
        if (next < 0 || next >= view.count) return;
        Document.moveSceneInList(id, delta, favourites);
        view.positionViewAtIndex(next, ListView.Contain);
        Qt.callLater(() => {
            const item = view.itemAtIndex(next);
            if (item && item.sceneId === id) item.forceActiveFocus();
        });
    }
    function finish() {
        if (dragging && targetId !== "")
            Document.reorderScene(sceneId, targetId, after, favourites);
        cancel();
    }
    function cancel() {
        sceneId = targetId = "";
        dragging = false;
    }
    Rectangle {
        width: parent.width
        height: 2
        y: Math.max(0, Math.min(reorder.height - height, reorder.lineY))
        visible: reorder.dragging && reorder.targetId !== ""
        color: Theme.c.accent
    }
    Timer {
        interval: 30
        repeat: true
        running: reorder.dragging
        onTriggered: {
            if (reorder.pointer.x < 0 || reorder.pointer.x > reorder.width) return;
            const step = reorder.pointer.y < 20 ? -8 : reorder.pointer.y > reorder.height - 20 ? 8 : 0;
            reorder.view.contentY = Math.max(0, Math.min(
                Math.max(0, reorder.view.contentHeight - reorder.view.height), reorder.view.contentY + step));
            reorder.updateTarget();
        }
    }
}
