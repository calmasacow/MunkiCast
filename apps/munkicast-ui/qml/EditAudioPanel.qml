// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The Audio Track Mixer: one strip per audio track, and the master.
///
/// Separate from the show's Audio Mixer panel, which meters buses going to air. This meters an
/// edit being reviewed, and its levels belong to the `.mtime` document rather than to the show
/// -- a cut carries its own balance to whatever machine opens it.
Item {
    id: panel

    readonly property var levels: Edit.levels
    readonly property bool ready: Edit.hasTimeline

    /// dB to a 0..1 fraction of the meter's height.
    ///
    /// -60 at the bottom, 0 at the top, and the scale deliberately not linear in dB: the top
    /// twelve decibels are where mixing decisions are made, so they get a third of the height.
    function meterFraction(db) {
        const clamped = Math.max(-60, Math.min(6, db));
        return Math.pow((clamped + 60) / 66, 1.8);
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
    }

    Text {
        anchors.centerIn: parent
        width: parent.width - 20
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        visible: !panel.ready
        text: qsTr("No timeline open")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Text {
        anchors.centerIn: parent
        width: parent.width - 20
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        visible: panel.ready && panel.levels.length === 0
        text: qsTr("No audio tracks. Add one from the timeline.")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    // The strips scroll sideways: a mix with eight tracks does not fit a docked panel, and
    // squeezing the strips would cost the meters the height that makes them readable.
    Flickable {
        anchors.fill: parent
        visible: panel.ready && panel.levels.length > 0
        contentWidth: strips.width
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        NoDragScroll {}

        Row {
            id: strips

            height: parent.height
            spacing: 1

            Repeater {
                model: panel.levels

                delegate: Item {
                    id: strip

                    required property var modelData

                    // A strip is as wide as its contents need and no wider. Sharing out the
                    // panel's width instead was tried and is wrong: one track then stretched
                    // across the whole panel, leaving its meter stranded a hand's width from
                    // its own fader.
                    width: 68
                    height: strips.height

                    Rectangle {
                        anchors.fill: parent
                        color: Theme.c.panelBackground
                    }

                    Column {
                        id: head

                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: 3
                        padding: 4

                        // Pan, drawn as the knob every mixer has: the angle is the value, and
                        // dragging up or down turns it. A slider would take width the strip
                        // does not have.
                        Item {
                            width: parent.width - 8
                            height: 30

                            Canvas {
                                id: knob

                                anchors.centerIn: parent
                                width: 26
                                height: 26

                                readonly property real pan: strip.modelData.pan || 0

                                onPanChanged: knob.requestPaint()

                                onPaint: {
                                    const ctx = getContext("2d");
                                    ctx.reset();
                                    const r = width / 2 - 2;
                                    ctx.strokeStyle = Theme.c.border;
                                    ctx.lineWidth = 2;
                                    ctx.beginPath();
                                    ctx.arc(width / 2, height / 2, r, 0, Math.PI * 2);
                                    ctx.stroke();
                                    // Straight up is centre; a quarter turn either way is hard
                                    // left or hard right, which is how a pan pot reads.
                                    const angle = -Math.PI / 2 + knob.pan * (Math.PI / 2);
                                    ctx.strokeStyle = Theme.c.accent;
                                    ctx.beginPath();
                                    ctx.moveTo(width / 2, height / 2);
                                    ctx.lineTo(width / 2 + Math.cos(angle) * r,
                                               height / 2 + Math.sin(angle) * r);
                                    ctx.stroke();
                                }

                                MouseArea {
                                    property real startY: 0
                                    property real startPan: 0

                                    anchors.fill: parent
                                    onPressed: (mouse) => {
                                        startY = mouse.y;
                                        startPan = knob.pan;
                                    }
                                    onPositionChanged: (mouse) => {
                                        if (!pressed)
                                            return;
                                        Edit.setTrackPan(strip.modelData.track,
                                                         startPan + (startY - mouse.y) / 60);
                                    }
                                    // Back to centre, which is the value anyone wants back
                                    // most often and the hardest to hit by dragging.
                                    onDoubleClicked: Edit.setTrackPan(strip.modelData.track, 0)
                                }
                            }
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: {
                                const p = strip.modelData.pan || 0;
                                if (Math.abs(p) < 0.005)
                                    return qsTr("C");
                                return (p < 0 ? qsTr("L") : qsTr("R")) +
                                       " " + Math.round(Math.abs(p) * 100);
                            }
                            font.pixelSize: 9
                            color: Theme.c.textMuted
                        }

                        // Automation mode, cycled rather than opened as a menu: there are five
                        // and the one in use is almost always Read.
                        Rectangle {
                            width: parent.width - 8
                            height: 15
                            radius: 2
                            color: autoMouse.containsMouse ? Theme.c.panelHeaderActive
                                                           : Theme.c.panelHeader

                            Text {
                                anchors.centerIn: parent
                                text: {
                                    const m = strip.modelData.automation || "read";
                                    return m.charAt(0).toUpperCase() + m.slice(1);
                                }
                                font.pixelSize: 9
                                color: Theme.c.text
                            }

                            MouseArea {
                                id: autoMouse

                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    const modes = ["off", "read", "touch", "latch", "write"];
                                    const at = modes.indexOf(strip.modelData.automation || "read");
                                    Edit.setTrackAutomation(
                                        strip.modelData.track,
                                        modes[(at + 1) % modes.length]);
                                }
                            }

                            ToolTipText {
                                text: qsTr("Automation mode — click to change")
                                visible: autoMouse.containsMouse
                                hoverArea: autoMouse
                            }
                        }

                        // Mute and solo, the two buttons a mix is actually checked with.
                        Row {
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: 3

                            Rectangle {
                                width: 20
                                height: 15
                                radius: 2
                                color: strip.modelData.muted ? "#a6453c" : Theme.c.panelHeader

                                Text {
                                    anchors.centerIn: parent
                                    text: "M"
                                    font.pixelSize: 9
                                    color: Theme.c.text
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: Edit.toggleTrack(strip.modelData.track, "muted")
                                }
                            }

                            Rectangle {
                                width: 20
                                height: 15
                                radius: 2
                                color: strip.modelData.solo ? "#b58a3c" : Theme.c.panelHeader

                                Text {
                                    anchors.centerIn: parent
                                    text: "S"
                                    font.pixelSize: 9
                                    color: Theme.c.text
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: Edit.toggleTrack(strip.modelData.track, "solo")
                                }
                            }
                        }
                    }

                    // ---- the meter and its fader ----
                    Item {
                        id: meterArea

                        anchors.top: head.bottom
                        anchors.bottom: footer.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 4

                        // The dB scale, so a reading is a number rather than a colour.
                        Column {
                            id: scale

                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 16

                            Repeater {
                                model: [0, -6, -12, -18, -24, -30, -42, -60]

                                delegate: Item {
                                    required property int modelData
                                    width: scale.width
                                    height: 1

                                    y: meterArea.height *
                                       (1 - panel.meterFraction(modelData)) - 4

                                    Text {
                                        text: modelData
                                        font.pixelSize: 8
                                        color: Theme.c.textMuted
                                    }
                                }
                            }
                        }

                        Rectangle {
                            id: meterWell

                            anchors.left: scale.right
                            anchors.leftMargin: 2
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 12
                            color: Theme.c.window
                            border.color: Theme.c.border

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.margins: 1
                                height: Math.max(
                                    0, (parent.height - 2) *
                                       panel.meterFraction(strip.modelData.db))
                                // Green up to -12, amber to -3, red above: the standard
                                // broadcast reading, so an operator does not have to learn a
                                // new one here.
                                color: strip.modelData.db > -3 ? "#c4524a"
                                     : strip.modelData.db > -12 ? "#c9a227"
                                     : "#4a9e5c"
                            }
                        }

                        // The fader. Dragging sets track gain; double-click is unity.
                        Item {
                            id: fader

                            anchors.left: meterWell.right
                            anchors.leftMargin: 4
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 16

                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 2
                                height: parent.height
                                color: Theme.c.border
                            }

                            Rectangle {
                                id: cap

                                readonly property real gain: strip.modelData.gain || 0

                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 14
                                height: 8
                                radius: 2
                                color: Theme.c.accent
                                y: Math.max(0, Math.min(
                                    fader.height - height,
                                    (fader.height - height) *
                                        (1 - panel.meterFraction(cap.gain))))
                            }

                            MouseArea {
                                anchors.fill: parent
                                onPositionChanged: (mouse) => {
                                    if (!pressed)
                                        return;
                                    // Back out of the same curve the meter uses, so the cap
                                    // lands under the pointer rather than near it.
                                    const f = 1 - Math.max(0, Math.min(1, mouse.y / fader.height));
                                    Edit.setTrackGain(strip.modelData.track,
                                                      Math.pow(f, 1 / 1.8) * 66 - 60);
                                }
                                onDoubleClicked: Edit.setTrackGain(strip.modelData.track, 0)
                            }
                        }
                    }

                    Column {
                        id: footer

                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        padding: 3
                        spacing: 1

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: (strip.modelData.gain || 0).toFixed(1)
                            font.pixelSize: 9
                            font.family: "monospace"
                            color: Theme.c.text
                        }

                        Text {
                            width: parent.width - 6
                            horizontalAlignment: Text.AlignHCenter
                            text: strip.modelData.name
                            font.pixelSize: 9
                            color: strip.modelData.audible ? Theme.c.text : Theme.c.textMuted
                            elide: Text.ElideMiddle
                        }
                    }
                }
            }

            // ---- the master ----
            Item {
                id: master

                width: 54
                height: strips.height

                Rectangle {
                    anchors.fill: parent
                    color: Theme.c.panelHeader
                }

                Text {
                    id: masterTitle

                    anchors.top: parent.top
                    anchors.topMargin: 6
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Mix")
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    color: Theme.c.text
                }

                Item {
                    id: masterFader

                    anchors.top: masterTitle.bottom
                    anchors.topMargin: 8
                    anchors.bottom: masterValue.top
                    anchors.bottomMargin: 6
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 18

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 2
                        height: parent.height
                        color: Theme.c.border
                    }

                    // Unity, marked: a master fader is pushed back to 0 far more often than
                    // to any other value, and a line to aim at is what makes that quick.
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 12
                        height: 1
                        color: Theme.c.textMuted
                        y: (masterFader.height - 8) * (1 - panel.meterFraction(0))
                    }

                    Rectangle {
                        width: 16
                        height: 8
                        radius: 2
                        anchors.horizontalCenter: parent.horizontalCenter
                        color: Theme.c.accent
                        y: Math.max(0, Math.min(
                            masterFader.height - height,
                            (masterFader.height - height) *
                                (1 - panel.meterFraction(Edit.masterGain))))
                    }

                    MouseArea {
                        anchors.fill: parent
                        onPositionChanged: (mouse) => {
                            if (!pressed)
                                return;
                            const f = 1 - Math.max(0, Math.min(1, mouse.y / masterFader.height));
                            Edit.setMasterGain(Math.pow(f, 1 / 1.8) * 66 - 60);
                        }
                        onDoubleClicked: Edit.setMasterGain(0)
                    }
                }

                Text {
                    id: masterValue

                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 6
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: Edit.masterGain.toFixed(1)
                    font.pixelSize: 9
                    font.family: "monospace"
                    color: Theme.c.text
                }
            }
        }
    }
}
