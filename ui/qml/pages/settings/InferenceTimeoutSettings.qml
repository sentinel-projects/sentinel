// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Sentinel.Desktop

ColumnLayout {
    id: root
    objectName: "inferenceTimeoutSettings"
    required property var viewModel
    property color accent: SentinelTheme.calmAccent
    property var policy: ({})
    property string saved: ""
    property string draftMode: "compatibility"
    property bool loaded: false
    property int unavailableReads: 0
    property bool loadFailed: false
    property bool dirty: false
    property bool awaiting: false
    property string pendingSignature: ""
    property double pendingUntil: 0
    property string status: ""
    readonly property bool connected: viewModel.daemonConnected
    readonly property bool supported: policy.supported === true
    readonly property var modes: supported ? [qsTr("Default / Compatibility"), qsTr("Extended"), qsTr("Custom")] : [qsTr("Default / Compatibility")]
    function signature(raw) {
        try {
            const p = raw.length ? JSON.parse(raw) : {mode: "compatibility", initialMs: 0, idleMs: 0, totalMs: 0}
            return JSON.stringify([p.mode, p.initialMs, p.idleMs, p.totalMs])
        } catch (_) { return "invalid" }
    }
    function refresh() {
        if (!connected) return
        const rows = viewModel.productSettings()
        const requested = rows.find(function(row) { return row.id === "models.inference-timeout" })
        const effective = rows.find(function(row) { return row.id === "models.inference-timeout-effective" })
        if (!requested || !effective) return
        unavailableReads = 0
        loadFailed = false
        policy = effective.value
        saved = requested.value || ""
        if ((!loaded && !dirty) || (!dirty && !awaiting)) restore()
        loaded = true
        if (awaiting && signature(saved) === pendingSignature) {
            awaiting = false
            dirty = false
            status = qsTr("Timeout preference saved. It applies to the next Chat request.")
            restore()
        }
    }
    function restore() {
        try {
            const p = saved.length ? JSON.parse(saved) : {mode: "compatibility", initialMs: 0, idleMs: 0, totalMs: 0}
            draftMode = p.mode
            initial.text = String(p.initialMs / 1000)
            idle.text = String(p.idleMs / 1000)
            total.text = String(p.totalMs / 1000)
            dirty = false
        } catch (_) { status = qsTr("The saved profile is invalid. Reset to Compatibility before starting Chat.") }
    }
    function save(reset) {
        let payload = ""
        if (!reset && draftMode !== "compatibility") {
            if (!initial.acceptableInput || !idle.acceptableInput || !total.acceptableInput ||
                initial.text.length === 0 || idle.text.length === 0 || total.text.length === 0 ||
                Number(total.text) <= 0 || (draftMode === "extended" && (Number(initial.text) <= 0 || Number(idle.text) <= 0))) {
                status = qsTr("Enter valid seconds. Total must be positive; Extended also requires positive initial and idle limits.")
                return
            }
            payload = JSON.stringify({version: 1, mode: draftMode, initialMs: Math.round(Number(initial.text) * 1000), idleMs: Math.round(Number(idle.text) * 1000), totalMs: Math.round(Number(total.text) * 1000)})
        }
        const result = viewModel.setProductSetting("models.inference-timeout", payload)
        if (result.accepted || result.code === "Pending") {
            awaiting = true
            pendingSignature = signature(payload)
            pendingUntil = Date.now() + 10000
            status = qsTr("Waiting for the daemon to confirm the saved preference…")
            refresh()
        } else status = result.code === "settings.timeout-unsupported" ? qsTr("This provider/model cannot honor the requested policy. Choose Compatibility or a supported text stream.") : qsTr("Could not save the timeout preference. Check the values, connection and writable settings storage.")
    }
    onConnectedChanged: {
        if (!connected) { loaded = false; unavailableReads = 0; loadFailed = false; policy = ({}) }
        if (!connected && awaiting) {
            awaiting = false
            status = qsTr("Connection lost. The save outcome is unknown. Reconnect to inspect the saved value; the change will not be replayed.")
        }
        if (connected) refresh()
    }
    Connections {
        target: root.viewModel
        function onRuntimeProjectionChanged() { root.refresh() }
    }
    Timer {
        interval: 1200; repeat: true; running: root.visible
        triggeredOnStart: true
        onTriggered: {
            root.refresh()
            if (root.connected && !root.loaded) {
                root.unavailableReads += 1
                root.loadFailed = root.unavailableReads >= 5
            }
            if (root.awaiting && Date.now() >= root.pendingUntil) {
                root.awaiting = false
                root.status = qsTr("Save was not confirmed. Inspect the stored value after reconnecting; no change was replayed.")
            }
        }
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("Chat waiting policy")
        color: SentinelTheme.textPrimary
        font.bold: true
        wrapMode: Text.WordWrap
    }
    Label {
        Layout.fillWidth: true
        text: !root.connected ? qsTr("Connect to the daemon to view the effective policy.") : !root.loaded ? (root.loadFailed ? qsTr("Timeout settings are unavailable from this daemon. Check its version and connection; no preference was changed.") : qsTr("Loading timeout preferences…")) : !root.supported ? qsTr("Adaptive waiting is unavailable for this provider/model. Compatibility remains available. Ollama, Claude/Gemini native and structured Agent requests do not support this profile.") : qsTr("Useful progress means parsed visible text, not network traffic or heartbeats. Stop remains available. Hardware-specific defaults have not been calibrated.")
        color: SentinelTheme.textMuted
        wrapMode: Text.WordWrap
    }
    SentinelComboBox {
        id: mode
        objectName: "inferenceTimeoutMode"
        Layout.fillWidth: true
        model: root.modes
        currentIndex: root.supported ? ["compatibility", "extended", "custom"].indexOf(root.draftMode) : 0
        enabled: root.connected && root.loaded && !root.awaiting
        Accessible.name: qsTr("Chat waiting policy")
        onActivated: function(index) { root.draftMode = ["compatibility", "extended", "custom"][index]; root.dirty = true }
    }
    Label {
        Layout.fillWidth: true
        visible: root.draftMode !== "compatibility"
        text: root.draftMode === "extended" ? qsTr("Choose longer finite limits for this operation. No duration is guessed from your hardware.") : qsTr("Zero disables initial or idle waiting checks. Total must remain finite. Limits use seconds, with up to three decimal places (use a decimal point).")
        color: SentinelTheme.textMuted
        wrapMode: Text.WordWrap
    }
    GridLayout {
        visible: root.draftMode !== "compatibility" && root.supported
        Layout.fillWidth: true
        columns: width < 420 ? 1 : 2
        enabled: root.connected && !root.awaiting
        Label { text: qsTr("Initial response (seconds)"); color: SentinelTheme.textPrimary; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        SentinelTextField {
            id: initial; objectName: "inferenceTimeoutInitial"; Layout.fillWidth: true
            Accessible.name: qsTr("Initial response timeout in seconds")
            validator: DoubleValidator { bottom: 0; top: 2147483.647; decimals: 3; locale: "C"; notation: DoubleValidator.StandardNotation }
            onTextEdited: root.dirty = true
        }
        Label { text: qsTr("No useful progress (seconds)"); color: SentinelTheme.textPrimary; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        SentinelTextField {
            id: idle; objectName: "inferenceTimeoutIdle"; Layout.fillWidth: true
            Accessible.name: qsTr("Useful progress idle timeout in seconds")
            validator: DoubleValidator { bottom: 0; top: 2147483.647; decimals: 3; locale: "C"; notation: DoubleValidator.StandardNotation }
            onTextEdited: root.dirty = true
        }
        Label { text: qsTr("Entire inference (seconds)"); color: SentinelTheme.textPrimary; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        SentinelTextField {
            id: total; objectName: "inferenceTimeoutTotal"; Layout.fillWidth: true
            Accessible.name: qsTr("Total inference timeout in seconds")
            validator: DoubleValidator { bottom: 0; top: 2147483.647; decimals: 3; locale: "C"; notation: DoubleValidator.StandardNotation }
            onTextEdited: root.dirty = true
        }
    }
    Label {
        objectName: "inferenceTimeoutEffective"
        Layout.fillWidth: true
        text: !root.connected ? qsTr("Effective policy is unavailable while disconnected.") : !root.loaded ? "" : root.policy.accepted !== true ? qsTr("The saved policy cannot run on this provider/model or is invalid. Reset to Compatibility or choose a supported text-streaming provider.") : qsTr("Effective for the model selected above — initial: %1 s; no useful progress: %2 s; total: %3 s. Zero means that check is disabled. A shorter configured provider or parent limit takes priority for explicit profiles. Agent classification/planning and approvals retain their separate limits.").arg(Number(root.policy.initialMs) / 1000).arg(Number(root.policy.idleMs) / 1000).arg(Number(root.policy.totalMs) / 1000)
        color: SentinelTheme.textMuted
        wrapMode: Text.WordWrap
    }
    Flow {
        Layout.fillWidth: true
        spacing: SentinelTheme.spaceSm
        SentinelButton {
            objectName: "inferenceTimeoutSave"
            text: root.awaiting ? qsTr("Saving…") : qsTr("Save waiting policy")
            enabled: root.loaded && root.connected && root.dirty && !root.awaiting && (root.draftMode === "compatibility" || root.supported)
            onClicked: root.save(false)
        }
        SentinelButton {
            objectName: "inferenceTimeoutReset"
            text: qsTr("Reset to Compatibility")
            enabled: root.loaded && root.connected && !root.awaiting
            onClicked: root.save(true)
        }
    }
    Label {
        objectName: "inferenceTimeoutStatus"
        Layout.fillWidth: true
        visible: text.length > 0
        text: root.status
        color: SentinelTheme.textPrimary
        wrapMode: Text.WordWrap
        Accessible.role: Accessible.StaticText
    }
}
