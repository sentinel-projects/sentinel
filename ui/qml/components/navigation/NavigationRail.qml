// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Effects
import Sentinel.Desktop

Rectangle {
    id: rail
    property bool connected: false
    property string connectionStatus: ""
    property string runtimeStatus: ""
    property alias settingsFocusItem: settingsButton
    required property string currentPage
    signal pageRequested(string pageName)
    width: 56
    color: SentinelTheme.surface
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Main navigation")

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8
        Repeater {
            model: [
                { page: "Dashboard", label: qsTr("Home"), icon: "home" },
                { page: "Models", label: qsTr("Models"), icon: "brain" },
                { page: "Inspector", label: qsTr("Inspector"), icon: "eye" },
                { page: "Tools", label: qsTr("Tools"), icon: "tool" },
                { page: "Extensions", label: qsTr("Extensions"), icon: "puzzle" }
            ]
            delegate: Button {
                id: navigationButton
                objectName: modelData.page + "NavigationButton"
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                focusPolicy: Qt.StrongFocus
                hoverEnabled: true
                Accessible.name: modelData.label
                checkable: true
                autoExclusive: true // Selecting the current page must keep its indicator checked.
                checked: rail.currentPage === modelData.page
                onClicked: rail.pageRequested(modelData.page)
                ToolTip.visible: hovered || activeFocus
                ToolTip.text: modelData.label
                contentItem: Item {
                    SentinelIcon {
                        anchors.centerIn: parent
                        name: navigationButton.modelData.icon
                        source: "qrc:/icons/tabler/" + name + ".svg"
                        iconSize: 20
                        tint: navigationButton.checked ? SentinelTheme.accent : SentinelTheme.textMuted
                    }
                }
                background: Rectangle {
                    radius: SentinelTheme.radiusMd
                    color: navigationButton.checked || navigationButton.hovered ? SentinelTheme.withAlpha(SentinelTheme.accent, 0.12) : "transparent"
                    border.width: navigationButton.activeFocus ? 2 : 0
                    border.color: SentinelTheme.accent
                }
            }
        }
        Item { Layout.fillHeight: true }
        Button {
            id: connectionButton
            objectName: "daemonStatusButton"
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            focusPolicy: Qt.StrongFocus
            hoverEnabled: true
            Accessible.name: qsTr("Daemon connection details")
            Accessible.description: rail.connectionStatus + "; " + rail.runtimeStatus
            onClicked: connectionDetails.open()
            contentItem: Item {
                Rectangle {
                    anchors.centerIn: parent
                    width: 8; height: 8; radius: 4
                    color: /loading|connecting|starting|waiting/i.test(rail.connectionStatus) ? SentinelTheme.warning
                : rail.connected ? SentinelTheme.success : SentinelTheme.error
                }
            }
            background: Rectangle {
                radius: SentinelTheme.radiusMd
                color: connectionButton.hovered ? SentinelTheme.surfaceHover : "transparent"
                border.width: connectionButton.activeFocus ? 2 : 0
                border.color: SentinelTheme.accent
            }
            ToolTip.visible: hovered || activeFocus
            ToolTip.text: qsTr("Daemon: %1").arg(rail.connectionStatus || qsTr("Unknown"))
            Popup {
                id: connectionDetails
                objectName: "daemonStatusPopup"
                onOpened: connectionCloseButton.forceActiveFocus(Qt.PopupFocusReason)
                parent: Overlay.overlay
                x: rail.width + 8
                y: Math.max(8, parent.height - height - 64)
                width: Math.min(360, parent.width - rail.width - 24)
                padding: 16
                modal: true
                focus: true
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                onClosed: connectionButton.forceActiveFocus(Qt.PopupFocusReason)
                background: Rectangle { color: SentinelTheme.backgroundRaised; radius: SentinelTheme.radiusMd; border.color: SentinelTheme.textMuted }
                contentItem: ColumnLayout {
                    spacing: 8
                    Label { Layout.fillWidth: true; text: qsTr("Daemon: %1").arg(rail.connectionStatus || qsTr("Unknown")); wrapMode: Text.WordWrap; color: SentinelTheme.textPrimary }
                    Label { Layout.fillWidth: true; text: qsTr("Provider / model: %1").arg(rail.runtimeStatus || qsTr("Not verified")); wrapMode: Text.WordWrap; color: SentinelTheme.textPrimary }
                    Label { Layout.fillWidth: true; text: qsTr("A connected daemon does not mean a model is ready for inference."); wrapMode: Text.WordWrap; color: SentinelTheme.textMuted }
                    SentinelButton { id: connectionCloseButton; text: qsTr("Close"); onClicked: connectionDetails.close() }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.12)
        }
        Button {
            id: settingsButton
            objectName: "settingsNavigationButton"
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            focusPolicy: Qt.StrongFocus
            hoverEnabled: true
            Accessible.name: qsTr("Settings")
            onClicked: rail.pageRequested("Settings")
            ToolTip.visible: hovered || activeFocus
            ToolTip.text: qsTr("Settings")
            contentItem: Item {
                SentinelIcon {
                    anchors.centerIn: parent
                    name: "settings"
                    source: "qrc:/icons/tabler/settings.svg"
                    iconSize: 20
                    tint: rail.currentPage === "Settings" ? SentinelTheme.accent : SentinelTheme.textMuted
                }
            }
            background: Rectangle {
                radius: SentinelTheme.radiusMd
                color: rail.currentPage === "Settings" || settingsButton.hovered ? SentinelTheme.withAlpha(SentinelTheme.accent, 0.12) : "transparent"
                border.width: settingsButton.activeFocus ? 2 : 0
                border.color: SentinelTheme.accent
            }
        }
    }
}
