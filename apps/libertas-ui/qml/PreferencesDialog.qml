// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import Libertas

/// Edit > Preferences: settings for this computer, not the collection.
Window {
    id: dialog
    objectName: "preferencesDialog"
    title: qsTr("Preferences")
    width: 560; height: 462
    minimumWidth: 480; minimumHeight: 462
    color: Theme.c.window
    modality: Qt.ApplicationModal
    flags: Qt.Dialog | Qt.FramelessWindowHint
    DialogGeometry { target: dialog; title: dialog.title }
    property bool automatic: true
    property int budgetMib: 2048
    property string cacheDir: ""
    property string problem: ""
    function open() {
        automatic = Preferences.gpuBudgetMib === 0;
        budgetMib = automatic ? 2048 : Preferences.gpuBudgetMib;
        cacheDir = Preferences.cacheDir;
        problem = "";
        show();
        requestActivate();
    }
    Column {
        anchors.fill: parent
        anchors.margins: 16
        anchors.topMargin: 48
        spacing: 12
        Text {
            text: qsTr("GPU memory")
            color: Theme.c.text
            font.pixelSize: 13
            font.bold: true
        }
        CheckLabel {
            objectName: "gpuBudgetAutomatic"
            text: qsTr("Automatic: half of the GPU memory free when the renderer starts")
            checked: dialog.automatic
            onToggled: dialog.automatic = !dialog.automatic
        }
        NumberField {
            objectName: "gpuBudgetMib"
            width: parent.width; label: qsTr("Compositor limit"); labelWidth: 140
            enabled: !dialog.automatic
            opacity: enabled ? 1 : 0.5
            decimals: 0; minimum: 256; maximum: 65536; suffix: "MiB"
            value: dialog.budgetMib
            onCommitted: value => dialog.budgetMib = Math.round(value)
        }
        Text {
            width: parent.width
            text: qsTr("Images, effects and decoded frames held on the GPU. A show that exceeds it stops compositing on the GPU. Applying restarts the renderer, which briefly blanks the preview and program.")
            color: Theme.c.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: 11
        }
        Rectangle {
            width: parent.width; height: 1
            color: Theme.c.divider
        }

        Text {
            text: qsTr("Asset cache")
            color: Theme.c.text
            font.pixelSize: 13
            font.bold: true
        }

        TextLine {
            objectName: "cacheDirField"
            width: parent.width
            label: qsTr("Directory"); labelWidth: 140
            value: dialog.cacheDir
            browsable: true
            onCommitted: newValue => dialog.cacheDir = newValue
            onBrowseRequested: {
                const chosen = FileDialog.directory(qsTr("Select cache directory"), dialog.cacheDir);
                if (chosen !== "")
                    dialog.cacheDir = chosen;
                else
                    dialog.problem = FileDialog.lastError();
            }
        }

        Row {
            spacing: 8
            DialogButton {
                objectName: "clearCache"
                text: qsTr("Clear Cache")
                enabled: !AssetCache.building
                onClicked: cacheConfirm.ask("clear")
            }
            DialogButton {
                objectName: "rebuildCacheFromPreferences"
                text: qsTr("Rebuild Cache")
                enabled: !AssetCache.building
                onClicked: cacheConfirm.ask("rebuild")
            }
        }

        Text {
            width: parent.width
            text: AssetCache.bytesHeld > 0
                ? qsTr("Prepared copies of this show's files, so it plays from local disk rather than from the mount the files live on. Holding %1 MiB. Changing the directory does not move what is already prepared; the new one is built on the next build.")
                    .arg(Math.round(AssetCache.bytesHeld / 1048576))
                : qsTr("Prepared copies of this show's files, so it plays from local disk rather than from the mount the files live on. Nothing is prepared yet.")
            color: Theme.c.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: 11
        }

        Text {
            visible: dialog.problem !== ""
            text: dialog.problem
            color: Theme.c.error
            font.pixelSize: 11
        }
        Row {
            spacing: 8
            DialogButton {
                text: qsTr("Apply"); primary: true
                objectName: "applyPreferences"
                onClicked: {
                    if (!Preferences.setCacheDir(dialog.cacheDir)) {
                        dialog.problem = qsTr("Could not save: the cache directory must be an absolute path.");
                        return;
                    }
                    if (Preferences.setGpuBudgetMib(dialog.automatic ? 0 : dialog.budgetMib))
                        dialog.close();
                    else
                        dialog.problem = qsTr("Could not save: enter 256 to 65536 MiB.");
                }
            }
            DialogButton { text: qsTr("Cancel"); onClicked: dialog.close() }
        }
    }

    // Both destructive, both confirmed. Hosted here rather than in MainMenu so the dialog is
    // self-contained: Preferences is its own Window and an overlay parented to the workspace
    // would appear behind it.
    CacheConfirm {
        id: cacheConfirm
        anchors.fill: parent
    }
}
