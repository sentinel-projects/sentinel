// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Sentinel.Desktop

Item {
    id: page
    required property var viewModel
    readonly property bool compact: width < 900
    property bool showingDetail: false
    property bool sidebarOpen: true

    onVisibleChanged: viewModel.active = visible
    Component.onCompleted: viewModel.active = visible

    function stamp(value) {
        return value && value.toLocaleString ? value.toLocaleString(Qt.locale(), Locale.ShortFormat) : ""
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: SentinelTheme.spaceMd

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("Agent Inspector")
                color: SentinelTheme.textPrimary
                font.pixelSize: 24
                font.bold: true
                Layout.fillWidth: true
            }
            SentinelButton {
                text: page.sidebarOpen ? qsTr("Hide history") : qsTr("Show history")
                onClicked: page.sidebarOpen = !page.sidebarOpen
            }
            Button {
                text: qsTr("Refresh")
                onClicked: page.viewModel.refresh()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Refresh run history")
            }
        }

        Text {
            Layout.fillWidth: true
            visible: page.viewModel.errorMessage.length > 0
            text: page.viewModel.errorMessage
            color: "#c94c4c"
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: SentinelTheme.spaceSm
            ComboBox {
                id: stateFilter
                Layout.preferredWidth: 145
                model: [qsTr("All states"), "Completed", "Failed", "Cancelled", "Interrupted"]
                onCurrentIndexChanged: page.viewModel.stateFilter = currentIndex === 0 ? "" : currentText
            }
            ComboBox {
                id: typeFilter
                Layout.preferredWidth: 155
                model: [qsTr("All types"), qsTr("Interactive Agent"), qsTr("Controlled Task"), qsTr("Subagent")]
                onCurrentIndexChanged: page.viewModel.typeFilter = currentIndex === 1 ? "interactive" : currentIndex === 2 ? "controlled-task" : currentIndex === 3 ? "subagent" : ""
            }
            TextField {
                Layout.fillWidth: true
                placeholderText: qsTr("Provider or model")
                onTextChanged: page.viewModel.providerFilter = text
            }
            TextField {
                Layout.fillWidth: true
                visible: !page.compact
                placeholderText: qsTr("Search summaries")
                onTextChanged: page.viewModel.searchText = text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 0
            spacing: SentinelTheme.spaceMd

            Rectangle {
                Layout.fillHeight: true
                Layout.fillWidth: page.compact
                Layout.preferredWidth: page.compact ? -1 : 330
                visible: page.sidebarOpen && (!page.compact || !page.showingDetail)
                color: SentinelTheme.backgroundRaised
                radius: 6
                border.color: SentinelTheme.accentBorderSubtle

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: SentinelTheme.spaceSm
                    spacing: SentinelTheme.spaceSm
                    Text {
                        text: qsTr("Recent runs")
                        color: SentinelTheme.textMuted
                        font.bold: true
                    }
                    ListView {
                        id: runList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 4
                        model: page.viewModel.runs
                        delegate: ItemDelegate {
        id: delegateScope1
                            objectName: "inspectorRun-" + entry.runId
                            required property var entry
                            width: runList.width
                            height: 94
                            highlighted: page.viewModel.selectedRunId === entry.runId
                            onClicked: {
                                page.viewModel.selectRun(entry.runId)
                                page.showingDetail = true
                            }
                            contentItem: Column {
                                spacing: 4
                                Text { text: delegateScope1.entry.goal || qsTr("Agent run"); color: SentinelTheme.textPrimary; width: parent.width; elide: Text.ElideRight; font.bold: true }
                                Text { text: (delegateScope1.entry.completionStatus || delegateScope1.entry.state) + "  /  " + delegateScope1.entry.runType; color: SentinelTheme.textMuted; width: parent.width; elide: Text.ElideRight }
                                Text { text: delegateScope1.entry.provider + " / " + delegateScope1.entry.model; color: SentinelTheme.textMuted; width: parent.width; elide: Text.ElideRight }
                                Text { text: page.stamp(delegateScope1.entry.startedAt) + "  " + delegateScope1.entry.duration + "  " + delegateScope1.entry.stepCount + qsTr(" steps"); color: SentinelTheme.textMuted; width: parent.width; elide: Text.ElideRight }
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: runList.count === 0 && page.viewModel.errorMessage.length === 0
                        text: qsTr("No runs in this view")
                        color: SentinelTheme.textMuted
                        horizontalAlignment: Text.AlignHCenter
                    }
                    Button {
                        Layout.fillWidth: true
                        visible: page.viewModel.hasMore
                        text: qsTr("Load more")
                        onClicked: page.viewModel.loadMore()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: !page.sidebarOpen || !page.compact || page.showingDetail
                color: SentinelTheme.backgroundRaised
                radius: 6
                border.color: SentinelTheme.accentBorderSubtle

                ScrollView {
                    anchors.fill: parent
                    anchors.margins: SentinelTheme.spaceLg
                    clip: true
                    contentWidth: availableWidth

                    ColumnLayout {
                        width: parent.width
                        spacing: SentinelTheme.spaceMd
                        Button {
                            visible: page.compact
                            text: qsTr("Back to runs")
                            onClicked: page.showingDetail = false
                        }
                        Text {
                            visible: page.viewModel.selectedRunId.length === 0
                            text: qsTr("Select a run")
                            color: SentinelTheme.textMuted
                        }
                        ColumnLayout {
                            visible: page.viewModel.selectedRunId.length > 0
                            Layout.fillWidth: true
                            spacing: SentinelTheme.spaceMd
                            property var run: page.viewModel.selectedRun

                            Text { text: parent.run.goal || qsTr("Agent run"); color: SentinelTheme.textPrimary; font.pixelSize: 20; font.bold: true; Layout.fillWidth: true; wrapMode: Text.Wrap }
                            Text { objectName: "inspectorRunState"; text: parent.run.runType + (parent.run.role ? "  /  " + parent.run.role : "") + "  /  " + (parent.run.completionStatus || parent.run.state); color: SentinelTheme.accent; font.bold: true }
                            Text { text: qsTr("Run ID: ") + parent.run.runId + "\n" + qsTr("Session: ") + parent.run.sessionId; color: SentinelTheme.textMuted; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.pixelSize: 12 }
                            Text { text: parent.run.provider + " / " + parent.run.model + "\n" + page.stamp(parent.run.startedAt) + " - " + page.stamp(parent.run.finishedAt) + "  " + parent.run.duration; color: SentinelTheme.textPrimary; Layout.fillWidth: true; wrapMode: Text.Wrap }
                            Text { text: qsTr("Current catalog: ") + (parent.run.providerCatalogStatus || qsTr("Unverified")) + (parent.run.providerCatalogDetail ? "  /  " + parent.run.providerCatalogDetail : ""); color: SentinelTheme.textMuted; Layout.fillWidth: true; wrapMode: Text.Wrap }
                            Text { text: qsTr("Context: %1 tokens, %2 items, %3 omitted%4").arg(parent.run.contextTokens).arg(parent.run.contextItems).arg(parent.run.contextOmitted).arg(parent.run.contextCompacted ? qsTr(", compacted") : ""); color: SentinelTheme.textMuted; Layout.fillWidth: true; wrapMode: Text.Wrap }
                            Text { text: parent.run.capabilitySnapshot || ""; visible: text.length > 0; color: SentinelTheme.textMuted; Layout.fillWidth: true; wrapMode: Text.Wrap }
                            Text { text: qsTr("Grounding: ") + (parent.run.grounding || qsTr("Not recorded")); color: SentinelTheme.textMuted }
                            Button {
                                visible: !!(parent.run.parentRunId && parent.run.parentRunId.length > 0)
                                text: qsTr("Parent run")
                                onClicked: page.viewModel.selectRun(parent.run.parentRunId)
                            }
                            Text { text: qsTr("Child runs"); color: SentinelTheme.textPrimary; font.bold: true }
                            Repeater {
                                model: page.viewModel.children
                                delegate: Button {
                                    required property var entry
                                    Layout.fillWidth: true
                                    text: entry.goal || entry.runId
                                    onClicked: page.viewModel.selectRun(entry.runId)
                                }
                            }
                            Text { text: qsTr("Timeline"); color: SentinelTheme.textPrimary; font.bold: true }
                            Repeater {
                                model: page.viewModel.timeline
                                delegate: ColumnLayout {
        id: delegateScope2
                                    required property var entry
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text { text: delegateScope2.entry.kind + "  /  " + delegateScope2.entry.title + (delegateScope2.entry.duration ? "  " + delegateScope2.entry.duration : ""); color: SentinelTheme.textPrimary; font.bold: true; Layout.fillWidth: true; wrapMode: Text.Wrap }
                                    Text { text: delegateScope2.entry.detail || ""; color: SentinelTheme.textMuted; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; visible: text.length > 0 }
                                    Text { text: [delegateScope2.entry.batchId ? qsTr("Batch %1").arg(delegateScope2.entry.batchId.slice(0, 8)) : "", delegateScope2.entry.source, delegateScope2.entry.observation, delegateScope2.entry.observationKind !== undefined ? qsTr("Observation %1").arg(delegateScope2.entry.observationKind) : "", delegateScope2.entry.failure, delegateScope2.entry.mutation, delegateScope2.entry.sandbox, delegateScope2.entry.toolCallId].filter(Boolean).join("  /  "); color: SentinelTheme.textMuted; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; visible: text.length > 0 }
                                }
                            }
                            Text { text: qsTr("Approvals"); color: SentinelTheme.textPrimary; font.bold: true }
                            Repeater {
                                model: page.viewModel.approvals
                                delegate: Text {
                                    required property var entry
                                    Layout.fillWidth: true
                                    text: entry.decision + "  /  " + entry.domain + "  /  " + entry.access + "  /  " + entry.scope + "\n" + entry.resource
                                    color: SentinelTheme.textMuted
                                    wrapMode: Text.WrapAnywhere
                                }
                            }
                            Text { text: qsTr("Evidence and claims"); color: SentinelTheme.textPrimary; font.bold: true }
                            Repeater {
                                model: page.viewModel.evidence
                                delegate: Text {
                                    required property var entry
                                    Layout.fillWidth: true
                                    text: entry.domain + "  /  " + entry.outcome + "  /  " + entry.freshness + "\n" + entry.resource + "  " + entry.toolCallId
                                    color: SentinelTheme.textMuted
                                    wrapMode: Text.WrapAnywhere
                                }
                            }
                            Repeater {
                                model: page.viewModel.claims
                                delegate: Text {
                                    required property var entry
                                    Layout.fillWidth: true
                                    text: entry.type + "  /  " + entry.verdict + "  /  " + entry.assertion + "\n" + entry.resource + "  " + entry.support
                                    color: SentinelTheme.textMuted
                                    wrapMode: Text.WrapAnywhere
                                }
                            }
                            Text { text: qsTr("Final answer"); color: SentinelTheme.textPrimary; font.bold: true; visible: !!parent.run.answer }
                            Text { text: parent.run.answer || ""; color: SentinelTheme.textPrimary; Layout.fillWidth: true; wrapMode: Text.Wrap; visible: text.length > 0 }
                            Text { text: qsTr("Failure: ") + parent.run.failure; color: "#c94c4c"; Layout.fillWidth: true; wrapMode: Text.Wrap; visible: !!parent.run.failure }
                            Text {
                                text: qsTr("Provider failure: ") + parent.run.providerErrorCategory
                                      + (parent.run.providerHttpStatus ? qsTr("  /  HTTP: ") + parent.run.providerHttpStatus : "")
                                      + qsTr("  /  Attempts: ") + parent.run.providerAttempts
                                      + qsTr("  /  Retry: ") + (parent.run.providerRetryOccurred ? qsTr("yes") : qsTr("no"))
                                color: "#c94c4c"
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                visible: !!parent.run.providerErrorCategory
                            }
                        }
                    }
                }
            }
        }
    }
}
