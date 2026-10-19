// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Dialogs
import QtQuick.Layouts
import Sentinel.Desktop

Item {
    id: root
    objectName: "backupSettingsTab"
    required property var viewModel
    property bool compact: false
    property bool notificationsOnly: false
    property bool generalOnly: false
    property color modeAccent: SentinelTheme.modeAccent(viewModel.currentModeName)
    property var soundManager: null
    readonly property int panelPadding: SentinelTheme.spaceLg

    readonly property var backup: root.viewModel.backupManager
    property var backupAvailability: ({})
    property var recovery: ({})
    property var selectedBackupDomains: ["settings"]
    property bool replaceImport: false
    property string recoveryStatus: ""
    readonly property var backupDomains: [
        { key: "settings", title: qsTr("Basic settings") },
        { key: "workspaceProfiles", title: qsTr("Workspace profiles") },
        { key: "extensions", title: qsTr("Extension configuration") },
        { key: "chat", title: qsTr("Conversations") },
        { key: "memory", title: qsTr("Memory") }
    ]
    function refreshRecovery() {
        const availability = root.viewModel.productBackupAvailability()
        const state = root.viewModel.productRecoveryState()
        if (JSON.stringify(availability) !== JSON.stringify(root.backupAvailability)) root.backupAvailability = availability
        if (JSON.stringify(state) !== JSON.stringify(root.recovery)) root.recovery = state
    }
    Timer { interval: 1500; repeat: true; triggeredOnStart: true; running: root.visible && !root.generalOnly && !root.notificationsOnly; onTriggered: root.refreshRecovery() }

    Dialog {
        id: restoreConfirmation
        objectName: "backupRestoreConfirmation"
        parent: Overlay.overlay
        modal: true
        anchors.centerIn: parent
        width: Math.min(540, root.width - 24)
        title: qsTr("Confirm selected domain restore")
        onOpened: restoreCancel.forceActiveFocus(Qt.PopupFocusReason)
        onClosed: { confirmRestore.checked = false; confirmRestore.forceActiveFocus(Qt.PopupFocusReason) }
        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true
                text: qsTr("Restore %1 only. This portable import has no application-wide crash recovery guarantee. Review the selected domain and mode before continuing.").arg(root.selectedBackupDomains.join(", "))
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: SentinelTheme.textPrimary
            }
            RowLayout {
                Button { id: restoreCancel; objectName: "backupRestoreCancel"; text: qsTr("Cancel"); onClicked: restoreConfirmation.close() }
                Button {
                    objectName: "backupRestoreApply"
                    text: qsTr("Restore selected domain")
                    enabled: root.viewModel.daemonConnected && !root.backup.busy && confirmRestore.checked && root.selectedBackupDomains.length === 1
                    onClicked: {
                        const selected = root.selectedBackupDomains.filter(function(key) { return root.backup.importDomains.indexOf(key) >= 0 })
                        root.backup.importFile(selected, root.replaceImport)
                        restoreConfirmation.close()
                    }
                }
            }
        }
    }

    signal openUpdateRequested()

    height: implicitHeight
    implicitHeight: visible ? systemContent.implicitHeight + panelPadding * 2 : 0

    ColumnLayout {
        id: systemContent
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: root.panelPadding
        spacing: SentinelTheme.spaceMd

        SectionTitle {
            visible: !root.generalOnly && !root.notificationsOnly
            title: qsTr("Backup & Recovery")
            subtitle: qsTr("Export one portable data domain at a time. Coordinated application backup and recovery are unavailable. Credentials, permission grants and agent runs are excluded.")
            Layout.fillWidth: true
        }
        SettingCard {
            visible: !root.generalOnly && !root.notificationsOnly
            title: qsTr("Backup data")
            subtitle: qsTr("Basic settings include runtime theme, language and network mode. Workspace profiles can contain sensitive instructions and paths; select them only for an explicit export. Managed memory uses authorized scope archives on the Memory page.")
            Label {
                objectName: "coordinatedBackupStatus"
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceMd
                text: !root.viewModel.daemonConnected ? qsTr("Connect to the daemon to inspect backup availability.")
                    : !root.backupAvailability.coordinatedState ? qsTr("Loading backup availability…")
                    : qsTr("Coordinated backup: %1. Consistent recovery across application data stores is not yet available. Use a single domain export or an authorized memory archive.").arg(root.backupAvailability.coordinatedState)
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: SentinelTheme.warning
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            Repeater {
                model: root.backupDomains
                SettingToggleRow {
                    required property var modelData
                    title: modelData.title
                    compact: root.compact
                    interactive: !root.backup.busy && root.backupAvailability[modelData.key] === true
                    checked: root.selectedBackupDomains.indexOf(modelData.key) >= 0
                    onToggled: (checked) => {
                        root.selectedBackupDomains = checked ? [modelData.key] : []
                        confirmRestore.checked = false
                    }
                }
            }
            Flow {
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceMd
                spacing: 8
                SentinelButton { objectName: "backupExportButton"; text: qsTr("Export selected domain"); enabled: root.viewModel.daemonConnected && !root.backup.busy && root.selectedBackupDomains.length > 0; onClicked: backupSaveDialog.open() }
                SentinelButton { objectName: "backupInspectButton"; text: qsTr("Choose backup to restore"); enabled: !root.backup.busy; onClicked: backupOpenDialog.open() }
                SentinelButton { visible: root.backup.cancellable; text: qsTr("Cancel transfer"); onClicked: root.backup.cancel() }
            }
            Label {
                visible: root.backup.preview.length > 0
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceMd
                text: root.backup.preview
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: SentinelTheme.textPrimary
            }
            SettingControlRow {
                visible: root.backup.importDomains.length > 0
                title: qsTr("Restore mode")
                subtitle: qsTr("Merge reports conflicts. Replace overwrites only the selected data domains.")
                compact: root.compact
                SentinelComboBox {
                    anchors.fill: parent
                    model: [qsTr("Merge"), qsTr("Replace selected data")]
                    currentIndex: root.replaceImport ? 1 : 0
                    onActivated: (index) => { root.replaceImport = index === 1; confirmRestore.checked = false }
                }
            }
            CheckBox {
                id: confirmRestore
                objectName: "backupReviewed"
                visible: root.backup.importDomains.length > 0
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceMd
                text: root.replaceImport ? qsTr("I reviewed this backup and allow replacement of the selected data.") : qsTr("I reviewed this backup and want to merge the selected data.")
                contentItem: Text { text: confirmRestore.text; color: SentinelTheme.textPrimary; wrapMode: Text.WordWrap; leftPadding: confirmRestore.indicator.width + 8; verticalAlignment: Text.AlignVCenter }
            }
            SentinelButton {
                id: restoreSelected
                objectName: "backupRestoreSelected"
                visible: root.backup.importDomains.length > 0
                Layout.margins: SentinelTheme.spaceMd
                text: qsTr("Restore selected data")
                enabled: root.viewModel.daemonConnected && !root.backup.busy && confirmRestore.checked && root.selectedBackupDomains.some(function(key) { return root.backup.importDomains.indexOf(key) >= 0 })
                onClicked: restoreConfirmation.open()
            }
            ProgressBar { visible: root.backup.busy; Layout.fillWidth: true; value: root.backup.progress }
            Label { Layout.fillWidth: true; Layout.margins: SentinelTheme.spaceMd; visible: root.backup.status.length > 0; text: root.backup.status; color: SentinelTheme.textMuted; wrapMode: Text.WordWrap; textFormat: Text.PlainText }
        }
        SettingCard {
            visible: !root.generalOnly && !root.notificationsOnly
            title: qsTr("Recovery status")
            subtitle: qsTr("Inspect interrupted work. Recovery never automatically restarts a model, tool or agent.")
            Label {
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceMd
                text: root.recovery.health ? qsTr("%1 · interrupted chat messages: %2 · agent runs: %3").arg(root.recovery.health).arg(root.recovery.interruptedChatMessages || 0).arg(root.recovery.interruptedAgentRuns || 0) : qsTr("Waiting for recovery information from the daemon…")
                color: SentinelTheme.textMuted
                wrapMode: Text.WordWrap
            }
            Repeater {
                model: root.recovery.conditions || []
                Label { required property var modelData; Layout.fillWidth: true; Layout.margins: SentinelTheme.spaceMd; text: modelData.domain + ": " + modelData.code; color: SentinelTheme.warning; textFormat: Text.PlainText; wrapMode: Text.WordWrap }
            }
            Repeater {
                model: root.recovery.interruptedModelOperations || []
                ColumnLayout {
                    id: interruptedDownload
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.margins: SentinelTheme.spaceMd
                    Label { text: interruptedDownload.modelData.filename; Layout.fillWidth: true; color: SentinelTheme.textPrimary; wrapMode: Text.WrapAnywhere }
                    SentinelButton {
                        text: qsTr("Discard interrupted download")
                        onClicked: {
                            const result = root.viewModel.resolveInterruptedModelOperation(interruptedDownload.modelData.id)
                            root.recoveryStatus = result.accepted ? qsTr("Interrupted download discarded.") : result.code === "Pending" ? qsTr("Discard requested. Recovery status will update after confirmation.") : qsTr("Could not discard the interrupted download.")
                            root.refreshRecovery()
                        }
                    }
                }
            }
            Label { visible: root.recoveryStatus.length > 0; text: root.recoveryStatus; Layout.fillWidth: true; Layout.margins: SentinelTheme.spaceMd; color: SentinelTheme.textMuted; wrapMode: Text.WordWrap }
        }

        SectionTitle {
            visible: !root.notificationsOnly
            title: root.generalOnly ? qsTr("Desktop") : qsTr("Updates & Diagnostics")
            subtitle: qsTr("System integration, notifications, updates, and audio feedback.")
            Layout.fillWidth: true
        }

        SettingCard {
            visible: !root.notificationsOnly
            title: root.generalOnly ? qsTr("Desktop") : qsTr("Updates")
            subtitle: qsTr("Check for application updates, release notes, and system runtime status.")

            SettingControlRow {
                visible: !root.generalOnly
                title: qsTr("Check for Updates")
                subtitle: qsTr("Current version: v%1 (%2)").arg(Qt.application.version).arg(Qt.platform.os)
                accent: root.modeAccent
                compact: root.compact
                showDivider: true
                controlWidth: root.compact ? 160 : 200

                SentinelButton {
                    accent: root.modeAccent
                    anchors.fill: parent
                    text: qsTr("Check Updates")
                    onClicked: root.openUpdateRequested()
                }
            }

            ColumnLayout {
                visible: !root.generalOnly
                Layout.fillWidth: true
                Layout.leftMargin: SentinelTheme.spaceMd
                Layout.rightMargin: SentinelTheme.spaceMd
                Layout.topMargin: SentinelTheme.spaceSm
                Layout.bottomMargin: SentinelTheme.spaceSm
                spacing: SentinelTheme.spaceSm

                InfoRow {
                    compact: root.compact
                    label: qsTr("Version")
                    value: Qt.application.version
                    Layout.fillWidth: true
                }

                InfoRow {
                    compact: root.compact
                    label: qsTr("Platform")
                    value: Qt.platform.os
                    Layout.fillWidth: true
                }

            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.05)
            }

            SettingToggleRow {
                visible: root.generalOnly
                title: qsTr("Enable Sound Effects")
                subtitle: qsTr("Play subtle audio cues for completions, notifications, and key interactions.")
                checked: root.viewModel.soundEffectsEnabled
                accent: root.modeAccent
                compact: root.compact
                showDivider: true
                onToggled: (checked) => root.viewModel.soundEffectsEnabled = checked
            }

            SettingToggleRow {
                visible: root.generalOnly
                title: qsTr("Enable Companion Service")
                subtitle: qsTr("Run background tray companion for system-wide shortcuts and quick assistant access.")
                checked: root.viewModel.companionEnabled
                accent: root.modeAccent
                compact: root.compact
                showDivider: true
                onToggled: (checked) => root.viewModel.companionEnabled = checked
            }

            SettingToggleRow {
                visible: root.generalOnly
                title: qsTr("Start Sentinel at login")
                subtitle: nativeDesktop.startupStatus
                checked: nativeDesktop.startAtLogin
                accent: root.modeAccent
                compact: root.compact
                onToggled: (checked) => nativeDesktop.startAtLogin = checked
            }
            Label {
                Layout.fillWidth: true
                visible: root.generalOnly
                text: qsTr("Quick Panel shortcut: %1").arg(nativeDesktop.shortcutStatus)
                color: SentinelTheme.textMuted
                wrapMode: Text.Wrap
            }
            Label {
                visible: root.generalOnly && nativeDesktop.notificationStatus.length > 0
                Layout.fillWidth: true
                text: qsTr("System notifications: %1").arg(nativeDesktop.notificationStatus)
                color: SentinelTheme.textMuted
                wrapMode: Text.Wrap
            }
            SentinelComboBox {
                visible: root.generalOnly
                model: ["Ctrl+Alt+Space", "Ctrl+Alt+S", "Disabled"]
                currentIndex: (nativeDesktop.shortcut === "" || nativeDesktop.shortcut === "Disabled") ? 2 : (nativeDesktop.shortcut === "Ctrl+Alt+S" ? 1 : 0)
                Accessible.name: qsTr("Global Quick Panel shortcut")
                onActivated: nativeDesktop.shortcut = currentIndex === 2 ? "Disabled" : currentText
            }

            SettingToggleRow {
                visible: !root.generalOnly
                title: qsTr("Developer Diagnostics Mode")
                subtitle: qsTr("Show read-only permission, tool, agent, notification, and task diagnostics. This does not change execution permissions.")
                checked: root.viewModel.developerModeEnabled
                accent: root.modeAccent
                compact: root.compact
                showDivider: true
                onToggled: (checked) => root.viewModel.developerModeEnabled = checked
            }

            SettingCard {
                visible: !root.generalOnly && root.viewModel.developerModeEnabled
                title: qsTr("Developer Diagnostics")
                subtitle: qsTr("Read-only runtime details for troubleshooting.")

                Label {
                    Layout.fillWidth: true
                    text: root.viewModel.diagnosticsCenterSummaries.join("\n")
                    color: SentinelTheme.textMuted
                    font.pixelSize: SentinelTheme.fontSmall
                    wrapMode: Text.WordWrap
                    leftPadding: SentinelTheme.spaceMd
                    rightPadding: SentinelTheme.spaceMd
                    bottomPadding: SentinelTheme.spaceSm
                }
            }

            SettingControlRow {
                visible: !root.generalOnly
                title: qsTr("Update Policy")
                subtitle: qsTr("Frequency for checking software updates and security releases.")
                accent: root.modeAccent
                compact: root.compact
                showDivider: true

                SentinelComboBox {
                    accent: root.modeAccent
                    anchors.fill: parent
                    model: [qsTr("Ask Before Checking"), qsTr("Weekly"), qsTr("On Startup"), qsTr("Never")]
                    currentIndex: {
                        var pol = root.viewModel.updateCheckPolicy
                        if (pol === "Weekly") return 1
                        if (pol === "On Startup") return 2
                        if (pol === "Never") return 3
                        return 0
                    }
                    onActivated: (index) => {
                        var policies = ["Ask Before Checking", "Weekly", "On Startup", "Never"]
                        root.viewModel.updateCheckPolicy = policies[index]
                    }
                }
            }


        }

        SectionTitle {
            visible: root.notificationsOnly
            title: qsTr("Delivery")
            subtitle: qsTr("Turn each notification type on or off. Your notification history stays available when alerts are paused.")
            Layout.fillWidth: true
            Layout.topMargin: SentinelTheme.spaceMd
        }

        SettingCard {
            visible: root.notificationsOnly
            SettingToggleRow {
                objectName: "settingsDnd"
                title: qsTr("Do Not Disturb")
                subtitle: qsTr("Pause banners and system alerts. Notifications remain in your history.")
                checked: root.viewModel.dndEnabled
                compact: root.compact
                onToggled: (checked) => root.viewModel.dndEnabled = checked
            }
            SettingControlRow {
                title: qsTr("Notification history")
                subtitle: qsTr("Review alerts, mark them read, or clear archived items.")
                compact: root.compact
                SentinelButton { anchors.fill: parent; objectName: "notificationHistoryButton"; text: qsTr("Open history"); Accessible.name: text; onClicked: root.viewModel.notificationCenterVisible = true }
            }
            Flow {
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceMd
                spacing: 8
                SentinelButton { text: qsTr("Mark all read"); onClicked: root.viewModel.markAllNotificationsRead() }
                SentinelButton { text: qsTr("Clear archived"); onClicked: root.viewModel.clearArchivedNotifications() }
            }
        }
        SettingCard {
            visible: root.notificationsOnly
            title: qsTr("Channels")
            subtitle: qsTr("Channel switches apply to banners and system alerts, alongside the delivery policy.")
            Repeater {
                model: [{ key: "Tasks", title: qsTr("Tasks") }, { key: "Security", title: qsTr("Permissions & security") }, { key: "Workspace", title: qsTr("Workspace") }, { key: "Brain", title: qsTr("Memory & knowledge") }]
                SettingToggleRow {
                    required property var modelData
                    title: modelData.title
                    compact: root.compact
                    checked: { root.viewModel.notificationCenterSummaries; return !root.viewModel.isChannelMuted(modelData.key) }
                    onToggled: (checked) => root.viewModel.setChannelMuted(modelData.key, !checked)
                }
            }
        }
        SettingCard {
            visible: root.notificationsOnly
            title: qsTr("Notification Types")
            subtitle: qsTr("Only the events you enable will notify you.")

            SettingControlRow {
                title: qsTr("Notification Policy")
                subtitle: qsTr("Filter level for system popups and banner alerts.")
                accent: root.modeAccent
                compact: root.compact

                SentinelComboBox {
                    accent: root.modeAccent
                    anchors.fill: parent
                    model: [qsTr("Important Only"), qsTr("All"), qsTr("Custom"), qsTr("Disabled")]
                    currentIndex: {
                        var pol = root.viewModel.notificationPolicy
                        if (pol === "All") return 1
                        if (pol === "Custom") return 2
                        if (pol === "Disabled") return 3
                        return 0
                    }
                    onActivated: (index) => {
                        var policies = ["Important Only", "All", "Custom", "Disabled"]
                        root.viewModel.notificationPolicy = policies[index]
                    }
                }
            }
            SettingToggleRow {
                title: qsTr("Model Downloads")
                subtitle: qsTr("Notify when local LLM downloads or weight verifications complete.")
                checked: root.viewModel.notifyModelDownloads
                accent: root.modeAccent
                compact: root.compact
                showDivider: true
                onToggled: (checked) => root.viewModel.notifyModelDownloads = checked
            }

            SettingToggleRow {
                title: qsTr("Model Removals")
                subtitle: qsTr("Alert when model files or cached weights are purged.")
                checked: root.viewModel.notifyModelRemovals
                accent: root.modeAccent
                compact: root.compact
                showDivider: true
                onToggled: (checked) => root.viewModel.notifyModelRemovals = checked
            }

            SettingToggleRow {
                title: qsTr("Agent Responses")
                subtitle: qsTr("Notify when autonomous agent tasks finish background execution.")
                checked: root.viewModel.notifyAgentResponses
                accent: root.modeAccent
                compact: root.compact
                showDivider: true
                onToggled: (checked) => root.viewModel.notifyAgentResponses = checked
            }

            SettingToggleRow {
                title: qsTr("System Updates")
                subtitle: qsTr("Alert when new Sentinel system versions or security updates are ready.")
                checked: root.viewModel.notifySystemUpdates
                accent: root.modeAccent
                compact: root.compact
                onToggled: (checked) => root.viewModel.notifySystemUpdates = checked
            }
        }

    }
    FileDialog {
        id: backupSaveDialog
        title: qsTr("Save Sentinel backup")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("Sentinel backup (*.json)")]
        onAccepted: root.backup.exportFile(selectedFile, root.selectedBackupDomains)
    }
    FileDialog {
        id: backupOpenDialog
        title: qsTr("Inspect Sentinel backup")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Sentinel backup (*.json)")]
        onAccepted: { confirmRestore.checked = false; root.backup.inspectFile(selectedFile) }
    }

}
