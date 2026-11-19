// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// New Timeline: its name, its picture size and its frame rate, chosen before it exists.
///
/// The + button used to make one straight away at a fixed format, which is a choice made for
/// the operator and only discovered once clips were on it. Rate cannot be changed after the
/// fact without re-timing every edit, so it is asked here.
///
/// ponytail: start timecode, audio track layout and mismatched-resolution handling are not
/// asked; timelines start at 00:00:00:00, stereo, scaled to fit. Add when a second choice exists.
Item {
    id: dialog

    objectName: "timelineDialog"
    anchors.fill: parent
    z: 300
    visible: false
    focus: visible

    property string timelineName: ""
    property string preset: "1920 x 1080 HD"
    property bool vertical: false
    property int frameWidth: 1920
    property int frameHeight: 1080
    property string rate: "30"
    property int videoTracks: 1
    property int audioTracks: 1

    /// Landscape sizes. Vertical swaps each one, which is what a phone-first edit is.
    readonly property var sizes: ({
        "720 x 720 Square": [720, 720],
        "1280 x 720 HD 720p": [1280, 720],
        "1080 x 1080 HD Square": [1080, 1080],
        "1920 x 1080 HD": [1920, 1080],
        "2160 x 2160 Ultra HD Square": [2160, 2160],
        "3840 x 2160 Ultra HD": [3840, 2160]
    })
    readonly property var presetNames: Object.keys(dialog.sizes).concat(["Custom"])

    /// Each rate as the exact fraction it stands for: 29.97 is 30000/1001, not 29.97, or a
    /// timecode drifts a frame every thirty-three seconds.
    readonly property var rates: ({
        "23.976": [24000, 1001], "24": [24, 1], "25": [25, 1], "29.97": [30000, 1001],
        "30": [30, 1], "50": [50, 1], "59.94": [60000, 1001], "60": [60, 1], "90": [90, 1],
        "96": [96, 1], "100": [100, 1], "119.88": [120000, 1001], "120": [120, 1]
    })
    readonly property var rateNames: ["23.976", "24", "25", "29.97", "30", "50", "59.94", "60",
                                      "90", "96", "100", "119.88", "120"]

    function applyPreset() {
        const s = dialog.sizes[dialog.preset];
        if (!s)
            return;
        dialog.frameWidth = dialog.vertical ? s[1] : s[0];
        dialog.frameHeight = dialog.vertical ? s[0] : s[1];
    }

    function open() {
        dialog.timelineName = qsTr("Timeline %1").arg(Edit.timelines.length + 1);
        dialog.preset = "1920 x 1080 HD";
        dialog.vertical = false;
        dialog.rate = "30";
        dialog.videoTracks = 1;
        dialog.audioTracks = 1;
        dialog.applyPreset();
        dialog.visible = true;
        dialog.forceActiveFocus();
    }

    function close() { dialog.visible = false; }

    function accept() {
        dialog.forceActiveFocus();  // Flush field edits before they are read.
        Qt.callLater(function() {
            const r = dialog.rates[dialog.rate];
            if (Edit.createTimeline(dialog.timelineName, r[0], r[1], dialog.frameWidth,
                                    dialog.frameHeight, dialog.videoTracks, dialog.audioTracks))
                dialog.visible = false;
        });
    }

    Keys.onEscapePressed: dialog.close()
    Keys.onReturnPressed: dialog.accept()
    Keys.onEnterPressed: dialog.accept()

    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.45
        MouseArea { anchors.fill: parent; onClicked: {} }
    }

    Rectangle {
        objectName: "timelineFrame"
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        DialogGeometry { minimumWidth: 400; minimumHeight: 300 }
        width: Math.min(parent.width - 48, 460)
        height: Math.min(parent.height - 48, form.implicitHeight + 110)
        color: Theme.c.panelBackground
        border.color: Theme.c.accent
        radius: 3
        MouseArea { anchors.fill: parent; onClicked: {} }

        Text {
            id: title
            anchors { left: parent.left; top: parent.top; margins: 16 }
            text: qsTr("New Timeline")
            font { pixelSize: 14; bold: true }
            color: Theme.c.text
        }

        Column {
            id: form
            anchors { left: parent.left; right: parent.right; top: title.bottom; margins: 16 }
            spacing: 8

            TextLine {
                objectName: "timelineNameField"
                width: parent.width; labelWidth: 120
                label: qsTr("Timeline name")
                value: dialog.timelineName
                commitOnEdit: true
                onCommitted: newValue => dialog.timelineName = newValue
            }

            ChoiceField {
                objectName: "timelineSizeField"
                width: parent.width; labelWidth: 120
                label: qsTr("Resolution")
                choices: dialog.presetNames
                value: dialog.preset
                onCommitted: newValue => { dialog.preset = newValue; dialog.applyPreset(); }
            }

            Row {
                spacing: 8
                x: 124

                NumberField {
                    objectName: "timelineWidthField"
                    width: 110; labelWidth: 14
                    label: "W"
                    decimals: 0; minimum: 16; maximum: 16384
                    value: dialog.frameWidth
                    onCommitted: newValue => { dialog.frameWidth = newValue; dialog.preset = "Custom"; }
                }

                NumberField {
                    objectName: "timelineHeightField"
                    width: 110; labelWidth: 14
                    label: "H"
                    decimals: 0; minimum: 16; maximum: 16384
                    value: dialog.frameHeight
                    onCommitted: newValue => { dialog.frameHeight = newValue; dialog.preset = "Custom"; }
                }
            }

            CheckLabel {
                objectName: "timelineVerticalField"
                x: 124
                text: qsTr("Use vertical resolution")
                checked: dialog.vertical
                onToggled: {
                    dialog.vertical = !dialog.vertical;
                    const w = dialog.frameWidth;
                    dialog.frameWidth = dialog.frameHeight;
                    dialog.frameHeight = w;
                }
            }

            ChoiceField {
                objectName: "timelineRateField"
                width: 240; labelWidth: 120
                label: qsTr("Frame rate")
                choices: dialog.rateNames
                value: dialog.rate
                onCommitted: newValue => dialog.rate = newValue
            }

            NumberField {
                width: 200; labelWidth: 120
                label: qsTr("Video tracks")
                decimals: 0; minimum: 1; maximum: 32
                value: dialog.videoTracks
                onCommitted: newValue => dialog.videoTracks = newValue
            }

            NumberField {
                width: 200; labelWidth: 120
                label: qsTr("Audio tracks")
                decimals: 0; minimum: 1; maximum: 32
                value: dialog.audioTracks
                onCommitted: newValue => dialog.audioTracks = newValue
            }
        }

        Row {
            anchors { right: parent.right; bottom: parent.bottom; margins: 16 }
            spacing: 8

            DialogButton {
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }

            DialogButton {
                objectName: "timelineAccept"
                text: qsTr("Create")
                primary: true
                enabled: dialog.timelineName.trim() !== ""
                onClicked: dialog.accept()
            }
        }
    }
}
