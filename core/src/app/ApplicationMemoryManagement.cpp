// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/app/ApplicationController.h"
#include "sentinel/core/app/BackupService.h"
#include "sentinel/core/chat/IConversationStore.h"
#include "sentinel/core/interfaces/IMemoryStore.h"
#include <QJsonDocument>
namespace sentinel::core {
QVariantMap ApplicationController::manageMemory(const QVariantMap& request) {
    const auto input = QJsonObject::fromVariantMap(request);
    const QString tag = input.value("requestTag").toString().left(64);
    const auto answer = [&](const QString& status, QJsonObject data = QJsonObject{}) {
        data.insert("status", status);
        data.insert("requestTag", tag);
        return data.toVariantMap();
    };
    const QSet<QString> allowed{"operation", "scope",    "search",    "cursor",      "id",
                                "revision",  "key",      "value",     "destination", "personal",
                                "legacy",    "session",  "workspace", "confirmed",   "confirmation",
                                "archive",   "conflict", "selected",  "offset",      "requestTag"};
    for (auto it = input.begin(); it != input.end(); ++it) {
        if (!allowed.contains(it.key())) {
            return answer("InvalidInput");
        }
    }
    for (const auto& key :
         {"operation", "search", "cursor", "id", "revision", "key", "value", "session", "workspace",
          "confirmation", "archive", "conflict", "requestTag"}) {
        if (input.contains(key) && !input.value(key).isString()) {
            return answer("InvalidInput");
        }
    }
    for (const auto& key : {"personal", "legacy", "confirmed"}) {
        if (input.contains(key) && !input.value(key).isBool()) {
            return answer("InvalidInput");
        }
    }
    for (const auto& key : {"scope", "destination", "offset"}) {
        if (input.contains(key) && (!input.value(key).isDouble() ||
                                    input.value(key).toDouble() != input.value(key).toInt())) {
            return answer("InvalidInput");
        }
    }
    const int number = input.value("scope").toInt(-1);
    if (number < 0 || number > 3 || input.value("archive").toString().toUtf8().size() > 98304 ||
        input.value("requestTag").toString().size() > 64) {
        return answer("InvalidInput");
    }
    MemoryManagementContext context{currentMemoryAccessContext(), input.value("legacy").toBool()};
    // Only this management call opts in. Chat and Agent sharing policy remains unchanged.
    context.access.includePersonal = input.value("personal").toBool();
    if (!memoryStore_ || !memoryStore_->isAvailable() || !conversationStore_) {
        return answer("Unavailable");
    }
    bool knownSession = false;
    for (const auto& record : conversationStore_->listConversations()) {
        if (record.id == context.access.sessionId) {
            knownSession = true;
            break;
        }
    }
    const auto scope = static_cast<MemoryScope>(number);
    if (!knownSession || !authorizedMemoryScope(scope, context)) {
        return answer("Unauthorized");
    }
    const auto op = input.value("operation").toString();
    const bool reading =
        op == "list" || op == "read" || op == "export" || op == "search" || op == "indexStatus";
    if (!reading && (input.value("session").toString() != context.access.sessionId ||
                     input.value("workspace").toString() != context.access.workspaceId)) {
        return answer("Conflict");
    }
    const auto changed = [&] {
        latestMemoryRecallSummary_ = MemoryRecallSummary{};
        emit memoryEntriesChanged();
        emit memoryRecallChanged();
        emit contextAssemblyChanged();
    };
    if (op == "search" || op == "indexStatus" || op == "refreshIndex") {
        if (scope == MemoryScope::Legacy) {
            return answer("Unauthorized");
        }
        context.access.retrievalScope = scope;
        if (op == "refreshIndex" &&
            (!input.value("confirmed").toBool() || input.value("confirmation") != "REFRESH")) {
            return answer("InvalidInput");
        }
        auto index = op == "refreshIndex"  ? memoryStore_->refreshMemoryIndex(context.access, 100)
                     : op == "indexStatus" ? memoryStore_->memoryIndexStatus(context.access)
                                           : MemoryIndexStatus{};
        QJsonObject data{{"index", memoryIndexStateName(index.state)},
                         {"semantic", "Unavailable"},
                         {"refreshed", index.refreshed},
                         {"session", context.access.sessionId},
                         {"workspace", context.access.workspaceId}};
        if (op == "search") {
            if (input.value("search").toString().size() > 256) {
                return answer("InvalidInput");
            }
            const auto found =
                memoryStore_->retrieveMemory(input.value("search").toString(), context.access, 10);
            QJsonArray rows;
            for (int i = 0; i < found.records.size(); ++i) {
                auto row = managedMemoryJson(found.records[i], true);
                if (i < found.relevance.size()) {
                    const auto rank = found.relevance[i];
                    row["relevance"] = QJsonObject{{"lexicalScore", rank.lexicalScore},
                                                   {"exactKey", rank.exactKey},
                                                   {"phrase", rank.phrase},
                                                   {"matchedTerms", rank.matchedTerms},
                                                   {"totalTerms", rank.totalTerms},
                                                   {"semanticSimilarity", QJsonValue::Null}};
                }
                rows.append(row);
            }
            data["records"] = rows;
            data["nextCursor"] = "";
            data["index"] = memoryIndexStateName(found.index.state);
            data["candidatesExamined"] = found.candidatesExamined;
            data["candidateLimitReached"] = found.candidateLimitReached;
            return answer(found.status == ScopedMemoryStatus::Ready          ? "Ready"
                          : found.status == ScopedMemoryStatus::Unauthorized ? "Unauthorized"
                          : found.status == ScopedMemoryStatus::InvalidInput ? "InvalidInput"
                                                                             : "Unavailable",
                          data);
        }
        return answer(index.state == MemoryIndexState::Unavailable ? "Unavailable" : "Ready", data);
    }
    if (op == "list") {
        auto listingAccess = context.access;
        listingAccess.retrievalScope = scope;
        const auto page = memoryStore_->listManagedMemory(
            {scope, input.value("search").toString(), input.value("cursor").toString(), 10},
            context);
        QJsonArray rows;
        for (const auto& r : page.records) {
            rows.append(managedMemoryJson(r, true));
        }
        return answer(memoryManagementStatusName(page.status),
                      {{"records", rows},
                       {"index", scope == MemoryScope::Legacy
                                     ? QStringLiteral("Unavailable")
                                     : memoryIndexStateName(
                                           memoryStore_->memoryIndexStatus(listingAccess).state)},
                       {"semantic", "Unavailable"},
                       {"nextCursor", page.nextCursor},
                       {"session", context.access.sessionId},
                       {"workspace", context.access.workspaceId}});
    }
    if (op == "export") {
        const auto result = BackupService::exportScopedMemory(*memoryStore_, scope, context);
        return answer(result.succeeded() ? "Ready" : result.detail,
                      {{"archive", QString::fromUtf8(result.data)}});
    }
    if (op == "import") {
        if (!input.value("confirmed").toBool() ||
            !QSet<QString>{"reject", "skipIdentical"}.contains(
                input.value("conflict").toString())) {
            return answer("InvalidInput");
        }
        const auto result = BackupService::importScopedMemory(
            *memoryStore_, scope, context, input.value("archive").toString().toUtf8(),
            input.value("conflict") == "skipIdentical");
        if (result.succeeded()) {
            changed();
        }
        return answer(result.succeeded() ? "Ready" : result.detail);
    }
    if (op == "read") {
        const auto result = memoryStore_->readManagedMemory(input.value("id").toString(), context);
        if (result.status != MemoryManagementStatus::Ready) {
            return answer(memoryManagementStatusName(result.status));
        }
        const auto& r = result.records.first();
        if (r.scope != scope) {
            return answer("Unauthorized");
        }
        const int offset = input.value("offset").toInt();
        if (offset < 0 || offset > r.value.size()) {
            return answer("InvalidInput");
        }
        if (offset > 0 && offset < r.value.size() && r.value.at(offset).isLowSurrogate() &&
            r.value.at(offset - 1).isHighSurrogate()) {
            return answer("InvalidInput");
        }
        int count = qMin(4096, static_cast<int>(r.value.size()) - offset);
        if (count > 0 && offset + count < r.value.size() &&
            r.value.at(offset + count - 1).isHighSurrogate() &&
            r.value.at(offset + count).isLowSurrogate()) {
            --count;
        }
        auto row = managedMemoryJson(r, true);
        row.insert("value", r.value.mid(offset, count));
        row.insert("offset", offset);
        row.insert("corrections", r.corrections);
        return answer("Ready", {{"record", row}});
    }
    QList<MemoryMutation> mutations;
    if (op == "clear" || op == "deleteSelected") {
        if (!input.value("confirmed").toBool() || input.value("confirmation") != "DELETE") {
            return answer("InvalidInput");
        }
        if (op == "clear") {
            const auto result = memoryStore_->clearManagedMemory(scope, context);
            if (result.status == MemoryManagementStatus::Ready) {
                changed();
            }
            return answer(memoryManagementStatusName(result.status));
        } else {
            if (!input.value("selected").isArray() ||
                input.value("selected").toArray().size() > 50) {
                return answer("InvalidInput");
            }
            for (const auto& item : input.value("selected").toArray()) {
                const auto row = item.toObject();
                if (row.size() != 2 || !row.value("id").isString() ||
                    !row.value("revision").isString()) {
                    return answer("InvalidInput");
                }
                const auto current =
                    memoryStore_->readManagedMemory(row.value("id").toString(), context);
                if (current.status != MemoryManagementStatus::Ready) {
                    return answer(memoryManagementStatusName(current.status));
                }
                if (current.records.first().scope != scope) {
                    return answer("Unauthorized");
                }
                MemoryMutation m;
                m.kind = MemoryMutation::Kind::Delete;
                m.record = current.records.first();
                m.expectedRevision = row.value("revision").toString();
                mutations.append(m);
            }
        }
    } else if (op == "create") {
        MemoryMutation m;
        m.kind = MemoryMutation::Kind::Create;
        m.record.scope = scope;
        m.record.key = input.value("key").toString();
        m.record.value = input.value("value").toString();
        mutations.append(m);
    } else if (op == "correct" || op == "delete" || op == "move") {
        if ((op == "delete" || op == "move") &&
            (!input.value("confirmed").toBool() ||
             input.value("confirmation").toString() != (op == "move" ? "MOVE" : "DELETE"))) {
            return answer("InvalidInput");
        }
        const auto current = memoryStore_->readManagedMemory(input.value("id").toString(), context);
        if (current.status != MemoryManagementStatus::Ready) {
            return answer(memoryManagementStatusName(current.status));
        }
        if (current.records.first().scope != scope) {
            return answer("Unauthorized");
        }
        MemoryMutation m;
        m.record = current.records.first();
        m.expectedRevision = input.value("revision").toString();
        if (op == "delete") {
            m.kind = MemoryMutation::Kind::Delete;
        } else if (op == "move") {
            const int dest = input.value("destination").toInt(-1);
            if (dest < 1 || dest > 3) {
                return answer("InvalidInput");
            }
            m.kind = MemoryMutation::Kind::Move;
            m.destination = static_cast<MemoryScope>(dest);
        } else {
            m.kind = MemoryMutation::Kind::Correct;
            m.record.key = input.value("key").toString();
            m.record.value = input.value("value").toString();
        }
        mutations.append(m);
    } else {
        return answer("InvalidInput");
    }
    if (mutations.isEmpty()) {
        return answer("Ready");
    }
    const auto result = memoryStore_->mutateManagedMemory(mutations, context);
    if (result.status == MemoryManagementStatus::Ready) {
        changed();
    }
    return answer(memoryManagementStatusName(result.status));
}
} // namespace sentinel::core
