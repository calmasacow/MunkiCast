// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtWebEngine
import MunkiCast

Item {
    id: panel
    required property string panelId
    readonly property string configuredUrl: {
        const dock = Workspace.browserDocks.find(entry => entry.id === panel.panelId);
        return dock ? dock.url : "";
    }
    property string loadError: ""

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Controls.ToolButton {
                text: qsTr("Back")
                enabled: web.canGoBack
                onClicked: web.goBack()
            }
            Controls.ToolButton {
                text: web.loading ? qsTr("Stop") : qsTr("Reload")
                onClicked: web.loading ? web.stop() : web.reload()
            }
            Controls.ToolButton {
                text: qsTr("Home")
                onClicked: web.url = panel.configuredUrl
            }
            Controls.Label {
                text: web.loading ? qsTr("Loading %1%").arg(web.loadProgress) : web.url.toString()
                elide: Text.ElideMiddle
                Layout.fillWidth: true
                leftPadding: 6
            }
        }
        Controls.Label {
            text: panel.loadError
            visible: text !== ""
            color: Theme.c.warning
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            padding: 6
        }
        WebEngineView {
            id: web
            objectName: "browserDockWebView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            url: panel.configuredUrl
            settings.localContentCanAccessFileUrls: false
            settings.localContentCanAccessRemoteUrls: false
            onLoadingChanged: request => {
                if (request.status === WebEngineView.LoadStartedStatus)
                    panel.loadError = "";
                else if (request.status === WebEngineView.LoadFailedStatus)
                    panel.loadError = qsTr("Could not load page: %1").arg(request.errorString);
            }
            onNewWindowRequested: request => {
                if (request.userInitiated) {
                    const popup = popupWindow.createObject(panel) as BrowserPopup;
                    request.openIn(popup.browser);
                    popup.show();
                }
            }
        }
    }
    component BrowserPopup: Window {
        id: popup
        title: popupBrowser.title
        width: 800
        height: 650
        property alias browser: popupBrowser
        onClosing: destroy()
        WebEngineView {
            id: popupBrowser
            anchors.fill: parent
            onWindowCloseRequested: popup.close()
        }
    }
    Component {
        id: popupWindow
        BrowserPopup {}
    }
}
