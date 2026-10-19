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
    property string indexStatus: "Unavailable"
    property bool rankedSearch: false
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
    property bool archiveStatusShown: false
    property bool archiveWasActive: false
    property bool archiveOptions: false
    property var historyRecord: ({})
    property int historyOffset: 0
    property int historyTotal: 0
    property string historyText: ""
    function archiveContext() { return {scope: scope, personal: personalConsent.checked, legacy: scope === 0, session: session, workspace: workspace, maxBytes: maxArchiveMiB.value * 1024 * 1024, maxRecords: maxArchiveRecords.value} }
    readonly property int scope: [1, 2, 3, 0][scopeChoice.currentIndex]
    readonly property var scopeNames: [qsTr("Legacy"), qsTr("Session"), qsTr("Project"), qsTr("Personal")]
    readonly property var provenanceNames: [qsTr("Unknown"), qsTr("User provided"), qsTr("Conversation content"), qsTr("Project derived"), qsTr("Retrieval result"), qsTr("Generated summary"), qsTr("Inferred")]

    function resetScope() {
        if (!initialized) return
        generation++; busy = false; pendingTag = ""; rows = []; indexStatus = "Unavailable"; selected = ({}); cursor = ""; nextCursor = "";
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
    function refresh(after) { cursor = after; request(rankedSearch && scope !== 0 ? "search" : "list", {search: search.text, cursor: after}) }
    function describe(code) {
        if (code === "Ready") return rows.length ? qsTr("Authorized records") : qsTr("No authorized records in this scope.")
        if (code === "Conflict") return qsTr("The record or active context changed. Reload before trying again. No overwrite was applied.")
        if (code === "Unauthorized") return qsTr("This scope or record is not authorized for the active conversation/workspace. Personal management requires explicit consent.")
        if (code === "NotFound") return qsTr("The record is no longer available in this context.")
        if (code === "LimitExceeded") return qsTr("Operation exceeds the bounded limit (500 records / 96 KiB archive or 256 correction receipts; v1 export retains its 64-receipt bound). Nothing was partially applied.")
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
            if (result.index !== undefined) root.indexStatus = result.index
            if (result.status !== "Ready") return
            if (operation === "list" || operation === "search") {
                root.rows = result.records || []; root.nextCursor = result.nextCursor || ""; root.session = result.session; root.workspace = result.workspace; root.selected = ({});
                root.status = root.describe("Ready") + (operation === "search" ? qsTr(" · Lexical ranking; %1 candidates examined%2. Semantic similarity unavailable.").arg(result.candidatesExamined).arg(result.candidateLimitReached ? qsTr(" (candidate cap reached)") : "") : "")
            } else if (operation === "read") {
                const record = result.record
                if (record.revision !== root.editing.revision) { root.status = root.describe("Conflict"); return }
                root.loadedValue += record.value
                if (root.loadedValue.length < record.valueLength) { root.request("read", {id: record.id, offset: root.loadedValue.length}); return }
                root.editing = record; valueField.text = root.loadedValue; editor.open()
            } else if (operation === "history") {
                if(result.revision !== root.historyRecord.revision) { root.status = root.describe("Conflict"); return }
                root.historyText = JSON.stringify(result.entries, null, 2) + "\nHistory SHA-256 (integrity, not authenticity): " + result.historySha256
                root.historyOffset = result.nextOffset; root.historyTotal = result.total; historyDialog.open()
            } else if (operation === "refreshIndex") {
                confirmation.close()
                root.status = qsTr("Index: %1. %2 projection rows refreshed. Repeat a batch if stale; stored memory is unchanged.").arg(result.index).arg(result.refreshed)
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
            ComboBox { id: scopeChoice; objectName: "memoryScopeChoice"; enabled: !root.viewModel.backupManager.memoryTransferActive; width: 150; model: [qsTr("Session"), qsTr("Project"), qsTr("Personal"), qsTr("Legacy")]; onActivated: { personalConsent.checked = false; root.resetScope() } }
            TextField { id: search; objectName: "memorySearch"; width: 220; maximumLength: 256; placeholderText: qsTr("Search this scope"); onAccepted: root.refresh("") }
            Button { objectName: "memoryReload"; text: qsTr("Reload"); enabled: !root.busy && !root.viewModel.backupManager.busy; onClicked: root.refresh("") }
            CheckBox { objectName: "memoryRankedSearch"; text: qsTr("Ranked recall preview"); enabled: !root.busy && !root.viewModel.backupManager.busy && root.scope !== 0; onToggled: { root.rankedSearch = checked; root.refresh("") } }
            Button { objectName: "memoryRefreshIndex"; text: qsTr("Refresh index batch"); enabled: !root.busy && !root.viewModel.backupManager.busy && root.session.length > 0 && root.scope !== 0; onClicked: { root.destructiveOp = "refreshIndex"; confirmation.open() } }
            Button { objectName: "memoryCreate"; text: qsTr("New memory"); enabled: !root.busy && !root.viewModel.backupManager.busy && root.session.length > 0 && root.scope !== 0; onClicked: { root.editing = ({}); keyField.text = ""; valueField.text = ""; editor.open() } }
            CheckBox { text: qsTr("Archive options"); checked: root.archiveOptions; onToggled: root.archiveOptions = checked }
            Button { objectName: "memoryExportV2"; text: qsTr("Export archive…"); enabled: !root.busy && !root.viewModel.backupManager.busy && root.session.length > 0 && !root.viewModel.backupManager.busy && !root.viewModel.backupManager.memoryReady; onClicked: saveLargeArchive.open() }
            Button { objectName: "memoryImportV2"; text: qsTr("Inspect / import…"); enabled: !root.busy && !root.viewModel.backupManager.busy && root.session.length > 0 && !root.viewModel.backupManager.busy && !root.viewModel.backupManager.memoryReady; onClicked: openLargeArchive.open() }
            Button { objectName: "memoryDeleteSelected"; text: qsTr("Delete selected…"); enabled: !root.busy && !root.viewModel.backupManager.busy && Object.keys(root.selected).length > 0; onClicked: { root.destructiveOp = "deleteSelected"; confirmation.open() } }
            Button { objectName: "memoryClearScope"; text: qsTr("Clear this scope…"); enabled: !root.busy && !root.viewModel.backupManager.busy && root.rows.length > 0; onClicked: { root.destructiveOp = "clear"; confirmation.open() } }
        }
        Flow {
            visible: root.archiveOptions || root.viewModel.backupManager.memoryTransferActive
            Layout.fillWidth: true; Layout.preferredHeight: childrenRect.height; spacing: 8
            Button { text: qsTr("Export v1 (small scope)"); enabled: !root.busy && !root.viewModel.backupManager.busy && root.session.length > 0; onClicked: root.request("export", {}) }
            Button { text: qsTr("Import v1 (small scope)…"); enabled: !root.busy && !root.viewModel.backupManager.busy && root.session.length > 0; onClicked: openArchive.open() }
            Label { text: qsTr("Large archive limits (bounded):"); color: SentinelTheme.textMuted }
            SpinBox { id: maxArchiveMiB; from: 1; to: 256; value: 64; editable: true; Accessible.name: qsTr("Maximum archive MiB") }
            Label { text: qsTr("MiB"); color: SentinelTheme.textMuted }
            SpinBox { id: maxArchiveRecords; from: 1; to: 100000; value: 10000; editable: true; Accessible.name: qsTr("Maximum archive records") }
            Button { objectName: "memoryArchiveCancel"; text: qsTr("Cancel archive"); enabled: root.viewModel.backupManager.memoryTransferActive && root.viewModel.backupManager.cancellable; onClicked: root.viewModel.backupManager.cancel() }
        }
        ProgressBar { objectName: "memoryArchiveProgress"; Layout.fillWidth: true; visible: root.viewModel.backupManager.memoryTransferActive; value: root.viewModel.backupManager.progress; indeterminate: root.viewModel.backupManager.busy && value === 0 }
        Label { objectName: "memoryArchiveStatus"; visible: root.archiveOptions || root.archiveStatusShown || root.viewModel.backupManager.memoryTransferActive; Layout.fillWidth: true; textFormat: Text.PlainText; wrapMode: Text.WordWrap; text: root.viewModel.backupManager.status; color: SentinelTheme.textPrimary }
        CheckBox { id: personalConsent; objectName: "memoryPersonalConsent"; visible: root.scope === 3; text: qsTr("I explicitly authorize Personal memory management in this profile (recall stays off)"); onToggled: root.resetScope() }
        Label { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText; color: SentinelTheme.textMuted; text: qsTr("Conversation: %1 · Workspace: %2").arg(root.session || qsTr("Unavailable")).arg(root.workspace || qsTr("None")) }
        Label { objectName: "memoryStatus"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; text: root.status; color: SentinelTheme.textPrimary; Accessible.role: Accessible.AlertMessage }
        Label { objectName: "memoryIndexStatus"; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; text: qsTr("Search index: %1 · Semantic similarity unavailable. %2").arg(root.indexStatus).arg(root.indexStatus === "Ready" || root.indexStatus === "Direct" ? "" : qsTr("Authorized lexical fallback; explicit bounded refresh available.")); color: SentinelTheme.textMuted }
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
                        Label { Layout.fillWidth: true; visible: !!entry.modelData.relevance; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: SentinelTheme.textMuted
                            text: entry.modelData.relevance ? qsTr("Lexical score %1 · Exact key: %2 · Phrase: %3 · Terms: %4/%5 · Semantic: unavailable").arg(entry.modelData.relevance.lexicalScore).arg(entry.modelData.relevance.exactKey).arg(entry.modelData.relevance.phrase).arg(entry.modelData.relevance.matchedTerms).arg(entry.modelData.relevance.totalTerms) : "" }
                        Label { Layout.fillWidth: true; textFormat: Text.PlainText; text: root.provenanceNames[entry.modelData.provenance] + " · " + entry.modelData.id; color: SentinelTheme.textMuted; wrapMode: Text.WrapAnywhere }
                    }
                    Button { objectName: "memoryInspectRecord"; text: qsTr("Inspect / edit"); enabled: !root.busy && !root.viewModel.backupManager.busy; onClicked: root.edit(entry.modelData) }
                }
            }
        }
        RowLayout {
            Button { text: qsTr("First page"); enabled: !root.busy && !root.viewModel.backupManager.busy && root.cursor.length > 0; onClicked: root.refresh("") }
            Button { text: qsTr("Next page"); enabled: !root.busy && !root.viewModel.backupManager.busy && root.nextCursor.length > 0; onClicked: root.refresh(root.nextCursor) }
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
                Button { objectName: "memorySave"; text: qsTr("Save"); enabled: !root.busy && !root.viewModel.backupManager.busy && keyField.text.trim().length > 0 && valueField.text.trim().length > 0 && valueField.text.length <= 65536; onClicked: root.request(root.editing.id ? "correct" : "create", {id: root.editing.id || "", revision: root.editing.revision || "", key: keyField.text, value: valueField.text}) }
                Button { objectName: "memoryHistory"; text: qsTr("History pages…"); visible: !!root.editing.id; enabled: !root.busy && !root.viewModel.backupManager.busy; onClicked: { root.historyRecord = root.editing; root.historyOffset = 0; root.request("history", {id: root.editing.id, offset: 0}) } }
                Button { objectName: "memoryDeleteOne"; text: qsTr("Delete…"); visible: !!root.editing.id; enabled: !root.busy && !root.viewModel.backupManager.busy; onClicked: { root.destructiveOp = "delete"; confirmation.open() } }
                Button { text: qsTr("Change scope…"); visible: !!root.editing.id; enabled: !root.busy && !root.viewModel.backupManager.busy; onClicked: { root.destructiveOp = "move"; confirmation.open() } }
            }
            Label { Layout.fillWidth: true; text: root.status; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: SentinelTheme.textPrimary }
        }
    }
    Dialog {
        id: confirmation; objectName: "memoryDestructiveConfirmation"; parent: Overlay.overlay; anchors.centerIn: parent; modal: true
        width: Math.min(560, root.width - 24); title: root.destructiveOp === "refreshIndex" ? qsTr("Confirm bounded index refresh") : root.destructiveOp === "move" ? qsTr("Explicit scope migration") : qsTr("Confirm deletion")
        onOpened: { confirmText.text = ""; movePersonal.checked = false; cancelDestructive.forceActiveFocus(Qt.PopupFocusReason) }
        onClosed: { if (editor.visible) keyField.forceActiveFocus(Qt.PopupFocusReason); else scopeChoice.forceActiveFocus(Qt.PopupFocusReason) }
        contentItem: ColumnLayout {
            Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; color: SentinelTheme.textPrimary
                text: root.destructiveOp === "refreshIndex" ? qsTr("Refresh only the authorized scope index in bounded batches. No memory content is changed, no embeddings are generated. Type REFRESH to proceed.") : qsTr("Scope: %1. This action affects stored memory only. It does not retract data sent to providers, erase historical backups or promise physical secure erasure. A scope move requires both source and destination authorization.").arg(root.scopeNames[root.scope]) }
            ComboBox { id: destination; visible: root.destructiveOp === "move"; model: [qsTr("Session"), qsTr("Project"), qsTr("Personal")] }
            CheckBox { id: movePersonal; visible: root.destructiveOp === "move" && destination.currentIndex === 2; text: qsTr("Explicitly authorize this Personal destination only") }
            TextField { id: confirmText; objectName: "memoryConfirmationText"; Layout.fillWidth: true; placeholderText: qsTr("Type DELETE, MOVE or REFRESH for the selected action") }
            RowLayout {
                Button { id: cancelDestructive; objectName: "memoryCancelDestructive"; text: qsTr("Cancel"); onClicked: confirmation.close() }
                Button { objectName: "memoryConfirmDestructive"; text: qsTr("Confirm"); enabled: !root.busy && !root.viewModel.backupManager.busy && confirmText.text === (root.destructiveOp === "refreshIndex" ? "REFRESH" : root.destructiveOp === "move" ? "MOVE" : "DELETE") && (root.destructiveOp !== "move" || destination.currentIndex !== 2 || movePersonal.checked)
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
            RowLayout { Button { id: importCancel; text: qsTr("Cancel"); onClicked: importConfirm.close() } Button { text: qsTr("Import"); enabled: !root.busy && !root.viewModel.backupManager.busy; onClicked: root.request("import", {archive: root.importData, confirmed: true, conflict: conflictChoice.currentIndex === 0 ? "reject" : "skipIdentical"}) } }
            Label { Layout.fillWidth: true; text: root.status; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: SentinelTheme.textPrimary }
        }
    }
    FileDialog { id: saveLargeArchive; title: qsTr("Export consistent scope archive to a NEW file"); fileMode: FileDialog.SaveFile; nameFilters: [qsTr("Sentinel memory archive (*.smem)")]; onAccepted: { root.archiveStatusShown = true; root.viewModel.backupManager.exportMemoryFile(selectedFile, root.archiveContext()) } }
    FileDialog { id: openLargeArchive; title: qsTr("Inspect scoped memory archive v1 or v2"); fileMode: FileDialog.OpenFile; nameFilters: [qsTr("Sentinel memory archives (*.smem *.json)")]; onAccepted: { root.archiveStatusShown = true; root.viewModel.backupManager.inspectMemoryFile(selectedFile, root.archiveContext()) } }
    Connections { target: root.viewModel.backupManager
        function onChanged() {
            const active = root.viewModel.backupManager.memoryTransferActive
            if (active) { root.archiveWasActive = true; root.archiveStatusShown = true }
            if (root.archiveWasActive && !active) { root.archiveWasActive = false; if(root.visible && !root.busy) root.refresh(""); Qt.callLater(function() { if(root.visible) scopeChoice.forceActiveFocus(Qt.PopupFocusReason) }) }
            if (root.visible && root.viewModel.backupManager.memoryReady && !largeImportConfirm.visible) largeImportConfirm.open()
        }
    }
    Dialog { id: largeImportConfirm; objectName: "memoryLargeImportConfirm"; parent: Overlay.overlay; modal: true; anchors.centerIn: parent; width: Math.min(600, root.width - 24); title: qsTr("Confirm scoped archive import")
        onOpened: largeImportCancel.forceActiveFocus(Qt.PopupFocusReason)
        onClosed: { scopeChoice.forceActiveFocus(Qt.PopupFocusReason); if(root.viewModel.backupManager.memoryReady && !root.viewModel.backupManager.busy) root.viewModel.backupManager.cancel() }
        contentItem: ColumnLayout {
            Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; text: root.viewModel.backupManager.preview; color: SentinelTheme.textPrimary }
            ComboBox { id: largeConflict; model: [qsTr("Reject duplicate UUIDs"), qsTr("Skip identical; reject differing records")] }
            Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; text: qsTr("Merge only: existing differing records are never replaced. Import attribution remains unverified historical data."); color: SentinelTheme.textMuted }
            RowLayout {
                Button { id: largeImportCancel; objectName: "memoryLargeImportCancel"; text: qsTr("Cancel"); onClicked: { root.viewModel.backupManager.cancel(); largeImportConfirm.close() } }
                Button { objectName: "memoryLargeImportApply"; text: qsTr("Import atomically"); enabled: root.viewModel.backupManager.memoryReady && !root.viewModel.backupManager.busy; onClicked: { root.viewModel.backupManager.restoreMemoryFile(largeConflict.currentIndex === 1); largeImportConfirm.close() } }
            }
        }
    }
    Dialog { id: historyDialog; objectName: "memoryHistoryDialog"; parent: Overlay.overlay; modal: true; anchors.centerIn: parent; width: Math.min(700,root.width-24); height: Math.min(550,root.height-24); title: qsTr("Correction / import history (%1 receipts)").arg(root.historyTotal)
        onOpened: historyClose.forceActiveFocus(Qt.PopupFocusReason)
        onClosed: { if(editor.visible) keyField.forceActiveFocus(Qt.PopupFocusReason); else scopeChoice.forceActiveFocus(Qt.PopupFocusReason) }
        contentItem: ColumnLayout {
            ScrollView { Layout.fillWidth: true; Layout.fillHeight: true; TextArea { readOnly: true; textFormat: TextEdit.PlainText; wrapMode: TextEdit.WrapAnywhere; text: root.historyText } }
            RowLayout { Button { id: historyClose; objectName: "memoryHistoryClose"; text: qsTr("Close"); onClicked: historyDialog.close() } Button { text: qsTr("Next 16 receipts"); enabled: !root.busy && !root.viewModel.backupManager.busy && root.historyOffset < root.historyTotal; onClicked: root.request("history",{id: root.historyRecord.id,offset:root.historyOffset}) } }
        }
    }

}
