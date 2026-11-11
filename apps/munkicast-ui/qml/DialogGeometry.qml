// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import MunkiCast

// Shared handles for in-workspace dialogs and native settings windows.
Item {
    id: handles
    anchors.fill: parent
    z: 1000
    property var target: parent
    property real minimumWidth: 320
    property real minimumHeight: 200
    property real dragLeft: 0
    property real dragWidth: width - dragLeft
    property bool resizable: true
    property string title: ""
    readonly property bool nativeWindow: target && typeof target.startSystemMove === "function"

    function constrain() {
        if (!target.visible || nativeWindow || !target.parent) return;
        if (target.width > target.parent.width) target.width = target.parent.width;
        if (target.height > target.parent.height) target.height = target.parent.height;
        target.x = Math.max(0, Math.min(target.x, target.parent.width - target.width));
        target.y = Math.max(0, Math.min(target.y, target.parent.height - target.height));
    }
    Connections {
        target: handles.nativeWindow ? null : handles.target.parent
        function onWidthChanged() { handles.constrain(); }
        function onHeightChanged() { handles.constrain(); }
    }
    Rectangle {
        visible: handles.title !== ""
        width: parent.width; height: 32
        color: Theme.c.panelHeader
        Text {
            x: 12; anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 48
            text: handles.title; elide: Text.ElideRight; color: Theme.c.text
        }
    }
    MouseArea {
        id: move
        objectName: "dialogMoveHandle"
        x: handles.dragLeft; width: handles.dragWidth - (handles.title !== "" ? 32 : 0)
        height: 32
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        Accessible.role: Accessible.TitleBar
        Accessible.name: qsTr("Move dialog")
        property point origin
        property point initialPosition
        onPressed: mouse => {
            origin = mapToItem(null, mouse.x, mouse.y);
            initialPosition = Qt.point(handles.target.x, handles.target.y);
            if (handles.nativeWindow) handles.target.startSystemMove();
        }
        onPositionChanged: mouse => {
            if (!pressed || handles.nativeWindow) return;
            const at = mapToItem(null, mouse.x, mouse.y);
            handles.target.x = initialPosition.x + at.x - origin.x;
            handles.target.y = initialPosition.y + at.y - origin.y;
            handles.constrain();
        }
    }
    Text {
        visible: handles.title !== ""
        anchors.right: parent.right
        width: 32; height: 32
        text: "×"; color: Theme.c.text; horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        MouseArea {
            anchors.fill: parent
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Close dialog")
            onClicked: handles.target.close()
        }
    }
    MouseArea {
        id: resize
        objectName: "dialogResizeHandle"
        visible: handles.resizable
        anchors.right: parent.right; anchors.bottom: parent.bottom
        width: 12; height: 12
        cursorShape: Qt.SizeFDiagCursor
        Accessible.role: Accessible.Grip
        Accessible.name: qsTr("Resize dialog")
        property point origin
        property size initialSize
        Text { anchors.centerIn: parent; text: "◢"; color: Theme.c.textMuted; font.pixelSize: 12 }
        onPressed: mouse => {
            origin = mapToItem(null, mouse.x, mouse.y);
            initialSize = Qt.size(handles.target.width, handles.target.height);
            if (handles.nativeWindow) handles.target.startSystemResize(Qt.RightEdge | Qt.BottomEdge);
        }
        onPositionChanged: mouse => {
            if (!pressed || handles.nativeWindow) return;
            const at = mapToItem(null, mouse.x, mouse.y);
            handles.target.width = Math.max(handles.minimumWidth, initialSize.width + at.x - origin.x);
            handles.target.height = Math.max(handles.minimumHeight, initialSize.height + at.y - origin.y);
            handles.constrain();
        }
    }
}
