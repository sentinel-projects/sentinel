// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Effects
import QtQuick.Layouts
import Sentinel.Desktop

ShellPanel {
    id: homeChat
    signal voiceSettingsRequested()
    required property var viewModel
    property bool compact: width < 760
    readonly property bool inChatMode: (viewModel.conversationHistoryMessageCount > 0)
                                       || sendBusy
    property color modeAccent: SentinelTheme.modeAccent(viewModel.currentModeName)
    readonly property bool chatReady: viewModel.localChatSendAvailable
    readonly property bool canSend: viewModel.localChatSendAvailable
    readonly property string sendState: viewModel.chatSendLifecycleState
    readonly property bool agentBusy: viewModel.agentLoopActive
    readonly property bool agentAwaitingApproval: viewModel.agentAwaitingApproval
    readonly property bool sendBusy: sendState === "queued" || sendState === "validating" || sendState === "sending"
                                     || sendState === "streaming" || agentBusy
    readonly property bool streamingActive: sendState === "streaming"
    property string pendingSendDraft: ""
    onSendStateChanged: {
        if (sendState === "failed" && pendingSendDraft.length > 0) {
            var composer = promptInput
            if (composer.text.length === 0)
                composer.text = pendingSendDraft
            pendingSendDraft = ""
        } else if (sendState === "completed" || sendState === "streaming" || sendState === "running") {
            pendingSendDraft = ""
        }
    }
    property int editTargetMessageId: 0
    property string editConversationId: ""
    readonly property string uiSelfCheck: "chat-scroll-safe-area composer-visible no-bridge-duplication"
    readonly property string disabledReason: viewModel.activeConversationArchived
                                             ? viewModel.activeConversationStateSummary
                                             : viewModel.localChatSendAvailabilitySummary
    readonly property bool sidebarEffectiveOpen: conversationSidebarOpen && width >= 900
    readonly property bool isLMStudio: homeChat.viewModel.selectedRuntimeProvider === "lm-studio"
    readonly property bool isLlamaCpp: homeChat.viewModel.selectedRuntimeProvider === "llama-cpp-server"
    readonly property bool isCloud: {
        var p = homeChat.viewModel.selectedRuntimeProvider
        return p === "cloud-api" || p === "openai" || p === "claude" || p === "gemini" || p === "deepseek" || p === "groq" || p === "mistral"
    }
    readonly property string localProviderLabel: isCloud ? "Cloud API" : (isLMStudio ? "LM Studio" : (isLlamaCpp ? "llama.cpp" : "Ollama"))
    property bool conversationSidebarOpen: true
    property string conversationFilter: ""
    property string sidebarView: "recent"
    property string deleteRequestError: ""
    property bool deleteRequestPending: false

    Connections {
        target: homeChat.viewModel
        ignoreUnknownSignals: true
        function onConversationDeleteCompleted(conversationId, succeeded, summary) {
            if (!homeChat.deleteRequestPending || conversationId !== homeChat.pendingDeleteConversationId) return
            homeChat.deleteRequestPending = false
            if (!succeeded) {
                homeChat.deleteRequestError = summary
                return
            }
            deleteConfirmDialog.close()
            homeChat.pendingDeleteConversationId = ""
            homeChat.pendingDeleteConversationTitle = ""
        }
    }
    property string pendingDeleteConversationId: ""
    property string pendingDeleteConversationTitle: ""   // "recent" | "pinned" | "archived"
    readonly property real resolutionScale: Math.max(0.7, Math.min(1.4, homeChat.height / 860.0))
    property bool showSuggestions: true
    property bool _suggestionsDismissed: false
    property var activeGreeting: ({ title: qsTr("Welcome to Sentinel") })

    Timer {
        id: dismissSuggestionsTimer
        interval: 5000
        running: homeChat.showSuggestions && !homeChat._suggestionsDismissed
        onTriggered: homeChat.showSuggestions = false
    }

    onShowSuggestionsChanged: {
        if (showSuggestions) {
            _suggestionsDismissed = false
        }
    }

    radius: SentinelTheme.radiusPanel
    color: "transparent"

    Connections {
        target: homeChat.viewModel
        function onVoiceTranscriptionCompleted(transcript) {
            if (!homeChat.visible) return
            promptInput.text = promptInput.text.length > 0 ? promptInput.text + " " + transcript : transcript
            promptInput.forceActiveFocus()
        }

        function onAppLanguageChanged() {
            homeChat.selectRandomGreeting();
            if (typeof suggestionGrid !== "undefined" && suggestionGrid)
                suggestionGrid.updateSuggestions();
        }
    }
    border.color: "transparent"

    function selectRandomGreeting() {
        const hour = new Date().getHours();
        activeGreeting = { title: hour >= 6 && hour < 12 ? qsTr("Good Morning, Operator")
            : hour >= 12 && hour < 18 ? qsTr("Good Afternoon, Operator")
            : hour >= 18 && hour < 22 ? qsTr("Good Evening, Operator") : qsTr("Good Night, Operator") };
    }

    function formatAttachmentSummary(summary) {
        if (!summary) return "";
        var parts = summary.split(" / ");
        if (parts.length < 3) return summary;

        var fileName = parts[0];
        var fileType = parts[1];
        var sizeBytes = parseInt(parts[2]);

        var sizeStr = "";
        if (sizeBytes < 1024) {
            sizeStr = sizeBytes + " B";
        } else if (sizeBytes < 1024 * 1024) {
            sizeStr = (sizeBytes / 1024).toFixed(1) + " KB";
        } else {
            sizeStr = (sizeBytes / (1024 * 1024)).toFixed(1) + " MB";
        }

        return fileName + " (" + sizeStr + ")";
    }

    function getAttachmentIcon(summary) {
        if (!summary) return "paperclip";
        var parts = summary.split(" / ");
        if (parts.length < 2) return "paperclip";
        var fileType = parts[1].toLowerCase();

        if (fileType === "image") {
            return "photo";
        } else if (fileType === "pdf" || fileType === "docx") {
            return "file-text";
        } else if (fileType === "source code") {
            return "code";
        } else {
            return "paperclip";
        }
    }

    function scrollToLatest(force) {
        if (force || recentMessages.followNewMessages)
            Qt.callLater(recentMessages.positionViewAtEnd)
    }

    // Coalesces conversation model refreshes: a single data change updates all
    // conversation arrays, so rebuild the ListView model at most once per event
    // loop iteration instead of once per changed array.
    function refreshConversationList() {
        Qt.callLater(function() {
            conversationList.model = filteredConversationIndexes(sidebarView)
        })
    }

    function focusComposer() {
        promptInput.forceActiveFocus()
    }

    function restoreDraft(text) {
        promptInput.text = text
        promptInput.forceActiveFocus()
    }

    function sendComposerText() {
        var prompt = promptInput.text.trim()
        if (prompt.length === 0 || !homeChat.canSend
                || (homeChat.sendBusy && !homeChat.agentAwaitingApproval))
            return
        homeChat.pendingSendDraft = promptInput.text
        var accepted = false
        if (homeChat.editTargetMessageId > 0
                && homeChat.editConversationId === homeChat.viewModel.activeConversationId) {
            accepted = homeChat.viewModel.editAndResendChatMessage(
                homeChat.editTargetMessageId, promptInput.text) !== ""
        } else {
            accepted = homeChat.viewModel.sendMessage(promptInput.text)
        }
        if (accepted) {
            promptInput.clear()
            homeChat.editTargetMessageId = 0
            homeChat.editConversationId = ""
            recentMessages.followNewMessages = true
            homeChat.scrollToLatest(true)
        } else {
            homeChat.pendingSendDraft = ""
        }
    }

    // Returns true if the conversation at the given conversationIds index is pinned
    function isPinned(index) {
        return viewModel.conversationPinnedSummaries[index] === "Pinned"
    }

    // Returns true if the conversation at the given conversationIds index is archived
    function isArchived(index) {
        return viewModel.conversationArchivedSummaries[index] === "Archived"
    }

    // Returns indexes of conversations matching the current view/filter
    // view: "recent" | "pinned" | "archived"
    function filteredConversationIndexes(view) {
        var normalized = conversationFilter.trim().toLowerCase()
        var result = []
        for (var i = 0; i < viewModel.conversationIds.length; ++i) {
            var archived = isArchived(i)
            var pinned = isPinned(i)
            if (view === "pinned" && !pinned) continue
            if (view === "archived" && !archived) continue
            if (view === "recent" && (pinned || archived)) continue
            var title = viewModel.conversationTitles[i] ?? ""
            var id = viewModel.conversationIds[i] ?? ""
            if (normalized.length === 0
                    || title.toLowerCase().indexOf(normalized) >= 0
                    || id.toLowerCase().indexOf(normalized) >= 0) {
                result.push(i)
            }
        }
        return result
    }

    onStreamingActiveChanged: {
        if (!streamingActive)
            scrollToLatest(true)
    }

    onSidebarViewChanged: {
        conversationList.model = filteredConversationIndexes(sidebarView)
    }

    onConversationFilterChanged: {
        conversationList.model = filteredConversationIndexes(sidebarView)
    }

    onInChatModeChanged: {
        if (!inChatMode) {
            conversationSidebarOpen = false
            selectRandomGreeting();
        }
    }

    Component.onCompleted: {
        if (!inChatMode) {
            conversationSidebarOpen = false
        }
        selectRandomGreeting();
        suggestionGrid.shuffleSuggestions();
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 0
        spacing: SentinelTheme.spaceMd

        ShellPanel {
            id: conversationRail
            readonly property real expandedWidth: homeChat.width < 1000 ? 210 : 260
            // Collapsed rail leaves no stub behind: the chat surface owns the
            // full width, and the rail only exists while it is opened.
            Layout.minimumWidth: 0
            Layout.maximumWidth: Layout.preferredWidth
            Layout.preferredWidth: homeChat.sidebarEffectiveOpen ? expandedWidth : 0
            visible: homeChat.sidebarEffectiveOpen
            Layout.fillHeight: true
            Layout.minimumHeight: 0
            color: SentinelTheme.backgroundBase
            border.color: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.05)
            showBrackets: false
            clip: true

            Behavior on Layout.preferredWidth {
                NumberAnimation {
                    duration: MotionTokens.duration(MotionTokens.normal, homeChat.viewModel.currentModeName)
                    easing.type: MotionTokens.enter
                }
            }

            // Collapsed-state controls (New Chat + sidebar toggle) live in the
            // chat surface header so the rail itself needs no collapsed stub.

            // Preserve the full layout while the clipped rail animates.
            ColumnLayout {
                width: conversationRail.expandedWidth - 2 * SentinelTheme.spaceSm
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.margins: SentinelTheme.spaceSm
                spacing: SentinelTheme.spaceSm
                visible: homeChat.sidebarEffectiveOpen && conversationRail.width >= conversationRail.expandedWidth - 1

                // Top bar: New Chat icon + Search field (with search icon inside) + Hide icon
                RowLayout {
                    Layout.fillWidth: true
                    spacing: SentinelTheme.spaceXs

                    // New Chat icon button
                    Button {
                        id: newChatIconBtn
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 30
                        hoverEnabled: true
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("New Chat")
                    Accessible.name: ToolTip.text
                        onClicked: {
                            homeChat.viewModel.createConversation("")
homeChat.conversationSidebarOpen = !homeChat.compact
                            promptInput.clear()
                            promptInput.clear()
                        }
                        contentItem: Text {
                            text: "+"
                            color: SentinelTheme.textPrimary
                            font.pixelSize: 18
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: SentinelTheme.radiusMd
                            color: InteractionTokens.surfaceColor(newChatIconBtn.hovered, newChatIconBtn.down, false, homeChat.modeAccent)
                            border.color: InteractionTokens.borderColor(newChatIconBtn.activeFocus, newChatIconBtn.hovered, false, homeChat.modeAccent)
                        }
                    }

                    // Search field with embedded search icon on the right
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 30
                        radius: SentinelTheme.radiusMd
                        color: conversationSearch.activeFocus
                               ? SentinelTheme.withAlpha(SentinelTheme.backgroundBase, 0.72)
                               : SentinelTheme.withAlpha(SentinelTheme.backgroundBase, 0.48)
                        border.color: conversationSearch.activeFocus
                                      ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.55)
                                      : SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.10)
                        layer.enabled: GraphicsInfo.api !== GraphicsInfo.Software && conversationSearch.activeFocus
                        layer.effect: MultiEffect {
                            shadowEnabled: true
                            shadowColor: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.12)
                            shadowVerticalOffset: 1
                            shadowBlur: 0.08
                            shadowOpacity: 1.0
                        }

                        TextInput {
                            id: conversationSearch
                            anchors.left: parent.left
                            anchors.right: searchIconBtn.left
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: SentinelTheme.spaceSm
                            anchors.rightMargin: SentinelTheme.spaceXs
                            text: homeChat.conversationFilter
                            onTextChanged: homeChat.conversationFilter = text
                            color: SentinelTheme.textPrimary
                            font.pixelSize: SentinelTheme.fontSmall
                            selectionColor: SentinelTheme.withAlpha(homeChat.modeAccent, 0.34)
                            selectedTextColor: SentinelTheme.textPrimary
                            clip: true

                            Text {
                                anchors.fill: parent
                                text: qsTr("Search")
                                color: SentinelTheme.textPlaceholder
                                font.pixelSize: SentinelTheme.fontSmall
                                verticalAlignment: Text.AlignVCenter
                                visible: conversationSearch.text.length === 0 && !conversationSearch.activeFocus
                            }
                        }

                        // Search icon button (right side of field)
                        Button {
                            id: searchIconBtn
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.rightMargin: 3
                            width: 24
                            height: 24
                            hoverEnabled: true
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Search")
                            enabled: conversationSearch.text.trim().length > 0
                            onClicked: homeChat.viewModel.searchConversation(conversationSearch.text)
                            contentItem: TablerGlyph {
                                text: "search"
                                font.pixelSize: 11
                                color: searchIconBtn.enabled ? SentinelTheme.textMuted : SentinelTheme.textPlaceholder
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: SentinelTheme.radiusSm
                                color: searchIconBtn.hovered ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.12) : "transparent"
                            }
                        }
                    }

                    // Hide icon button (compact, right side of sidebar)
                    Button {
                        id: sidebarToggleOpen
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 30
                        hoverEnabled: true
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Hide sidebar")
                    Accessible.name: ToolTip.text
                        onClicked: homeChat.conversationSidebarOpen = false
                        contentItem: TablerGlyph {
                            text: "x"
                            color: SentinelTheme.textMuted
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: SentinelTheme.radiusMd
                            color: InteractionTokens.surfaceColor(sidebarToggleOpen.hovered, sidebarToggleOpen.down, false, homeChat.modeAccent)
                            border.color: InteractionTokens.borderColor(sidebarToggleOpen.activeFocus, sidebarToggleOpen.hovered, false, homeChat.modeAccent)
                        }
                    }
                }

                // Section tabs: Recent | Pinned | Archived
                RowLayout {
                    Layout.fillWidth: true
                    spacing: SentinelTheme.spaceXs

                    Button {
                        id: recentTabBtn
                        Layout.fillWidth: true
                        Layout.preferredHeight: 26
                        hoverEnabled: true
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Recent chats")
                        onClicked: homeChat.sidebarView = "recent"
                        contentItem: Text {
                            text: qsTr("Recent")
                            color: homeChat.sidebarView === "recent" ? homeChat.modeAccent : SentinelTheme.textMuted
                            font.pixelSize: SentinelTheme.fontTiny
                            font.bold: homeChat.sidebarView === "recent"
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: SentinelTheme.radiusSm
                            color: homeChat.sidebarView === "recent"
                                   ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.12)
                                   : (recentTabBtn.hovered ? SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.06) : "transparent")
                            border.color: homeChat.sidebarView === "recent"
                                          ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.28)
                                          : "transparent"
                        }
                    }

                    Button {
                        id: pinnedTabBtn
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 26
                        hoverEnabled: true
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Pinned chats")
                        onClicked: homeChat.sidebarView = (homeChat.sidebarView === "pinned" ? "recent" : "pinned")
                        contentItem: TablerGlyph {
                            text: "pin"
                            font.pixelSize: 12
                            color: homeChat.sidebarView === "pinned" ? homeChat.modeAccent : SentinelTheme.textMuted
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: SentinelTheme.radiusSm
                            color: homeChat.sidebarView === "pinned"
                                   ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.12)
                                   : (pinnedTabBtn.hovered ? SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.06) : "transparent")
                            border.color: homeChat.sidebarView === "pinned"
                                          ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.28)
                                          : "transparent"
                        }
                    }

                    Button {
                        id: archivedTabBtn
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 26
                        hoverEnabled: true
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Archived chats")
                        onClicked: homeChat.sidebarView = (homeChat.sidebarView === "archived" ? "recent" : "archived")
                        contentItem: TablerGlyph {
                            text: "database"
                            font.pixelSize: 12
                            color: homeChat.sidebarView === "archived" ? homeChat.modeAccent : SentinelTheme.textMuted
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: SentinelTheme.radiusSm
                            color: homeChat.sidebarView === "archived"
                                   ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.12)
                                   : (archivedTabBtn.hovered ? SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.06) : "transparent")
                            border.color: homeChat.sidebarView === "archived"
                                          ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.28)
                                          : "transparent"
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: homeChat.viewModel.conversationDeleteLastStatus === "Failed" || homeChat.viewModel.conversationDeleteLastStatus === "Refused"
                    text: homeChat.viewModel.conversationDeleteLastResultSummary
                    color: SentinelTheme.warning
                    wrapMode: Text.WordWrap
                }
                // Unified conversation list (Recent / Pinned / Archived views)
                ListView {
                    id: conversationList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 100
                    clip: true
                    spacing: SentinelTheme.spaceXs
                    // Re-evaluate model when view changes or conversation data changes
                    model: homeChat.filteredConversationIndexes(homeChat.sidebarView)

                    // Force refresh when sidebarView tab changes
                    onVisibleChanged: {
                        if (visible)
                            model = homeChat.filteredConversationIndexes(homeChat.sidebarView)
                    }

                    // Force refresh when underlying data changes
                    Connections {
                        target: homeChat.viewModel
                        function onChatMessagesChanged() { homeChat.refreshConversationList() }
                    }

                    delegate: Item {
                        id: convItem
                        required property int modelData   // this is the sourceIndex into conversationIds
                        readonly property string convId: {
                            var ids = homeChat.viewModel.conversationIds
                            return (modelData >= 0 && modelData < ids.length) ? ids[modelData] : ""
                        }
                        readonly property string convTitle: {
                            var titles = homeChat.viewModel.conversationTitles
                            return (modelData >= 0 && modelData < titles.length) ? titles[modelData] : ""
                        }
                        readonly property bool active: convId === homeChat.viewModel.activeConversationId
                        readonly property bool pinned: homeChat.isPinned(modelData)
                        readonly property bool archived: homeChat.isArchived(modelData)
                        width: ListView.view.width
                        height: 38

                        // Background button for selecting conversation (left side)
                        Button {
                            id: convItemBtn
                            anchors.left: parent.left
                            anchors.right: convItemMenuBtn.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.rightMargin: 2
                            hoverEnabled: true
                            onClicked: {
                                homeChat.viewModel.switchConversation(convItem.convId)
if (homeChat.compact)
                                    homeChat.conversationSidebarOpen = false
                                homeChat.scrollToLatest(true)
                            }

                            background: Rectangle {
                                radius: SentinelTheme.radiusMd
                                color: InteractionTokens.surfaceColor(convItemBtn.hovered, convItemBtn.down, convItem.active, homeChat.modeAccent)
                                border.color: InteractionTokens.borderColor(convItemBtn.activeFocus, convItemBtn.hovered, convItem.active, homeChat.modeAccent)
                            }

                            contentItem: RowLayout {
                                spacing: 4
                                anchors.fill: parent
                                anchors.leftMargin: SentinelTheme.spaceSm
                                anchors.rightMargin: SentinelTheme.spaceXs

                                Text {
                                    Layout.fillWidth: true
                                    text: convItem.convTitle
                                    color: convItem.active ? SentinelTheme.textPrimary : SentinelTheme.textMuted
                                    font.pixelSize: SentinelTheme.fontSmall
                                    font.bold: convItem.active
                                    verticalAlignment: Text.AlignVCenter
                                    maximumLineCount: 1
                                    elide: Text.ElideRight
                                }

                                TablerGlyph {
                                    visible: convItem.pinned
                                    text: "pin"
                                    font.pixelSize: 10
                                    color: homeChat.modeAccent
                                }
                            }
                        }

                        // Overflow 3-dots Menu button (right side, on top with z: 2)
                        Button {
                            id: convItemMenuBtn
                            objectName: "chatConversationMenu-" + convItem.convId
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.rightMargin: 4
                            width: 28
                            height: 28
                            z: 2
                            hoverEnabled: true
                            opacity: (convItemBtn.hovered || convItemMenuBtn.hovered || convItemMenu.opened || convItem.active) ? 1.0 : 0.45
                            onClicked: convItemMenu.popup()

                            contentItem: Text {
                                text: "⋮"
                                color: convItemMenuBtn.hovered ? homeChat.modeAccent : SentinelTheme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: SentinelTheme.radiusSm
                                color: convItemMenuBtn.hovered ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.20) : "transparent"
                                border.color: convItemMenuBtn.hovered ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.3) : "transparent"
                                border.width: 1
                            }
                        }

                        Menu {
                            id: convItemMenu
                            width: 180

                            background: Rectangle {
                                radius: SentinelTheme.radiusMd
                                color: SentinelTheme.withAlpha(SentinelTheme.backgroundRaised, 0.94)
                                border.color: SentinelTheme.withAlpha(homeChat.modeAccent, 0.25)
                                border.width: 1
                            }

                            // Pin / Unpin
                            MenuItem {
                                text: convItem.pinned ? qsTr("Unpin") : qsTr("Pin")
                                icon.source: convItem.pinned ? "qrc:/icons/tabler/map-pin.svg" : "qrc:/icons/tabler/pin.svg"
                                onTriggered: {
                                    if (convItem.pinned)
                                        homeChat.viewModel.unpinConversation(convItem.convId)
                                    else
                                        homeChat.viewModel.pinConversation(convItem.convId)
                                }
                            }

                            // Archive / Unarchive
                            MenuItem {
                                text: convItem.archived ? qsTr("Unarchive") : qsTr("Archive")
                                icon.source: convItem.archived ? "qrc:/icons/tabler/archive.svg" : "qrc:/icons/tabler/box.svg"
                                onTriggered: {
                                    if (convItem.archived)
                                        homeChat.viewModel.unarchiveConversation(convItem.convId)
                                    else
                                        homeChat.viewModel.archiveConversation(convItem.convId)
                                }
                            }

                            // Delete — opens confirmation dialog
                            MenuSeparator {}

                            MenuItem {
                                objectName: "chatDeleteConversation"
                                text: qsTr("Delete")
                                icon.source: "qrc:/icons/tabler/trash.svg"
                                onTriggered: {
                                    homeChat.pendingDeleteConversationId = convItem.convId
                                    homeChat.pendingDeleteConversationTitle = convItem.convTitle
                                    deleteConfirmDialog.open()
                                }
                            }
                        }
                    }

                    // Empty state
                    Rectangle {
                        anchors.centerIn: parent
                        visible: conversationList.count === 0
                        implicitWidth: noChatsLabel.implicitWidth + SentinelTheme.spaceLg
                        implicitHeight: noChatsLabel.implicitHeight + SentinelTheme.spaceSm
                        radius: SentinelTheme.radiusSm
                        color: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.03)
                        border.color: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.06)
                        layer.enabled: GraphicsInfo.api !== GraphicsInfo.Software
                        layer.effect: MultiEffect {
                            shadowEnabled: true
                            shadowColor: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.08)
                            shadowVerticalOffset: 1
                            shadowBlur: 0.06
                            shadowOpacity: 1.0
                        }

                        Text {
                            id: noChatsLabel
                            anchors.centerIn: parent
                            text: homeChat.sidebarView === "pinned" ? qsTr("No pinned chats")
                                  : homeChat.sidebarView === "archived" ? qsTr("No archived chats")
                                  : qsTr("No chats yet")
                            color: SentinelTheme.textPlaceholder
                            font.pixelSize: SentinelTheme.fontSmall
                        }
                    }
                }
            }
        }

        ShellPanel {
            id: mainChatSurfacePanel
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 0
            color: SentinelTheme.backgroundBase
            border.color: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.05)
            showBrackets: false
            clip: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: homeChat.inChatMode ? SentinelTheme.spaceMd : SentinelTheme.spaceLg
                spacing: homeChat.inChatMode ? SentinelTheme.spaceMd : SentinelTheme.spaceMd * homeChat.resolutionScale

                // Chat-surface header: owns the New Chat and sidebar controls so
                // the conversation rail never needs a collapsed 44px stub.
                RowLayout {
                    id: chatHeaderBar
                    objectName: "chatHeaderBar"
                    Layout.fillWidth: true
                    spacing: SentinelTheme.spaceXs

                    Button {
                        id: headerNewChatBtn
                        objectName: "chatNewConversation"
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 30
                        hoverEnabled: true
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("New Chat")
                        Accessible.name: ToolTip.text
                        onClicked: {
                            homeChat.viewModel.createConversation("")
                            promptInput.clear()
                            if (homeChat.compact)
                                homeChat.conversationSidebarOpen = false
                        }
                        contentItem: Text {
                            text: "+"
                            color: SentinelTheme.textPrimary
                            font.pixelSize: 18
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: SentinelTheme.radiusMd
                            color: InteractionTokens.surfaceColor(headerNewChatBtn.hovered, headerNewChatBtn.down, false, homeChat.modeAccent)
                            border.color: InteractionTokens.borderColor(headerNewChatBtn.activeFocus, headerNewChatBtn.hovered, false, homeChat.modeAccent)
                        }
                    }

                    Button {
                        id: headerSidebarToggle
                        objectName: "chatSidebarToggle"
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 30
                        hoverEnabled: true
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Show sidebar")
                        Accessible.name: ToolTip.text
                        visible: !homeChat.sidebarEffectiveOpen
                        onClicked: homeChat.conversationSidebarOpen = true
                        contentItem: TablerGlyph {
                            text: "menu"
                            color: SentinelTheme.textPrimary
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: SentinelTheme.radiusMd
                            color: InteractionTokens.surfaceColor(headerSidebarToggle.hovered, headerSidebarToggle.down, false, homeChat.modeAccent)
                            border.color: InteractionTokens.borderColor(headerSidebarToggle.activeFocus, headerSidebarToggle.hovered, false, homeChat.modeAccent)
                        }
                    }

                    Item { Layout.fillWidth: true; Layout.minimumWidth: 0 }

                    Text {
                        visible: homeChat.inChatMode
                        text: homeChat.viewModel.conversationListCurrentTitle
                        color: SentinelTheme.textPrimary
                        font.pixelSize: homeChat.compact ? SentinelTheme.fontSmall : SentinelTheme.fontBody
                        font.bold: true
                        maximumLineCount: 1
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                ErrorBanner {
                    objectName: "chatSendErrorBanner"
                    Layout.fillWidth: true
                    show: homeChat.sendState === "failed"
                          && homeChat.viewModel.chatSendLifecycleSummary.length > 0
                    visible: show
                    message: homeChat.viewModel.chatSendLifecycleSummary
                }

            Item {
                id: homeCenterWrapper
                objectName: "chatGreeting"
                Layout.fillWidth: true
                Layout.fillHeight: !homeChat.inChatMode
                visible: !homeChat.inChatMode

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width, 720 * homeChat.resolutionScale)
                    spacing: SentinelTheme.spaceMd * 1.5 * homeChat.resolutionScale

                    CoreWarden {
                        id: homeWarden
                        viewModel: homeChat.viewModel
                        imageSize: homeChat.height < 650 ? 96 : 144
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignHCenter
                    }

                    Item {
                        id: greetingAreaWrapper
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignHCenter
                        implicitHeight: greetingArea.implicitHeight

                        scale: greetingMouse.containsMouse ? 1.012 : 1.0
                        Behavior on scale {
                            NumberAnimation { duration: MotionTokens.fast; easing.type: MotionTokens.enter }
                        }

                        MouseArea {
                            id: greetingMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                homeChat._suggestionsDismissed = true;
                                dismissSuggestionsTimer.stop();
                                homeChat.showSuggestions = !homeChat.showSuggestions;
                                if (homeChat.showSuggestions) {
                                    suggestionGrid.shuffleSuggestions();
                                }
                            }
                        }

                        ColumnLayout {
                            id: greetingArea
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            spacing: SentinelTheme.spaceMd * homeChat.resolutionScale

                            Text {
                                id: greetingLabel
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignHCenter
                                horizontalAlignment: Text.AlignHCenter
                                text: homeChat.activeGreeting.title
                                color: SentinelTheme.textPrimary
                                font.pixelSize: (homeChat.compact ? SentinelTheme.fontDisplay : SentinelTheme.fontHero) * homeChat.resolutionScale
                                font.bold: true
                                font.family: SentinelTheme.fontFamily
                            }

                            Text {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignHCenter
                                horizontalAlignment: Text.AlignHCenter
                                text: homeWarden.readinessSummary()
                                color: SentinelTheme.textMuted
                                font.pixelSize: (homeChat.compact ? SentinelTheme.fontBody : SentinelTheme.fontCard) * homeChat.resolutionScale
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    Item {
                        id: suggestionsWrapper
                        Layout.fillWidth: true
                        implicitHeight: homeChat.showSuggestions ? suggestionGrid.implicitHeight : 0
                        visible: opacity > 0.0
                        opacity: homeChat.showSuggestions ? 1.0 : 0.0
                        clip: true
                        Layout.bottomMargin: homeChat.showSuggestions ? (SentinelTheme.spaceSm * homeChat.resolutionScale) : 0

                        Behavior on implicitHeight {
                            NumberAnimation { duration: MotionTokens.duration(300); easing.type: Easing.InOutQuad }
                        }
                        Behavior on opacity {
                            NumberAnimation { duration: MotionTokens.duration(250); easing.type: Easing.InOutQuad }
                        }
                        Behavior on Layout.bottomMargin {
                            NumberAnimation { duration: MotionTokens.duration(300) }
                        }

                        GridLayout {
                            id: suggestionGrid
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            columns: homeChat.compact ? 1 : 2
                            rowSpacing: SentinelTheme.spaceSm * homeChat.resolutionScale
                            columnSpacing: SentinelTheme.spaceSm * homeChat.resolutionScale

                            property var suggestions: []

                            function getAllSuggestions() {
                                return [
                                    { icon: "books", title: qsTr("Özet Çıkarma"), prompt: qsTr("Eklediğim PDF belgesinin yönetici özetini ve ana hatlarını çıkar") },
                                    { icon: "chart-bar", title: qsTr("Excel Formül Asistanı"), prompt: qsTr("İki farklı sütundaki verileri karşılaştırıp eşleşmeyenleri bulan bir Excel formülü yaz") },
                                    { icon: "notes", title: qsTr("İş E-postası"), prompt: qsTr("Müşteriye veya yöneticiye durumu açıklayan kibar ve profesyonel bir e-posta taslağı yaz") },
                                    { icon: "bulb", title: qsTr("İçerik Fikirleri"), prompt: qsTr("Sosyal medya veya blog için ilgi çekici 5 içerik fikri ve başlığı bul") },
                                    { icon: "brain", title: qsTr("Basitçe Açıkla"), prompt: qsTr("Karmaşık bir kavramı veya teoriyi 10 yaşındaki birine anlatır gibi sade ve anlaşılır biçimde açıkla") },
                                    { icon: "world", title: qsTr("Dil Öğrenimi"), prompt: qsTr("Verilen bir metni İngilizceye doğal ve akıcı bir şekilde çevir ve kullanılan önemli kalıpları göster") },
                                    { icon: "calendar", title: qsTr("Haftalık Yemek Planı"), prompt: qsTr("Sağlıklı, pratik ve bütçe dostu 5 günlük akşam yemeği menüsü ve alışveriş listesi oluştur") },
                                    { icon: "plane", title: qsTr("Seyahat Rotası"), prompt: qsTr("3 günlük bir şehir gezisi (örneğin Kapadokya veya Roma) için ayrıntılı ve optimize edilmiş bir rota planla") },
                                    { icon: "gift", title: qsTr("Hediye Önerileri"), prompt: qsTr("Belirli bir bütçeye ve ilgi alanlarına göre arkadaşım/ailem için 5 özgün hediye önerisi sun") },
                                    { icon: "clock", title: qsTr("Zaman Yönetimi"), prompt: qsTr("Günün yoğun temposunu yönetmek ve ertelemeyi önlemek için Pomodoro tabanlı bir günlük plan hazırlamamda yardımcı ol") },
                                    { icon: "clipboard", title: qsTr("Toplantı Tutanakları"), prompt: qsTr("Toplantıda karışık olarak aldığım notları maddeler halinde düzenli bir toplantı özetine dönüştür") },
                                    { icon: "tool", title: qsTr("Temel Otomasyon"), prompt: qsTr("Bilgisayardaki dosya adlarını topluca değiştiren veya düzenleyen basit bir betik veya komut satırı kodu yaz") },
                                    { icon: "search", title: qsTr("Metin Düzenleme"), prompt: qsTr("Yazdığım bu metni dil bilgisi, imla ve anlatım bozuklukları açısından inceleyip daha profesyonel hale getir") },
                                    { icon: "target", title: qsTr("Hedef Planlama"), prompt: qsTr("Kişisel hedeflerimi veya bir projenin çeyreklik hedeflerini (OKR) belirlemek için bir çerçeve öner") },
                                    { icon: "chart-line", title: qsTr("Veri Analizi"), prompt: qsTr("Bir tablo veya veri setindeki önemli eğilimleri, desenleri ve anomalileri özetleyen bir rapor taslağı hazırla") },
                                    { icon: "friends", title: qsTr("Diplomatik Yanıt"), prompt: qsTr("Müşteri veya iş ortağından gelen beklenmedik bir talebe ya da gecikmeye karşı profesyonel ve yapıcı bir yanıt yaz") },
                                    { icon: "notebook", title: qsTr("Yaratıcı Yazarlık"), prompt: qsTr("Belirli bir tema etrafında ilgi çekici bir kısa hikaye başlangıcı veya yaratıcı yazı taslağı oluştur") },
                                    { icon: "speakerphone", title: qsTr("Bülten Hazırlama"), prompt: qsTr("Aylık güncellemelerimizi veya ürün lansmanımızı duyuran ilgi çekici bir e-bülten taslağı yaz") },
                                    { icon: "code", title: qsTr("Python CSV İşleme"), prompt: qsTr("İki CSV dosyasını birleştiren ve filtreleyen basit bir Python betiği yaz") },
                                    { icon: "ruler", title: qsTr("Sunum Taslağı"), prompt: qsTr("Belirli bir konuda etkileyici ve akıcı bir sunum slayt yapısı ve konuşma notları tasarla") },
                                    { icon: "bolt", title: qsTr("Klavye Kısayolları"), prompt: qsTr("İşletim sisteminde veya sık kullanılan bir uygulamada iş akışını hızlandıracak en pratik kısayolları listele") },
                                    { icon: "school", title: qsTr("Öğrenme Planı"), prompt: qsTr("Yeni bir konuyu sıfırdan öğrenmek için (örneğin temel finans veya temel fotoğrafçılık) 4 haftalık adım adım çalışma planı hazırla") },
                                    { icon: "heartbeat", title: qsTr("Sağlıklı Yaşam"), prompt: qsTr("Masa başında çalışanlar için gün içinde yapılabilecek esneme egzersizleri ve duruş düzeltme önerileri listele") },
                                    { icon: "pencil", title: qsTr("Blog Yazısı Taslağı"), prompt: qsTr("Belirli bir konuda SEO uyumlu, alt başlıkları ve giriş paragrafı hazır olan detaylı bir blog yazısı şablonu oluştur") }
                                ];
                            }

                            function shuffleSuggestions() {
                                var temp = getAllSuggestions().slice();
                                var result = [];
                                for (var i = 0; i < 4; i++) {
                                    if (temp.length === 0) break;
                                    var randIdx = Math.floor(Math.random() * temp.length);
                                    result.push(temp[randIdx]);
                                    temp.splice(randIdx, 1);
                                }
                                suggestions = result;
                            }

                            function updateSuggestions() {
                                shuffleSuggestions();
                            }

                            Component.onCompleted: {
                                shuffleSuggestions();
                            }

                            Connections {
                                target: homeChat.viewModel
                                function onAppLanguageChanged() { suggestionGrid.shuffleSuggestions() }
                            }

                            Repeater {
                                model: suggestionGrid.suggestions
                                delegate: Rectangle {
        required property var modelData
        id: delegateScope1
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 62 * homeChat.resolutionScale
                                    radius: SentinelTheme.radiusMd
                                    color: sugMouse.containsMouse
                                           ? SentinelTheme.withAlpha(SentinelTheme.backgroundRaised, 0.88)
                                           : SentinelTheme.withAlpha(SentinelTheme.backgroundRaised, 0.48)
                                    border.color: sugMouse.containsMouse
                                                  ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.28)
                                                  : SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.06)
                                    border.width: 1
                                    layer.enabled: GraphicsInfo.api !== GraphicsInfo.Software && (sugMouse.containsMouse || sugMouse.pressed)
                                    layer.effect: MultiEffect {
                                        shadowEnabled: true
                                        shadowColor: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.15)
                                        shadowVerticalOffset: 2
                                        shadowBlur: 0.10
                                        shadowOpacity: 1.0
                                    }

                                    Behavior on color { ColorAnimation { duration: MotionTokens.fast } }
                                    Behavior on border.color { ColorAnimation { duration: MotionTokens.fast } }

                                    MouseArea {
                                        id: sugMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            promptInput.text = delegateScope1.modelData.prompt
                                            promptInput.forceActiveFocus()
                                        }
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: SentinelTheme.spaceSm
                                        spacing: SentinelTheme.spaceSm

                                        TablerGlyph {
                                            text: delegateScope1.modelData.icon
                                            font.pixelSize: 22 * homeChat.resolutionScale
                                            Layout.alignment: Qt.AlignVCenter
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Layout.alignment: Qt.AlignVCenter

                                            Text {
                                                text: delegateScope1.modelData.title
                                                font.pixelSize: SentinelTheme.fontSmall
                                                font.bold: true
                                                color: SentinelTheme.textPrimary
                                            }

                                            Text {
                                                text: delegateScope1.modelData.prompt
                                                font.pixelSize: SentinelTheme.fontTiny
                                                color: SentinelTheme.textMuted
                                                elide: Text.ElideRight
                                                maximumLineCount: 1
                                                Layout.fillWidth: true
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }




                }

                CoreWarden {
                    viewModel: homeChat.viewModel
                    imageSize: 48
                    visible: homeChat.inChatMode && homeChat.sendBusy
                             && coreWardenPreferences.mode !== "off" && homeChat.height >= 600
                    Layout.alignment: Qt.AlignHCenter
                }
                RuntimeStateStrip {
                    Layout.fillWidth: true
                    visible: homeChat.inChatMode && (homeChat.sendBusy || homeChat.agentAwaitingApproval)
                    viewModel: homeChat.viewModel
                    accent: homeChat.modeAccent
                }
            }

        ListView {
            id: recentMessages
            property bool followNewMessages: true
            function nearBottom() {
                return contentHeight <= height || (contentY + height >= contentHeight - 96)
            }
            Layout.fillWidth: true
            Layout.fillHeight: homeChat.inChatMode
            Layout.minimumHeight: 0
            visible: homeChat.inChatMode
            clip: true
            cacheBuffer: 800
            spacing: homeChat.compact ? SentinelTheme.spaceSm : SentinelTheme.spaceMd
            model: homeChat.viewModel.chatMessages
            boundsBehavior: Flickable.StopAtBounds
            boundsMovement: Flickable.StopAtBounds
            maximumFlickVelocity: 2200
            flickDeceleration: 5200
            activeFocusOnTab: true
            keyNavigationWraps: true
            focusPolicy: Qt.StrongFocus
            bottomMargin: SentinelTheme.spaceXl + SentinelTheme.spaceMd
            ScrollBar.vertical: ScrollBar {
                id: recentMessagesScrollBar
                policy: ScrollBar.AsNeeded
                contentItem: Rectangle {
                    implicitWidth: 4
                    radius: 2
                    color: SentinelTheme.withAlpha(homeChat.modeAccent, recentMessagesScrollBar.active ? 0.34 : 0.18)
                }
                background: Rectangle {
                    color: "transparent"
                }
            }
            onCountChanged: homeChat.scrollToLatest(false)
            Component.onCompleted: Qt.callLater(positionViewAtEnd)
            onMovementStarted: followNewMessages = nearBottom()
            onMovementEnded: followNewMessages = nearBottom()
            onFlickEnded: followNewMessages = nearBottom()

            add: Transition {
                NumberAnimation {
                    property: "opacity"
                    from: 0.0
                    to: 1.0
                    duration: MotionTokens.duration(MotionTokens.message, homeChat.viewModel.currentModeName)
                    easing.type: MotionTokens.enter
                }
                NumberAnimation {
                    property: "scale"
                    from: 0.985
                    to: 1.0
                    duration: MotionTokens.duration(MotionTokens.message, homeChat.viewModel.currentModeName)
                    easing.type: MotionTokens.enter
                }
            }

            delegate: Rectangle {
                id: recentMessage
                objectName: messageRole === "user" ? "userMessageBubble" : messageRole === "assistant" ? "assistantMessageBubble" : "systemMessage"
                required property int index
                required property int messageId
                required property string messageRole
                required property string messageStatus
                required property int replyToMessageId
                required property string content
                required property string stateNotice
                readonly property bool displayable: messageRole !== "system"

                width: Math.min(ListView.view.width * 0.82, 800)
                anchors.right: messageRole === "user" && parent ? parent.right : undefined
                anchors.left: messageRole !== "user" && parent ? parent.left : undefined
                height: displayable ? recentMessageColumn.implicitHeight + SentinelTheme.spaceMd : 0
                visible: displayable
                radius: SentinelTheme.radiusMd
                color: messageRole === "user"
                       ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.10)
                       : SentinelTheme.panel
                border.color: SentinelTheme.withAlpha(messageRole === "user"
                                                       ? homeChat.modeAccent
                                                       : SentinelTheme.textPrimary,
                                                       messageRole === "user" ? 0.12 : 0.08)
                opacity: 1.0
                scale: 1.0
                layer.enabled: GraphicsInfo.api !== GraphicsInfo.Software && msgArea.containsMouse
                layer.effect: MultiEffect {
                    shadowEnabled: true
                    shadowColor: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.10)
                    shadowVerticalOffset: 1
                    shadowBlur: 0.08
                    shadowOpacity: 1.0
                }

                Behavior on color {
                    ColorAnimation {
                        duration: MotionTokens.normal
                        easing.type: MotionTokens.standard
                    }
                }

                MouseArea {
                    id: msgArea
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                }

                Rectangle {
                    width: 3
                    height: parent.height - SentinelTheme.spaceSm
                    radius: 2
                    anchors.left: parent.left
                    anchors.leftMargin: SentinelTheme.spaceXs
                    anchors.verticalCenter: parent.verticalCenter
                    visible: recentMessage.messageRole !== "user"
                    color: SentinelTheme.withAlpha(homeChat.modeAccent, 0.42)
                }

                ColumnLayout {
                    id: recentMessageColumn
                    x: SentinelTheme.spaceSm
                    y: SentinelTheme.spaceSm
                    width: parent.width - SentinelTheme.spaceSm * 2
                    spacing: SentinelTheme.spaceXs

                    RowLayout {
                        Layout.fillWidth: true

                        Text {
                            Layout.fillWidth: true
                            text: recentMessage.messageRole === "user" ? qsTr("You") : qsTr("Sentinel")
                            color: recentMessage.messageRole === "user"
                                   ? homeChat.modeAccent
                                   : SentinelTheme.textMuted
                            font.pixelSize: SentinelTheme.fontTiny
                            elide: Text.ElideRight
                        }

                        Button {
                            id: copyButton
                            Layout.preferredWidth: 46
                            Layout.preferredHeight: 24
                            text: qsTr("Copy")
                            hoverEnabled: true
                            focusPolicy: Qt.StrongFocus
                            onClicked: {
                                messageBody.forceActiveFocus()
                                messageBody.selectAll()
                                messageBody.copy()
                                messageBody.deselect()
                            }

                            contentItem: Text {
                                text: copyButton.text
                                color: copyButton.enabled ? SentinelTheme.textMuted : SentinelTheme.textPlaceholder
                                font.pixelSize: SentinelTheme.fontTiny
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            background: Rectangle {
                                radius: SentinelTheme.radiusSm
                                color: InteractionTokens.surfaceColor(copyButton.hovered, copyButton.down,
                                                                       copyButton.activeFocus,
                                                                       homeChat.modeAccent)
                                border.color: InteractionTokens.borderColor(copyButton.activeFocus,
                                                                             copyButton.hovered,
                                                                             false,
                                                                             homeChat.modeAccent)
                            }
                        }

                        Button {
                            id: messageMenuButton
                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 24
                            text: "⋮"
                            hoverEnabled: true
                            focusPolicy: Qt.StrongFocus
                            onClicked: messageMenu.popup()

                            contentItem: Text {
                                text: messageMenuButton.text
                                color: SentinelTheme.textMuted
                                font.pixelSize: SentinelTheme.fontSmall
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            background: Rectangle {
                                radius: SentinelTheme.radiusSm
                                color: InteractionTokens.surfaceColor(messageMenuButton.hovered,
                                                                       messageMenuButton.down,
                                                                       messageMenuButton.activeFocus,
                                                                       homeChat.modeAccent)
                                border.color: InteractionTokens.borderColor(messageMenuButton.activeFocus,
                                                                             messageMenuButton.hovered,
                                                                             false,
                                                                             homeChat.modeAccent)
                            }

                            Menu {
                                id: messageMenu
                                width: 200

                                background: Rectangle {
                                    radius: SentinelTheme.radiusMd
                                    color: SentinelTheme.withAlpha(SentinelTheme.backgroundRaised, 0.94)
                                    border.color: SentinelTheme.withAlpha(homeChat.modeAccent, 0.25)
                                    border.width: 1
                                }

                                MenuItem {
                                    text: qsTr("Edit")
                                    enabled: recentMessage.messageRole === "user"
                                    onTriggered: {
                                        promptInput.text = recentMessage.content
                                        homeChat.editTargetMessageId = recentMessage.messageId
                                        homeChat.editConversationId = homeChat.viewModel.activeConversationId
                                        promptInput.forceActiveFocus()
                                    }
                                }
                                MenuItem {
                                    text: qsTr("Regenerate")
                                    enabled: recentMessage.messageRole !== "user"
                                             && recentMessage.replyToMessageId > 0
                                             && homeChat.canSend && !homeChat.sendBusy
                                    onTriggered: homeChat.viewModel.regenerateChatResponse(
                                        recentMessage.replyToMessageId)
                                }
                                MenuItem {
                                    text: qsTr("Retry")
                                    enabled: recentMessage.messageRole !== "user"
                                             && (recentMessage.messageStatus === "failed"
                                                 || recentMessage.messageStatus === "interrupted"
                                                 || recentMessage.messageStatus === "error")
                                             && !homeChat.sendBusy
                                    onTriggered: homeChat.viewModel.retryChatResponse(
                                        recentMessage.messageId)
                                }

                                MenuItem {
                                    text: qsTr("Copy Text")
                                    onTriggered: {
                                        messageBody.forceActiveFocus()
                                        messageBody.selectAll()
                                        messageBody.copy()
                                        messageBody.deselect()
                                    }
                                }

                                MenuSeparator {}

                                MenuItem {
                                    readonly property bool convPinned: {
                                        var idx = homeChat.viewModel.conversationIds.indexOf(homeChat.viewModel.activeConversationId)
                                        return idx >= 0 && homeChat.isPinned(idx)
                                    }
                                    text: convPinned ? qsTr("Unpin conversation") : qsTr("Pin conversation")
                                    onTriggered: {
                                        var idx = homeChat.viewModel.conversationIds.indexOf(homeChat.viewModel.activeConversationId)
                                        if (idx >= 0) {
                                            if (convPinned)
                                                homeChat.viewModel.unpinConversation(homeChat.viewModel.activeConversationId)
                                            else
                                                homeChat.viewModel.pinConversation(homeChat.viewModel.activeConversationId)
                                        }
                                    }
                                }
                                MenuItem {
                                    readonly property bool convArchived: {
                                        var idx = homeChat.viewModel.conversationIds.indexOf(homeChat.viewModel.activeConversationId)
                                        return idx >= 0 && homeChat.isArchived(idx)
                                    }
                                    text: convArchived ? qsTr("Unarchive conversation") : qsTr("Archive conversation")
                                    onTriggered: {
                                        if (convArchived)
                                            homeChat.viewModel.unarchiveConversation(homeChat.viewModel.activeConversationId)
                                        else
                                            homeChat.viewModel.archiveConversation(homeChat.viewModel.activeConversationId)
                                    }
                                }

                                MenuSeparator {}

                                MenuItem {
                                    text: qsTr("Export Markdown")
                                    onTriggered: homeChat.viewModel.exportTranscript("markdown")
                                }
                                MenuItem {
                                    text: qsTr("Export TXT")
                                    onTriggered: homeChat.viewModel.exportTranscript("txt")
                                }

                                MenuSeparator {}

                                MenuItem {
                                    text: qsTr("Delete conversation")
                                    onTriggered: {
                                        homeChat.pendingDeleteConversationId = homeChat.viewModel.activeConversationId
                                        var idx = homeChat.viewModel.conversationIds.indexOf(homeChat.viewModel.activeConversationId)
                                        homeChat.pendingDeleteConversationTitle = idx >= 0 ? homeChat.viewModel.conversationTitles[idx] : homeChat.viewModel.activeConversationId
                                        deleteConfirmDialog.open()
                                    }
                                }
                            }
                        }
                    }

                    TextEdit {
                        id: messageBody
                        Layout.fillWidth: true
                        text: recentMessage.content.length > 0
                              ? recentMessage.content : recentMessage.stateNotice
                        color: SentinelTheme.textPrimary
                        font.pixelSize: SentinelTheme.fontSmall
                        wrapMode: Text.WordWrap
                        readOnly: true
                        selectByMouse: true
                        selectByKeyboard: true
                        textFormat: TextEdit.PlainText
                        selectionColor: SentinelTheme.withAlpha(homeChat.modeAccent, 0.34)
                        selectedTextColor: SentinelTheme.textPrimary
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: recentMessage.content.length > 0 && recentMessage.stateNotice.length > 0
                        text: recentMessage.stateNotice
                        color: SentinelTheme.textMuted
                        font.pixelSize: SentinelTheme.fontTiny
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        Rectangle {
            id: sharedComposer
            objectName: "chatComposer"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            radius: 20
            color: SentinelTheme.backgroundRaised
            border.width: 1
            border.color: InteractionTokens.borderColor(promptInput.activeFocus, composerHover.hovered, false, homeChat.modeAccent)
            implicitHeight: composerContent.implicitHeight + 32
            HoverHandler { id: composerHover }
            DropArea {
                anchors.fill: parent
                onDropped: function(drop) {
                    if (drop.hasUrls)
                        for (var i = 0; i < drop.urls.length; ++i)
                            homeChat.viewModel.attachFileToChat(drop.urls[i].toString())
                }
            }
            ColumnLayout {
                id: composerContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 16
                spacing: 12
                Flow {
                    Layout.fillWidth: true
                    visible: homeChat.viewModel.attachmentSummaries.length > 0
                    spacing: 6
                    Repeater {
                        model: homeChat.viewModel.attachmentSummaries
                        delegate: Label {
                            required property string modelData
                            text: homeChat.formatAttachmentSummary(modelData)
                            color: SentinelTheme.textPrimary
                            padding: 6
                            background: Rectangle { radius: 6; color: SentinelTheme.withAlpha(homeChat.modeAccent, 0.12) }
                        }
                    }
                    SentinelButton { text: qsTr("Clear attachments"); onClicked: homeChat.viewModel.clearAttachments() }
                }
                TextArea {
                    id: promptInput
                    objectName: "chatPromptInput"
                    Layout.fillWidth: true
                    Layout.minimumHeight: 64
                    Layout.maximumHeight: 160
                    placeholderText: homeChat.sendBusy ? qsTr("Sentinel is responding") : qsTr("Ask Sentinel")
                    enabled: !homeChat.viewModel.activeConversationArchived && (!homeChat.sendBusy || homeChat.agentAwaitingApproval)
                    color: SentinelTheme.textPrimary
                    placeholderTextColor: SentinelTheme.textPlaceholder
                    font.pixelSize: 16
                    wrapMode: TextEdit.WordWrap
                    selectByMouse: true
                    background: Item {}
                    onTextChanged: homeChat.viewModel.recoveryDraftText = text
                    Keys.onPressed: function(event) {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier)) {
                            homeChat.sendComposerText()
                            event.accepted = true
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    SentinelButton {
                        id: attachButton
                        objectName: "chatAttachButton"
                        text: "+"
                        tooltipText: qsTr("Add image or file")
                        Accessible.name: tooltipText
                        Layout.preferredWidth: 36
                        onClicked: attachMenu.open()
                        Menu {
                            id: attachMenu
                            y: -height
                            MenuItem { text: qsTr("Add image"); onTriggered: imageFileDialog.open() }
                            MenuItem { text: qsTr("Add file"); onTriggered: documentFileDialog.open() }
                        }
                    }
                    Repeater {
                        model: [{mode: "Chat", icon: "message-circle", label: qsTr("Chat")}, {mode: "Agent", icon: "robot", label: qsTr("Agent")}]
                        delegate: Button {
                            id: modeButton
                            objectName: "chatMode" + modelData.mode
                            required property var modelData
                            Layout.preferredWidth: 36
                            Layout.preferredHeight: 36
                            focusPolicy: Qt.StrongFocus
                            hoverEnabled: true
                            enabled: !homeChat.sendBusy
                            Accessible.name: modelData.label
                            ToolTip.visible: hovered || activeFocus
                            ToolTip.text: modelData.label
                            onClicked: homeChat.viewModel.currentModeName = modelData.mode
                            contentItem: TablerGlyph {
                                text: modeButton.modelData.icon
                                color: homeChat.viewModel.currentModeName === modeButton.modelData.mode ? homeChat.modeAccent : SentinelTheme.textMuted
                                font.pixelSize: 20
                            }
                            background: Rectangle {
                                radius: 10
                                color: homeChat.viewModel.currentModeName === modeButton.modelData.mode || modeButton.hovered ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.12) : "transparent"
                                border.width: modeButton.activeFocus ? 2 : 0
                                border.color: homeChat.modeAccent
                            }
                        }
                    }
                    Button {
                        id: micButton
                        objectName: "chatVoiceButton"
                        Layout.preferredWidth: 36
                        Layout.preferredHeight: 36
                        hoverEnabled: true
                        focusPolicy: Qt.StrongFocus
                        Accessible.name: homeChat.viewModel.voiceRecordingActive ? qsTr("Stop recording") : qsTr("Voice input")
                        ToolTip.visible: hovered || activeFocus
                        ToolTip.text: homeChat.viewModel.voiceInputStatus.length > 0 ? homeChat.viewModel.voiceInputMessage : Accessible.name
                        enabled: !homeChat.sendBusy
                        onClicked: homeChat.viewModel.voiceRecordingActive ? homeChat.viewModel.stopVoiceCapture() : homeChat.viewModel.startVoiceCapture()
                        contentItem: TablerGlyph {
                            text: homeChat.viewModel.voiceRecordingActive ? "player-pause" : "microphone"
                            color: homeChat.viewModel.voiceRecordingActive ? SentinelTheme.warning : SentinelTheme.textMuted
                            font.pixelSize: 20
                        }
                        background: Rectangle {
                            radius: 10
                            color: micButton.hovered ? SentinelTheme.withAlpha(homeChat.modeAccent, 0.12) : "transparent"
                            border.width: micButton.activeFocus ? 2 : 0
                            border.color: homeChat.modeAccent
                        }
                    }
                    Item { Layout.fillWidth: true; Layout.minimumWidth: 0 }
                    SentinelComboBox {
                        id: composerProvider
                        objectName: "chatProviderSelector"
                        Layout.preferredWidth: homeChat.width < 760 ? 100 : 145
                        Layout.minimumWidth: 70
                        Layout.preferredHeight: 36
                        font.pixelSize: 12
                        Accessible.name: qsTr("Provider")
                        enabled: !homeChat.sendBusy
                        model: homeChat.viewModel.selectableRuntimeProviderLabels
                        currentIndex: homeChat.viewModel.selectableRuntimeProviderIds.indexOf(homeChat.viewModel.selectedRuntimeProvider)
                        displayText: currentIndex >= 0 ? model[currentIndex] : homeChat.viewModel.activeRuntimeProviderLabel
                        onActivated: function(index) { homeChat.viewModel.selectedRuntimeProvider = homeChat.viewModel.selectableRuntimeProviderIds[index] }
                    }
                    SentinelComboBox {
                        id: composerModel
                        objectName: "chatModelSelector"
                        readonly property var modelNames: homeChat.isLMStudio ? homeChat.viewModel.loadedLMStudioModelNames : homeChat.viewModel.ollamaModelNames
                        Layout.preferredWidth: homeChat.width < 760 ? 130 : 200
                        Layout.minimumWidth: 80
                        Layout.preferredHeight: 36
                        font.pixelSize: 12
                        Accessible.name: qsTr("Model")
                        enabled: modelNames.length > 0 && !homeChat.sendBusy
                        model: modelNames
                        currentIndex: modelNames.indexOf(homeChat.viewModel.selectedLocalModel)
                        displayText: homeChat.viewModel.selectedLocalModel || qsTr("Choose model")
                        onActivated: function(index) { homeChat.viewModel.selectedLocalModel = modelNames[index] }
                    }
                    Button {
                        id: sendButton
                        objectName: "chatSendButton"
                        Layout.preferredWidth: 38
                        Layout.preferredHeight: 38
                        focusPolicy: Qt.StrongFocus
                        hoverEnabled: true
                        enabled: homeChat.sendBusy || (promptInput.text.trim().length > 0 && homeChat.canSend)
                        Accessible.name: homeChat.sendBusy ? qsTr("Stop") : qsTr("Send")
                        ToolTip.visible: hovered || activeFocus
                        ToolTip.text: Accessible.name
                        onClicked: homeChat.sendBusy && !homeChat.agentAwaitingApproval ? homeChat.viewModel.cancelLocalInference() : homeChat.sendComposerText()
                        contentItem: Item {
                            TablerGlyph { anchors.centerIn: parent; text: "arrow-up"; color: SentinelTheme.textPrimary; font.pixelSize: 22; visible: !homeChat.sendBusy || homeChat.agentAwaitingApproval }
                            Rectangle { anchors.centerIn: parent; width: 12; height: 12; radius: 2; color: SentinelTheme.textPrimary; visible: homeChat.sendBusy && !homeChat.agentAwaitingApproval }
                        }
                        background: Rectangle {
                            radius: 19
                            color: SentinelTheme.withAlpha(homeChat.modeAccent, sendButton.enabled ? 0.24 : 0.08)
                            border.width: sendButton.activeFocus ? 2 : 0
                            border.color: homeChat.modeAccent
                        }
                    }
                }
                SentinelButton {
                    objectName: "chatVoiceSetupButton"
                    visible: homeChat.viewModel.voiceInputStatus.length > 0
                    text: qsTr("Voice & Audio settings")
                    Accessible.name: text
                    onClicked: homeChat.voiceSettingsRequested()
                }
                Label {
                    Layout.fillWidth: true
                    visible: homeChat.viewModel.voiceInputStatus.length > 0 || homeChat.viewModel.attachmentError.length > 0
                    text: homeChat.viewModel.attachmentError || homeChat.viewModel.voiceInputMessage
                    color: SentinelTheme.textMuted
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }
        }


        }
    }
}

    FileDialog {
        id: imageFileDialog
        title: qsTr("Upload Image")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            qsTr("Images (*.png *.jpg *.jpeg *.webp *.gif *.bmp)"),
            qsTr("All files (*)")
        ]
        onAccepted: homeChat.viewModel.attachFileToChat(selectedFile.toString())
    }

    FileDialog {
        id: documentFileDialog
        title: qsTr("Upload File")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            qsTr("Documents (*.pdf *.txt *.md *.markdown *.docx *.csv *.json)"),
            qsTr("Source code files (*.cpp *.h *.hpp *.qml *.js *.ts *.py *.java *.cs *.go *.rs *.swift)"),
            qsTr("All files (*)")
        ]
        onAccepted: homeChat.viewModel.attachFileToChat(selectedFile.toString())
    }

    // Permanent delete confirmation dialog
    Dialog {
        id: deleteConfirmDialog
        objectName: "chatDeleteConfirmation"
        onOpened: homeChat.deleteRequestError = ""
        anchors.centerIn: parent
        modal: true
        dim: true
        padding: 0
        closePolicy: homeChat.deleteRequestPending ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: SentinelTheme.radiusLg
            color: SentinelTheme.backgroundRaised
            border.color: SentinelTheme.withAlpha(SentinelTheme.warning, 0.35)
            layer.enabled: GraphicsInfo.api !== GraphicsInfo.Software
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.12)
                shadowVerticalOffset: 2
                shadowBlur: 0.10
                shadowOpacity: 1.0
            }
        }

        contentItem: ColumnLayout {
            spacing: 0
            width: 340

            // Header
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: "transparent"
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceLg
                spacing: SentinelTheme.spaceMd

                // Warning icon + title
                RowLayout {
                    spacing: SentinelTheme.spaceSm

                    TablerGlyph {
                        text: "alert-triangle"
                        font.pixelSize: 20
                        color: SentinelTheme.warning
                    }

                    Text {
                        text: qsTr("Delete conversation")
                        color: SentinelTheme.textPrimary
                        font.pixelSize: SentinelTheme.fontCard
                        font.bold: true
                        Layout.fillWidth: true
                    }
                }

                // Conversation name
                Rectangle {
                    Layout.fillWidth: true
                    radius: SentinelTheme.radiusMd
                    color: SentinelTheme.withAlpha(SentinelTheme.warning, 0.07)
                    border.color: SentinelTheme.withAlpha(SentinelTheme.warning, 0.18)
                    implicitHeight: deleteConvLabel.implicitHeight + SentinelTheme.spaceSm * 2

                    Text {
                        id: deleteConvLabel
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: SentinelTheme.spaceSm
                        text: homeChat.pendingDeleteConversationTitle
                        color: SentinelTheme.textPrimary
                        font.pixelSize: SentinelTheme.fontSmall
                        font.bold: true
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("This action cannot be undone. The conversation will be permanently removed.")
                    color: SentinelTheme.textMuted
                    font.pixelSize: SentinelTheme.fontSmall
                    wrapMode: Text.WordWrap
                }

                Label {
                    Layout.fillWidth: true
                    visible: homeChat.deleteRequestError.length > 0
                    text: homeChat.deleteRequestError
                    color: SentinelTheme.warning
                    wrapMode: Text.WordWrap
                }
                // Buttons
                RowLayout {
                    Layout.fillWidth: true
                    spacing: SentinelTheme.spaceSm

                    // Cancel
                    Button {
                        id: deleteCancelBtn
                        objectName: "chatDeleteCancel"
                        enabled: !homeChat.deleteRequestPending
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        text: qsTr("Cancel")
                        hoverEnabled: true
                        onClicked: deleteConfirmDialog.close()

                        contentItem: Text {
                            text: deleteCancelBtn.text
                            color: SentinelTheme.textPrimary
                            font.pixelSize: SentinelTheme.fontSmall
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: SentinelTheme.radiusMd
                            color: InteractionTokens.surfaceColor(deleteCancelBtn.hovered, deleteCancelBtn.down, false, homeChat.modeAccent)
                            border.color: InteractionTokens.borderColor(deleteCancelBtn.activeFocus, deleteCancelBtn.hovered, false, homeChat.modeAccent)
                        }
                    }

                    // Confirm delete
                    Button {
                        id: deleteConfirmBtn
                        objectName: "chatDeleteConfirm"
                        enabled: !homeChat.deleteRequestPending
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        text: qsTr("Delete")
                        hoverEnabled: true
                        onClicked: {
                            homeChat.deleteRequestError = ""
                            homeChat.deleteRequestPending = true
                            if (!homeChat.viewModel.requestPermanentDeleteConversation(homeChat.pendingDeleteConversationId)) {
                                homeChat.deleteRequestPending = false
                                homeChat.deleteRequestError = qsTr("The conversation could not be deleted. Check the daemon connection and stop any active request before retrying.")
                                return
                            }
                        }

                        contentItem: Text {
                            text: deleteConfirmBtn.text
                            color: SentinelTheme.warning
                            font.pixelSize: SentinelTheme.fontSmall
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: SentinelTheme.radiusMd
                            color: deleteConfirmBtn.hovered
                                   ? SentinelTheme.withAlpha(SentinelTheme.warning, 0.14)
                                   : SentinelTheme.withAlpha(SentinelTheme.warning, 0.07)
                            border.color: SentinelTheme.withAlpha(SentinelTheme.warning, deleteConfirmBtn.hovered ? 0.45 : 0.28)
                        }
                    }
                }
            }
        }
    }
}
