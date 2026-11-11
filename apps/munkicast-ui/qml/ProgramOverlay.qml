// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import MunkiCast

/// Selection on the program picture: drag the source, or a handle to resize it.
///
/// The rectangle is the one the compositor paints, for whichever source is selected. A handle
/// changes scale, and position so the opposite edge stays put, unless Alt is held: then the
/// center stays and both sides move. The aspect ratio is kept unless Shift is held, which frees
/// it -- a source has a shape of its own and distorting it is the gesture to ask for, not the
/// one to get by default. Alt and Shift together do both. The layout box is a different control.
/// The whole press is one undo step.
Item {
    id: overlay

    required property Item inputSurface
    required property string canvasId
    /// How close, in screen pixels, an edge has to come before it snaps. Screen pixels rather
    /// than canvas units so the feel does not change with the canvas size or the panel's zoom.
    property real snapPx: 12
    property bool editable: true

    required property real canvasWidth
    required property real canvasHeight

    objectName: "programOverlay"
    anchors.fill: parent
    // The popup lives on the window, so explicitly dismiss it when its dock/view hides.
    onVisibleChanged: { if (!overlay.visible && canvasMenu) canvasMenu.close(); }

    property bool dragging: false
    property var hold: null
    property string edges: ""
    property real originX: 0
    property real originY: 0
    property real liveX: 0
    property real liveY: 0
    property real liveW: 0
    property real liveH: 0
    property int cursorShape: Qt.ArrowCursor

    readonly property var handleEdges: ["lt", "t", "rt", "l", "r", "lb", "b", "rb"]

    /// Follows the document, except while a drag is in hand: the outline tracks the pointer,
    /// and the picture follows the commands.
    readonly property var placement: {
        // A selected node can move without its ID changing. Retain the document dependency
        // in control flow; an unused property read is discarded by the QML compiler.
        if (!overlay.editable || Document.revision < 0) return null;
        if (overlay.dragging)
            return overlay.hold;
        if (Document.selectedNode === "")
            return null;
        const rect = Document.placedRect(overlay.canvasId, Document.selectedNode);
        return rect && rect.width > 0 ? rect : null;
    }

    readonly property var shownRect: overlay.dragging
            ? {x: overlay.liveX, y: overlay.liveY, width: overlay.liveW, height: overlay.liveH}
            : overlay.placement

    readonly property var shownFrame: {
        const rect = overlay.shownRect;
        if (!rect || !(rect.width > 0) || overlay.width <= 0 || overlay.height <= 0 ||
                overlay.canvasWidth <= 0 || overlay.canvasHeight <= 0)
            return null;
        const sx = overlay.width / overlay.canvasWidth;
        const sy = overlay.height / overlay.canvasHeight;
        return {x: rect.x * sx, y: rect.y * sy, w: rect.width * sx, h: rect.height * sy};
    }

    readonly property bool selectionLocked: {
        const held = overlay.dragging ? overlay.hold : overlay.placement;
        return !!(held && held.locked === true);
    }

    function toCanvas(x, y) {
        if (overlay.canvasWidth <= 0 || overlay.canvasHeight <= 0 ||
                overlay.width <= 0 || overlay.height <= 0)
            return Qt.point(0, 0);
        return Qt.point(x * overlay.canvasWidth / overlay.width,
                        y * overlay.canvasHeight / overlay.height);
    }

    function centerOf(edges) {
        const frame = overlay.shownFrame;
        if (!frame)
            return Qt.point(0, 0);
        let x = frame.x;
        let y = frame.y;
        if (edges.indexOf("r") >= 0)
            x = frame.x + frame.w;
        else if (edges.indexOf("l") < 0)
            x = frame.x + frame.w / 2;
        if (edges.indexOf("b") >= 0)
            y = frame.y + frame.h;
        else if (edges.indexOf("t") < 0)
            y = frame.y + frame.h / 2;
        return Qt.point(x, y);
    }

    function cursorFor(edges) {
        if (edges === "lt" || edges === "rb")
            return Qt.SizeFDiagCursor;
        if (edges === "rt" || edges === "lb")
            return Qt.SizeBDiagCursor;
        if (edges === "l" || edges === "r")
            return Qt.SizeHorCursor;
        return Qt.SizeVerCursor;
    }

    function handleAt(x, y) {
        const frame = overlay.shownFrame;
        if (!frame)
            return "";
        for (let i = 0; i < overlay.handleEdges.length; ++i) {
            const edges = overlay.handleEdges[i];
            const center = overlay.centerOf(edges);
            if (Math.abs(x - center.x) <= 8 && Math.abs(y - center.y) <= 8)
                return edges;
        }
        return "";
    }

    function contains(rect, point) {
        return point.x >= rect.x && point.x < rect.x + rect.width &&
               point.y >= rect.y && point.y < rect.y + rect.height;
    }

    /// A locked node is not here to be clicked: the pointer passes through it to whatever is
    /// behind, which is the point of locking an overlay that covers the canvas. It stays
    /// selectable in the Sources list, so its lock can be lifted and its properties edited.
    function hitNode(x, y) {
        const point = overlay.toCanvas(x, y);
        const nodes = Document.placedNodes(overlay.canvasId);
        for (let i = 0; i < nodes.length; ++i) {
            if (nodes[i].locked === true)
                continue;
            // A group is hit-tested against where its contents are, not against its own placed
            // rect, which is the whole canvas when the group has no size of its own.
            const box = nodes[i].isGroup === true
                    ? {x: nodes[i].hitX, y: nodes[i].hitY,
                       width: nodes[i].hitWidth, height: nodes[i].hitHeight}
                    : nodes[i];
            if (overlay.contains(box, point))
                return nodes[i];
        }
        if (overlay.placement && overlay.placement.locked !== true &&
            overlay.handleAt(x, y) !== "")
            return overlay.placement;
        return null;
    }

    /// The rectangle a handle drag wants, from the press and the keys held now.
    /// Recomputed from the press every move, so letting Shift go returns to the locked shape.
    /// `free` is the Shift key: aspect is locked unless it is held.
    ///
    /// That is the inverse of the first version, which followed the drawing-tool convention where
    /// a drag is free and Shift constrains. A source is a picture with a shape of its own, and the
    /// common case is resizing it without distorting it -- a stretched camera or a squashed screen
    /// is almost never what was wanted, and noticing it later costs a take. So the default is the
    /// safe one and the destructive one is the gesture you have to ask for. Alt is unchanged.
    function resized(start, edges, dx, dy, free, alt) {
        const min = 8;
        const aspect = start.height > 0 ? start.width / start.height : 1;
        const dragX = edges.indexOf("l") >= 0 || edges.indexOf("r") >= 0;
        const dragY = edges.indexOf("t") >= 0 || edges.indexOf("b") >= 0;
        let growX = 0;
        let growY = 0;
        if (edges.indexOf("r") >= 0)
            growX = dx;
        else if (edges.indexOf("l") >= 0)
            growX = -dx;
        if (edges.indexOf("b") >= 0)
            growY = dy;
        else if (edges.indexOf("t") >= 0)
            growY = -dy;
        if (alt) {
            growX *= 2;
            growY *= 2;
        }
        let nw = dragX ? Math.max(min, start.width + growX) : start.width;
        let nh = dragY ? Math.max(min, start.height + growY) : start.height;
        if (!free && aspect > 0) {
            if (dragX && dragY) {
                const rx = Math.abs(nw - start.width) / start.width;
                const ry = Math.abs(nh - start.height) / start.height;
                if (rx >= ry)
                    nh = nw / aspect;
                else
                    nw = nh * aspect;
            } else if (dragX) {
                nh = nw / aspect;
            } else if (dragY) {
                nw = nh * aspect;
            }
            if (nw < min || nh < min) {
                const lift = Math.max(min / nw, min / nh);
                nw *= lift;
                nh *= lift;
            }
        }
        const cx = start.x + start.width / 2;
        const cy = start.y + start.height / 2;
        let nx = start.x;
        let ny = start.y;
        if (alt) {
            // Alt means the centre stays put, on both axes, whatever the size became. Deciding
            // per axis -- as an earlier version did, recentring only the dragged one -- lets the
            // rectangle drift whenever the other axis changed for any other reason: a corner
            // handle, or the aspect lock resizing it. The centre is the whole promise of the
            // modifier, so it is held unconditionally and the sizes are hung off it.
            nx = cx - nw / 2;
            ny = cy - nh / 2;
        } else {
            if (edges.indexOf("r") >= 0)
                nx = start.x;
            else if (edges.indexOf("l") >= 0)
                nx = start.x + start.width - nw;
            else
                nx = start.x + (start.width - nw) / 2;
            if (edges.indexOf("b") >= 0)
                ny = start.y;
            else if (edges.indexOf("t") >= 0)
                ny = start.y + start.height - nh;
            else
                ny = start.y + (start.height - nh) / 2;
        }
        return overlay.snapped({x: nx, y: ny, w: nw, h: nh}, edges, free, aspect, alt);
    }

    /// Pulls an edge that came within `snapPx` of a canvas edge or the canvas centre onto it.
    ///
    /// The threshold is in screen pixels, not canvas units, so it feels the same distance to the
    /// hand whatever the canvas is and however far the preview is zoomed out -- a 12px tolerance
    /// on a 4K canvas shown in a small panel would otherwise be a quarter of an inch of mouse
    /// travel on one and a hair on another.
    ///
    /// Only the dragged edges snap. Snapping an edge nobody is moving would shift the rectangle
    /// under a gesture that did not ask to move it.
    ///
    /// With the aspect locked the snap still applies, and the other axis is recomputed from the
    /// ratio afterwards so the shape is kept. Only one axis can snap in that case -- the one with
    /// a dragged edge -- because the other is no longer free to land anywhere.
    function snapped(box, edges, free, aspect, alt) {
        if (overlay.canvasWidth <= 0 || overlay.canvasHeight <= 0 || overlay.width <= 0)
            return box;
        const perPx = overlay.canvasWidth / overlay.width; // canvas units per screen pixel
        const tol = overlay.snapPx * perPx;
        const lines = [0, overlay.canvasWidth / 2, overlay.canvasWidth];
        const rows = [0, overlay.canvasHeight / 2, overlay.canvasHeight];
        let x = box.x;
        let y = box.y;
        let w = box.w;
        let h = box.h;
        const near = (v, to) => Math.abs(v - to) <= tol;

        if (edges.indexOf("l") >= 0) {
            for (const line of lines)
                if (near(x, line)) { w += x - line; x = line; break; }
        } else if (edges.indexOf("r") >= 0) {
            for (const line of lines)
                if (near(x + w, line)) { w = line - x; break; }
        }
        if (edges.indexOf("t") >= 0) {
            for (const row of rows)
                if (near(y, row)) { h += y - row; y = row; break; }
        } else if (edges.indexOf("b") >= 0) {
            for (const row of rows)
                if (near(y + h, row)) { h = row - y; break; }
        }
        // Alt means the centre is the fixed point, so an edge that snapped has to take its
        // opposite number with it -- otherwise the snap silently undoes the modifier, which is
        // what made a centred resize drift 11px off centre the moment an edge came near 1080.
        if (alt) {
            if (Math.abs(w - box.w) > 1e-9 || Math.abs(x - box.x) > 1e-9) {
                const cx = box.x + box.w / 2;
                w = 2 * Math.max(Math.abs(cx - x), Math.abs((x + w) - cx));
                x = cx - w / 2;
            }
            if (Math.abs(h - box.h) > 1e-9 || Math.abs(y - box.y) > 1e-9) {
                const cy = box.y + box.h / 2;
                h = 2 * Math.max(Math.abs(cy - y), Math.abs((y + h) - cy));
                y = cy - h / 2;
            }
        }
        // With the aspect locked, restore the ratio from whichever axis the snap moved, then put
        // the rectangle back against the edge the drag is anchored to. Without this the snap
        // would quietly distort the source -- the exact thing the locked mode exists to prevent.
        if (!free && aspect > 0) {
            const movedX = Math.abs(w - box.w) > 1e-9;
            const movedY = Math.abs(h - box.h) > 1e-9;
            if (movedX && !movedY) {
                h = w / aspect;
            } else if (movedY && !movedX) {
                w = h * aspect;
            } else if (movedX && movedY) {
                // Both edges found a line. Keep the horizontal one and let the vertical follow,
                // rather than landing on neither.
                h = w / aspect;
            }
            if (alt) {
                x = box.x + box.w / 2 - w / 2;
                y = box.y + box.h / 2 - h / 2;
            } else {
                if (edges.indexOf("l") >= 0)
                    x = box.x + box.w - w;
                if (edges.indexOf("t") >= 0)
                    y = box.y + box.h - h;
            }
        }
        // A snap that collapsed the rectangle is not a snap. Dropped rather than clamped, so the
        // drag carries on from where it was instead of jumping to a minimum.
        if (w < 8 || h < 8)
            return box;
        return {x: x, y: y, w: w, h: h};
    }

    function press(x, y) {
        canvasMenu.close();
        const selected = overlay.placement;
        const onHandle = selected && selected.locked !== true ? overlay.handleAt(x, y) : "";
        let target = null;
        if (onHandle !== "")
            target = selected;
        else
            target = overlay.hitNode(x, y);
        if (!target)
            return;
        Document.focusPlacedNode(overlay.canvasId, target.id);
        if (target.locked === true)
            return;
        const point = overlay.toCanvas(x, y);
        overlay.hold = target;
        overlay.liveX = target.x;
        overlay.liveY = target.y;
        overlay.liveW = target.width;
        overlay.liveH = target.height;
        overlay.edges = onHandle;
        overlay.originX = point.x;
        overlay.originY = point.y;
        overlay.dragging = true;
        overlay.cursorShape = onHandle === "" ? Qt.SizeAllCursor : overlay.cursorFor(onHandle);
    }

    function drag(x, y, modifiers) {
        if (!overlay.dragging || !overlay.hold)
            return;
        const point = overlay.toCanvas(x, y);
        const dx = point.x - overlay.originX;
        const dy = point.y - overlay.originY;
        const start = overlay.hold;
        if (overlay.edges === "") {
            overlay.liveX = start.x + dx;
            overlay.liveY = start.y + dy;
            overlay.liveW = start.width;
            overlay.liveH = start.height;
            Document.setDrawnRect(start.x + dx, start.y + dy, start.width, start.height, true);
            return;
        }
        const free = (modifiers & Qt.ShiftModifier) !== 0;
        const alt = (modifiers & Qt.AltModifier) !== 0;
        const box = overlay.resized(start, overlay.edges, dx, dy, free, alt);
        overlay.liveX = box.x;
        overlay.liveY = box.y;
        overlay.liveW = box.w;
        overlay.liveH = box.h;
        Document.setDrawnRect(box.x, box.y, box.w, box.h, true);
    }

    function release() {
        if (!overlay.dragging)
            return;
        overlay.dragging = false;
        overlay.edges = "";
        overlay.cursorShape = Qt.ArrowCursor;
        Document.endGesture();
    }

    function hover(x, y) {
        if (overlay.placement && overlay.placement.locked !== true) {
            const edges = overlay.handleAt(x, y);
            if (edges !== "") {
                overlay.cursorShape = overlay.cursorFor(edges);
                return;
            }
            if (overlay.contains(overlay.placement, overlay.toCanvas(x, y))) {
                overlay.cursorShape = Qt.SizeAllCursor;
                return;
            }
        }
        overlay.cursorShape = Qt.ArrowCursor;
    }

    function row(text, shortcut, enabled, action) {
        return {text: text, shortcut: shortcut, enabled: enabled, checked: false, action: action};
    }

    function projectorRows() {
        function choices(preview) {
            const children = [overlay.row(qsTr("Windowed"), "", true,
                () => Preview.openProjector(overlay.canvasId, preview, false, "")),
                overlay.row(qsTr("Fullscreen"), "", true,
                () => Preview.openProjector(overlay.canvasId, preview, true, ""))];
            const screens = Preview.projectorScreens();
            for (const screen of screens)
                children.push(overlay.row(qsTr("Fullscreen — %1").arg(screen.name), "", true,
                    () => Preview.openProjector(overlay.canvasId, preview, true, screen.name)));
            return children;
        }
        const rows = [{text: qsTr("Program Projector"), children: choices(false)}];
        if (Document.mainDeck.studio)
            rows.push({text: qsTr("Preview Projector"), children: choices(true)});
        const windows = Preview.projectors().filter(w => w.active);
        if (windows.length > 0)
            rows.push({text: qsTr("Close Projector"), children: windows.map(w =>
                overlay.row((w.preview ? qsTr("Preview %1%2") : qsTr("Program %1%2")).arg(w.id).arg(w.screen ? " — " + w.screen : ""),
                            "", true, () => Preview.closeProjector(w.id)))});
        return rows;
    }

    function menuRows() {
        const id = Document.selectedNode;
        const sep = {separator: true};
        return [
            overlay.row(qsTr("Filters"), "", id !== "", () => Document.showFilters(id)),
            {
                text: qsTr("Order"),
                checked: false,
                children: [
                    overlay.row(qsTr("Move Up"), "Ctrl+Up", true,
                                () => Document.moveNode(id, -1)),
                    overlay.row(qsTr("Move Down"), "Ctrl+Down", true,
                                () => Document.moveNode(id, 1)),
                    overlay.row(qsTr("Move to Top"), "Ctrl+Home", true,
                                () => Document.moveSelectedToEnd(true)),
                    overlay.row(qsTr("Move to Bottom"), "Ctrl+End", true,
                                () => Document.moveSelectedToEnd(false))
                ]
            },
            transformMenu.menu(),
            sep,
            overlay.row(qsTr("Copy"), "Ctrl+C", true, () => Document.copySelectedNodes()),
            overlay.row(qsTr("Paste"), "Ctrl+V", Document.pasteDescription() !== "",
                        () => Document.paste()),
            sep,
            overlay.row(qsTr("Remove"), "Del", true, () => Document.removeSelectedNodes()),
            overlay.row(qsTr("Rename..."), "F2", Document.selectedNodes.length === 1,
                        () => Document.requestRename())
        ];
    }

    function openMenu(x, y) {
        const target = overlay.editable ? overlay.hitNode(x, y) : null;
        // A right-click on something already selected keeps the rest of the selection, so the
        // menu can act on all of it. The same rule as the sources list.
        if (target && Document.selectedNodes.indexOf(target.id) < 0)
            Document.focusPlacedNode(overlay.canvasId, target.id);
        const at = overlay.mapToItem(canvasMenu, x, y);
        const rows = overlay.projectorRows();
        if (target) rows.push({separator: true}, ...overlay.menuRows());
        canvasMenu.popup(at.x, at.y, rows);
    }

    Rectangle {
        objectName: "programSelection"
        visible: overlay.shownFrame !== null
        x: overlay.shownFrame ? overlay.shownFrame.x : 0
        y: overlay.shownFrame ? overlay.shownFrame.y : 0
        width: overlay.shownFrame ? overlay.shownFrame.w : 0
        height: overlay.shownFrame ? overlay.shownFrame.h : 0
        color: "transparent"
        border.width: 1
        border.color: overlay.selectionLocked ? Theme.c.textMuted : Theme.c.accent
    }

    Repeater {
        model: overlay.handleEdges

        delegate: Rectangle {
            required property string modelData
            objectName: "programHandle"
            visible: overlay.shownFrame !== null && !overlay.selectionLocked
            width: 8
            height: 8
            x: overlay.centerOf(modelData).x - 4
            y: overlay.centerOf(modelData).y - 4
            color: Theme.c.window
            border.width: 1
            border.color: Theme.c.accent
        }
    }

    TransformMenu { id: transformMenu }

    MouseArea {
        // Paint in canvas coordinates; receive input throughout the panel's letterbox area.
        parent: overlay.inputSurface
        anchors.fill: parent
        z: 3
        visible: overlay.visible
        hoverEnabled: true
        acceptedButtons: overlay.editable ? Qt.LeftButton | Qt.RightButton : Qt.RightButton
        preventStealing: true
        cursorShape: overlay.cursorShape
        enabled: overlay.canvasWidth > 0 && overlay.canvasHeight > 0

        onPressed: (ev) => {
            const at = mapToItem(overlay, ev.x, ev.y);
            if (ev.button === Qt.RightButton)
                overlay.openMenu(at.x, at.y);
            else
                overlay.press(at.x, at.y);
        }
        onPositionChanged: (ev) => {
            const at = mapToItem(overlay, ev.x, ev.y);
            if (overlay.dragging)
                overlay.drag(at.x, at.y, ev.modifiers);
            else if (overlay.editable)
                overlay.hover(at.x, at.y);
        }
        onReleased: overlay.release()
        onCanceled: overlay.release()
    }

    StripMenu {
        id: canvasMenu
        parent: overlay.Window.contentItem || overlay.inputSurface
        z: 100
        objectName: "canvasMenu"
    }
}
