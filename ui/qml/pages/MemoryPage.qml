// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import Sentinel.Desktop

FocusScope {
    id: root
    objectName: "memoryManagementPage"
    required property var viewModel
    property var rows: []
    property var selected: ({})
    property string cursor: ""
    property string nextCursor: ""
    property string session: ""
    property string workspace: ""
    property string status: ""
    property string pendingTag: ""
    property string pendingOp: ""
    property int generation: 0
    property bool busy: false
    property bool initialized: false
    readonly property string activeConversation: viewModel.activeConversationId || ""
    readonly property string activeWorkspace: viewModel.selectedWorkspaceId || ""
    onActiveConversationChanged: if (initialized) resetScope()
    onActiveWorkspaceChanged: if (initialized) resetScope()
    Component.onCompleted: { initialized = true; if (visible) resetScope() }
    property var editing: ({})
    property string loadedValue: ""
    property string exportData: ""
    property string importData: ""
    property int importCount: 0
    property string destructiveOp: ""
    readonly property int scope: [1, 2, 3, 0][scopeChoice.currentIndex]
    readonly property var scopeNames: [qsTr("Legacy"), qsTr("Session"), qsTr("Project"), qsTr("Personal")]
    readonly property var provenanceNames: [qsTr("Unknown"), qsTr("User provided"), qsTr("Conversation content"), qsTr("Project derived"), qsTr("Retrieval result"), qsTr("Generated summary"), qsTr("Inferred")]

    function resetScope() {
        if (!initialized) return
        generation++; busy = false; pendingTag = ""; rows = []; selected = ({}); cursor = ""; nextCursor = "";
        session = ""; workspace = ""; editor.close(); confirmation.close(); importConfirm.close();
        if (visible) refresh("")
    }
    function request(operation, extra) {
        if (busy) return
        generation++; pendingTag = String(generation); pendingOp = operation; busy = true; status = qsTr("Loading…")
        let payload = {operation: operation, scope: scope, personal: personalConsent.checked || (operation === "move" && movePersonal.checked), legacy: scope === 0,
            session: session, workspace: workspace, requestTag: pendingTag}
        for (let key in extra) payload[key] = extra[key]
        viewModel.manageMemory(payload)
    }
    function refresh(after) { cursor = after; request("list", {search: search.text, cursor: after}) }
    function describe(code) {
        if (code === "Ready") return rows.length ? qsTr("Authorized records") : qsTr("No authorized records in this scope.")
        if (code === "Conflict") return qsTr("The record or active context changed. Reload before trying again. No overwrite was applied.")
        if (code === "Unauthorized") return qsTr("This scope or record is not authorized for the active conversation/workspace. Personal management requires explicit consent.")
        if (code === "NotFound") return qsTr("The record is no longer available in this context.")
        if (code === "LimitExceeded") return qsTr("Operation exceeds the bounded limit (500 records / 96 KiB archive or 64 correction receipts). Nothing was partially applied.")
        if (code === "Unavailable") return qsTr("Memory storage or the daemon is unavailable. Reconnect and reload.")
        return qsTr("Invalid or unsupported data. Check the fields, archive version and integrity. Nothing was partially applied.")
    }
    function edit(row) {
        editing = row; loadedValue = ""; keyField.text = row.key; valueField.text = "";
        request("read", {id: row.id, offset: 0})
    }
    Connections {
        target: root.viewModel
        function onMemoryManagementCompleted(result) {
            if (result.requestTag !== root.pendingTag) return
            root.busy = false
            const operation = root.pendingOp
            root.status = root.describe(result.status)
            if (result.status !== "Ready") return
            if (operation === "list") {
                root.rows = result.records || []; root.nextCursor = result.nextCursor || ""; root.session = result.session; root.workspace = result.workspace; root.selected = ({});
                root.status = root.describe("Ready")
            } else if (operation === "read") {
                const record = result.record
                if (record.revision !== root.editing.revision) { root.status = root.describe("Conflict"); return }
                root.loadedValue += record.value
                if (root.loadedValue.length < record.valueLength) { root.request("read", {id: record.id, offset: root.loadedValue.length}); return }
                root.editing = record; valueField.text = root.loadedValue; editor.open()
            } else if (operation === "export") {
                root.exportData = result.archive; saveArchive.open(); root.status = qsTr("Choose where to save this scope archive.")
            } else {
                editor.close(); confirmation.close(); importConfirm.close(); root.importData = ""; root.refresh("")
            }
        }

    }
    onVisibleChanged: if (initialized && visible) resetScope()
    ColumnLayout {
        anchors.fill: parent
        spacing: 10
        Label { text: qsTr("Memory"); font.pixelSize: 24; color: SentinelTheme.textPrimary }
        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; color: SentinelTheme.textMuted
            text: qsTr("Manage data for the active conversation and verified workspace. Memory is untrusted historical data. Personal management does not enable Personal recall. Legacy records remain excluded until explicitly reclassified.") }
        Flow {
            Layout.fillWidth: true; Layout.preferredHeight: childrenRect.height; spacing: 8
            ComboBox { id: scopeChoice; objectName: "memoryScopeChoice"; width: 150; model: [qsTr("Session"), qsTr("Project"), qsTr("Personal"), qsTr("Legacy")]; onActivated: { personalConsent.checked = false; root.resetScope() } }
            TextField { id: search; objectName: "memorySearch"; width: 220; maximumLength: 256; placeholderText: qsTr("Search this scope"); onAccepted: root.refresh("") }
            Button { objectName: "memoryReload"; text: qsTr("Reload"); enabled: !root.busy; onClicked: root.refresh("") }
            Button { objectName: "memoryCreate"; text: qsTr("New memory"); enabled: !root.busy && root.session.length > 0 && root.scope !== 0; onClicked: { root.editing = ({}); keyField.text = ""; valueField.text = ""; editor.open() } }
            Button { text: qsTr("Export this scope"); enabled: !root.busy && root.session.length > 0; onClicked: root.request("export", {}) }
            Button { text: qsTr("Import archive…"); enabled: !root.busy && root.session.length > 0; onClicked: openArchive.open() }
            Button { objectName: "memoryDeleteSelected"; text: qsTr("Delete selected…"); enabled: !root.busy && Object.keys(root.selected).length > 0; onClicked: { root.destructiveOp = "deleteSelected"; confirmation.open() } }
            Button { objectName: "memoryClearScope"; text: qsTr("Clear this scope…"); enabled: !root.busy && root.rows.length > 0; onClicked: { root.destructiveOp = "clear"; confirmation.open() } }
        }
        CheckBox { id: personalConsent; objectName: "memoryPersonalConsent"; visible: root.scope === 3; text: qsTr("I explicitly authorize Personal memory management in this profile (recall stays off)"); onToggled: root.resetScope() }
        Label { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText; color: SentinelTheme.textMuted; text: qsTr("Conversation: %1 · Workspace: %2").arg(root.session || qsTr("Unavailable")).arg(root.workspace || qsTr("None")) }
        Label { objectName: "memoryStatus"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; text: root.status; color: SentinelTheme.textPrimary; Accessible.role: Accessible.AlertMessage }
        BusyIndicator { running: root.busy; visible: root.busy; Layout.preferredHeight: 24 }
        ListView {
            id: list
            objectName: "memoryRecords"
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 6
            model: root.rows
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: entry
                required property var modelData
                width: list.width; height: detail.implicitHeight + 20; color: SentinelTheme.surface; radius: 8
                RowLayout {
                    anchors.fill: parent; anchors.margins: 10
                    CheckBox {
                        objectName: "memorySelectRecord"
                        Accessible.name: qsTr("Select %1").arg(entry.modelData.key)
                        checked: !!root.selected[entry.modelData.id]
                        onToggled: { let next = Object.assign({}, root.selected); if (checked) next[entry.modelData.id] = entry.modelData.revision; else delete next[entry.modelData.id]; root.selected = next }
                    }
                    ColumnLayout {
                        id: detail; Layout.fillWidth: true
                        Label { Layout.fillWidth: true; textFormat: Text.PlainText; text: entry.modelData.key; color: SentinelTheme.textPrimary; wrapMode: Text.WrapAnywhere; font.bold: true }
                        Label { Layout.fillWidth: true; textFormat: Text.PlainText; text: entry.modelData.value; maximumLineCount: 2; elide: Text.ElideRight; color: SentinelTheme.textMuted; wrapMode: Text.WrapAnywhere }
                        Label { Layout.fillWidth: true; textFormat: Text.PlainText; text: root.provenanceNames[entry.modelData.provenance] + " · " + entry.modelData.id; color: SentinelTheme.textMuted; wrapMode: Text.WrapAnywhere }
                    }
                    Button { objectName: "memoryInspectRecord"; text: qsTr("Inspect / edit"); enabled: !root.busy; onClicked: root.edit(entry.modelData) }
                }
            }
        }
        RowLayout {
            Button { text: qsTr("First page"); enabled: !root.busy && root.cursor.length > 0; onClicked: root.refresh("") }
            Button { text: qsTr("Next page"); enabled: !root.busy && root.nextCursor.length > 0; onClicked: root.refresh(root.nextCursor) }
            Label { text: qsTr("10 records per page"); color: SentinelTheme.textMuted }
        }
    }
    Dialog {
        id: editor; objectName: "memoryEditor"; parent: Overlay.overlay; modal: true
        width: Math.min(760, root.width - 24); height: Math.min(650, root.height - 24); anchors.centerIn: parent
        title: root.editing.id ? qsTr("Inspect / correct memory") : qsTr("Create %1 memory").arg(root.scopeNames[root.scope])
        onOpened: keyField.forceActiveFocus(Qt.PopupFocusReason)
        onClosed: scopeChoice.forceActiveFocus(Qt.PopupFocusReason)
        contentItem: ColumnLayout {
            ScrollView { Layout.fillWidth: true; Layout.preferredHeight: 150; visible: !!root.editing.id
                TextArea { readOnly: true; textFormat: TextEdit.PlainText; wrapMode: TextEdit.WrapAnywhere; selectByMouse: true
                text: qsTr("ID: %1\nOwner: %2\nWorkspace: %3\nProvenance: %4\nSource: %5\nCreated: %6\nModified: %7\nRecall eligible: %8\nCorrection / import receipts: %9").arg(root.editing.id || "").arg(root.editing.owner || "").arg(root.editing.workspace || "").arg(root.provenanceNames[root.editing.provenance] || "").arg(root.editing.source || "").arg(root.editing.created || qsTr("Unknown")).arg(root.editing.updated || qsTr("Unknown")).arg(root.editing.recallEligible ? qsTr("Yes, subject to scope/budget") : qsTr("No")).arg(JSON.stringify(root.editing.corrections || []))
                }
            }
            TextField { id: keyField; objectName: "memoryKey"; Layout.fillWidth: true; maximumLength: 256; placeholderText: qsTr("Key") }
            ScrollView { Layout.fillWidth: true; Layout.fillHeight: true; TextArea { id: valueField; objectName: "memoryValue"; textFormat: TextEdit.PlainText; wrapMode: TextEdit.WrapAnywhere; selectByMouse: true; placeholderText: qsTr("Stored value (plain text)") } }
            Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; text: qsTr("A correction preserves original provenance and source, with a separate user correction receipt."); color: SentinelTheme.textMuted }
            Flow {
                Layout.fillWidth: true; Layout.preferredHeight: childrenRect.height; spacing: 8
                Button { text: qsTr("Cancel"); onClicked: editor.close() }
                Button { objectName: "memorySave"; text: qsTr("Save"); enabled: !root.busy && keyField.text.trim().length > 0 && valueField.text.trim().length > 0 && valueField.text.length <= 65536; onClicked: root.request(root.editing.id ? "correct" : "create", {id: root.editing.id || "", revision: root.editing.revision || "", key: keyField.text, value: valueField.text}) }
                Button { objectName: "memoryDeleteOne"; text: qsTr("Delete…"); visible: !!root.editing.id; enabled: !root.busy; onClicked: { root.destructiveOp = "delete"; confirmation.open() } }
                Button { text: qsTr("Change scope…"); visible: !!root.editing.id; enabled: !root.busy; onClicked: { root.destructiveOp = "move"; confirmation.open() } }
            }
            Label { Layout.fillWidth: true; text: root.status; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: SentinelTheme.textPrimary }
        }
    }
    Dialog {
        id: confirmation; objectName: "memoryDestructiveConfirmation"; parent: Overlay.overlay; anchors.centerIn: parent; modal: true
        width: Math.min(560, root.width - 24); title: root.destructiveOp === "move" ? qsTr("Explicit scope migration") : qsTr("Confirm deletion")
        onOpened: { confirmText.text = ""; movePersonal.checked = false; cancelDestructive.forceActiveFocus(Qt.PopupFocusReason) }
        onClosed: { if (editor.visible) keyField.forceActiveFocus(Qt.PopupFocusReason); else scopeChoice.forceActiveFocus(Qt.PopupFocusReason) }
        contentItem: ColumnLayout {
            Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; color: SentinelTheme.textPrimary
                text: qsTr("Scope: %1. This action affects stored memory only. It does not retract data sent to providers, erase historical backups or promise physical secure erasure. A scope move requires both source and destination authorization.").arg(root.scopeNames[root.scope]) }
            ComboBox { id: destination; visible: root.destructiveOp === "move"; model: [qsTr("Session"), qsTr("Project"), qsTr("Personal")] }
            CheckBox { id: movePersonal; visible: root.destructiveOp === "move" && destination.currentIndex === 2; text: qsTr("Explicitly authorize this Personal destination only") }
            TextField { id: confirmText; objectName: "memoryConfirmationText"; Layout.fillWidth: true; placeholderText: qsTr("Type DELETE to delete, or MOVE to change scope") }
            RowLayout {
                Button { id: cancelDestructive; objectName: "memoryCancelDestructive"; text: qsTr("Cancel"); onClicked: confirmation.close() }
                Button { objectName: "memoryConfirmDestructive"; text: qsTr("Confirm"); enabled: !root.busy && confirmText.text === (root.destructiveOp === "move" ? "MOVE" : "DELETE") && (root.destructiveOp !== "move" || destination.currentIndex !== 2 || movePersonal.checked)
                    onClicked: {
                        let extra = {confirmed: true, confirmation: confirmText.text, id: root.editing.id || "", revision: root.editing.revision || "", destination: destination.currentIndex + 1}
                        if (root.destructiveOp === "deleteSelected") extra.selected = Object.keys(root.selected).map(function(id) { return {id: id, revision: root.selected[id]} })
                        root.request(root.destructiveOp, extra)
                    }
                }
            }
            Label { Layout.fillWidth: true; text: root.status; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: SentinelTheme.textPrimary }
        }
    }
    FileDialog { id: saveArchive; title: qsTr("Export authorized memory scope"); fileMode: FileDialog.SaveFile; nameFilters: [qsTr("Sentinel memory archive (*.json)")]; onAccepted: root.status = root.viewModel.backupManager.saveMemoryArchive(selectedFile, root.exportData) ? qsTr("Scope archive saved.") : qsTr("Archive could not be saved. No full backup was claimed.") }
    FileDialog { id: openArchive; title: qsTr("Import memory archive into selected scope"); fileMode: FileDialog.OpenFile; nameFilters: [qsTr("Sentinel memory archive (*.json)")]; onAccepted: { const result = root.viewModel.backupManager.loadMemoryArchive(selectedFile); root.status = root.describe(result.status); if (result.status === "Ready") { root.importData = result.archive; root.importCount = result.count; importConfirm.open() } } }
    Dialog {
        id: importConfirm; parent: Overlay.overlay; modal: true; anchors.centerIn: parent; width: Math.min(540, root.width - 24); title: qsTr("Import authorized memory")
        onOpened: importCancel.forceActiveFocus(Qt.PopupFocusReason)
        onClosed: scopeChoice.forceActiveFocus(Qt.PopupFocusReason)
        contentItem: ColumnLayout {
            Label { Layout.fillWidth: true; textFormat: Text.PlainText; wrapMode: Text.WordWrap; text: qsTr("Import %1 records into %2 only. Serialized owners must match this context. Archive provenance is unverified imported data; no commands or paths are executed. Conflicting edits are never overwritten.").arg(root.importCount).arg(root.scopeNames[root.scope]); color: SentinelTheme.textPrimary }
            ComboBox { id: conflictChoice; model: [qsTr("Reject all duplicate UUIDs"), qsTr("Skip identical records; reject conflicts")] }
            RowLayout { Button { id: importCancel; text: qsTr("Cancel"); onClicked: importConfirm.close() } Button { text: qsTr("Import"); enabled: !root.busy; onClicked: root.request("import", {archive: root.importData, confirmed: true, conflict: conflictChoice.currentIndex === 0 ? "reject" : "skipIdentical"}) } }
            Label { Layout.fillWidth: true; text: root.status; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: SentinelTheme.textPrimary }
        }
    }
}
