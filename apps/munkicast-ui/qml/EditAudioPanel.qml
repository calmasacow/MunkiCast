// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The mix: one meter per audio track, and the master.
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

    Row {
        id: strips

        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.top: parent.top
        anchors.topMargin: 8
        anchors.bottom: master.top
        anchors.bottomMargin: 8
        visible: panel.ready
        spacing: 10

        Repeater {
            model: panel.levels

            delegate: Item {
                id: strip

                required property var modelData

                width: 26
                height: strips.height

                Text {
                    id: stripName

                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    text: strip.modelData.name
                    font.pixelSize: 9
                    color: strip.modelData.audible ? Theme.c.text : Theme.c.textMuted
                }

                Rectangle {
                    id: meterTrack

                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: stripName.bottom
                    anchors.topMargin: 4
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 14
                    width: 10
                    radius: 2
                    color: Theme.c.window
                    border.width: 1
                    border.color: Theme.c.border

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 1
                        height: (meterTrack.height - 2) *
                                panel.meterFraction(strip.modelData.db)
                        radius: 1
                        // Green until it is loud, amber approaching the top, red at it: the
                        // one piece of colour coding every meter in the world agrees on.
                        color: strip.modelData.db > -1 ? Theme.c.error
                             : strip.modelData.db > -6 ? Theme.c.warning
                             : "#3fa65f"

                        Behavior on height {
                            NumberAnimation { duration: 60 }
                        }
                    }

                    // 0 dB, so the eye has something to judge the rest against.
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        y: (meterTrack.height - 2) * (1 - panel.meterFraction(0))
                        height: 1
                        color: Theme.c.textMuted
                        opacity: 0.6
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    text: strip.modelData.db <= -60 ? "−∞"
                                                    : strip.modelData.db.toFixed(0)
                    font.pixelSize: 8
                    font.family: "monospace"
                    color: Theme.c.textMuted
                }
            }
        }
    }

    // The master, across the bottom: the one fader that is reached for most often, so it is
    // not in the row of per-track strips where it could be grabbed by mistake.
    Item {
        id: master

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 46
        visible: panel.ready

        Text {
            id: masterLabel

            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.top: parent.top
            text: qsTr("Mix")
            font.pixelSize: 10
            color: Theme.c.textMuted
        }

        Text {
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.top: parent.top
            text: (Edit.masterGain > 0 ? "+" : "") + Edit.masterGain.toFixed(1) + " dB"
            font.pixelSize: 10
            font.family: "monospace"
            color: Theme.c.text
        }

        Rectangle {
            id: faderTrack

            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.top: masterLabel.bottom
            anchors.topMargin: 8
            height: 4
            radius: 2
            color: Theme.c.border

            // Unity is marked, because "back to where it was" is the most common thing asked
            // of a master fader and hunting for it by number is slow.
            Rectangle {
                x: faderTrack.width * (60 / 72)
                width: 1
                height: 8
                y: -2
                color: Theme.c.textMuted
            }

            Rectangle {
                id: faderHandle

                // -60..+12 across the track.
                x: Math.max(0, Math.min(faderTrack.width - width,
                       faderTrack.width * ((Edit.masterGain + 60) / 72) - width / 2))
                y: -4
                width: 10
                height: 12
                radius: 2
                color: faderMouse.pressed ? Theme.c.accent : Theme.c.text
            }

            MouseArea {
                id: faderMouse

                anchors.fill: parent
                anchors.topMargin: -10
                anchors.bottomMargin: -10
                cursorShape: Qt.SizeHorCursor

                function setFrom(x) {
                    const fraction = Math.max(0, Math.min(1, x / faderTrack.width));
                    Edit.setMasterGain(fraction * 72 - 60);
                }

                onPressed: mouse => setFrom(mouse.x)
                onPositionChanged: mouse => { if (pressed) setFrom(mouse.x); }
                onReleased: Edit.endGesture()
                // Double-click to unity, which is what the mark above is for.
                onDoubleClicked: {
                    Edit.setMasterGain(0);
                    Edit.endGesture();
                }
            }
        }
    }
}
