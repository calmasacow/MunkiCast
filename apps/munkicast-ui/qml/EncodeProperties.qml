// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// The settings for whatever is selected on the graph.
///
/// Selection is view state, not document state: the graph holds it and hands it here. Nothing
/// about which node someone is looking at belongs in a file that goes to another machine.
///
/// Every field edits one node. With nothing selected the column says so rather than showing
/// the first one, for the same reason PropertiesPanel does (ADR-0037).
Item {
    id: panel

    required property string selectedKind
    required property string selectedId

    property var encode: ({})
    property var audio: ({})
    property string destinationName: ""
    property string outputVideoId: ""
    property string outputAudioId: ""
    property var destination: ({})
    property string chosenPreset: ""
    property string newPresetName: ""
    property var pendingKeys: ({})
    property var savingKeys: ({})
    property var savedKeys: ({})
    readonly property string pendingKey: panel.pendingKeys[panel.selectedId] || ""
    function setPendingKey(id, value) {
        const keys = Object.assign({}, panel.pendingKeys);
        if (value === "") delete keys[id];
        else keys[id] = value;
        panel.pendingKeys = keys;
    }
    function savePendingKey() {
        const id = panel.selectedId;
        if (panel.pendingKey === "" || panel.savingKeys[id] ||
            panel.savedKeys[id] === panel.pendingKey) return;
        const saving = Object.assign({}, panel.savingKeys);
        saving[id] = true;
        panel.savingKeys = saving;
        Document.saveOutputKey(id, panel.pendingKey);
    }
    onPendingKeyChanged: if (pendingKey === "") destinationKey.clear()
    readonly property bool streamDestination: panel.selectedKind === "output" &&
        panel.destination.destinationKind !== "file" && panel.destination.destinationKind !== "replay"
    readonly property var connector: Document.destinationKinds.find(
        entry => entry.id === panel.destination.service) || ({})
    readonly property var servers: panel.connector.servers || []
    readonly property bool usesKey: panel.streamDestination && panel.destination.destinationKind !== "srt"
    readonly property bool accountBusy: Document.connectors.busy && Document.connectors.target === panel.selectedId
    property string youtubeStream: ""
    readonly property var extra: panel.encode.extra || ({})
    readonly property string implementation: panel.encode.implementation || ""

    property size canvasSize: Qt.size(0, 0)
    property string videoCanvasId: ""
    property var canvasIds: []
    property var canvasLabels: ({})
    readonly property bool rescaled: (panel.encode.width || 0) > 0
    property bool customResolution: false
    onSelectedIdChanged: { customResolution = false; youtubeStream = ""; refresh(); }
    onSelectedKindChanged: refresh()
    Component.onCompleted: refresh()

    function refresh() {
        panel.encode = panel.selectedKind === "encode" ? Document.encodeSettings(panel.selectedId) : ({});
        panel.audio = panel.selectedKind === "audio-encode" ? Document.audioEncodeSettings(panel.selectedId) : ({});
        const output = panel.selectedKind === "output" ? Document.outputEncoderSettings(panel.selectedId) : ({});
        panel.destination = output;
        panel.destinationName = output.outputName || "";
        panel.outputVideoId = output.id || "";
        panel.outputAudioId = panel.selectedKind === "output" ? Document.outputAudioEncode(panel.selectedId) : "";
        panel.canvasSize = Document.canvasSizeOf(Document.encodeCanvasOf(panel.selectedId));
        panel.videoCanvasId = Document.encodeCanvasOf(panel.selectedId);
        panel.canvasIds = Document.canvasIds();
        const labels = {};
        for (const id of panel.canvasIds)
            labels[id] = Document.canvasNameOf(id);
        panel.canvasLabels = labels;
    }

    // Document emits `changed` after every command. Invokable lookups below must use this
    // revision in their control flow: an unused `void revision` read is optimized away by
    // compiled QML, leaving an open dropdown's labels/choices stale until it is recreated.
    property int revision: 0
    Connections {
        target: Document
        function onChanged() { panel.revision++; panel.refresh(); }
        function onOutputKeySaveFinished(id, saved) {
            const saving = Object.assign({}, panel.savingKeys);
            delete saving[id];
            panel.savingKeys = saving;
            if (saved) {
                const keys = Object.assign({}, panel.savedKeys);
                keys[id] = panel.pendingKeys[id];
                panel.savedKeys = keys;
            }
        }
    }

    Connections {
        target: Document.connectors
        function onChanged() { panel.revision++; }
    }

    function setExtra(key, value) {
        Document.setEncodeExtra(panel.selectedId, key, value);
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.panelBackground
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: Theme.c.border
    }

    Text {
        anchors.centerIn: parent
        visible: panel.selectedKind === ""
        text: qsTr("No node selected")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Flickable {
        anchors.fill: parent
        anchors.leftMargin: 1
        visible: panel.selectedKind !== ""
        contentHeight: body.implicitHeight + 24
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // Nothing scrolls by being dragged (ADR-0039); the wheel has to come back because
        // Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

        Column {
            id: body

            x: 14
            y: 12
            width: parent.width - 28
            spacing: 6

            Row {
                width: parent.width
                spacing: 6

                Text {
                    id: titleLabel
                    visible: headerIcon.visible
                    text: qsTr("Properties:")
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Theme.c.accent
                    bottomPadding: 6
                }

                Image {
                    id: headerIcon
                    objectName: "propertiesServiceIcon"
                    visible: panel.selectedKind === "output" && (panel.destination.serviceIcon || "") !== ""
                    source: visible ? panel.destination.serviceIcon : ""
                    width: 18
                    height: 18
                    y: (titleLabel.implicitHeight - titleLabel.bottomPadding - height) / 2
                    sourceSize: Qt.size(36, 36)
                    fillMode: Image.PreserveAspectFit
                }

                Text {
                    width: parent.width - (headerIcon.visible ? titleLabel.width + headerIcon.width + 12 : 0)
                    text: {
                        if (panel.revision < 0) return "";
                        if (panel.selectedKind === "encode")
                            return qsTr("Properties: %1").arg(panel.encode.name || "");
                        if (panel.selectedKind === "audio-encode")
                            return qsTr("Properties: %1").arg(panel.audio.name || "");
                        if (panel.selectedKind === "canvas")
                            return qsTr("Properties: %1").arg(
                                Document.canvasNameOf(panel.selectedId));
                        if (headerIcon.visible)
                            return nodeName.value;
                        if (panel.selectedKind === "output" || panel.selectedKind === "bus")
                            return qsTr("Properties: %1").arg(nodeName.value);
                        return qsTr("Properties");
                    }
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Theme.c.accent
                    elide: Text.ElideRight
                    bottomPadding: 6
                }
            }

            // Names for the destination and bus selections.
            QtObject {
                id: nodeName

                readonly property string value: {
                    if (panel.revision < 0) return "";
                    if (panel.selectedKind === "output")
                        return panel.destinationName;
                    if (panel.selectedKind === "bus") {
                        const buses = Document.audioBusChoices();
                        for (let i = 0; i < buses.length; ++i)
                            if (buses[i].id === panel.selectedId)
                                return buses[i].name;
                    }
                    return "";
                }
            }

            // --- a video encode ---------------------------------------------------------
            Column {
                width: parent.width
                spacing: 6
                visible: panel.selectedKind === "encode"

                ChoiceField {
                    objectName: "savedEncoderPreset"
                    width: parent.width
                    label: qsTr("Saved preset")
                    labelWidth: 150
                    choices: [""].concat(Document.encoderPresetNames)
                    choiceLabels: ({"": qsTr("Choose a preset")})
                    value: panel.chosenPreset
                    onCommitted: value => panel.chosenPreset = value
                }
                Row {
                    spacing: 6
                    DialogButton {
                        objectName: "applyEncoderPreset"
                        text: qsTr("Apply preset")
                        enabled: panel.chosenPreset !== ""
                        onClicked: Document.applyEncoderPreset(panel.selectedId, panel.chosenPreset)
                    }
                    DialogButton {
                        objectName: "replaceEncoderPreset"
                        text: qsTr("Replace selected")
                        enabled: panel.chosenPreset !== "" && panel.chosenPreset !== "Default 16:9" && panel.chosenPreset !== "Default 9:16"
                        onClicked: Document.saveEncoderPreset(panel.selectedId, panel.chosenPreset, true)
                    }
                    DialogButton {
                        objectName: "removeEncoderPreset"
                        text: qsTr("Delete preset")
                        enabled: panel.chosenPreset !== "" && panel.chosenPreset !== "Default 16:9" && panel.chosenPreset !== "Default 9:16"
                        onClicked: {
                            if (Document.removeEncoderPreset(panel.chosenPreset)) panel.chosenPreset = "";
                        }
                    }
                }
                TextLine {
                    id: presetName
                    objectName: "newEncoderPresetName"
                    width: parent.width
                    label: qsTr("New preset name")
                    labelWidth: 150
                    value: panel.newPresetName
                    commitOnEdit: true
                    onCommitted: value => panel.newPresetName = value
                }
                DialogButton {
                    objectName: "saveEncoderPreset"
                    text: qsTr("Save as new preset")
                    enabled: panel.newPresetName.trim().length > 0
                    onClicked: {
                        if (Document.saveEncoderPreset(panel.selectedId, panel.newPresetName)) {
                            panel.chosenPreset = panel.newPresetName.trim();
                            panel.newPresetName = "";
                            presetName.clear();
                        }
                    }
                }
                ChoiceField {
                    objectName: "encodeCanvasInput"
                    width: parent.width
                    label: qsTr("Canvas")
                    labelWidth: 150
                    choices: panel.canvasIds
                    choiceLabels: panel.canvasLabels
                    value: panel.videoCanvasId
                    onCommitted: value => Document.setEncodeCanvas(panel.selectedId, value)
                }

                ChoiceField {
                    objectName: "encodeVideoEncoder"
                    width: parent.width
                    label: qsTr("Video Encoder")
                    labelWidth: 150
                    choices: Document.codecs.map(c => c.title)
                    value: {
                        for (const c of Document.codecs)
                            if (c.codec === panel.encode.codec && c.implementation === panel.implementation)
                                return c.title;
                        return panel.encode.codec + "/" + panel.implementation;
                    }
                    onCommitted: value => {
                        const codec = Document.codecs.find(c => c.title === value);
                        if (codec) Document.setEncodeEncoder(panel.selectedId, codec.codec, codec.implementation);
                    }
                }

                ChoiceField {
                    objectName: "encodeRescale"
                    width: parent.width
                    label: qsTr("Rescale Output")
                    labelWidth: 150
                    choices: ["disabled", "bilinear", "bicubic", "lanczos"]
                    choiceLabels: ({"disabled": qsTr("Disabled"), "bilinear": qsTr("Bilinear"),
                                    "bicubic": qsTr("Bicubic"), "lanczos": qsTr("Lanczos")})
                    value: panel.rescaled ? (panel.extra.rescale_filter || "bicubic") : "disabled"
                    onCommitted: value => Document.setEncodeRescale(panel.selectedId,
                        value === "disabled" ? 0 : (panel.encode.width || panel.canvasSize.width),
                        value === "disabled" ? 0 : (panel.encode.height || panel.canvasSize.height),
                        value === "disabled" ? "" : value)
                }

                ChoiceField {
                    objectName: "encodeResolution"
                    width: parent.width
                    label: qsTr("Resolution")
                    labelWidth: 150
                    enabled: panel.rescaled
                    choices: {
                        const sizes = ["3840x2160", "2560x1440", "1920x1080", "1600x900", "1280x720",
                                       "1080x1920", "720x1280"];
                        const current = (panel.encode.width || panel.canvasSize.width) + "x"
                                      + (panel.encode.height || panel.canvasSize.height);
                        if (!sizes.includes(current)) sizes.unshift(current);
                        return sizes.concat(["custom"]);
                    }
                    choiceLabels: ({"custom": qsTr("Custom…")})
                    value: panel.customResolution ? "custom"
                         : (panel.encode.width || panel.canvasSize.width) + "x"
                         + (panel.encode.height || panel.canvasSize.height)
                    onCommitted: value => {
                        panel.customResolution = value === "custom";
                        if (!panel.customResolution) {
                            const size = value.split("x").map(Number);
                            Document.setEncodeRescale(panel.selectedId, size[0], size[1]);
                        }
                    }
                }

                Row {
                    width: parent.width
                    spacing: 8
                    visible: panel.rescaled && panel.customResolution
                    NumberField {
                        width: (parent.width - 8) / 2
                        label: qsTr("Width")
                        decimals: 0
                        minimum: 2
                        maximum: 16384
                        value: panel.encode.width || 0
                        onCommitted: value => Document.setEncodeRescale(panel.selectedId,
                            Math.round(value), panel.encode.height)
                    }
                    NumberField {
                        width: (parent.width - 8) / 2
                        label: qsTr("Height")
                        decimals: 0
                        minimum: 2
                        maximum: 16384
                        value: panel.encode.height || 0
                        onCommitted: value => Document.setEncodeRescale(panel.selectedId,
                            panel.encode.width, Math.round(value))
                    }
                }

                Rectangle { width: parent.width; height: 1; color: Theme.c.border }

                TextLine {
                    width: parent.width
                    objectName: "encodeName"
                    label: qsTr("Encoder Name")
                    labelWidth: 150
                    value: panel.encode.name || ""
                    onCommitted: value => Document.renameEncode(panel.selectedId, value)
                }

                ChoiceField {
                    width: parent.width
                    objectName: "encodeRateControl"
                    label: qsTr("Rate Control")
                    labelWidth: 150
                    choices: panel.implementation === "x264" ? ["cbr", "vbr", "crf"]
                           : panel.implementation === "nvenc" ? ["cbr", "vbr", "cqp"]
                           : ["cbr", "vbr"]
                    choiceLabels: ({"cbr": qsTr("Constant Bitrate"),
                                    "vbr": qsTr("Variable Bitrate"),
                                    "cqp": qsTr("Constant QP"),
                                    "crf": qsTr("Constant Quality")})
                    value: panel.encode.rateControl || "cbr"
                    onCommitted: value => Document.setEncodeOptions(
                        panel.selectedId, value, panel.encode.preset || "",
                        panel.encode.keyframeMs ?? 2000)
                }

                NumberField {
                    width: parent.width
                    objectName: "encodeBitrate"
                    label: qsTr("Bitrate")
                    labelWidth: 150
                    minimum: 1
                    maximum: 1000000
                    decimals: 0
                    suffix: qsTr("Kbps")
                    steppers: true
                    step: 500
                    visible: !["cqp", "crf"].includes(panel.encode.rateControl || "cbr")
                    value: panel.encode.bitrate || 0
                    onCommitted: value => Document.setEncodeBitrate(
                        panel.selectedId, Math.max(1, Math.round(value)))
                }

                NumberField {
                    objectName: "encodeQuality"
                    width: parent.width
                    label: panel.encode.rateControl === "cqp" ? qsTr("Constant QP") : qsTr("Quality (CRF)")
                    labelWidth: 150
                    visible: ["cqp", "crf"].includes(panel.encode.rateControl)
                    decimals: 0
                    steppers: true
                    minimum: 0
                    maximum: panel.encode.codec === "av1" ? 63 : 51
                    value: Number(panel.extra.quality ?? 23)
                    onCommitted: value => panel.setExtra("quality", Math.round(value))
                }

                NumberField {
                    width: parent.width
                    objectName: "encodeKeyframe"
                    label: qsTr("Keyframe Interval")
                    labelWidth: 150
                    minimum: 0
                    maximum: 600
                    decimals: 0
                    suffix: qsTr("s")
                    steppers: true
                    value: (panel.encode.keyframeMs || 0) / 1000
                    onCommitted: value => Document.setEncodeOptions(
                        panel.selectedId, panel.encode.rateControl || "cbr",
                        panel.encode.preset || "", Math.round(value * 1000))
                }

                ChoiceField {
                    width: parent.width
                    objectName: "encodePreset"
                    label: panel.implementation === "x264" ? qsTr("CPU Usage Preset")
                                                           : qsTr("Preset")
                    labelWidth: 150
                    choices: panel.implementation === "nvenc"
                             ? ["p1", "p2", "p3", "p4", "p5", "p6", "p7"]
                           : panel.implementation === "x264"
                             ? ["", "ultrafast", "superfast", "veryfast", "faster", "fast",
                                "medium", "slow", "slower", "veryslow", "placebo"]
                           : [""]
                    choiceLabels: ({"": qsTr("Encoder default"),
                                    "p1": "P1: Fastest", "p2": "P2: Faster", "p3": "P3: Fast",
                                    "p4": "P4: Medium", "p5": "P5: Slow (Good Quality)",
                                    "p6": "P6: Slower", "p7": "P7: Slowest (Best Quality)"})
                    value: panel.encode.preset || ""
                    onCommitted: value => Document.setEncodeOptions(
                        panel.selectedId, panel.encode.rateControl || "cbr", value,
                        panel.encode.keyframeMs ?? 2000)
                }

                // The nvenc knobs. These live in EncoderConfig::extra, which is where the
                // outputs manager already drives them from -- same keys, same meanings.
                ChoiceField {
                    width: parent.width
                    visible: panel.implementation === "nvenc"
                    objectName: "encodeTuning"
                    label: qsTr("Tuning")
                    labelWidth: 150
                    choices: ["hq", "ll", "ull", "lossless"]
                    choiceLabels: ({"hq": qsTr("High Quality"), "ll": qsTr("Low Latency"),
                                    "ull": qsTr("Ultra Low Latency"),
                                    "lossless": qsTr("Lossless")})
                    value: String(panel.extra["nvenc.tuning"] ?? "hq")
                    onCommitted: value => panel.setExtra("nvenc.tuning", value)
                }

                ChoiceField {
                    width: parent.width
                    visible: panel.implementation === "nvenc"
                    objectName: "encodeMultipass"
                    label: qsTr("Multipass Mode")
                    labelWidth: 150
                    choices: ["disabled", "quarter", "full"]
                    choiceLabels: ({"disabled": qsTr("Single Pass"),
                                    "quarter": qsTr("Two Passes (Quarter Resolution)"),
                                    "full": qsTr("Two Passes (Full Resolution)")})
                    value: String(panel.extra["nvenc.multipass"] ?? "quarter")
                    onCommitted: value => panel.setExtra("nvenc.multipass", value)
                }

                ChoiceField {
                    width: parent.width
                    visible: ["nvenc", "x264"].includes(panel.implementation)
                    objectName: "encodeProfile"
                    label: qsTr("Profile")
                    labelWidth: 150
                    choices: panel.implementation === "x264" ? ["", "baseline", "main", "high"]
                           : (panel.encode.codec === "h264") ? ["baseline", "main", "high"]
                           : (panel.encode.codec === "hevc") ? ["main", "main10"] : ["main"]
                    choiceLabels: ({"": qsTr("Encoder default"), "baseline": qsTr("Baseline"), "main": qsTr("Main"),
                                    "high": qsTr("High"), "main10": qsTr("Main 10")})
                    value: String(panel.extra[panel.implementation + ".profile"] ??
                                  (panel.encode.codec === "h264" ? "high" : "main"))
                    onCommitted: value => panel.setExtra(panel.implementation + ".profile", value)
                }

                Item { width: 1; height: 4 }

                CheckLabel {
                    visible: panel.implementation === "nvenc"
                    objectName: "encodeLookahead"
                    text: qsTr("Look Ahead")
                    checked: panel.extra["nvenc.lookahead"] === true
                    onToggled: panel.setExtra("nvenc.lookahead",
                                              !(panel.extra["nvenc.lookahead"] === true))
                }

                CheckLabel {
                    visible: panel.implementation === "nvenc"
                    objectName: "encodeQuantization"
                    text: qsTr("Adaptive Quantization")
                    checked: panel.extra["nvenc.adaptive_quantization"] !== false
                    onToggled: panel.setExtra(
                        "nvenc.adaptive_quantization",
                        !(panel.extra["nvenc.adaptive_quantization"] !== false))
                }

                Item { width: 1; height: 4 }

                NumberField {
                    width: parent.width
                    visible: panel.implementation === "nvenc"
                    objectName: "encodeBframes"
                    label: qsTr("B-Frames")
                    labelWidth: 150
                    decimals: 0
                    steppers: true
                    minimum: 0
                    maximum: 4
                    value: Number(panel.extra["nvenc.bframes"] ?? 2)
                    onCommitted: value => panel.setExtra("nvenc.bframes", Math.round(value))
                }

                ChoiceField {
                    objectName: "encodeGpu"
                    width: parent.width
                    visible: panel.implementation === "nvenc"
                    label: qsTr("GPU Selection")
                    labelWidth: 150
                    choices: ["-1"].concat(Telemetry.encodingGpus.map(g => String(g.index)))
                    choiceLabels: {
                        const labels = {"-1": qsTr("Automatic")};
                        for (const gpu of Telemetry.encodingGpus)
                            labels[String(gpu.index)] = "GPU " + gpu.index + " (" + gpu.name + ")";
                        return labels;
                    }
                    value: String(panel.extra.gpu ?? -1)
                    onCommitted: value => panel.setExtra("gpu", Number(value))
                }

                ChoiceField {
                    width: parent.width
                    visible: panel.implementation === "x264"
                    label: qsTr("Tuning")
                    labelWidth: 150
                    choices: ["", "film", "animation", "grain", "stillimage", "psnr", "ssim",
                              "fastdecode", "zerolatency"]
                    choiceLabels: ({"": qsTr("Encoder default")})
                    value: String(panel.extra["x264.tune"] ?? "")
                    onCommitted: value => panel.setExtra("x264.tune", value)
                }

                CheckLabel {
                    visible: panel.implementation === "x264"
                    text: qsTr("Use Custom Buffer Size")
                    checked: panel.extra["x264.custom_buffer"] === true
                    onToggled: panel.setExtra("x264.custom_buffer", !checked)
                }

                NumberField {
                    width: parent.width
                    visible: panel.implementation === "x264" && panel.extra["x264.custom_buffer"] === true
                    label: qsTr("Buffer Size")
                    labelWidth: 150
                    decimals: 0
                    minimum: 0
                    maximum: 1000000
                    suffix: qsTr("Kbps")
                    value: Number(panel.extra["x264.buffer_kbps"] ?? 6000)
                    onCommitted: value => panel.setExtra("x264.buffer_kbps", Math.round(value))
                }

                TextLine {
                    width: parent.width
                    objectName: "encodeOptions"
                    visible: ["nvenc", "x264"].includes(panel.implementation)
                    label: qsTr("Encoder Options")
                    labelWidth: 150
                    value: String(panel.extra[panel.implementation === "x264"
                                              ? "x264.options" : "nvenc.options"] ?? "")
                    onCommitted: value => panel.setExtra(
                        panel.implementation === "x264" ? "x264.options" : "nvenc.options",
                        value)
                }
            }

            // --- an audio encode --------------------------------------------------------
            Column {
                width: parent.width
                spacing: 6
                visible: panel.selectedKind === "audio-encode"

                TextLine {
                    width: parent.width
                    label: qsTr("Encoder Name")
                    labelWidth: 150
                    value: panel.audio.name || ""
                    onCommitted: value => Document.renameAudioEncode(panel.selectedId, value)
                }

                ChoiceField {
                    width: parent.width
                    label: qsTr("Audio Track")
                    labelWidth: 150
                    choices: {
                        if (panel.revision < 0) return [];
                        return Document.audioBusChoices().map(b => b.id);
                    }
                    choiceLabels: {
                        if (panel.revision < 0) return ({});
                        const out = {};
                        for (const b of Document.audioBusChoices())
                            out[b.id] = b.name;
                        return out;
                    }
                    value: panel.audio.busId || ""
                    onCommitted: value => Document.setAudioEncodeBus(panel.selectedId, value)
                }

                ChoiceField {
                    width: parent.width
                    label: qsTr("Codec")
                    labelWidth: 150
                    choices: ["aac", "opus"]
                    choiceLabels: ({"aac": "AAC", "opus": "Opus"})
                    value: panel.audio.codec || "aac"
                    onCommitted: value => Document.setAudioEncodeCodec(panel.selectedId, value)
                }

                NumberField {
                    width: parent.width
                    label: qsTr("Bitrate")
                    labelWidth: 150
                    decimals: 0
                    suffix: qsTr("Kbps")
                    steppers: true
                    step: 32
                    minimum: 1
                    value: panel.audio.bitrateKbps || 0
                    onCommitted: value => Document.setAudioEncodeBitrate(
                        panel.selectedId, Math.max(1, Math.round(value)))
                }
            }

            // --- a destination ----------------------------------------------------------
            Column {
                width: parent.width
                spacing: 6
                visible: panel.selectedKind === "output"

                TextLine {
                    objectName: "encodeOutputName"
                    width: parent.width
                    label: qsTr("Name")
                    labelWidth: 150
                    value: nodeName.value
                    onCommitted: value => Document.renameOutput(panel.selectedId, value)
                }

                ReplaySettings {
                    width: parent.width
                    visible: panel.destination.destinationKind === "replay"
                    outputId: panel.selectedId
                }

                TextLine {
                    objectName: "recordingDirectory"
                    width: parent.width
                    visible: panel.destination.destinationKind === "file"
                    label: qsTr("Output folder")
                    labelWidth: 100
                    value: panel.destination.url || ""
                    placeholder: { if (panel.revision < 0) return ""; return Document.recordingDirectory(); }
                    browsable: true
                    onCommitted: value => Document.setRecordingDirectory(panel.selectedId, value)
                    onBrowseRequested: {
                        const outputId = panel.selectedId;
                        const path = FileDialog.directory(qsTr("Select recording directory"),
                            panel.destination.url || Document.recordingDirectory());
                        if (path !== "")
                            Document.setRecordingDirectory(outputId, path);
                        else if (FileDialog.lastError() !== "")
                            NoticeLog.add(FileDialog.lastError());
                    }
                }

                Text {
                    width: parent.width
                    visible: panel.destination.destinationKind === "file"
                    text: qsTr("Recordings use MKV with a unique filename for every take.")
                    font.pixelSize: 10
                    color: Theme.c.textMuted
                    wrapMode: Text.WordWrap
                }

                ChoiceField {
                    objectName: "encodeDestinationType"
                    width: parent.width
                    visible: panel.streamDestination
                    label: qsTr("Destination type")
                    labelWidth: 150
                    // Shut-down services stay selectable only on an output already using one.
                choices: Document.destinationKinds.filter(k => !k.discontinued
                    || k.id === panel.destination.service).map(k => k.id)
                    choiceLabels: {
                        const labels = {};
                        for (const kind of Document.destinationKinds) labels[kind.id] = kind.title;
                        return labels;
                    }
                    value: panel.destination.service || "rtmp"
                    onCommitted: value => {
                        panel.setPendingKey(panel.selectedId, "");
                        const saved = Object.assign({}, panel.savedKeys);
                        delete saved[panel.selectedId];
                        panel.savedKeys = saved;
                        Document.setOutputService(panel.selectedId, value);
                    }
                }

                ChoiceField {
                    objectName: "encodeDestinationServer"
                    width: parent.width
                    visible: panel.streamDestination && panel.servers.length > 0
                    label: qsTr("Server")
                    labelWidth: 150
                    choices: panel.servers.map(server => server.id)
                    choiceLabels: {
                        const labels = {};
                        for (const server of panel.servers) labels[server.id] = server.title;
                        return labels;
                    }
                    value: panel.destination.url || ""
                    onCommitted: value => Document.setOutputDestination(panel.selectedId,
                        value.startsWith("rtmps://") ? "rtmps" : "rtmp", value)
                }

                TextLine {
                    objectName: "oauthClientId"
                    width: parent.width
                    visible: panel.streamDestination && !!panel.connector.oauth
                    label: qsTr("OAuth client ID")
                    labelWidth: 150
                    value: {
                        if (panel.revision < 0) return "";
                        return Document.connectors.clientId(panel.destination.service || "");
                    }
                    placeholder: qsTr("Register MunkiCast with the provider")
                    onCommitted: value => Document.connectors.setClientId(panel.destination.service, value)
                }

                TextLine {
                    width: parent.width
                    visible: panel.streamDestination && panel.destination.service === "youtube"
                    label: qsTr("OAuth client secret")
                    labelWidth: 150
                    echoMode: TextInput.Password
                    placeholder: qsTr("Desktop client secret; kept in memory")
                    onCommitted: value => Document.connectors.setGoogleSecret(value)
                }

                DialogButton {
                    objectName: "connectDestinationAccount"
                    visible: panel.streamDestination && !!panel.connector.oauth
                    text: panel.accountBusy ? qsTr("Cancel connection") : qsTr("Connect account…")
                    enabled: !Document.connectors.busy || panel.accountBusy
                    onClicked: {
                        if (panel.accountBusy) Document.connectors.cancel();
                        else Document.connectOutputAccount(panel.selectedId);
                    }
                }

                Text {
                    width: parent.width
                    visible: panel.streamDestination && !!panel.connector.oauth &&
                             Document.connectors.target === panel.selectedId
                    text: Document.connectors.status
                    color: Theme.c.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }

                ChoiceField {
                    objectName: "youtubeStream"
                    width: parent.width
                    visible: panel.accountBusy && Document.connectors.streams.length > 0
                    label: qsTr("YouTube stream")
                    labelWidth: 150
                    choices: Document.connectors.streams.map(stream => stream.id)
                    choiceLabels: {
                        const labels = {};
                        for (const stream of Document.connectors.streams) labels[stream.id] = stream.title;
                        return labels;
                    }
                    value: panel.youtubeStream
                    onCommitted: value => {
                        panel.youtubeStream = value;
                        Document.connectors.chooseStream(value);
                    }
                }

                DialogButton {
                    visible: panel.streamDestination && !!panel.connector.keyLink
                    text: qsTr("Get stream key…")
                    onClicked: Qt.openUrlExternally(panel.connector.keyLink)
                }

                TextLine {
                    objectName: "encodeDestinationUrl"
                    width: parent.width
                    visible: panel.streamDestination && panel.servers.length === 0
                    label: panel.destination.destinationKind === "whip" ? qsTr("WHIP endpoint") : qsTr("Server URL")
                    labelWidth: 150
                    value: panel.destination.url || ""
                    onCommitted: value => Document.setOutputDestination(
                        panel.selectedId, panel.destination.service === "rtmp"
                            ? (value.startsWith("rtmps://") ? "rtmps" : "rtmp")
                            : panel.destination.destinationKind, value)
                }

                TextLine {
                    id: destinationKey
                    objectName: "encodeDestinationKey"
                    width: parent.width
                    visible: panel.usesKey
                    label: panel.destination.destinationKind === "whip" ? qsTr("Bearer token") : qsTr("Stream key")
                    labelWidth: 150
                    echoMode: TextInput.Password
                    placeholder: panel.destination.credentialRef
                        ? qsTr("Saved in keyring - enter replacement key") : qsTr("Stream key")
                    value: panel.pendingKey
                    commitOnEdit: true
                    enabled: !panel.savingKeys[panel.selectedId]
                    onCommitted: value => panel.setPendingKey(panel.selectedId, value)
                    onEditingFinished: panel.savePendingKey()
                }

                DialogButton {
                    objectName: "encodeSaveDestinationKey"
                    visible: panel.usesKey
                    text: qsTr("Save key to keyring")
                    enabled: panel.pendingKey.length > 0 && !panel.savingKeys[panel.selectedId]
                             && panel.savedKeys[panel.selectedId] !== panel.pendingKey
                    onClicked: panel.savePendingKey()
                }

                ChoiceField {
                    width: parent.width
                    label: qsTr("Video Encoder")
                    labelWidth: 150
                    choices: {
                        if (panel.revision < 0) return [];
                        return Document.encodeChoices().map(e => e.id);
                    }
                    choiceLabels: {
                        if (panel.revision < 0) return ({});
                        const out = {};
                        for (const e of Document.encodeChoices())
                            out[e.id] = e.name;
                        return out;
                    }
                    value: panel.outputVideoId
                    onCommitted: value => Document.setOutputEncode(panel.selectedId, value)
                }

                ChoiceField {
                    width: parent.width
                    label: qsTr("Audio Encoder")
                    labelWidth: 150
                    choices: {
                        if (panel.revision < 0) return [];
                        return [""].concat(Document.audioEncodeChoices().map(e => e.id));
                    }
                    choiceLabels: {
                        if (panel.revision < 0) return ({});
                        const out = {"": qsTr("Not connected")};
                        for (const e of Document.audioEncodeChoices())
                            out[e.id] = e.name;
                        return out;
                    }
                    value: panel.outputAudioId
                    onCommitted: value => Document.setOutputAudioEncode(panel.selectedId, value)
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    visible: panel.outputAudioId === ""
                    text: qsTr("Audio 0 is not connected. Choose an audio encoder.")
                    font.pixelSize: 9
                    color: Theme.c.warning
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    visible: panel.usesKey
                    text: panel.destination.credentialRef
                          ? qsTr("Key saved in system keyring.") : qsTr("Enter a stream key if required by the server.")
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                    topPadding: 8
                }
                Text {
                    objectName: "streamReadiness"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    visible: panel.streamDestination
                    text: {
                        if (panel.revision < 0) return "";
                        const target = Document.streamingTarget();
                        return target.outputId ? qsTr("Ready to go live") : target.reason || "";
                    }
                    font.pixelSize: 10
                    color: Theme.c.text
                }
            }

            // --- a canvas or an audio track ---------------------------------------------
            //
            // These are what the project has rather than what the operator arranged, so they
            // are edited where they are owned -- the canvas manager and the mixer -- and said
            // here rather than given a second set of fields that could disagree.
            Column {
                width: parent.width
                spacing: 6
                visible: panel.selectedKind === "canvas" || panel.selectedKind === "bus"

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: panel.selectedKind === "canvas"
                          ? qsTr("Canvases are sources on this graph. Size, rate and colour "
                               + "space are set in the canvas manager.")
                          : qsTr("Audio tracks are sources on this graph. Levels and routing "
                               + "are set in the mixer.")
                    font.pixelSize: 10
                    color: Theme.c.textMuted
                }
            }
        }
    }
}
