// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as Controls
import MunkiCast

Item {
    id: dialog
    objectName: "filtersDialog"
    anchors.fill: parent
    z: 10000
    visible: false
    property string owner: ""
    property string selected: ""
    property var details: ({})
    property var current: ({})
    property var types: []
    property string addType: ""
    property string audioAddType: ""
    readonly property var domains: details.audioOnly ? [true] : details.hasAudio ? [true, false] : [false]
    property string previewCanvas: ""
    property string customKey: ""
    property bool refreshing: false
    function refresh() {
        if (refreshing) return;
        refreshing = true;
        details = Document.filterDetails(owner);
        if (!details.owner) { visible = false; refreshing = false; return; }
        const entries = details.effects || [];
        current = entries.find(e => e.id === selected) || entries[0] || {};
        selected = current.id || "";
        types = Document.filterTypes(owner);
        if (!types.some(t => t.type === addType)) addType = types.length ? types[0].type : "";
        refreshing = false;
    }
    function open(id) {
        owner = id; selected = ""; refresh(); visible = !!details.owner;
        previewCanvas = Preview.setFilterTarget(details.audioOnly ? "" : owner);
        forceActiveFocus();
    }
    function edit(key, value) {
        const values = Object.assign({}, current.values);
        const field = (current.schema || []).find(f => f.key === key);
        values[key] = field && field.displayScale ? value / field.displayScale : value;
        Document.editFilter(owner, selected, current.name, current.enabled, values);
    }
    function defaults() {
        const values = Object.assign({}, current.values);
        for (const field of current.schema || [])
            if (field.defaultValue !== undefined) values[field.key] = field.defaultValue;
        Document.editFilter(owner, selected, current.name, current.enabled, values);
    }
    onVisibleChanged: if (!visible) { Preview.setFilterTarget(""); previewCanvas = ""; }
    Connections { target: Document; function onChanged() { if (dialog.visible) Qt.callLater(dialog.refresh); } }
    Timer { interval: 1000; repeat: true; running: dialog.visible && dialog.current.type === "filter.vst" && dialog.current.status === "Loading plugin"; onTriggered: dialog.refresh() }
    Keys.onEscapePressed: visible = false
    Rectangle { anchors.fill: parent; color: "#80000000" }
    MouseArea { anchors.fill: parent; onClicked: {} onWheel: wheel => { wheel.accepted = true; } }
    Rectangle {
        id: frame
        width: Math.min(parent.width - 32, 1050)
        height: Math.min(parent.height - 32, 800)
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        objectName: "filtersFrame"
        DialogGeometry { minimumWidth: 620; minimumHeight: 440 }
        color: Theme.c.panelBackground; border.color: Theme.c.accent
        Text { x: 16; y: 12; text: qsTr("Filters — %1").arg(dialog.details.name || ""); color: Theme.c.text; font.pixelSize: 16 }
        Text { x: 16; y: 38; text: qsTr("Changes apply to every use of this source. Undo restores edits."); color: Theme.c.textMuted; font.pixelSize: 11 }
        Repeater {
            model: dialog.domains
            delegate: Rectangle {
                id: pane
                required property bool modelData
                required property int index
                objectName: modelData ? "audioFilterPane" : "videoFilterPane"
                readonly property var entries: (dialog.details.effects || []).filter(e => !!e.audio === modelData)
                readonly property var available: dialog.types.filter(t => !!t.audio === modelData)
                readonly property int selectedIndex: entries.findIndex(e => e.id === dialog.selected)
                readonly property string chosen: modelData ? dialog.audioAddType : dialog.addType
                readonly property string addType: available.some(t => t.type === chosen) ? chosen : available.length ? available[0].type : ""
                x: 12; y: 64 + index * (height + 12); width: 240
                height: (frame.height - 124 - (dialog.domains.length - 1) * 12) / dialog.domains.length
                color: Theme.c.window; border.color: Theme.c.border
                Text { x: 8; y: 8; text: pane.modelData ? qsTr("Audio Filters") : qsTr("Video Filters"); color: Theme.c.textMuted; font.pixelSize: 12 }
                ListView {
                    id: list
                    objectName: "filterList"
                    x: 4; y: 32; width: parent.width - 8; height: parent.height - 112
                    clip: true; interactive: false
                    NoDragScroll {}
                    model: pane.entries; spacing: 4
                    delegate: Rectangle {
                        id: entry
                        required property var modelData
                        objectName: "assignedFilter"
                        property string effectId: modelData.id
                        width: list.width; height: 34
                        color: dialog.selected === modelData.id ? Theme.c.panelHeaderActive : "transparent"
                        MouseArea { anchors.fill: parent; onClicked: { dialog.selected = entry.modelData.id; dialog.refresh(); } }
                        CheckLabel {
                            x: 4; width: 24; height: parent.height; text: ""
                            checked: entry.modelData.enabled
                            onToggled: Document.editFilter(dialog.owner, entry.modelData.id, entry.modelData.name, !checked, entry.modelData.values)
                        }
                        Text { x: 32; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 38; elide: Text.ElideRight; text: entry.modelData.name; color: Theme.c.text; font.pixelSize: 12 }
                    }
                }
                ChoiceField {
                    objectName: "filterType"
                    x: 6; y: parent.height - 74; width: parent.width - 12
                    label: ""; labelWidth: 0
                    choices: pane.available.map(t => t.type)
                    choiceLabels: { const labels = {}; for (const t of pane.available) labels[t.type] = t.name; return labels; }
                    value: pane.addType
                    onCommitted: value => { if (pane.modelData) dialog.audioAddType = value; else dialog.addType = value; }
                }
                Row {
                    x: 6; anchors.bottom: parent.bottom; anchors.bottomMargin: 6; spacing: 4
                    DialogButton {
                        objectName: "filterAdd"; minimumWidth: 44; text: qsTr("Add"); enabled: pane.addType !== ""
                        onClicked: {
                            Document.addFilter(dialog.owner, pane.addType);
                            dialog.refresh();
                            const entries = dialog.details.effects;
                            if (entries.length) dialog.selected = entries[entries.length - 1].id;
                            dialog.refresh();
                        }
                    }
                    DialogButton { objectName: "filterUp"; minimumWidth: 40; text: qsTr("Up"); enabled: pane.selectedIndex > 0; onClicked: Document.moveFilter(dialog.owner, dialog.selected, -1) }
                    DialogButton { objectName: "filterDown"; minimumWidth: 48; text: qsTr("Down"); enabled: pane.selectedIndex >= 0 && pane.selectedIndex + 1 < pane.entries.length; onClicked: Document.moveFilter(dialog.owner, dialog.selected, 1) }
                    DialogButton { objectName: "filterRemove"; minimumWidth: 66; text: qsTr("Remove"); enabled: pane.selectedIndex >= 0; onClicked: Document.removeFilter(dialog.owner, dialog.selected) }
                }
            }
        }
        Rectangle {
            id: preview
            objectName: "filterSourcePreview"
            x: 270; y: 64; width: frame.width - 286
            height: visible ? Math.min(250, frame.height * 0.32) : 0
            visible: !dialog.details.audioOnly
            color: "#101010"; border.color: Theme.c.border
            readonly property int generation: Preview.generation
            readonly property bool ready: dialog.previewCanvas !== "" && generation >= 0 && Preview.hasFrame(dialog.previewCanvas)
            Image {
                anchors.fill: parent; anchors.margins: 1
                cache: false; fillMode: Image.PreserveAspectFit
                source: preview.ready ? "image://program/" + dialog.previewCanvas + "/" + preview.generation : ""
            }
            Text {
                anchors.centerIn: parent; visible: !preview.ready
                text: qsTr("Waiting for source preview"); color: Theme.c.textMuted
            }
            Text {
                anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 6
                text: preview.ready ? Preview.caption(dialog.previewCanvas) : ""
                color: Theme.c.textMuted; font.pixelSize: 10
            }
        }
        Flickable {
            id: scroll
            interactive: false; NoDragScroll {}
            x: 270; y: preview.visible ? preview.y + preview.height + 12 : 64
            width: frame.width - 286; height: frame.height - y - 98
            clip: true; contentHeight: settings.height
            Controls.ScrollBar.vertical: Controls.ScrollBar { policy: Controls.ScrollBar.AsNeeded }
            Column {
                id: settings
                width: scroll.width - 14; spacing: 8
                visible: !!dialog.selected
                TextLine {
                    objectName: "filterName"
                    width: parent.width; label: qsTr("Name"); labelWidth: 145
                    value: dialog.current.name || ""
                    onCommitted: value => { if (!dialog.refreshing && value !== dialog.current.name) Document.editFilter(dialog.owner, dialog.selected, value, dialog.current.enabled, dialog.current.values); }
                }
                CheckLabel {
                    objectName: "filterEnabled"
                    x: 149; width: parent.width - x; height: 24; text: qsTr("Enabled")
                    checked: dialog.current.enabled === true
                    onToggled: Document.editFilter(dialog.owner, dialog.selected, dialog.current.name, !checked, dialog.current.values)
                }
                Text { width: parent.width; wrapMode: Text.WordWrap; text: dialog.current.status || ""; visible: text !== ""; color: Theme.c.textMuted }
                Text { width: parent.width; wrapMode: Text.WordWrap; visible: dialog.current.type === "filter.vst"; text: qsTr("Linux VST2 (.so). Generic parameter controls; plugin windows are not supported. Adds at least 10 ms audio latency without automatic compensation."); color: Theme.c.textMuted }
                Text { width: parent.width; wrapMode: Text.WordWrap; visible: dialog.current.type === "filter.hdr-tonemap" || dialog.current.type === "filter.sdr-white"; text: qsTr("These controls map into SDR output. End-to-end HDR output is not supported."); color: Theme.c.textMuted }
                Text { text: qsTr("No properties available."); visible: (dialog.current.schema || []).length === 0; color: Theme.c.textMuted }
                ChoiceField {
                    width: parent.width; label: qsTr("Key color type"); labelWidth: 145
                    visible: dialog.current.type === "filter.color-key" || dialog.current.type === "filter.chroma-key"
                    choices: ["Green", "Blue", "Magenta", "Custom"]
                    value: { const c = (dialog.current.values || {}).key_color; return dialog.customKey === dialog.selected ? "Custom" : c === "#00ff00" ? "Green" : c === "#0000ff" ? "Blue" : c === "#ff00ff" ? "Magenta" : "Custom"; }
                    onCommitted: value => {
                        dialog.customKey = value === "Custom" ? dialog.selected : "";
                        if (value !== "Custom") dialog.edit("key_color", value === "Green" ? "#00ff00" : value === "Blue" ? "#0000ff" : "#ff00ff");
                    }
                }
                ChoiceField {
                    width: parent.width; label: qsTr("Resolution preset"); labelWidth: 145
                    visible: dialog.current.type === "filter.scale"
                    choices: ["Custom", "1920x1080", "2560x1440", "3840x2160", "1080x1920", "16:9", "9:16", "4:3"]
                    value: {
                        const values = dialog.current.values || {};
                        const size = values.width + (values.aspect_only ? ":" : "x") + values.height;
                        return choices.indexOf(size) >= 0 ? size : "Custom";
                    }
                    onCommitted: value => {
                        if (value === "Custom") return;
                        const aspect = value.indexOf(":") >= 0, parts = value.split(aspect ? ":" : "x");
                        const values = Object.assign({}, dialog.current.values, {width: Number(parts[0]), height: Number(parts[1]), aspect_only: aspect});
                        Document.editFilter(dialog.owner, dialog.selected, dialog.current.name, dialog.current.enabled, values);
                    }
                }
                ChoiceField {
                    width: parent.width; label: qsTr("Preset"); labelWidth: 145
                    visible: dialog.current.type === "filter.expander"
                    choices: ["Custom", "Expander", "Gate"]; value: "Custom"
                    onCommitted: value => {
                        if (value === "Custom") return;
                        const values = Object.assign({}, dialog.current.values, {ratio: value === "Gate" ? 10 : 2, threshold: -40, attack_time: 10, release_time: value === "Gate" ? 125 : 50, output_gain: 0, detector: "RMS"});
                        Document.editFilter(dialog.owner, dialog.selected, dialog.current.name, dialog.current.enabled, values);
                    }
                }
                Repeater {
                    model: dialog.current.schema || []
                    delegate: FilterSetting {
                        required property var modelData
                        width: settings.width; field: modelData
                        onCommitted: value => dialog.edit(modelData.key, value)
                        onBrowseRequested: {
                            const owner = dialog.owner, effect = dialog.selected;
                            const path = FileDialog.pick(qsTr("Choose filter file"), String(modelData.value || ""), false);
                            if (path !== "" && owner === dialog.owner && effect === dialog.selected) dialog.edit(modelData.key, path);
                            else if (FileDialog.lastError() !== "") NoticeLog.add(FileDialog.lastError());
                        }
                    }
                }
            }
        }
        DialogButton { objectName: "filterDefaults"; x: 270; y: frame.height - 88; text: qsTr("Defaults"); enabled: !!dialog.selected; onClicked: dialog.defaults() }
        DialogButton {
            anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 12
            text: qsTr("Close"); onClicked: dialog.visible = false
        }
    }
}
