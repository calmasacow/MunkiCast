// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// The bus graph: one strip per bus, and the selected source's own routing beside it.
///
/// Sources and buses expose live meters, gain, pan, mute and monitor flags. The footer
/// selects the local playback sink; None disables playback without changing program routing.
/// Document edits are commands and undo with the rest of the show.
///
/// A dragged fader runs a command per move and the session collapses the run into one undo
/// step, landing back where the drag began. That is what `Document.setBusGain(..., dragging)`
/// and `endGesture()` are for; without the collapsing, one drag would bury every other edit
/// under four hundred "Change bus level" entries.
Item {
    id: panel

    /// Whether hidden strips are drawn. A view toggle, so it lives here rather than in the
    /// document: it is what one operator is looking at now, not a property of the show
    /// (ADR-0032).
    property bool showHidden: false

    /// How many strips are hidden right now, across both halves. Drives the footer caption and
    /// whether the eye is worth pressing.
    readonly property int hiddenCount: {
        if (Document.revision < 0) return 0;
        return Document.hiddenStripCount();
    }
    /// Below this the routing line goes, because a strip's fader matters more than a sentence
    /// about the selected source -- the arrow on each strip still routes it. 130 is the panel's
    /// minimum height in llayout, and this has to leave a usable strip at that size.
    readonly property bool showRouting: panel.height >= 150
    /// dB at the top and bottom of a fader. -60 reads as off without being a special case.
    readonly property real gainMin: -60
    readonly property real gainMax: 12

    property var monitoringDevices: ({ "choices": ["none", "default"],
                                        "choiceLabels": { "none": qsTr("None"), "default": qsTr("System default") } })
    function refreshMonitoringDevices() {
        monitoringDevices = Document.monitoringOutputChoices();
    }
    Component.onCompleted: refreshMonitoringDevices()
    onVisibleChanged: if (visible) refreshMonitoringDevices()

    function showBusMenu(x, y, id, kind, locked, hidden) {
        stripMenu.popup(x, y, [
            { text: qsTr("Filters"), action: () => Document.showFilters(id) },
            { text: qsTr("Lock Level"), checked: locked,
              action: () => Document.setBusLevelLocked(id, !locked) },
            { text: qsTr("Hide source"), checked: hidden,
              action: () => Document.setBusHidden(id, !hidden) },
            { separator: true },
            { text: qsTr("Delete channel"), enabled: kind !== "program",
              action: () => Document.removeBus(id) }
        ]);
    }

    function faderFraction(db) {
        return (Math.max(panel.gainMin, Math.min(panel.gainMax, db)) - panel.gainMin) /
               (panel.gainMax - panel.gainMin);
    }

    function faderDb(fraction) {
        const db = panel.gainMin + Math.max(0, Math.min(1, fraction)) *
                   (panel.gainMax - panel.gainMin);
        // Quarter-dB steps: fine enough to be a fader, coarse enough that nudging it twice to
        // the same place does not land two commands on the undo stack.
        return Math.round(db * 4) / 4;
    }

    function dbLabel(db) {
        if (db <= panel.gainMin)
            return "-∞";
        return (db > 0 ? "+" : "") + db.toFixed(1);
    }

    /// The sources in the scene on screen, packed from the left.
    ///
    /// This is the half an operator arriving from OBS is looking for: one strip per thing
    /// making sound, appearing because it is in this scene and going away when it leaves. The
    /// destinations below are ours and may not survive; these are the mixer.
    ListView {
        id: sourceStrips

        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: routing.top
        // Never past the divider: the destinations keep their room even in a narrow panel, and
        // the sources scroll instead.
        width: Math.min(contentWidth, divider.x - x)
        orientation: ListView.Horizontal
        clip: true
        model: Document.mixerStrips
        boundsBehavior: Flickable.StopAtBounds
        // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
        // because Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

        delegate: Item {
            id: src

            required property string nodeId
            required property string sceneId
            required property string name
            required property string busId
            required property string busName
            required property real gainDb
            required property bool mute
            required property bool monitor
            required property bool hidden
            required property bool levelLocked
            required property bool selected

            objectName: "sourceStrip"
            // A hidden strip takes no room unless "show hidden" is on, and is dimmed when it
            // does, so "hidden but revealed" is never mistaken for an ordinary strip.
            visible: !src.hidden || panel.showHidden
            width: src.visible ? 84 : 0
            height: ListView.view.height
            opacity: src.hidden ? 0.45 : 1.0

            property real shownGain: src.gainDb
            property bool dragging: false
            property var meter: ({})
            Connections {
                target: Preview
                function onChanged() { src.meter = Preview.audioLevel(src.nodeId); }
            }

            readonly property bool compact: src.height < 150
            readonly property real fixedHeight:
                16 + 14 + 20 + (src.compact ? 0 : 12) + 3 * ((src.compact ? 4 : 5) - 1)

            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                radius: 2
                color: src.selected ? Theme.c.panelHeaderActive : Theme.c.panelHeader
                border.width: 1
                border.color: src.selected ? Theme.c.accent : Theme.c.border
            }

            Column {
                id: srcColumn

                anchors.fill: parent
                anchors.margins: 6
                spacing: 3

                Text {
                    width: parent.width
                    height: 16
                    objectName: "mixerSourceName"
                    text: src.name
                    font.pixelSize: 10
                    font.bold: true
                    color: Theme.c.text
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                }

                // Where it goes. Unrouted is not muted -- one is silent by omission and the
                // other on purpose -- so an unrouted strip says so in the warning colour
                // rather than looking like every other strip at rest.
                Text {
                    width: parent.width
                    height: 12
                    visible: !src.compact
                    text: src.busId === "" ? qsTr("not routed") : src.busName
                    font.pixelSize: 9
                    color: src.busId === "" ? Theme.c.warning : Theme.c.textMuted
                    opacity: src.busId === "" ? 0.9 : 0.7
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                }

                MixerFader {
                    width: parent.width
                    height: Math.max(srcColumn.height - src.fixedHeight, 24)
                    gainDb: src.gainDb
                    muted: src.mute
                    gainMin: panel.gainMin
                    gainMax: panel.gainMax
                    locked: src.levelLocked
                    meterAvailable: src.meter.available === true
                    peakLeft: src.meter.left || 0
                    peakRight: src.meter.right || 0
                    onMoved: (db) => Document.setNodeGain(src.sceneId, src.nodeId, db, true)
                    onReleased: Document.endGesture()
                    onReset: Document.setNodeGain(src.sceneId, src.nodeId, 0)
                    onShownGainChanged: src.shownGain = shownGain
                    onDraggingChanged: src.dragging = dragging
                }

                Text {
                    width: parent.width
                    height: 14
                    text: panel.dbLabel(src.shownGain) + " dB"
                    font.pixelSize: 10
                    font.family: "monospace"
                    color: src.dragging ? Theme.c.accent : Theme.c.text
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                Item {
                    width: parent.width
                    height: 20

                    // The two buttons every strip has, and the only two (ADR-0031).
                    Row {
                        anchors.centerIn: parent
                        spacing: 3

                        ToolButton {
                            label: "M"
                            tip: src.mute ? qsTr("Unmute source") : qsTr("Mute source")
                            active: src.mute
                            activeColor: Theme.c.error
                            onClicked: Document.setNodeMute(src.sceneId, src.nodeId, !src.mute)
                        }

                        ToolButton {
                            objectName: "monitorButton"
                            label: "H"
                            tip: src.monitor ? qsTr("Stop monitoring this source")
                                             : qsTr("Monitor this source (local only)")
                            active: src.monitor
                            activeColor: Theme.c.accent
                            onClicked: Document.setNodeMonitor(src.sceneId, src.nodeId,
                                                               !src.monitor)
                        }
                    }
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                // Behind the controls, so clicking the strip selects the node without
                // swallowing a fader drag or the mute button.
                z: -1
                onClicked: (ev) => {
                    if (src.sceneId === Document.selectedScene)
                        Document.selectNode(src.nodeId);
                    if (ev.button !== Qt.RightButton)
                        return;
                    const at = mapToItem(panel, ev.x, ev.y);
                    stripMenu.popup(at.x, at.y, [
                        { text: qsTr("Filters"), action: () => Document.showFilters(src.nodeId) },
                        {
                            text: qsTr("Route audio"),
                            children: [{text: qsTr("Not routed"), checked: src.busId === "",
                                action: () => Document.routeNode(src.sceneId, src.nodeId, "")}]
                                .concat(Document.audioBusChoices().map(bus => ({
                                    text: bus.name, checked: src.busId === bus.id,
                                    action: () => Document.routeNode(src.sceneId, src.nodeId, bus.id)
                                })))
                        },
                        {
                            text: qsTr("Lock Level"),
                            checked: src.levelLocked,
                            action: () => Document.setNodeLevelLocked(
                                src.sceneId, src.nodeId, !src.levelLocked)
                        },
                        {
                            text: qsTr("Hide source"),
                            checked: src.hidden,
                            action: () => Document.setNodeHidden(
                                src.sceneId, src.nodeId, !src.hidden)
                        }
                    ]);
                }
            }
        }

        // An empty left half is the honest answer to "what has audio in this scene", and
        // better than nothing at all, which reads as a panel that failed to load.
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.top: parent.top
            anchors.topMargin: 10
            visible: sourceStrips.count === 0
            text: qsTr("No source in this scene has audio.")
            font.pixelSize: 10
            color: Theme.c.textMuted
            opacity: 0.7
        }
    }

    /// Just left of the leftmost destination. The gap between the two groups does most of the
    /// separating; this says the gap is a boundary rather than empty space.
    Rectangle {
        id: divider

        objectName: "mixerDivider"
        anchors.top: parent.top
        anchors.bottom: routing.top
        anchors.topMargin: 6
        anchors.bottomMargin: 6
        // Right-justified with the buses, so it moves as they do and the sources never reach
        // past it.
        x: Math.max(strips.x - 5, 60)
        width: 1
        color: Theme.c.border
    }

    /// The destinations, packed to the right.
    ///
    /// OBS does not show these at all. They stay because the operator asked for them to stay
    /// for now, knowing they may go -- so this is a decision to leave alone, not an oversight
    /// to tidy up later.
    ListView {
        id: strips

        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: routing.top
        width: Math.min(contentWidth, parent.width * 0.6)
        orientation: ListView.Horizontal
        clip: true
        model: Document.buses
        boundsBehavior: Flickable.StopAtBounds
        // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
        // because Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

        delegate: Item {
            id: strip

            required property string busId
            required property string name
            required property string kind
            required property real gainDb
            required property real pan
            required property bool mute
            required property bool monitor
            required property bool hidden
            required property bool levelLocked
            required property int channels
            required property int feedCount
            required property int sendCount
            required property bool carriesSelection
            property var meter: ({})
            Connections {
                target: Preview
                function onChanged() { strip.meter = Preview.audioLevel(strip.busId); }
            }

            objectName: "busStrip"
            visible: !strip.hidden || panel.showHidden
            width: strip.visible ? 84 : 0
            height: ListView.view.height
            opacity: strip.hidden ? 0.45 : 1.0

            /// What the fader is showing right now. It follows the document except while being
            /// dragged, so the value under the finger is not fighting the model's.
            property real shownGain: strip.gainDb
            property bool dragging: false

            /// A docked panel can be made short, and a mixer has to survive it. Below this the
            /// strip drops the kind line, the feed count and the pan -- in that order of value
            /// to someone running a show -- rather than squeezing the fader into nothing. The
            /// panel's own minimum height (llayout's studio_defaults) is the point at which
            /// what is left is still usable.
            readonly property bool compact: strip.height < 150
            /// Everything in the column that is not the fader, so the fader can have the rest.
            readonly property real fixedHeight:
                16 + 14 + 20 + (strip.compact ? 0 : 12 + 12 + 10) +
                3 * ((strip.compact ? 4 : 7) - 1)

            onGainDbChanged: {
                if (!strip.dragging)
                    strip.shownGain = strip.gainDb;
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                // Behind the controls, so it cannot swallow a fader drag or a button.
                z: -1
                onClicked: (ev) => {
                    const at = mapToItem(panel, ev.x, ev.y);
                    panel.showBusMenu(at.x, at.y, strip.busId, strip.kind,
                                      strip.levelLocked, strip.hidden);
                }
            }

            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                radius: 2
                color: strip.carriesSelection ? Theme.c.panelHeaderActive : Theme.c.panelHeader
                border.width: 1
                border.color: strip.carriesSelection ? Theme.c.accent : Theme.c.border
            }

            Column {
                id: column

                anchors.fill: parent
                anchors.margins: 6
                spacing: 3

                Item {
                    width: parent.width
                    height: 16

                    Text {
                        anchors.fill: parent
                        visible: !rename.visible
                        objectName: "mixerChannelName"
                        text: strip.name
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: Theme.c.text
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter

                        MouseArea {
                            anchors.fill: parent
                            onDoubleClicked: {
                                rename.text = strip.name;
                                rename.visible = true;
                                rename.forceActiveFocus();
                                rename.selectAll();
                            }
                        }
                    }

                    TextInput {
                        id: rename
                        objectName: "mixerChannelNameEditor"

                        anchors.fill: parent
                        visible: false
                        font.pixelSize: 11
                        color: Theme.c.text
                        selectionColor: Theme.c.accent
                        selectByMouse: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter

                        function commit() {
                            if (!visible)
                                return;
                            visible = false;
                            if (text !== strip.name)
                                Document.renameBus(strip.busId, text);
                        }
                        onAccepted: commit()
                        onActiveFocusChanged: {
                            if (!activeFocus)
                                commit();
                        }
                        Keys.onEscapePressed: visible = false
                    }
                }

                Text {
                    width: parent.width
                    height: 12
                    visible: !strip.compact
                    text: strip.kind + " · " + strip.channels + "ch"
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                    opacity: 0.8
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                }

                // What feeds this bus. Zero is worth saying out loud: a bus nothing routes to
                // looks exactly like a working one until the show starts.
                Text {
                    width: parent.width
                    height: 12
                    visible: !strip.compact
                    text: strip.feedCount === 0
                        ? qsTr("nothing feeding")
                        : qsTr("%n source(s)", "", strip.feedCount)
                    font.pixelSize: 9
                    color: strip.feedCount === 0 ? Theme.c.warning : Theme.c.textMuted
                    opacity: strip.feedCount === 0 ? 0.9 : 0.7
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                }

                // The fader takes whatever the rows above and below did not, which is why they
                // all declare a height: a Column cannot be asked for "the rest".
                MixerFader {
                    width: parent.width
                    height: Math.max(column.height - strip.fixedHeight, 24)
                    gainDb: strip.gainDb
                    muted: strip.mute
                    gainMin: panel.gainMin
                    gainMax: panel.gainMax
                    locked: strip.levelLocked
                    meterAvailable: strip.meter.available === true
                    peakLeft: strip.meter.left || 0
                    peakRight: strip.meter.right || 0
                    onMoved: (db) => Document.setBusGain(strip.busId, db, true)
                    onReleased: Document.endGesture()
                    onReset: Document.setBusGain(strip.busId, 0)
                    // The dB readout below follows the knob, not the document, so it is right
                    // mid-drag.
                    onShownGainChanged: strip.shownGain = shownGain
                    onDraggingChanged: strip.dragging = dragging
                }

                Text {
                    width: parent.width
                    height: 14
                    text: panel.dbLabel(strip.shownGain) + " dB"
                    font.pixelSize: 10
                    font.family: "monospace"
                    color: strip.dragging ? Theme.c.accent : Theme.c.text
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                Item {
                    width: parent.width
                    height: 20

                    Row {
                        anchors.centerIn: parent
                        spacing: 3

                        ToolButton {
                            label: "M"
                            tip: strip.mute ? qsTr("Unmute this bus") : qsTr("Mute this bus")
                            active: strip.mute
                            activeColor: Theme.c.error
                            onClicked: Document.setBusMute(strip.busId, !strip.mute)
                        }

                        // Heard locally, never removed from the mix. There is no solo:
                        // ADR-0031. "H" rather than a headphone glyph, which resolves to
                        // colour emoji here and so cannot be tinted to show it is latched.
                        ToolButton {
                            objectName: "monitorButton"
                            label: "H"
                            tip: strip.monitor ? qsTr("Stop monitoring this bus")
                                               : qsTr("Monitor this bus (local only)")
                            active: strip.monitor
                            activeColor: Theme.c.accent
                            onClicked: Document.setBusMonitor(strip.busId, !strip.monitor)
                        }

                        ToolButton {
                            label: "→"
                            tip: qsTr("Send the selected source's audio here")
                            enabled: Document.selectedNode !== "" &&
                                     Document.nodeProperties.hasAudio === true
                            onClicked: Document.routeSelectedNode(strip.busId)
                        }
                    }
                }

                // Pan, as a strip rather than a knob: it is the one control here that reads
                // better as a position than as a number.
                Item {
                    width: parent.width
                    height: 10
                    visible: !strip.compact

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: 3
                        radius: 1
                        color: Theme.c.window
                        border.width: 1
                        border.color: Theme.c.border
                    }

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        x: (parent.width - width) * (strip.pan + 1) / 2
                        width: 8
                        height: 8
                        radius: 4
                        color: panMouse.containsMouse
                            ? Theme.c.accent : Theme.c.panelHeaderActive
                        border.width: 1
                        border.color: Theme.c.border
                    }

                    MouseArea {
                        id: panMouse

                        objectName: "busPan"
                        anchors.fill: parent
                        anchors.margins: -3
                        hoverEnabled: true
                        onClicked: (mouse) => {
                            Document.setBusPan(strip.busId,
                                               Math.round((mouse.x / width * 2 - 1) * 20) / 20);
                        }
                        onDoubleClicked: Document.setBusPan(strip.busId, 0)
                    }
                }
            }
        }
    }

    // Where the selected source's audio goes. Routing lives here rather than in the properties
    // panel because the question "which bus is this on" is answered by looking at the buses.
    Rectangle {
        id: routing

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        height: visible ? 30 : 0
        visible: panel.showRouting
        color: Theme.c.panelBackground

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.c.border
        }

        Text {
            id: routingText

            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.right: routingActions.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: 10
            color: Theme.c.textMuted
            elide: Text.ElideRight
            text: {
                if (Document.selectedNode === "")
                    return qsTr("Select a source to route its audio.");
                const props = Document.nodeProperties;
                if (props.hasAudio !== true)
                    return qsTr("%1 has no audio of its own.").arg(props.name);
                if (props.audioBus === "")
                    // Unrouted is not muted, and the difference matters: one is silent on
                    // purpose and the other is silent by omission.
                    return qsTr("%1 is not routed anywhere — it is silent.").arg(props.name);
                return qsTr("%1 → %2 at %3 dB%4")
                    .arg(props.name)
                    .arg(props.audioBusName)
                    .arg(panel.dbLabel(props.audioGain))
                    .arg(props.audioMute ? qsTr(" (muted)") : "");
            }
        }

        Row {
            id: routingActions

            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            ToolButton {
                label: "M"
                tip: qsTr("Mute the selected source")
                enabled: Document.nodeProperties.hasAudio === true
                active: Document.nodeProperties.audioMute === true
                activeColor: Theme.c.error
                onClicked: Document.setSelectedNodeMute(
                    Document.nodeProperties.audioMute !== true)
            }

            ToolButton {
                label: "⊘"
                tip: qsTr("Unroute the selected source")
                enabled: Document.nodeProperties.hasAudio === true &&
                         Document.nodeProperties.audioBus !== ""
                onClicked: Document.routeSelectedNode("")
            }
        }
    }

    StripMenu {
        id: stripMenu

        z: 100
    }

    Rectangle {
        id: footer

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 26
        color: Theme.c.panelHeader

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.c.border
        }

        Row {
            id: footerActions
            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            ToolButton {
                objectName: "addAudioOutput"
                width: 76
                label: qsTr("+ Output")
                tip: qsTr("Add an audio output channel")
                onClicked: {
                    const names = Document.audioBusChoices().map(bus => bus.name);
                    let number = 1;
                    while (names.includes(qsTr("Output %1").arg(number))) ++number;
                    Document.addBus(qsTr("Output %1").arg(number), "track");
                    Qt.callLater(() => strips.positionViewAtEnd());
                }
            }

            ToolButton {
                objectName: "showHiddenToggle"
                label: ""
                tip: panel.showHidden ? qsTr("Hide hidden strips")
                                      : qsTr("Show hidden strips")
                active: panel.showHidden
                enabled: panel.hiddenCount > 0 || panel.showHidden
                onClicked: panel.showHidden = !panel.showHidden

                EyeIcon {
                    anchors.centerIn: parent
                    open: panel.showHidden
                    color: parent.enabled ? Theme.c.text : Theme.c.textMuted
                }
            }
        }

        ChoiceField {
            objectName: "monitoringOutputField"
            anchors.left: footerActions.right
            anchors.leftMargin: 6
            anchors.right: refreshMonitoring.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            label: width >= 180 ? qsTr("Monitor output") : ""
            labelWidth: width >= 180 ? 85 : 0
            Accessible.name: qsTr("Monitor output")
            choices: panel.monitoringDevices.choices
            choiceLabels: panel.monitoringDevices.choiceLabels
            value: Document.monitoringOutput
            onCommitted: newValue => Document.setMonitoringOutput(newValue)
        }

        ToolButton {
            id: refreshMonitoring
            objectName: "refreshMonitoringOutputs"
            width: 62
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            label: qsTr("Refresh")
            tip: panel.monitoringDevices.error || qsTr("Refresh audio outputs")
            onClicked: panel.refreshMonitoringDevices()
        }
    }
}
