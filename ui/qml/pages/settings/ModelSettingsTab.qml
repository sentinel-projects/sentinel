// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Sentinel.Desktop

Item {
    id: root
    required property var viewModel
    property bool compact: false
    property bool advanced: false
    property color modeAccent: SentinelTheme.modeAccent(viewModel.currentModeName)
    property var voiceFileDialog: null
    property var soundManager: null
    property string capabilityError: ""
    property var cloudConnectionState: ({})
    Connections {
        target: root.viewModel
        function onRuntimeProjectionChanged() {
            if (root.isCloudProvider && root.viewModel.daemonConnected)
                root.cloudConnectionState = root.viewModel.providerSettingsState(root.currentProvider)
        }
    }
    readonly property int panelPadding: SentinelTheme.spaceLg
    readonly property var cloudProviderNames: ["OpenAI", "Claude", "Gemini", "DeepSeek", "Groq", "Mistral"]
    readonly property string currentProvider: root.viewModel.selectedRuntimeProvider
    readonly property bool isCloudProvider: {
        var p = (root.currentProvider || "").toLowerCase()
        return p === "cloud-api" || p === "openai" || p === "claude" || p === "gemini" ||
               p === "deepseek" || p === "groq" || p === "mistral"
    }
    readonly property var modelList: {
        if (root.currentProvider === "ollama") return root.viewModel.ollamaModelNames
        if (root.currentProvider === "lm-studio") return root.viewModel.loadedLMStudioModelNames
        return root.viewModel.ollamaModelNames
    }
    readonly property var inferencePresetModel: [
        { "name": qsTr("Precise"), "temp": 0.20, "topP": 0.80 },
        { "name": qsTr("Balanced"), "temp": 0.70, "topP": 0.90 },
        { "name": qsTr("Creative"), "temp": 1.10, "topP": 0.95 }
    ]
    readonly property var inferencePresetLabels: [
        qsTr("Precise"),
        qsTr("Balanced"),
        qsTr("Creative")
    ]

    height: implicitHeight
    implicitHeight: visible ? mainLayout.implicitHeight + panelPadding * 2 : 0

    function hasSelectableModels() {
        return root.modelList.length > 0
    }

    function capabilityTitle(id) {
        switch (id) {
        case "streaming": return qsTr("Streaming")
        case "structuredOutput": return qsTr("Structured Output")
        case "nativeToolCalling": return qsTr("Native Tool Calling")
        case "visionInput": return qsTr("Vision Input")
        case "audioInput": return qsTr("Audio Input")
        case "audioOutput": return qsTr("Audio Output")
        case "contextWindow": return qsTr("Context Window")
        case "maxOutputTokens": return qsTr("Maximum Output Tokens")
        default: return id
        }
    }

    function capabilityOverrideLabel(value) {
        if (value === "supported") return qsTr("Supported")
        if (value === "unsupported") return qsTr("Unsupported")
        if (value === "custom") return qsTr("Custom")
        return qsTr("Automatic")
    }

    function inferencePresetIndex() {
        var bestIndex = 0
        var bestDistance = 999
        for (let i = 0; i < root.inferencePresetModel.length; ++i) {
            const preset = root.inferencePresetModel[i]
            const distance = Math.abs(root.viewModel.localInferenceTemperature - preset.temp)
                         + Math.abs(root.viewModel.localInferenceTopP - preset.topP)
            if (distance < bestDistance) {
                bestDistance = distance
                bestIndex = i
            }
        }
        return bestDistance < 0.18 ? bestIndex : -1
    }

    function applyInferencePreset(index) {
        if (index < 0 || index >= root.inferencePresetModel.length)
            return
        var preset = root.inferencePresetModel[index]
        root.viewModel.localInferenceTemperature = preset.temp
        root.viewModel.localInferenceTopP = preset.topP
    }

    ColumnLayout {
        id: mainLayout
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: root.panelPadding
        spacing: SentinelTheme.spaceMd

        SectionTitle {
            title: qsTr("AI Settings & Runtimes")
            subtitle: qsTr("Configure and inspect local AI inference runtimes, providers, and endpoints.")
            Layout.fillWidth: true
        }

        SettingCard {
            title: qsTr("Active Runtime Status")

            SettingControlRow {
                title: qsTr("Provider")
                subtitle: qsTr("Active LLM engine backend.")
                accent: root.modeAccent
                compact: root.compact
                showDivider: true

                Item {
                    anchors.fill: parent
                SentinelComboBox {
                    id: runtimeProviderCombo
                    accent: root.modeAccent
                    anchors.left: parent.left
                    anchors.right: statusLight.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    implicitHeight: 36
                    model: root.viewModel.selectableRuntimeProviderLabels
                    currentIndex: {
                        var sel = root.viewModel.selectedRuntimeProvider
                        var ids = root.viewModel.selectableRuntimeProviderIds
                        var idx = ids.indexOf(sel)
                        if (idx < 0 && (sel === "openai" || sel === "claude" ||
                                        sel === "gemini" || sel === "deepseek" ||
                                        sel === "groq"   || sel === "mistral")) {
                            idx = ids.indexOf("cloud-api")
                        }
                        return idx >= 0 ? idx : 0
                    }
                    displayText: currentIndex >= 0 ? currentText : root.viewModel.activeRuntimeProviderLabel
                    onActivated: (index) => {
                        var ids = root.viewModel.selectableRuntimeProviderIds
                        if (index >= 0 && index < ids.length) {
                            const providerId = ids[index]
                            root.viewModel.selectedRuntimeProvider = providerId
                        }
                    }
                }
                    Rectangle {
                        id: statusLight
                        objectName: "providerStatusLight"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12; height: 12; radius: 6
                        readonly property string readinessState: (root.viewModel.activeRuntimeReadinessState || "").toLowerCase()
                        color: ["ready", "healthy", "available", "connected"].indexOf(readinessState) >= 0 ? SentinelTheme.success
                               : /error|failed|unavailable|unreachable|missing|blocked|offline|invalid|disabled|incompatible|unauthorized/.test(readinessState) ? SentinelTheme.error : SentinelTheme.warning
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Indicator
                        Accessible.name: qsTr("Runtime status: %1").arg(root.viewModel.activeRuntimeReadinessState || qsTr("Waiting"))
                        ToolTip.visible: statusHover.hovered || activeFocus
                        ToolTip.text: root.viewModel.activeRuntimeReadinessSummary || Accessible.name
                        HoverHandler { id: statusHover }
                    }
                }
            }

            SettingControlRow {
                visible: root.advanced
                title: qsTr("Provider Catalog")
                subtitle: root.viewModel.activeRuntimeProviderLabel
                accent: root.modeAccent
                compact: root.compact
                showDivider: true

                StatusChip {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    width: parent.width
                    value: root.viewModel.providerStatus
                    accent: root.modeAccent
                    active: root.viewModel.providerStatus === "Available"
                         || root.viewModel.providerStatus === "Configured model only"
                    muted: root.viewModel.providerStatus === "Pending"
                        || root.viewModel.providerStatus === "Unverified"
                }
            }

            SettingControlRow {
                title: qsTr("Active Model")
                subtitle: root.hasSelectableModels()
                          ? root.viewModel.selectedLocalModelSummary
                          : root.isCloudProvider
                              ? (root.viewModel.cloudModelDiscoveryError.length > 0
                                     ? root.viewModel.cloudModelDiscoveryError
                                     : root.viewModel.activeRuntimeReadinessSummary.length > 0
                                           ? root.viewModel.activeRuntimeReadinessSummary
                                           : qsTr("No models returned by the provider API."))
                              : root.currentProvider === "ollama" && root.viewModel.ollamaDiscoveryStatus.length > 0
                                  ? root.viewModel.ollamaDiscoveryStatus
                                  : qsTr("Model selection is available after the active local runtime reports models.")
                accent: root.modeAccent
                compact: root.compact

                SentinelComboBox {
                    id: activeModelCombo
                    accent: root.modeAccent
                    anchors.fill: parent
                    implicitHeight: 36
                    enabled: root.hasSelectableModels()
                    model: root.hasSelectableModels() ? root.modelList : [qsTr("None selected")]
                    currentIndex: root.hasSelectableModels()
                                  ? root.modelList.indexOf(root.viewModel.selectedLocalModel)
                                  : 0
                    displayText: {
                        if (root.hasSelectableModels())
                            return currentIndex >= 0 ? currentText : root.viewModel.activeLocalModelName
                        return root.viewModel.activeLocalModelName
                               ? root.viewModel.activeLocalModelName
                               : qsTr("None selected")
                    }
                    onActivated: (index) => {
                        if (index >= 0 && index < root.modelList.length)
                            root.viewModel.selectedLocalModel = root.modelList[index]
                    }
                }
            }
        }

        SettingCard {
            title: qsTr("Model Capabilities")
            subtitle: root.currentProvider + " / " + (root.viewModel.selectedLocalModel || qsTr("No model selected"))

            Text {
                visible: root.viewModel.modelCapabilitySettings.length === 0
                text: qsTr("Select a model to edit capabilities.")
                color: SentinelTheme.textMuted
                font.pixelSize: SentinelTheme.fontSmall
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceMd
            }

            Repeater {
                model: root.viewModel.modelCapabilitySettings

                delegate: SettingControlRow {
                    id: capabilityRow
                    required property var modelData
                    required property int index
                    title: root.capabilityTitle(modelData.id)
                    subtitle: qsTr("Resolved: %1 | Override: %2")
                                  .arg(modelData.resolved)
                                  .arg(root.capabilityOverrideLabel(modelData.override))
                    accent: root.modeAccent
                    compact: root.compact
                    controlWidth: root.compact ? 180 : 240
                    showDivider: index < root.viewModel.modelCapabilitySettings.length - 1

                    SentinelComboBox {
                        visible: !capabilityRow.modelData.numeric
                        anchors.fill: parent
                        accent: root.modeAccent
                        model: [qsTr("Automatic"), qsTr("Supported"), qsTr("Unsupported")]
                        currentIndex: capabilityRow.modelData.override === "supported" ? 1
                                      : capabilityRow.modelData.override === "unsupported" ? 2 : 0
                        onActivated: (choice) => {
                            var value = ["automatic", "supported", "unsupported"][choice]
                            root.capabilityError = root.viewModel.setModelCapabilityOverride(
                                capabilityRow.modelData.id, value) ? "" : qsTr("Unable to save capability override.")
                        }
                    }

                    RowLayout {
                        visible: capabilityRow.modelData.numeric
                        anchors.fill: parent
                        spacing: SentinelTheme.spaceXs

                        SentinelComboBox {
                            id: numberMode
                            Layout.preferredWidth: numberField.visible ? 92 : parent.width
                            Layout.fillHeight: true
                            accent: root.modeAccent
                            model: [qsTr("Automatic"), qsTr("Custom")]
                            currentIndex: capabilityRow.modelData.override === "custom" ? 1 : 0
                            onActivated: (choice) => {
                                var value = choice === 0 ? 0
                                          : capabilityRow.modelData.id === "contextWindow" ? 8192 : 256
                                root.capabilityError = root.viewModel.setModelCapabilityNumberOverride(
                                    capabilityRow.modelData.id, value) ? "" : qsTr("Output tokens must be below the context window.")
                            }
                        }

                        SentinelTextField {
                            id: numberField
                            visible: capabilityRow.modelData.override === "custom"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            text: capabilityRow.modelData.overrideValue > 0
                                  ? String(capabilityRow.modelData.overrideValue) : ""
                            inputMethodHints: Qt.ImhDigitsOnly
                            validator: IntValidator { bottom: 1; top: 1000000 }
                            onEditingFinished: {
                                var value = Number(text)
                                root.capabilityError = Number.isInteger(value) && value > 0 &&
                                    root.viewModel.setModelCapabilityNumberOverride(capabilityRow.modelData.id, value)
                                    ? "" : qsTr("Enter a positive value below the context window.")
                            }
                        }
                    }
                }
            }

            Text {
                visible: root.viewModel.modelCapabilityCompatibilitySummary.length > 0
                text: root.viewModel.modelCapabilityCompatibilitySummary
                color: SentinelTheme.textMuted
                font.pixelSize: SentinelTheme.fontSmall
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceMd
            }

            Text {
                visible: root.capabilityError.length > 0
                text: root.capabilityError
                color: SentinelTheme.errorText
                font.pixelSize: SentinelTheme.fontSmall
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.margins: SentinelTheme.spaceMd
            }

            Item {
                visible: root.viewModel.modelCapabilitySettings.length > 0
                Layout.fillWidth: true
                Layout.preferredHeight: 44

                SentinelButton {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: SentinelTheme.spaceMd
                    text: qsTr("Reset capability overrides")
                    accent: root.modeAccent
                    onClicked: {
                        root.viewModel.resetModelCapabilityOverrides()
                        root.capabilityError = ""
                    }
                }
            }
        }

        SettingCard {
            title: qsTr("Endpoints & Routing")
            subtitle: qsTr("Configure server URLs and provider routing logic.")

            SettingControlRow {
                title: qsTr("Routing Mode")
                subtitle: qsTr("Strategy for selecting between local models and cloud providers.")
                accent: root.modeAccent
                compact: root.compact
                showDivider: true

                SentinelComboBox {
                    id: routingModeCombo
                    accent: root.modeAccent
                    anchors.fill: parent
                    implicitHeight: 36
                    model: root.viewModel.availableRoutingModes
                    currentIndex: root.viewModel.availableRoutingModes.indexOf(root.viewModel.currentRoutingMode)
                    displayText: currentIndex >= 0 ? currentText : root.viewModel.currentRoutingMode
                    onActivated: (index) => {
                        if (index >= 0 && index < root.viewModel.availableRoutingModes.length)
                            root.viewModel.setRoutingModeByName(root.viewModel.availableRoutingModes[index])
                    }
                }
            }

            SettingControlRow {
                title: qsTr("Ollama Endpoint")
                subtitle: qsTr("Local REST endpoint for Ollama daemon.")
                accent: root.modeAccent
                compact: root.compact
                showDivider: true

                SentinelTextField {
                    anchors.fill: parent
                    placeholderText: "http://127.0.0.1:11434"
                    text: root.viewModel.ollamaEndpoint
                    onEditingFinished: root.viewModel.ollamaEndpoint = text
                }
            }

            SettingControlRow {
                title: qsTr("LM Studio Endpoint")
                subtitle: qsTr("Local REST endpoint for LM Studio server.")
                accent: root.modeAccent
                compact: root.compact
                showDivider: true

                SentinelTextField {
                    anchors.fill: parent
                    placeholderText: "http://127.0.0.1:1234"
                    text: root.viewModel.lmStudioEndpoint
                    onEditingFinished: root.viewModel.lmStudioEndpoint = text
                }
            }

            SettingControlRow {
                title: qsTr("llama.cpp Endpoint")
                subtitle: qsTr("Local REST endpoint for llama-server process.")
                accent: root.modeAccent
                compact: root.compact
                showDivider: true

                SentinelTextField {
                    anchors.fill: parent
                    placeholderText: "http://127.0.0.1:8080"
                    text: root.viewModel.llamaCppEndpoint
                    onEditingFinished: root.viewModel.llamaCppEndpoint = text
                }
            }

            SettingControlRow {
                title: qsTr("Cloud API Endpoint")
                subtitle: qsTr("Custom OpenAI-compatible cloud gateway endpoint.")
                accent: root.modeAccent
                compact: root.compact

                SentinelTextField {
                    anchors.fill: parent
                    placeholderText: "https://api.openai.com/v1"
                    text: root.viewModel.cloudApiEndpoint
                    onEditingFinished: root.viewModel.cloudApiEndpoint = text
                }
            }
        }

        SettingCard {
            title: qsTr("Cloud Providers")
            subtitle: qsTr("Select a cloud provider and configure its API credentials. Credentials use the configured secure credential store.")

            SettingControlRow {
                title: qsTr("Active Cloud Provider")
                subtitle: qsTr("Primary provider for cloud LLM inference.")
                accent: root.modeAccent
                compact: root.compact
                showDivider: true

                SentinelComboBox {
                    id: cloudProviderCombo
                    accent: root.modeAccent
                    anchors.fill: parent
                    implicitHeight: 36
                    model: root.cloudProviderNames
                    currentIndex: {
                        // Settings stores lowercase ids; the display list is
                        // capitalized, so compare case-insensitively.
                        var stored = (root.viewModel.selectedCloudProvider || "").toLowerCase()
                        for (let i = 0; i < root.cloudProviderNames.length; ++i) {
                            if (root.cloudProviderNames[i].toLowerCase() === stored)
                                return i
                        }
                        return 0
                    }
                    displayText: currentIndex >= 0 ? currentText : root.viewModel.selectedCloudProvider
                    onActivated: (index) => {
                        if (index >= 0 && index < root.cloudProviderNames.length)
                            root.viewModel.selectedCloudProvider = root.cloudProviderNames[index]
                    }
                }
            }

            SettingControlRow {
                visible: (root.viewModel.selectedCloudProvider || "").toLowerCase() === "openai"
                title: root.currentProvider === "cloud-api" ? qsTr("Cloud API Key") : qsTr("OpenAI API Key")
                subtitle: root.currentProvider === "cloud-api"
                    ? qsTr("API key for the Cloud API Endpoint above, including OpenAI-compatible services.")
                    : qsTr("Authentication key for OpenAI endpoints.")
                accent: root.modeAccent
                compact: root.compact

                SentinelTextField {
                    anchors.fill: parent
                    echoMode: TextInput.Password
                    placeholderText: qsTr("Paste API key")
                    text: root.viewModel.openAiApiKey
                    onEditingFinished: {
                        if (text.trim().length > 0) {
                            root.viewModel.openAiApiKey = text.trim()
                            text = ""
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.isCloudProvider
                wrapMode: Text.WordWrap
                color: SentinelTheme.textMuted
                text: root.cloudConnectionState.credentialPresent === true
                    ? (root.viewModel.activeRuntimeReadinessState === "Available"
                       ? qsTr("API key stored. Model catalog available.")
                       : qsTr("API key stored. %1").arg(root.viewModel.activeRuntimeReadinessSummary
                             || root.cloudConnectionState.discoveryDetail
                             || qsTr("Connection has not been verified.")))
                    : qsTr("No API key confirmed. Paste a key and press Tab to save it.")
            }

            SettingControlRow {
                visible: (root.viewModel.selectedCloudProvider || "").toLowerCase() === "claude"
                title: qsTr("Claude API Key")
                subtitle: qsTr("Authentication key for Anthropic Claude 3.5 Sonnet and Opus models.")
                accent: root.modeAccent
                compact: root.compact

                SentinelTextField {
                    anchors.fill: parent
                    echoMode: TextInput.Password
                    placeholderText: "sk-ant-..."
                    text: root.viewModel.claudeApiKey
                    onEditingFinished: root.viewModel.claudeApiKey = text
                }
            }

            SettingControlRow {
                visible: (root.viewModel.selectedCloudProvider || "").toLowerCase() === "gemini"
                title: qsTr("Gemini API Key")
                subtitle: qsTr("Authentication key for Google Gemini Pro and Flash models.")
                accent: root.modeAccent
                compact: root.compact

                SentinelTextField {
                    anchors.fill: parent
                    echoMode: TextInput.Password
                    placeholderText: "AIza..."
                    text: root.viewModel.geminiApiKey
                    onEditingFinished: root.viewModel.geminiApiKey = text
                }
            }

            SettingControlRow {
                visible: (root.viewModel.selectedCloudProvider || "").toLowerCase() === "deepseek"
                title: qsTr("DeepSeek API Key")
                subtitle: qsTr("Authentication key for DeepSeek V3 and DeepSeek R1 reasoning endpoints.")
                accent: root.modeAccent
                compact: root.compact

                SentinelTextField {
                    anchors.fill: parent
                    echoMode: TextInput.Password
                    placeholderText: "sk-..."
                    text: root.viewModel.deepseekApiKey
                    onEditingFinished: root.viewModel.deepseekApiKey = text
                }
            }

            SettingControlRow {
                visible: (root.viewModel.selectedCloudProvider || "").toLowerCase() === "groq"
                title: qsTr("Groq API Key")
                subtitle: qsTr("Authentication key for ultra-fast Groq LPU inference.")
                accent: root.modeAccent
                compact: root.compact

                SentinelTextField {
                    anchors.fill: parent
                    echoMode: TextInput.Password
                    placeholderText: "gsk_..."
                    text: root.viewModel.groqApiKey
                    onEditingFinished: root.viewModel.groqApiKey = text
                }
            }

            SettingControlRow {
                visible: (root.viewModel.selectedCloudProvider || "").toLowerCase() === "mistral"
                title: qsTr("Mistral API Key")
                subtitle: qsTr("Authentication key for Mistral Large, Codestral, and NeMo endpoints.")
                accent: root.modeAccent
                compact: root.compact

                SentinelTextField {
                    anchors.fill: parent
                    echoMode: TextInput.Password
                    placeholderText: "sk-..."
                    text: root.viewModel.mistralApiKey
                    onEditingFinished: root.viewModel.mistralApiKey = text
                }
            }
        }

        SentinelButton {
            Layout.fillWidth: true
            text: root.advanced ? qsTr("Hide advanced settings") : qsTr("Show advanced settings")
            onClicked: root.advanced = !root.advanced
        }
        SettingCard {
            visible: root.advanced
            title: qsTr("Inference Parameters")
            subtitle: qsTr("Fine-tune sampling temperature, top-p nucleus sampling, and context boundaries.")

            SettingControlRow {
                title: qsTr("Inference Presets")
                subtitle: qsTr("Quickly apply paired temperature and top-p values.")
                accent: root.modeAccent
                compact: root.compact
                showDivider: true

                SentinelComboBox {
                    id: inferencePresetCombo
                    accent: root.modeAccent
                    anchors.fill: parent
                    implicitHeight: 36
                    model: root.inferencePresetLabels
                    currentIndex: root.inferencePresetIndex()
                    displayText: currentIndex >= 0 ? currentText : qsTr("Custom")
                    onActivated: (index) => root.applyInferencePreset(index)
                }
            }

            SettingControlRow {
                title: qsTr("Temperature (%1)").arg(root.viewModel.localInferenceTemperature.toFixed(2))
                subtitle: qsTr("Higher values produce more creative responses, lower values make output more deterministic.")
                accent: root.modeAccent
                compact: root.compact
                showDivider: true
                controlWidth: root.compact ? 150 : 200

                SentinelSlider {
                    accent: root.modeAccent
                    anchors.fill: parent
                    from: 0.0
                    to: 1.5
                    stepSize: 0.05
                    value: root.viewModel.localInferenceTemperature
                    onValueChanged: root.viewModel.localInferenceTemperature = value
                }
            }

            SettingControlRow {
                title: qsTr("Top-P (%1)").arg(root.viewModel.localInferenceTopP.toFixed(2))
                subtitle: qsTr("Nucleus sampling cumulative probability threshold.")
                accent: root.modeAccent
                compact: root.compact
                showDivider: true
                controlWidth: root.compact ? 150 : 200

                SentinelSlider {
                    accent: root.modeAccent
                    anchors.fill: parent
                    from: 0.1
                    to: 1.0
                    stepSize: 0.05
                    value: root.viewModel.localInferenceTopP
                    onValueChanged: root.viewModel.localInferenceTopP = value
                }
            }

            SettingControlRow {
                title: qsTr("Max Tokens")
                subtitle: qsTr("Maximum token generation length per response.")
                accent: root.modeAccent
                compact: root.compact
                showDivider: true
                controlWidth: root.compact ? 150 : 200

                SentinelSpinBox {
                    accent: root.modeAccent
                    anchors.fill: parent
                    from: 64
                    to: 32768
                    stepSize: 64
                    value: root.viewModel.localInferenceMaxTokens
                    onValueModified: root.viewModel.localInferenceMaxTokens = value
                }
            }

            SettingToggleRow {
                id: requestTimeoutToggle
                title: qsTr("Request timeout")
                subtitle: checked
                          ? qsTr("Stop a request after the selected limit.")
                          : qsTr("No client-side time limit. You can still stop a response manually.")
                accent: root.modeAccent
                compact: root.compact
                checked: root.viewModel.localInferenceTimeoutMs > 0
                showDivider: true
                onToggled: (enabled) => {
                    root.viewModel.localInferenceTimeoutMs = enabled ? 30000 : 0
                }
            }

            SettingControlRow {
                visible: requestTimeoutToggle.checked
                title: qsTr("Timeout (%1 ms)").arg(root.viewModel.localInferenceTimeoutMs)
                subtitle: qsTr("Only applies while request timeout is enabled.")
                accent: root.modeAccent
                compact: root.compact
                controlWidth: root.compact ? 150 : 200

                SentinelSpinBox {
                    accent: root.modeAccent
                    anchors.fill: parent
                    from: 1000
                    to: 300000
                    stepSize: 1000
                    value: root.viewModel.localInferenceTimeoutMs
                    onValueModified: root.viewModel.localInferenceTimeoutMs = value
                }
            }
        }
    }
}
