// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound

import QtQuick
import QtQml
import QtQuick.Controls as Controls
import Libertas

Controls.MenuBar {
    id: bar
    objectName: "mainMenu"
    required property var mainWindow
    required property var collectionDialog
    property bool statusBarVisible: true
    readonly property bool editable: Document.selectedNode !== "" && !Document.nodeProperties.locked
    readonly property var extraPanels: {
        const revision = Workspace.revision;
        const ids = Workspace.closedPanels.slice();
        for (let i = 0; i < Workspace.panelCount; ++i) {
            for (const id of Workspace.panelAt(i).tabs) {
                if (ids.indexOf(id) < 0) ids.push(id);
            }
        }
        return ids.filter(id => /^(program|scenes|sources):/.test(id));
    }
    function showPanel(id) {
        Workspace.openPanel(id);
        Workspace.activate(id);
    }

    PreferencesDialog { id: preferences }
    BrowserDocksDialog { id: browserDocks; parent: bar.mainWindow.contentItem }
    WebsocketSettingsDialog { id: websocketSettings }
    McpSettingsDialog { id: mcpSettings }

    // Parented to the window's content item, as FiltersDialog is: an overlay inside the menu
    // bar would be clipped to 28 pixels of height.
    CacheConfirm {
        id: cacheConfirm
        parent: bar.mainWindow.contentItem
        anchors.fill: parent
        // Above the progress overlay: confirming a rebuild is what *starts* the build, so the
        // two are briefly on screen together.
        z: 10001
    }

    implicitHeight: 28
    rightPadding: 64
    background: Rectangle { color: Theme.c.panelHeader }
    delegate: Controls.MenuBarItem {
        id: heading
        objectName: "mainMenuHeading"
        implicitHeight: 28
        font.pixelSize: 12
        contentItem: Text {
            text: heading.text.replace(/&/g, "")
            font: heading.font
            color: Theme.c.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: heading.highlighted ? Theme.c.panelHeaderActive : "transparent"
        }
    }

    component Entry: Controls.MenuItem {
        id: entry
        implicitHeight: 28
        font.pixelSize: 12
        contentItem: Text {
            leftPadding: 24
            rightPadding: 20
            text: entry.text
            font: entry.font
            color: entry.enabled ? Theme.c.text : Theme.c.textMuted
            opacity: entry.enabled ? 1 : 0.55
            verticalAlignment: Text.AlignVCenter
        }
        indicator: Text {
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            text: entry.checked ? "\u2713" : ""
            color: Theme.c.accent
        }
        background: Rectangle {
            color: entry.highlighted ? Theme.c.panelHeaderActive : "transparent"
        }
    }
    component Menu: Controls.Menu {
        implicitWidth: 264
        padding: 4
        delegate: Entry {}
        background: Rectangle {
            color: Theme.c.panelBackground
            border.color: Theme.c.border
            radius: 3
        }
    }
    component Pending: Entry { enabled: false }
    component Divider: Controls.MenuSeparator {
        contentItem: Rectangle { implicitHeight: 1; color: Theme.c.border }
    }

    Menu {
        title: qsTr("&File")
        Entry { text: qsTr("Save project"); enabled: Document.projectPath !== ""; onTriggered: Document.save() }
        Pending { text: qsTr("Show Recordings") }
        Pending { text: qsTr("Remux Recordings") }
        Divider {}
        Menu {
            title: qsTr("Settings")
            Entry { text: qsTr("Canvas"); onTriggered: bar.showPanel("canvas") }
            Entry { text: qsTr("Outputs"); onTriggered: bar.showPanel("outputs") }
            Entry { text: qsTr("Keyboard Shortcuts"); onTriggered: bar.showPanel("shortcuts") }
        }
        Pending { text: qsTr("Show Settings Folder") }
        Pending { text: qsTr("Show Profile Folder") }
        Divider {}
        Entry {
            text: qsTr("Exit")
            onTriggered: {
                if (Document.dirty) Document.save();
                if (Document.dirty)
                    bar.mainWindow.report(qsTr("Project has unsaved changes. Save successfully before exiting."));
                else bar.mainWindow.close();
            }
        }
    }
    Menu {
        title: qsTr("&Edit")
        Entry { objectName: "menuUndo"; text: qsTr("Undo"); enabled: Document.canUndo; onTriggered: Document.undo() }
        Entry { text: qsTr("Redo"); enabled: Document.canRedo; onTriggered: Document.redo() }
        Divider {}
        Entry { text: qsTr("Copy"); enabled: Document.selectedNode !== ""; onTriggered: Document.copySelectedNode() }
        Entry { text: qsTr("Paste"); onTriggered: Document.paste() }
        Pending { text: qsTr("Paste (Duplicate)") }
        Divider {}
        Pending { text: qsTr("Copy Filters") }
        Pending { text: qsTr("Paste Filters") }
        Divider {}
        Entry { objectName: "menuPreferences"; text: qsTr("Preferences…"); onTriggered: preferences.open() }
        Divider {}
        Menu {
            title: qsTr("Transform")
            enabled: bar.editable
            Entry { text: qsTr("Reset Transform"); onTriggered: Document.resetTransform() }
            Entry { text: qsTr("Rotate 90 degrees clockwise"); onTriggered: Document.rotateBy(90) }
            Entry { text: qsTr("Rotate 90 degrees counterclockwise"); onTriggered: Document.rotateBy(-90) }
            Entry { text: qsTr("Flip Horizontal"); onTriggered: Document.flipHorizontal() }
            Entry { text: qsTr("Flip Vertical"); onTriggered: Document.flipVertical() }
            Divider {}
            Entry { text: qsTr("Fit to Canvas"); onTriggered: Document.fitToCanvas(false) }
            Entry { text: qsTr("Stretch to Canvas"); onTriggered: Document.fitToCanvas(true) }
            Entry { text: qsTr("Center on Canvas"); onTriggered: Document.centerOnCanvas(true, true) }
        }
        Menu {
            title: qsTr("Order")
            enabled: bar.editable
            Entry { text: qsTr("Move Up"); onTriggered: Document.moveNode(Document.selectedNode, -1) }
            Entry { text: qsTr("Move Down"); onTriggered: Document.moveNode(Document.selectedNode, 1) }
            Entry { text: qsTr("Move to Top"); onTriggered: Document.moveSelectedToEnd(true) }
            Entry { text: qsTr("Move to Bottom"); onTriggered: Document.moveSelectedToEnd(false) }
        }
        Pending { text: qsTr("Preview Scaling") }
        Pending { text: qsTr("Lock Preview") }
        Divider {}
        Entry { text: qsTr("Advanced Audio Properties"); onTriggered: bar.showPanel("audio-mixer") }
    }
    Menu {
        title: qsTr("&View")
        Entry { text: qsTr("Reset UI"); onTriggered: Workspace.resetLayout() }
        Entry {
            text: qsTr("Fullscreen Interface")
            checkable: true
            checked: bar.mainWindow.visibility === Window.FullScreen
            onTriggered: checked ? bar.mainWindow.showFullScreen() : bar.mainWindow.showNormal()
        }
        Divider {}
        Pending { text: qsTr("Dock Toolbars") }
        Pending { text: qsTr("Source Icons") }
        Entry {
            objectName: "menuStatusBar"
            text: qsTr("Status Bar")
            checkable: true
            checked: bar.statusBarVisible
            onTriggered: bar.statusBarVisible = checked
        }
        Divider {}
        Entry { text: qsTr("Stats"); onTriggered: bar.showPanel("broadcast-stats") }
        Pending { text: qsTr("Open Multiview") }
        Pending { text: qsTr("Always On Top") }
    }
    Menu {
        id: docks
        title: qsTr("&Docks")
        Pending { text: qsTr("Lock Docks") }
        Entry { objectName: "menuSaveLayout"; text: qsTr("Save Layout"); onTriggered: Workspace.saveLayoutNow() }
        Entry { objectName: "menuRecallLayout"; text: qsTr("Recall Saved Layout"); onTriggered: Workspace.recallLayout() }
        Entry { text: qsTr("Reset Docks"); onTriggered: Workspace.resetLayout() }
        Divider {}
        Entry {
            objectName: "menuBrowserDocks"
            text: qsTr("Custom Browser Docks…")
            onTriggered: browserDocks.open()
        }
        Menu {
            id: browserMenu
            title: qsTr("Browser Docks")
            enabled: Workspace.browserDocks.length > 0
            Instantiator {
                model: Workspace.browserDocks
                delegate: Entry {
                    required property var modelData
                    text: modelData.name
                    checkable: true
                    checked: { const revision = Workspace.revision; return Workspace.layoutHasPanel(modelData.id); }
                    onTriggered: checked ? bar.showPanel(modelData.id) : Workspace.closePanel(modelData.id)
                }
                onObjectAdded: (index, object) => browserMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => browserMenu.removeItem(object)
            }
        }
        Instantiator {
            model: ["audio-mixer", "broadcast-stats", "program", "outputs", "properties",
                    "canvas", "favourites", "scenes", "sources", "controls", "info", "log", "shortcuts"]
            delegate: Entry {
                required property string modelData
                text: Workspace.title(modelData)
                checkable: true
                checked: { const revision = Workspace.revision; return Workspace.layoutHasPanel(modelData); }
                onTriggered: checked ? bar.showPanel(modelData) : Workspace.closePanel(modelData)
            }
            onObjectAdded: (index, object) => docks.insertItem(index + 7, object)
            onObjectRemoved: (index, object) => docks.removeItem(object)
        }
        Divider {}
        Menu {
            id: extraCanvases
            title: qsTr("Extra Canvases")
            enabled: bar.extraPanels.length > 0
            Instantiator {
                model: bar.extraPanels
                delegate: Entry {
                    required property string modelData
                    text: Workspace.title(modelData)
                    checkable: true
                    checked: { const revision = Workspace.revision; return Workspace.layoutHasPanel(modelData); }
                    onTriggered: checked ? bar.showPanel(modelData) : Workspace.closePanel(modelData)
                }
                onObjectAdded: (index, object) => extraCanvases.insertItem(index, object)
                onObjectRemoved: (index, object) => extraCanvases.removeItem(object)
            }
        }
        Entry {
            text: qsTr("Countdown Timers")
            checkable: true
            checked: { const revision = Workspace.revision; return Workspace.layoutHasPanel("countdowns"); }
            onTriggered: checked ? bar.showPanel("countdowns") : Workspace.closePanel("countdowns")
        }
        Pending { text: qsTr("Chat") }
        Pending { text: qsTr("Activity") }
        Pending { text: qsTr("Stream Info") }
        Pending { text: qsTr("Overlays") }
    }
    // Top-level rather than inside Edit, because what it governs is the whole show: whether
    // this machine plays from local copies at all. The directory it keeps them in is in
    // Preferences, where the other machine-wide settings are.
    Menu {
        title: qsTr("Cachin&g")
        Entry {
            objectName: "menuCachingEnabled"
            text: qsTr("Enable Caching")
            checkable: true
            checked: AssetCache.enabled
            enabled: !AssetCache.building
            onTriggered: Preferences.setCachingEnabled(checked)
        }
        Divider {}
        Entry {
            objectName: "menuBuildCache"
            // Two entries rather than one that changes meaning. The single entry asked for
            // confirmation only when nothing was pending, so clicking it with work outstanding
            // started a build with no dialog at all -- which read as the dialog being broken.
            text: qsTr("Build Cache")
            // Enabled while caching is off on purpose: preparing the copies before switching
            // over is the order that does not interrupt a show.
            enabled: !AssetCache.building && AssetCache.pendingCount() > 0
            onTriggered: AssetCache.build(false)
        }
        Entry {
            objectName: "menuRebuildCache"
            text: qsTr("Rebuild Cache")
            enabled: !AssetCache.building
            onTriggered: cacheConfirm.ask("rebuild")
        }
        Entry {
            objectName: "menuDeleteCache"
            text: qsTr("Delete Cache")
            enabled: !AssetCache.building
            onTriggered: cacheConfirm.ask("clear")
        }
    }
    Menu {
        title: qsTr("&Profile")
        Pending { text: qsTr("New...") }
        Pending { text: qsTr("Duplicate...") }
        Pending { text: qsTr("Rename...") }
        Pending { text: qsTr("Remove") }
        Divider {}
        Pending { text: qsTr("Import...") }
        Pending { text: qsTr("Export...") }
        Divider {}
        Pending { text: qsTr("Profiles are not available yet") }
    }
    Menu {
        id: collectionsMenu
        title: qsTr("&Scene Collection")
        Entry { objectName: "menuCollectionNew"; text: qsTr("New..."); onTriggered: bar.collectionDialog.open("new") }
        Entry { objectName: "menuCollectionDuplicate"; text: qsTr("Duplicate..."); onTriggered: bar.collectionDialog.open("duplicate") }
        Entry { objectName: "menuCollectionRename"; text: qsTr("Rename..."); onTriggered: bar.collectionDialog.open("rename") }
        Entry { objectName: "menuCollectionRemove"; text: qsTr("Remove..."); onTriggered: bar.collectionDialog.open("remove") }
        Divider {}
        Entry { objectName: "menuCollectionImport"; text: qsTr("Import..."); onTriggered: bar.collectionDialog.open("import") }
        Entry { objectName: "menuCollectionExport"; text: qsTr("Export..."); onTriggered: bar.collectionDialog.open("export") }
        Divider {}
        Entry { text: qsTr("Project Problems"); onTriggered: bar.showPanel("info") }
        Divider {}
        Entry { text: Document.projectName; checkable: true; checked: true; enabled: false }
        Instantiator {
            model: Document.collections
            delegate: Entry {
                required property var modelData
                text: modelData.name
                visible: modelData.path !== Document.projectPath
                height: visible ? implicitHeight : 0
                enabled: visible
                onTriggered: Document.openCollection(modelData.path)
            }
            onObjectAdded: (index, object) => collectionsMenu.insertItem(collectionsMenu.count, object)
            onObjectRemoved: (index, object) => collectionsMenu.removeItem(object)
        }
    }
    Menu {
        title: qsTr("&Tools")
        Pending { text: qsTr("Auto-Configuration Wizard") }
        Pending { text: qsTr("Plugin Manager") }
        Divider {}
        Pending { text: qsTr("Output Timer") }
        Pending { text: qsTr("Scripts") }
        Entry {
            objectName: "menuWebsocketSettings"
            text: qsTr("WebSocket Server Settings")
            onTriggered: websocketSettings.open()
        }
        Entry {
            objectName: "menuMcpSettings"
            text: qsTr("MCP Server Settings")
            onTriggered: mcpSettings.open()
        }
        Pending { text: qsTr("Source Profiler") }
    }
    Menu {
        title: qsTr("&Help")
        Entry { text: qsTr("Help Portal"); onTriggered: Qt.openUrlExternally("https://github.com/calmasacow/Libertas-Studio#readme") }
        Entry { text: qsTr("Visit Website"); onTriggered: Qt.openUrlExternally("https://github.com/calmasacow/Libertas-Studio") }
        Pending { text: qsTr("Join Discord Server") }
        Divider {}
        Entry { text: qsTr("Log"); onTriggered: bar.showPanel("log") }
        Pending { text: qsTr("Restart in Safe Mode") }
        Divider {}
        Pending { text: qsTr("What's New") }
        Pending { text: qsTr("Release Notes") }
        Entry { text: qsTr("About"); onTriggered: bar.mainWindow.report(qsTr("Libertas Studio 0.0.1 - GPL-3.0-or-later. Disabled menu items are not implemented yet.")) }
    }
}
