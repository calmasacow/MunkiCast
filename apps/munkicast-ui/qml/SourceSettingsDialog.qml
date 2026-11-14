// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import MunkiCast

/// What one source is configured to be: the gear at the bottom of the Sources panel, and a
/// double-click on a row.
///
/// The shape is the one OBS gets right and the operator asked for -- a preview above, the
/// fields below it, Defaults on the left of the bottom bar and OK/Cancel on the right -- but
/// the fields are not hand-built per type. They come from `Document.sourceSettingsSchema`, so
/// a new setting on a source type is a line in a table in C++ and nothing in this file.
///
/// Nothing is committed while the panel is open. Every edit goes into `pending`, and OK sends
/// the lot as one `SetSource`, which is one undo step named for the source. That is what makes
/// Cancel honest: there is nothing to put back, because nothing was done. The alternative --
/// committing per field and reverting on Cancel -- leaves the abandoned edits on the undo stack
/// for the operator to press Ctrl+Z through later.
///
/// The preview is honest, the same rule the add dialog follows. A colour shows the colour being
/// chosen. An image shows the file being chosen, from `pending`, so it is on screen before OK.
/// Everything else says there is no pipeline attached, because there is not.
///
/// Drawn inside the workspace rather than as its own window (ADR-0015). Browse is the exception:
/// it opens the desktop's file chooser, the XDG portal, which this machine routes to Omanta.
/// The path lands in `pending` and is not applied until OK.
Item {
    id: dialog

    /// The source being configured. Empty when the panel is closed.
    property string sourceId: ""
    property var details: ({})
    property var schema: []
    /// Edits made since the panel opened, by settings key. Replaced wholesale on every change,
    /// because assigning into a var object in place notifies nothing.
    property var pending: ({})
    property string pendingName: ""

    readonly property bool known: Object.keys(dialog.details).length > 0
    readonly property string typeId: dialog.known ? dialog.details.type : ""
    /// The image file sitting in `pending`, or empty. A local path, the same string OK will store.
    readonly property string pendingFile: {
        const path = dialog.pending["file"];
        return typeof path === "string" ? path : "";
    }

    function localFileUrl(path) {
        if (path === "")
            return "";
        const absolute = path.startsWith("/") ? path : ("/" + path);
        return "file://" + encodeURI(absolute);
    }

    /// The renderer draws this source into a hidden canvas; this is that canvas's key, or
    /// empty. Live, so a camera's framing can be judged while its mode is being chosen.
    ///
    /// It shows the source as it is *now*, not as the unsaved edits would make it: settings
    /// reach the worker only when OK is pressed, and republishing on every keystroke would
    /// reconfigure the worker -- blanking the picture -- on the way to each digit of a
    /// resolution. The colour and image cards below stay as they are, because those two can be
    /// drawn from `pending` directly and so can preview an edit before it is applied.
    property string previewKey: ""
    /// Types whose picture the renderer has to make. A colour or an image file is drawn in QML
    /// from the pending values, which also previews the edit, so neither belongs here.
    readonly property bool renderPreview: ["source.v4l2", "source.blackmagic", "source.media",
                                           "source.slideshow", "source.playlist", "source.screen",
                                           "source.window", "source.game", "source.browser"]
                                          .indexOf(dialog.typeId) >= 0
    function refreshPreview() {
        dialog.previewKey = Preview.setPreviewTarget(
            "settings", dialog.visible && dialog.renderPreview ? dialog.sourceId : "");
    }

    signal closed

    anchors.fill: parent
    visible: false
    focus: visible

    function open(id) {
        const found = Document.sourceDetails(id);
        if (Object.keys(found).length === 0) {
            NoticeLog.add(qsTr("That source is no longer in the project."));
            return;
        }
        dialog.sourceId = id;
        dialog.details = found;
        dialog.schema = Document.sourceSettingsSchema(found.type);
        dialog.pending = Object.assign({}, found.settings);
        dialog.pendingName = found.name;
        dialog.visible = true;
        dialog.forceActiveFocus();
        dialog.refreshPreview();
    }

    function close() {
        dialog.visible = false;
        dialog.sourceId = "";
        dialog.refreshPreview();
        dialog.details = ({});
        dialog.schema = [];
        dialog.pending = ({});
        dialog.closed();
    }

    function setValue(key, value) {
        const next = Object.assign({}, dialog.pending);
        next[key] = value;
        dialog.pending = next;
    }

    /// Back to what a brand-new source of this type would be. The name is left alone: nobody
    /// pressing Defaults on a camera's settings means "and call it Camera again".
    function restoreDefaults() {
        const next = {};
        for (const f of dialog.schema) {
            if (f.kind !== "note")
                next[f.key] = f.value;
        }
        dialog.pending = next;
    }

    function accept() {
        Document.applySourceSettings(dialog.sourceId, dialog.pendingName, dialog.pending);
        dialog.close();
    }

    Keys.onEscapePressed: dialog.close()

    // The workspace behind is inert while this is up, and clicking away closes it.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: dialog.close()
        onWheel: (wheel) => { wheel.accepted = true; }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
        opacity: 0.55
    }

    Rectangle {
        id: frame

        objectName: "sourceSettingsFrame"
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        DialogGeometry { minimumWidth: 480; minimumHeight: 400 }
        width: Math.min(parent.width - 48, 760)
        height: Math.min(parent.height - 48, 660)
        color: Theme.c.panelBackground
        border.width: 1
        border.color: Theme.c.accent
        radius: 3

        MouseArea {
            anchors.fill: parent
            onClicked: {}
        }

        // ---- the title ----------------------------------------------------------------
        Item {
            id: titleBar

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: 34

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 13
                anchors.right: typeBadge.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: dialog.known ? qsTr("Settings — %1").arg(dialog.pendingName) : ""
                font.pixelSize: 13
                font.weight: Font.DemiBold
                color: Theme.c.text
                elide: Text.ElideRight
            }

            Row {
                id: typeBadge

                anchors.right: parent.right
                anchors.rightMargin: 13
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                // ADR-0025: safe to hold a safety net, said where the operator is already
                // looking at what this source is.
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: dialog.known && dialog.details.selfContained
                    text: "⛨"
                    font.pixelSize: 11
                    color: Theme.c.good
                    opacity: 0.8
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: dialog.known ? dialog.details.typeTitle : ""
                    font.pixelSize: 11
                    color: Theme.c.textMuted
                }
            }
        }

        // ---- the preview ---------------------------------------------------------------
        Rectangle {
            id: preview

            objectName: "sourceSettingsPreview"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: titleBar.bottom
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            height: Math.round(Math.min(frame.height * 0.3, (frame.width - 24) * 9 / 16))
            color: Theme.c.window
            border.width: 1
            border.color: Theme.c.border
            radius: 2
            clip: true

            // Tracks what is being chosen rather than what is stored. A colour is the colour.
            // An image is the file. Both are on screen before OK, and neither is committed by
            // being shown.
            //
            // The colour is letterboxed to the card's own size rather than filling the box, so
            // that the width and height below are visibly the thing they say they are. The image
            // is letterboxed the same way, so a wide file stays a wide file in this card.
            Rectangle {
                readonly property real aspect: {
                    const w = Number(dialog.pending["width"]);
                    const h = Number(dialog.pending["height"]);
                    return (w > 0 && h > 0) ? w / h : 16 / 9;
                }

                anchors.centerIn: parent
                visible: dialog.typeId === "source.color"
                width: Math.min(parent.width - 2, (parent.height - 2) * aspect)
                height: Math.min(parent.height - 2, (parent.width - 2) / aspect)
                color: {
                    const hex = dialog.pending["color"];
                    return /^#[0-9a-fA-F]{6}$/.test(hex === undefined ? "" : String(hex))
                         ? String(hex) : "transparent";
                }
            }

            Image {
                id: still

                objectName: "sourceSettingsImage"
                anchors.fill: parent
                anchors.margins: 1
                visible: dialog.typeId === "source.image" && still.status === Image.Ready
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false
                mipmap: true
                source: dialog.typeId === "source.image"
                        ? dialog.localFileUrl(dialog.pendingFile) : ""
            }

            // The live picture, for the types whose pixels only the renderer has. Same card and
            // the same letterboxing as the colour and image previews, so one rule covers all.
            Image {
                id: live
                objectName: "sourceSettingsLive"

                anchors.fill: parent
                anchors.margins: 1
                visible: dialog.renderPreview && live.status === Image.Ready
                fillMode: Image.PreserveAspectFit
                // The provider hands back an image it already holds; an asynchronous reload
                // would only add a frame of blank between updates.
                asynchronous: false
                cache: false
                source: dialog.previewKey !== "" && Preview.hasFrame(dialog.previewKey)
                        ? "image://program/" + dialog.previewKey + "/" + Preview.generation : ""
            }

            // What the card says before the first frame: a camera still opening, or one that
            // cannot open at all.
            Text {
                anchors.centerIn: parent
                visible: dialog.renderPreview && !live.visible
                text: dialog.previewKey === "" ? qsTr("no preview") : qsTr("waiting for a picture")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }

            // A screen keeps its written description for when the monitor is unplugged or was
            // never found, which a blank picture would not explain.
            Column {
                readonly property var screen: dialog.typeId === "source.screen"
                    ? Document.screenDetails(String(dialog.pending["output"] === undefined
                                                    ? "" : dialog.pending["output"]))
                    : ({})
                readonly property bool known: screen && screen.name !== undefined

                anchors.centerIn: parent
                visible: dialog.typeId === "source.screen" && !live.visible
                spacing: 3

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: parent.known ? parent.screen.name : qsTr("no screen found")
                    font.pixelSize: 15
                    color: Theme.c.text
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: parent.known
                    text: parent.known
                          ? parent.screen.width + " \u00d7 " + parent.screen.height +
                            "  \u00b7  " + parent.screen.hz.toFixed(2) + " Hz"
                          : ""
                    font.pixelSize: 11
                    color: Theme.c.textMuted
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: parent.known && parent.screen.description !== ""
                    text: parent.known ? parent.screen.description : ""
                    font.pixelSize: 10
                    color: Theme.c.textMuted
                    opacity: 0.7
                }
            }

            Column {
                anchors.centerIn: parent
                visible: dialog.typeId !== "source.color" && dialog.typeId !== "source.screen" &&
                         !still.visible
                spacing: 4

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: dialog.known ? dialog.details.icon : ""
                    font.pixelSize: 26
                    color: Theme.c.textMuted
                    opacity: 0.75
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: dialog.typeId === "source.image" && dialog.pendingFile !== ""
                          ? (still.status === Image.Error
                             ? qsTr("could not read that image")
                             : qsTr("reading the image"))
                          : ["source.v4l2", "source.text", "source.media", "source.slideshow", "source.playlist", "source.browser", "source.blackmagic"].includes(dialog.typeId)
                            ? qsTr("Source appears in Program after applying these settings")
                            : qsTr("no pipeline attached")
                    font.pixelSize: 11
                    color: Theme.c.textMuted
                    opacity: 0.7
                }
            }
        }

        // ---- the fields ----------------------------------------------------------------
        Flickable {
            id: scroll

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: preview.bottom
            anchors.bottom: bottomBar.top
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.topMargin: 10
            anchors.bottomMargin: 8
            contentHeight: fields.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
            // because Flickable drops it along with the drag.
            interactive: false

            NoDragScroll {}

            Column {
                id: fields

                width: scroll.width
                spacing: 4

                Text {
                    width: parent.width
                    visible: dialog.typeId === "source.v4l2"
                    text: qsTr("Capture FPS is independent of canvas FPS. Settings apply on OK. Check requested, negotiated and received FPS in Broadcast Statistics.")
                    color: Theme.c.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }

                // The source's own name, which is a different thing from the source node's: one
                // camera can sit in six scenes under six labels, and this is the camera.
                TextLine {
                    objectName: "sourceNameField"
                    width: parent.width
                    label: qsTr("Name")
                    labelWidth: 150
                    labelAlign: Text.AlignRight
                    value: dialog.pendingName
                    commitOnEdit: true
                    onCommitted: newValue => dialog.pendingName = newValue
                }

                Text {
                    width: parent.width
                    text: dialog.known
                        ? (dialog.details.useCount === 1
                            ? qsTr("Used in one place. Changing it changes it there.")
                            : qsTr("Used in %1 places. Changing it changes all of them.")
                                  .arg(dialog.details.useCount))
                        : ""
                    visible: dialog.known && dialog.details.useCount > 0
                    leftPadding: 154
                    bottomPadding: 6
                    font.pixelSize: 10
                    color: dialog.known && dialog.details.useCount > 1
                         ? Theme.c.warning : Theme.c.textMuted
                    wrapMode: Text.WordWrap
                }

                Repeater {
                    model: dialog.schema

                    delegate: SettingRow {
                        id: settingRow

                        required property var modelData

                        objectName: "sourceSettingRow"
                        width: fields.width
                        field: {
                            if (dialog.typeId === "source.v4l2" && settingRow.modelData.key === "videoFormat") {
                                const formats = Document.videoFormatChoices(String(dialog.pending.device || ""));
                                return Object.assign({}, settingRow.modelData, formats);
                            }
                            return settingRow.modelData;
                        }
                        labelWidth: 150
                        currentValue: dialog.pending[settingRow.modelData.key]
                        onCommitted: newValue =>
                            dialog.setValue(settingRow.modelData.key, newValue)
                        onBrowseRequested: {
                            const key = settingRow.modelData.key;
                            const current = dialog.pending[key];
                            const path = FileDialog.pick(qsTr("Choose a file"),
                                                         typeof current === "string" ? current : "",
                                                         dialog.typeId === "source.image" && key === "file");
                            if (path !== "")
                                dialog.setValue(key, path);
                            else if (FileDialog.lastError() !== "")
                                NoticeLog.add(FileDialog.lastError());
                        }
                    }
                }

                // A type this build has no field table for -- a project written by a later
                // build. Said plainly, because an empty panel looks like a broken one.
                Text {
                    width: parent.width
                    visible: dialog.schema.length === 0
                    topPadding: 24
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("This build has no settings for a %1 source.")
                              .arg(dialog.known ? dialog.details.typeTitle : "")
                    font.pixelSize: 11
                    color: Theme.c.textMuted
                }
            }
        }

        // ---- the bottom bar -------------------------------------------------------------
        Rectangle {
            id: bottomBar

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 1
            height: 44
            color: Theme.c.panelHeader

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: Theme.c.border
            }

            DialogButton {
                objectName: "sourceDefaultsButton"
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Defaults")
                onClicked: dialog.restoreDefaults()
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                DialogButton {
                    objectName: "sourceSettingsOkButton"
                    text: qsTr("OK")
                    primary: true
                    onClicked: dialog.accept()
                }

                DialogButton {
                    objectName: "sourceSettingsCancelButton"
                    text: qsTr("Cancel")
                    onClicked: dialog.close()
                }
            }
        }
    }
}
