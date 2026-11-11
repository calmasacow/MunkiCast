// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import MunkiCast

QtObject {
    function row(text, shortcut, enabled, action) {
        return {text: text, shortcut: shortcut, enabled: enabled, checked: false, action: action};
    }

    function menu() {
        const locked = Document.nodeProperties.locked === true;
        const sep = {separator: true};
        return {
            text: qsTr("Transform"),
            checked: false,
            children: [
                row(qsTr("Edit Transform"), "Ctrl+E", true,
                    () => Workspace.focusPanel("properties")),
                row(qsTr("Copy Transform"), "Ctrl+Shift+C", true,
                    () => Document.copyTransform()),
                row(qsTr("Paste Transform"), "Ctrl+Shift+V",
                    !locked && Document.hasCopiedTransform(),
                    () => Document.pasteTransform()),
                row(qsTr("Reset Transform"), "Ctrl+R", !locked,
                    () => Document.resetTransform()),
                sep,
                row(qsTr("Rotate 90 degrees CW"), "", !locked,
                    () => Document.rotateBy(90)),
                row(qsTr("Rotate 90 degrees CCW"), "", !locked,
                    () => Document.rotateBy(-90)),
                row(qsTr("Rotate 180 degrees"), "", !locked,
                    () => Document.rotateBy(180)),
                sep,
                row(qsTr("Flip Horizontal"), "", !locked,
                    () => Document.flipHorizontal()),
                row(qsTr("Flip Vertical"), "", !locked,
                    () => Document.flipVertical()),
                sep,
                row(qsTr("Fit to screen"), "Ctrl+F", !locked,
                    () => Document.fitToCanvas(false)),
                // Ctrl+S is Save. The item is here; it does not wear that key.
                row(qsTr("Stretch to screen"), "", !locked,
                    () => Document.fitToCanvas(true)),
                row(qsTr("Center to screen"), "Ctrl+D", !locked,
                    () => Document.centerOnCanvas(true, true)),
                row(qsTr("Center Vertically"), "", !locked,
                    () => Document.centerOnCanvas(false, true)),
                row(qsTr("Center Horizontally"), "", !locked,
                    () => Document.centerOnCanvas(true, false))
            ]
        };
    }
}
