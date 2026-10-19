// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts
import Sentinel.Desktop

ApplicationWindow {
    id: root
    width: 1320
    height: 860
    minimumWidth: 780
    minimumHeight: 640
    visible: true
    title: qsTr("Sentinel")
    color: SentinelTheme.backgroundBase
    property var viewModel: shellViewModel
    property bool shellReady: true
    property string lastPrimaryPage: "Dashboard"
    property string currentShellPage: "Dashboard"
    readonly property bool compactLayout: root.width < 1080
    readonly property bool wideLayout: root.width >= SentinelTheme.breakpointWide
    readonly property int shellEntranceOffset: root.shellReady || MotionTokens.reduced(root.viewModel.currentModeName) ? 0 : 8
    readonly property int pageMotionOffset: MotionTokens.reduced(root.viewModel.currentModeName) ? 0 : 10
    readonly property bool nativeTitlebar: true

    Component.onCompleted: Qt.callLater(function() {
        SentinelTheme.activeTheme = root.viewModel.themeName
        SentinelTheme.reducedMotion = root.viewModel.reducedMotionEnabled
        SentinelTheme.highContrast = root.viewModel.highContrastEnabled
            SentinelTheme.reducedTransparency = root.viewModel.reducedTransparencyEnabled
        SentinelTheme.uiDensity = root.viewModel.uiDensity
        MotionTokens.reducedMotion = root.viewModel.reducedMotionEnabled
        root.shellReady = true
        splashScreen.close()
        if (!root.viewModel.onboardingComplete)
            onboardingScreen.active = true
        else if (root.viewModel.recoveryDraftText.length > 0)
            recoveryModal.open()
    })

    Binding {
        target: SentinelTheme
        property: "activeTheme"
        value: root.viewModel.themeName
    }

    Connections {
        target: root.viewModel
        function onThemeNameChanged() {
            SentinelTheme.activeTheme = root.viewModel.themeName
        }
        function onNativeExperienceChanged() {
            SentinelTheme.reducedMotion = root.viewModel.reducedMotionEnabled
            SentinelTheme.highContrast = root.viewModel.highContrastEnabled
            SentinelTheme.reducedTransparency = root.viewModel.reducedTransparencyEnabled
            SentinelTheme.uiDensity = root.viewModel.uiDensity
            MotionTokens.reducedMotion = root.viewModel.reducedMotionEnabled
        }
        function onRequestWindowActive(pageName) {
            root.show()
            root.raise()
            root.requestActivate()
            if (pageName.length > 0) {
                root.navigateToPage(pageName)
            }
        }
        function onRequestCompanionChat() {
            trayCompanionWindow.toggleVisibility()
        }
        function onOnboardingCompleteChanged() {
            if (!root.viewModel.onboardingComplete && !onboardingScreen.active) {
                onboardingScreen.active = true
            }
        }
    }



    function navigateToPage(pageName) {
        if (pageName === "Settings") {
            root.openSettings()
            return
        }
        if (pageName === "Extensions" || pageName === "Tools") {
            root.currentShellPage = pageName
            extensionsHub.openSection(pageName === "Tools" ? "tools" : "plugins")
            return
        }
        if (pageName === "Dashboard" || pageName === "Models" || pageName === "Inspector" || pageName === "Memory") {
            root.currentShellPage = pageName
            return
        }
        root.viewModel.currentPage = pageName
    }

    function openSettings(category) {
        root.currentShellPage = "Settings"
        settingsPage.jumpTo(category || "Interface")
    }

    function openUpdateModal() {
        if (updateModal.updateState !== "available"
            && updateModal.updateState !== "downloading"
            && updateModal.updateState !== "completed") {
            updateModal.open()
            updateModal.startCheckAndDownload()
        } else {
            updateModal.open()
        }
    }

    function focusChatComposer() {
        root.currentShellPage = "Dashboard"
        Qt.callLater(function() {
            dashboardPage.focusComposer()
        })
    }

    background: Atmosphere {
        modeName: root.viewModel.currentModeName
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: navigationRail.width + 8
        anchors.rightMargin: 8
        anchors.topMargin: 8 + root.shellEntranceOffset
        anchors.bottomMargin: 8 - root.shellEntranceOffset
        spacing: SentinelTheme.spaceMd
        opacity: root.shellReady ? 1.0 : 0.0

        Behavior on opacity {
            NumberAnimation {
                duration: MotionTokens.duration(MotionTokens.page, root.viewModel.currentModeName)
                easing.type: MotionTokens.enter
            }
        }



        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 0
            spacing: SentinelTheme.spaceMd

        ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 0
                spacing: SentinelTheme.spaceMd


                ErrorBanner {
                    id: globalErrorBanner
                    Layout.fillWidth: true
                    Layout.maximumHeight: root.viewModel.globalErrorVisible ? 48 : 0
                    show: root.viewModel.globalErrorVisible
                    message: root.viewModel.globalErrorMessage
                    severity: root.viewModel.globalErrorSeverity
                    actionLabel: root.viewModel.globalErrorActionLabel
                    modeName: root.viewModel.currentModeName
                    onActionTriggered: root.viewModel.triggerGlobalErrorAction()
                    onDismissed: root.viewModel.dismissGlobalError()
                }

                // Page stack: Dashboard / Models with slide + crossfade transitions
                Item {
                    id: pageStack
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 0

                    property real dashSlide: root.currentShellPage === "Models" ? -1 : 0
                    property real modelsSlide: root.currentShellPage === "Dashboard" ? 1 : 0

                    DashboardPage {
                        id: dashboardPage
                        onVoiceSettingsRequested: root.openSettings("Voice")
                        viewModel: root.viewModel
                        anchors.fill: parent
                        visible: root.currentShellPage === "Dashboard"
                        opacity: root.currentShellPage === "Dashboard" ? 1.0 : 0.0

                        transform: Translate {
                            id: dashTranslate
                            x: pageStack.dashSlide * root.pageMotionOffset * 2
                        }

                        Behavior on opacity {
                            NumberAnimation { duration: MotionTokens.duration(MotionTokens.page, root.viewModel.currentModeName); easing.type: Easing.OutCubic }
                        }
                    }

                    ModelsPage {
                        id: modelsPage
                        anchors.fill: parent
                        visible: opacity > 0
                        opacity: root.currentShellPage === "Models" ? 1.0 : 0.0

                        transform: Translate {
                            id: modelsTranslate
                            x: pageStack.modelsSlide * root.pageMotionOffset * 2
                        }

                        Behavior on opacity {
                            NumberAnimation { duration: MotionTokens.duration(MotionTokens.page, root.viewModel.currentModeName); easing.type: Easing.OutCubic }
                        }
                    }

                    SettingsPage {
                        id: settingsPage
                        anchors.fill: parent
                        visible: root.currentShellPage === "Settings"
                        viewModel: root.viewModel
                        soundManager: soundManager
                        onOpenUpdateRequested: root.openUpdateModal()
                    }

                    ExtensionsHubPage {
                        id: extensionsHub
                        anchors.fill: parent
                        visible: root.currentShellPage === "Extensions" || root.currentShellPage === "Tools"
                        viewModel: root.viewModel
                        onSettingsRequested: function(category) { root.openSettings(category) }
                    }

                    MemoryPage {
                        anchors.fill: parent
                        viewModel: root.viewModel
                        visible: root.currentShellPage === "Memory"
                    }

                    AgentInspectorPage {
                        id: inspectorPage
                        anchors.fill: parent
                        visible: root.currentShellPage === "Inspector"
                        viewModel: agentInspectorViewModel
                    }

                    Behavior on dashSlide {
                        NumberAnimation {
                            duration: MotionTokens.duration(MotionTokens.page, root.viewModel.currentModeName)
                            easing.type: Easing.OutCubic
                        }
                    }

                    Behavior on modelsSlide {
                        NumberAnimation {
                            duration: MotionTokens.duration(MotionTokens.page, root.viewModel.currentModeName)
                            easing.type: Easing.OutCubic
                        }
                    }
                }
            }
        }
    }

    NavigationRail {
        id: navigationRail
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        currentPage: root.currentShellPage
        connectionStatus: root.viewModel.daemonConnectionStatus
        connected: root.viewModel.daemonConnected
        runtimeStatus: root.viewModel.activeRuntimeProviderLabel + " / " + root.viewModel.activeRuntimeReadinessSummary
        onPageRequested: function(pageName) { root.navigateToPage(pageName) }
    }

    SoundManager {
        id: soundManager
        enabled: !root.viewModel.dndEnabled && root.viewModel.soundEffectsEnabled
    }

    SplashScreen {
        id: splashScreen
        modeName: root.viewModel.currentModeName
    }

    CommandPalette {
        id: commandPalette
        viewModel: root.viewModel
        onOpenSettingsRequested: function(category) { root.openSettings(category) }
        onOpenUpdateRequested: root.openUpdateModal()
        onFocusChatRequested: root.focusChatComposer()
    }

    NotificationCenterPanel {
        fallbackFocusItem: navigationRail.settingsFocusItem
        viewModel: root.viewModel
        onOpenSettingsRequested: root.openSettings("Notifications")
    }

    TrayCompanionWindow {
        id: trayCompanionWindow
        viewModel: root.viewModel
        controller: quickPanelController
    }

    OnboardingScreen {
        id: onboardingScreen
        onConfigureModelsRequested: root.openSettings("AI")
        viewModel: root.viewModel
        anchors.fill: parent
        onFinished: {
            root.viewModel.onboardingComplete = true
            onboardingScreen.active = false
        }
    }

    UpdateProgressModal {
        id: updateModal
        viewModel: root.viewModel
        currentVersion: Qt.application.version
        modeName: root.viewModel.currentModeName
        accent: SentinelTheme.modeAccent(root.viewModel.currentModeName)
        onCheckRequested: {
            root.viewModel.checkForUpdates()
        }
        onDownloadRequested: {
            var downloadUrl = root.viewModel.lastAssetUrl
            if (downloadUrl && downloadUrl.length > 0) {
                updateModal.startDownload()
                root.viewModel.startDownload(downloadUrl)
            }
        }
        onUpdateCompleted: {
            root.viewModel.relaunchApplication()
        }
    }

    SentinelOverlayModal {
        id: recoveryModal
        accent: SentinelTheme.warning
        modeName: root.viewModel.currentModeName
        preferredWidth: Math.min(520, root.width - SentinelTheme.space4Xl)
        preferredHeight: 260
        // Non-destructive default: Enter restores the locally drafted text rather than
        // discarding it. Discard remains reachable with Tab / Shift+Tab.
        initialFocusItem: recoveryRestore

        contentItem: ColumnLayout {
            Accessible.role: Accessible.Dialog
            Accessible.name: qsTr("Restore previous session")
            Accessible.description: qsTr("Sentinel saved your last draft locally. Restore it into the composer or discard it.")
            spacing: SentinelTheme.spaceMd
            Label {
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceLg
                text: qsTr("Restore previous session?")
                color: SentinelTheme.textPrimary
                font.pixelSize: SentinelTheme.fontCard
                font.bold: true
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: SentinelTheme.spaceLg
                Layout.rightMargin: SentinelTheme.spaceLg
                text: qsTr("Sentinel saved your last draft locally. Restore it into the composer or discard it.")
                color: SentinelTheme.textMuted
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceLg
                SentinelButton {
                    text: qsTr("Discard")
                    onClicked: {
                        root.viewModel.recoveryDraftText = ""
                        recoveryModal.close()
                    }
                }
                Item { Layout.fillWidth: true }
                SentinelButton {
                    id: recoveryRestore
                    text: qsTr("Restore")
                    onClicked: {
                        dashboardPage.restoreDraft(root.viewModel.recoveryDraftText)
                        root.viewModel.recoveryDraftText = ""
                        recoveryModal.close()
                    }
                }
            }
        }
    }

    Connections {
        target: root.viewModel
        function onRequestOpenUpdateModal() {
            root.openUpdateModal()
        }
        function onCurrentPageChanged() {
            if (root.viewModel.currentPage === "Settings") {
                root.openSettings()
                root.viewModel.currentPage = root.lastPrimaryPage
                return
            }
            root.lastPrimaryPage = root.viewModel.currentPage
        }
        function onUpdateCheckCompleted(available, version, releaseNotes, downloadUrl, assetSize) {
            if (available) {
                updateModal.availableVersion = version
                if (releaseNotes && releaseNotes.length > 0) {
                    updateModal.releaseNotesText = releaseNotes
                }
                if (assetSize > 0) {
                    var sizeMB = (assetSize / (1024 * 1024)).toFixed(1)
                    updateModal.packageSizeText = sizeMB + " MB"
                }
                updateModal.updateState = "available"
            } else {
                var wf = root.viewModel.updateWorkflowState
                if (wf && wf.startsWith("Update check failed")) {
                    updateModal.updateState = "error"
                    updateModal.errorMessage = wf
                } else {
                    updateModal.updateState = "upToDate"
                }
            }
        }
        function onUpdateDownloadProgressChanged(bytesReceived, bytesTotal, speedBytesPerSec) {
            updateModal.updateState = "downloading"
            updateModal.downloadProgress = bytesTotal > 0 ? bytesReceived / bytesTotal : 0.0

            var receivedMB = (bytesReceived / (1024 * 1024)).toFixed(1)
            var totalMB = (bytesTotal / (1024 * 1024)).toFixed(1)
            updateModal.downloadedBytesText = receivedMB + " MB / " + totalMB + " MB"

            if (speedBytesPerSec > 0) {
                var speedText
                if (speedBytesPerSec > 1024 * 1024) {
                    speedText = (speedBytesPerSec / (1024 * 1024)).toFixed(1) + " MB/s"
                } else if (speedBytesPerSec > 1024) {
                    speedText = (speedBytesPerSec / 1024).toFixed(1) + " KB/s"
                } else {
                    speedText = speedBytesPerSec.toFixed(0) + " B/s"
                }
                updateModal.downloadSpeedText = speedText
            }
        }
        function onUpdateDownloadFinished(success, filePath) {
            if (success) {
                updateModal.updateState = "completed"
                updateModal.downloadProgress = 1.0
            } else {
                updateModal.updateState = "error"
                updateModal.errorMessage = qsTr("Download failed. Please check your network connection and try again.")
            }
        }
    }

    SentinelOverlayModal {
        id: daemonApprovalModal
        objectName: "daemonApprovalModal"
        preferredWidth: 620
        preferredHeight: 380
        closePolicy: Popup.NoAutoClose
        // Safety: focus lands on Deny, so a stray Enter/Return can never approve.
        // Allow once is only reachable by an explicit Tab + activation.
        initialFocusItem: daemonApprovalDeny
        readonly property var approval: root.viewModel.pendingDaemonApproval
        visible: root.viewModel.daemonConnected && !!approval.approval_id
        contentItem: ColumnLayout {
            Accessible.role: Accessible.Dialog
            Accessible.name: qsTr("Tool approval required")
            Accessible.description: qsTr("An agent requested permission to run a tool. Review the request, then deny, allow once, or cancel the run.")
            spacing: SentinelTheme.spaceMd
            Label {
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceLg
                text: qsTr("Tool approval required")
                font.bold: true
                color: SentinelTheme.textPrimary
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: SentinelTheme.spaceLg
                Layout.rightMargin: SentinelTheme.spaceLg
                text: (daemonApprovalModal.approval.tool || "") + "\n"
                    + (daemonApprovalModal.approval.detail || "") + "\n"
                    + qsTr("Risk: %1").arg(daemonApprovalModal.approval.risk || 0) + "\n"
                    + (daemonApprovalModal.approval.resources || []).map(function(resource) {
                        return qsTr("Resource: %1 (domain %2, access %3)").arg(resource.resource || "").arg(resource.domain).arg(resource.access)
                    }).join("\n")
                wrapMode: Text.WrapAnywhere
                color: SentinelTheme.textPrimary
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            Item { Layout.fillHeight: true }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceLg
                SentinelButton {
                    text: qsTr("Cancel run")
                    onClicked: root.viewModel.cancelDaemonRun()
                }
                Item { Layout.fillWidth: true }
                SentinelButton {
                    id: daemonApprovalDeny
                    objectName: "daemonApprovalDeny"
                    text: qsTr("Deny")
                    onClicked: root.viewModel.respondToDaemonApproval(false)
                }
                SentinelButton {
                    objectName: "daemonApprovalAllow"
                    text: qsTr("Allow once")
                    onClicked: root.viewModel.respondToDaemonApproval(true)
                }
            }
        }
    }

    Shortcut {
        sequences: ["Ctrl+K", "Meta+K"]
        onActivated: commandPalette.openPalette()
    }

    Shortcut {
        sequences: ["Ctrl+1", "Meta+1"]
        onActivated: root.navigateToPage("Dashboard")
    }

    Shortcut {
        sequences: ["Ctrl+2", "Meta+2"]
        onActivated: root.navigateToPage("Models")
    }

    Shortcut {
        sequences: ["Ctrl+4", "Meta+4"]
        onActivated: root.openSettings()
    }

    Shortcut {
        sequences: ["Ctrl+L", "Meta+L"]
        onActivated: root.focusChatComposer()
    }

    Shortcut {
        sequences: ["Ctrl+,", "Meta+,"]
        onActivated: root.openSettings()
    }

    Shortcut {
        sequences: ["Ctrl+Shift+C", "Meta+Shift+C"]
        onActivated: trayCompanionWindow.toggleVisibility()
    }

    Shortcut {
        sequence: "Esc"
        onActivated: {
            if (root.viewModel.notificationCenterVisible)
                root.viewModel.notificationCenterVisible = false
            else if (commandPalette.opened)
                commandPalette.close()
            else if (root.currentShellPage === "Settings" || root.currentShellPage === "Extensions" || root.currentShellPage === "Tools")
                root.currentShellPage = "Dashboard"
        }
    }
}
