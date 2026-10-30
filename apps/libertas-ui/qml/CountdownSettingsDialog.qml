// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import Libertas

/// One timer's settings, sectioned as the OBS plugin's dialog is: General, Timer Start, Timer
/// End, Display Format, Timer Type, Text Colours.
///
/// Edits are held here and written on OK or Apply rather than per keystroke, which is what the
/// plugin does and what keeps one visit to this dialog to one undo step instead of a dozen.
Window {
    id: dialog
    objectName: "countdownSettings"
    title: qsTr("Timer")
    width: 500; height: 732
    minimumWidth: 420; minimumHeight: 512
    color: Theme.c.window
    flags: Qt.Dialog | Qt.FramelessWindowHint
    DialogGeometry { target: dialog; title: dialog.title }

    property string sourceId: ""
    /// The timer as it is in the document, which is what Cancel goes back to.
    property var timer: ({})
    /// The pending edits, keyed as configureCountdown names them. Empty means nothing changed.
    property var pending: ({})
    readonly property bool dirty: Object.keys(pending).length > 0

    function open(id) {
        sourceId = id;
        reload();
        show();
        requestActivate();
    }

    function reload() {
        for (const row of Document.countdowns)
            if (row.id === dialog.sourceId) {
                dialog.timer = row;
                dialog.title = qsTr("Timer %1").arg(row.timerId || "");
                break;
            }
        dialog.pending = ({});
    }

    /// The edited value if there is one, else the document's, else the default.
    function field(key, fallback) {
        if (dialog.pending[key] !== undefined)
            return dialog.pending[key];
        return dialog.timer[key] === undefined ? fallback : dialog.timer[key];
    }

    /// A fresh object every time, never the same one mutated: assigning an object back onto the
    /// property it came from is not a change as far as QML is concerned, so `dirty` and every
    /// field bound through `field()` would go on showing the previous state -- and Apply, which
    /// is gated on `dirty`, would do nothing.
    function edit(key, value) {
        const next = {};
        for (const existing in dialog.pending)
            next[existing] = dialog.pending[existing];
        next[key] = value;
        dialog.pending = next;
    }

    function apply() {
        if (!dialog.dirty)
            return true;
        const before = JSON.stringify(dialog.timer);
        Document.configureCountdown(dialog.sourceId, dialog.pending);
        dialog.reload();
        // configureCountdown refuses a bad setting wholesale and says why, so an unchanged
        // document after a write means it was rejected and the dialog stays open on it.
        return JSON.stringify(dialog.timer) !== before;
    }

    // Follows the document, so a timer renamed or retimed from the dock while this is open does
    // not show stale values underneath the edits.
    Connections {
        target: Document
        function onChanged() { if (dialog.visible && !dialog.dirty) dialog.reload(); }
    }

    Flickable {
        id: scroll
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
        anchors.margins: 12
        anchors.topMargin: 44
        contentWidth: width
        contentHeight: body.height
        clip: true
        interactive: false
        NoDragScroll {}

        Column {
            id: body
            width: parent.width
            spacing: 10

            component Section: Rectangle {
                id: section
                default property alias content: inner.data
                required property string title
                width: body.width
                height: inner.height + heading.height + 18
                color: Theme.c.panelBackground
                border.width: 1
                border.color: Theme.c.border
                radius: 3

                Text {
                    id: heading
                    x: 10; y: 7
                    text: section.title
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    color: Theme.c.text
                }

                Column {
                    id: inner
                    x: 10
                    y: heading.height + 12
                    width: parent.width - 20
                    spacing: 6
                }
            }

            Section {
                title: qsTr("General")
                TextLine {
                    width: parent.width; label: qsTr("Timer ID"); labelWidth: 110
                    value: dialog.field("timerId", "")
                    onCommitted: value => dialog.edit("timerId", value)
                }
                ChoiceField {
                    width: parent.width; label: qsTr("Source"); labelWidth: 110
                    choices: {
                        if (Document.revision < 0) return [];
                        return Document.countdownTextSources().map(s => s.id);
                    }
                    choiceLabels: {
                        if (Document.revision < 0) return ({});
                        const names = {};
                        for (const s of Document.countdownTextSources()) names[s.id] = s.name;
                        return names;
                    }
                    value: dialog.sourceId
                    // The binding is which source this timer *is*, not a setting on it, so it
                    // is not editable here; the dock binds and unbinds.
                    enabled: false
                }
                Text {
                    width: parent.width
                    text: qsTr("The timer writes the time into this text source. Add that source to each scene that should show it.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    color: Theme.c.textMuted
                }
            }

            Section {
                title: qsTr("Timer Start Settings")
                CheckLabel {
                    text: qsTr("Start On Stream Start")
                    checked: dialog.field("startOnStreamStart", false)
                    onToggled: dialog.edit("startOnStreamStart", !checked)
                }
                CheckLabel {
                    x: 16
                    text: qsTr("Reset")
                    enabled: dialog.field("startOnStreamStart", false)
                    checked: dialog.field("resetOnStreamStart", false)
                    onToggled: if (enabled) dialog.edit("resetOnStreamStart", !checked)
                }
                Text {
                    width: parent.width
                    visible: dialog.field("startOnStreamStart", false)
                    text: qsTr("Saved, but nothing starts a broadcast in this build yet, so nothing triggers it.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    color: Theme.c.warning
                }
            }

            Section {
                title: qsTr("Timer End Settings")
                Row {
                    width: parent.width
                    spacing: 8
                    CheckLabel {
                        width: 110
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Message")
                        checked: dialog.field("showEndMessage", true)
                        onToggled: dialog.edit("showEndMessage", !checked)
                    }
                    TextLine {
                        width: parent.width - 118
                        anchors.verticalCenter: parent.verticalCenter
                        label: ""; labelWidth: 0
                        enabled: dialog.field("showEndMessage", true)
                        value: dialog.field("endMessage", "SOON")
                        onCommitted: value => dialog.edit("endMessage", value)
                    }
                }
                Row {
                    width: parent.width
                    spacing: 8
                    CheckLabel {
                        width: 110
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Scene")
                        checked: dialog.field("showEndScene", false)
                        onToggled: dialog.edit("showEndScene", !checked)
                    }
                    ChoiceField {
                        width: parent.width - 118
                        anchors.verticalCenter: parent.verticalCenter
                        label: ""; labelWidth: 0
                        enabled: dialog.field("showEndScene", false)
                        choices: {
                            if (Document.revision < 0) return [];
                            return [""].concat(Document.countdownScenes().map(s => s.id));
                        }
                        choiceLabels: {
                            if (Document.revision < 0) return ({});
                            const names = {"": qsTr("None")};
                            for (const s of Document.countdownScenes()) names[s.id] = s.name;
                            return names;
                        }
                        value: dialog.field("endScene", "")
                        onCommitted: value => dialog.edit("endScene", value)
                    }
                }
            }

            Section {
                title: qsTr("Display Format")
                Flow {
                    width: parent.width
                    spacing: 12
                    Repeater {
                        model: [
                            {key: "showDays", label: qsTr("Days"), on: false},
                            {key: "showHours", label: qsTr("Hours"), on: false},
                            {key: "showMinutes", label: qsTr("Min."), on: true},
                            {key: "showSeconds", label: qsTr("Sec."), on: true}
                        ]
                        delegate: CheckLabel {
                            required property var modelData
                            text: modelData.label
                            checked: dialog.field(modelData.key, modelData.on)
                            onToggled: dialog.edit(modelData.key, !checked)
                        }
                    }
                }
                CheckLabel {
                    text: qsTr("Leading Zero")
                    checked: dialog.field("leadingZero", true)
                    onToggled: dialog.edit("leadingZero", !checked)
                }
                Row {
                    width: parent.width
                    spacing: 8
                    CheckLabel {
                        width: 110
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Format Output")
                        checked: dialog.field("useFormattedOutput", false)
                        onToggled: dialog.edit("useFormattedOutput", !checked)
                    }
                    TextLine {
                        width: parent.width - 118
                        anchors.verticalCenter: parent.verticalCenter
                        label: ""; labelWidth: 0
                        enabled: dialog.field("useFormattedOutput", false)
                        value: dialog.field("format", "%time%")
                        placeholder: "IN: %time%"
                        onCommitted: value => dialog.edit("format", value)
                    }
                }
                Text {
                    width: parent.width
                    text: qsTr("%time% is the formatted time. %days% %hours% %minutes% %seconds% are the parts; add 2 (%seconds2%) to pad them to two digits.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    color: Theme.c.textMuted
                }
            }

            Section {
                title: qsTr("Timer Type")
                CheckLabel {
                    text: qsTr("Count Up")
                    checked: dialog.field("countUp", false)
                    onToggled: dialog.edit("countUp", !checked)
                }
                CheckLabel {
                    text: qsTr("Steady Second Increment (Period Timer)")
                    checked: dialog.field("steady", false)
                    onToggled: dialog.edit("steady", !checked)
                }
                Text {
                    width: parent.width
                    text: qsTr("Steady counts ticks rather than the clock: it never skips a second, but drifts if the machine is busy.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    color: Theme.c.textMuted
                }
            }

            Section {
                title: qsTr("Enable Text Colors")
                CheckLabel {
                    text: qsTr("Enable Text Colours")
                    checked: dialog.field("useTextColour", false)
                    onToggled: dialog.edit("useTextColour", !checked)
                }
                ColourField {
                    width: parent.width
                    label: qsTr("Main Text Color"); labelWidth: 110
                    enabled: dialog.field("useTextColour", false)
                    value: dialog.field("mainTextColour", "#ffffff")
                    onCommitted: value => dialog.edit("mainTextColour", value)
                }
                Text {
                    width: parent.width
                    text: qsTr("Rules:")
                    font.pixelSize: 11
                    color: Theme.c.textMuted
                }
                Column {
                    id: rules
                    width: parent.width
                    spacing: 4
                    enabled: dialog.field("useTextColour", false)

                    /// Rules travel as "min,max,r,g,b,a" joined by ";", which is how the document
                    /// stores them -- settings have no list type.
                    readonly property var parsed: {
                        const text = dialog.field("colourRules", "");
                        if (text === "")
                            return [];
                        return text.split(";").filter(r => r !== "").map(rule => {
                            const n = rule.split(",").map(Number);
                            return {minMs: n[0] || 0, maxMs: n[1] || 0,
                                    colour: "#" + [n[2], n[3], n[4]].map(v =>
                                        ("0" + Math.max(0, Math.min(255, v || 0)).toString(16)).slice(-2)).join("")};
                        });
                    }

                    function write(list) {
                        dialog.edit("colourRules", list.map(r => {
                            const hex = r.colour.replace("#", "");
                            return [Math.round(r.minMs), Math.round(r.maxMs),
                                    parseInt(hex.substr(0, 2), 16), parseInt(hex.substr(2, 2), 16),
                                    parseInt(hex.substr(4, 2), 16), 255].join(",");
                        }).join(";"));
                    }

                    Repeater {
                        model: rules.parsed
                        delegate: Row {
                            id: rule
                            required property int index
                            required property var modelData
                            width: rules.width
                            spacing: 6

                            NumberField {
                                width: (rules.width - 100) / 2
                                anchors.verticalCenter: parent.verticalCenter
                                label: qsTr("From"); labelWidth: 34
                                decimals: 0; minimum: 0; suffix: qsTr("s"); steppers: true
                                value: rule.modelData.minMs / 1000
                                onCommitted: value => {
                                    const list = rules.parsed;
                                    list[rule.index].minMs = Math.round(value * 1000);
                                    rules.write(list);
                                }
                            }
                            NumberField {
                                width: (rules.width - 100) / 2
                                anchors.verticalCenter: parent.verticalCenter
                                label: qsTr("To"); labelWidth: 24
                                decimals: 0; minimum: 0; suffix: qsTr("s"); steppers: true
                                value: rule.modelData.maxMs / 1000
                                onCommitted: value => {
                                    const list = rules.parsed;
                                    list[rule.index].maxMs = Math.round(value * 1000);
                                    rules.write(list);
                                }
                            }
                            ColourField {
                                width: 60
                                anchors.verticalCenter: parent.verticalCenter
                                label: ""; labelWidth: 0
                                value: rule.modelData.colour
                                onCommitted: value => {
                                    const list = rules.parsed;
                                    list[rule.index].colour = value;
                                    rules.write(list);
                                }
                            }
                            ToolButton {
                                anchors.verticalCenter: parent.verticalCenter
                                label: "✕"
                                tip: qsTr("Remove this rule")
                                onClicked: {
                                    const list = rules.parsed;
                                    list.splice(rule.index, 1);
                                    rules.write(list);
                                }
                            }
                        }
                    }

                    Text {
                        width: parent.width
                        visible: rules.parsed.length === 0
                        text: qsTr("No rules. The main colour is used for the whole countdown.")
                        wrapMode: Text.WordWrap
                        font.pixelSize: 10
                        color: Theme.c.textMuted
                    }

                    DialogButton {
                        text: qsTr("Add rule")
                        onClicked: {
                            const list = rules.parsed;
                            list.push({minMs: 0, maxMs: 10000, colour: "#ff0000"});
                            rules.write(list);
                        }
                    }
                }
                Text {
                    width: parent.width
                    text: qsTr("The first rule whose range contains the time wins; otherwise the main colour is used.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    color: Theme.c.textMuted
                }
            }
        }
    }

    Row {
        id: footer
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        spacing: 8

        DialogButton {
            text: qsTr("OK"); primary: true
            objectName: "countdownSettingsOk"
            onClicked: if (!dialog.dirty || dialog.apply()) dialog.close()
        }
        DialogButton {
            text: qsTr("Cancel")
            onClicked: { dialog.reload(); dialog.close(); }
        }
        DialogButton {
            text: qsTr("Apply")
            enabled: dialog.dirty
            onClicked: dialog.apply()
        }
    }
}
