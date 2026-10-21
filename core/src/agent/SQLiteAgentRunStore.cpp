// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/agent/SQLiteAgentRunStore.h"
#include "sentinel/core/agent/ClaimGroundingResolver.h"
#include "sentinel/core/memory/SqlitePragmas.h"
#include "sentinel/core/security/AuthorizationResolver.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <algorithm>

namespace sentinel::core {
namespace {

struct Connection {
    QString name = QStringLiteral("sentinel_run_%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    QSqlDatabase db;
    explicit Connection(const QString& path, bool readOnly = false) {
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (readOnly) {
            db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        }
        if (db.open()) {
            QSqlQuery pragma(db);
            pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
            pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"));
        }
    }
    ~Connection() {
        db = {};
        QSqlDatabase::removeDatabase(name);
    }
};

QString timeText(const QDateTime& value) {
    return value.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime parsedTime(const QVariant& value) {
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}

QString bounded(const QString& value, int length) {
    QString text = value.simplified().left(length);
    static const QRegularExpression assignment(
        QStringLiteral("(?i)\\b(api[_ -]?key|access[_ -]?token|password|secret)\\s*[:=]\\s*[^\\s,;]+"));
    text.replace(assignment, QStringLiteral("[credential redacted]"));
    static const QRegularExpression providerKey(QStringLiteral("\\bsk-[A-Za-z0-9_-]{12,}\\b"));
    text.replace(providerKey, QStringLiteral("[credential redacted]"));
    return text;
}

bool apply(QSqlDatabase& db, const QString& sql, const QVariantList& values, QString& error) {
    QSqlQuery query(db);
    if (!query.prepare(sql)) {
        error = query.lastError().text();
        return false;
    }
    for (const auto& value : values)
        query.addBindValue(value);
    if (!query.exec()) {
        error = query.lastError().text();
        return false;
    }
    return true;
}

StoredAgentRun readRun(const QSqlQuery& query) {
    StoredAgentRun run{query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
            query.value(3).toString(), query.value(4).toString(), query.value(5).toString(),
            query.value(6).toString(), query.value(7).toString(), query.value(8).toString(),
            query.value(9).toString(), query.value(10).toString(), parsedTime(query.value(11)),
            parsedTime(query.value(12)), query.value(13).toString(), query.value(14).toInt(),
            query.value(15).toInt(), query.value(16).toInt(), query.value(17).toBool(),
            query.value(18).toInt(), query.value(19).toString(), query.value(20).toString(),
            query.value(21).toString(), query.value(22).toInt(), query.value(23).toInt(),
            query.value(24).toBool(), query.value(25).toString(), query.value(26).toString(),
            query.value(27).toString(), query.value(28).toInt()};
    run.workspaceId = query.value(29).toString();
    run.workspaceName = query.value(30).toString();
    run.presetId = query.value(31).toString();
    run.profileVersion = query.value(32).toString();
    return run;
}

const QString runColumns = QStringLiteral(
    "run_id,session_id,parent_run_id,parent_tool_call_id,run_type,state,provider_id,model_id,"
    "goal_summary,final_answer,failure,started_at,finished_at,grounding_mode,context_tokens,"
    "context_items,context_omitted,context_compacted,"
    "(SELECT COUNT(*) FROM agent_steps s WHERE s.run_id=agent_runs.run_id),"
    "capability_snapshot,role,provider_error_category,provider_http_status,"
    "provider_attempts,provider_retry_occurred,provider_request_lifecycle,terminal_reason,"
    "delegation_purpose,provider_recovery_attempts,workspace_id,workspace_name,preset_id,profile_version");

QString lifecycleName(ChatRequestLifecycle lifecycle) {
    switch (lifecycle) {
    case ChatRequestLifecycle::Pending: return QStringLiteral("Pending");
    case ChatRequestLifecycle::Running: return QStringLiteral("Running");
    case ChatRequestLifecycle::Completed: return QStringLiteral("Completed");
    case ChatRequestLifecycle::Cancelled: return QStringLiteral("Cancelled");
    case ChatRequestLifecycle::TimedOut: return QStringLiteral("TimedOut");
    case ChatRequestLifecycle::RateLimited: return QStringLiteral("RateLimited");
    case ChatRequestLifecycle::Failed: return QStringLiteral("Failed");
    }
    return QStringLiteral("Failed");
}

bool ensureColumn(QSqlDatabase& db, const QString& table, const QString& column,
                  const QString& declaration, QString& error) {
    QSqlQuery info(db);
    if (!info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        error = info.lastError().text();
        return false;
    }
    while (info.next())
        if (info.value(1).toString() == column)
            return true;
    info.finish();
    QSqlQuery alter(db);
    if (!alter.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2").arg(table, declaration))) {
        error = alter.lastError().text();
        return false;
    }
    return true;
}

} // namespace

SQLiteAgentRunStore::SQLiteAgentRunStore(QString databasePath,
                                         std::shared_ptr<StorageAdmission> admission,
                                         std::uint64_t generation)
    : admission_(std::move(admission)), generation_(generation   ? generation
                                                    : admission_ ? admission_->snapshot().generation
                                                                 : 0),
      databasePath_(std::move(databasePath)) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return;
    }
    ready_ = initialize();
}

StorageAdmission::Writer SQLiteAgentRunStore::admitWrite() {
    if (!admission_) {
        return {};
    }
    auto writer = admission_->admit(generation_);
    if (!writer) {
        lastError_ = QStringLiteral("Storage admission refused (busy, stale or shutdown).");
    }
    return writer;
}

bool SQLiteAgentRunStore::initialize() {
    QFileInfo file(databasePath_);
    if (!QDir().mkpath(file.absolutePath())) {
        lastError_ = QStringLiteral("Agent history directory is unavailable.");
        return false;
    }
    Connection connection(databasePath_);
    if (!connection.db.isOpen()) {
        lastError_ = connection.db.lastError().text();
        return false;
    }
    applySqlitePerformancePragmas(connection.db);
    QSqlQuery versionCheck(connection.db);
    if (!versionCheck.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' "
                                          "AND name='agent_run_schema_metadata'"))) {
        lastError_ = versionCheck.lastError().text();
        return false;
    }
    const bool hasMetadata = versionCheck.next();
    versionCheck.finish();
    if (hasMetadata) {
        if (!versionCheck.exec(QStringLiteral("SELECT value FROM agent_run_schema_metadata "
                                               "WHERE key='schema_version'"))) {
            lastError_ = versionCheck.lastError().text();
            return false;
        }
        if (versionCheck.next() && versionCheck.value(0).toInt() > 7) {
            lastError_ = QStringLiteral("Unsupported Agent run schema version.");
            return false;
        }
        versionCheck.finish();
    }
    const QStringList schema{
        QStringLiteral("CREATE TABLE IF NOT EXISTS agent_run_schema_metadata("
                       "key TEXT PRIMARY KEY,value INTEGER NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS agent_runs("
                       "run_id TEXT PRIMARY KEY,session_id TEXT NOT NULL,parent_run_id TEXT,"
                       "parent_tool_call_id TEXT,run_type TEXT NOT NULL,started_at TEXT NOT NULL,"
                       "finished_at TEXT,state TEXT NOT NULL,provider_id TEXT,model_id TEXT,"
                       "goal_summary TEXT,final_answer TEXT,failure TEXT,grounding_mode TEXT,"
                       "context_tokens INTEGER,context_items INTEGER,context_omitted INTEGER,"
                       "context_compacted INTEGER,capability_snapshot TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS agent_steps("
                       "step_id TEXT PRIMARY KEY,run_id TEXT NOT NULL REFERENCES agent_runs(run_id) "
                       "ON DELETE CASCADE,sequence INTEGER NOT NULL,step_type TEXT NOT NULL,"
                       "status TEXT NOT NULL,started_at TEXT NOT NULL,finished_at TEXT,"
                       "tool_call_id TEXT,observation_kind INTEGER,filesystem_failure INTEGER)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS agent_tool_calls("
                       "tool_call_id TEXT PRIMARY KEY,step_id TEXT NOT NULL REFERENCES agent_steps(step_id) "
                       "ON DELETE CASCADE,tool_id TEXT NOT NULL,source TEXT,status TEXT NOT NULL,"
                       "resource_summary TEXT,observation_summary TEXT,started_at TEXT NOT NULL,"
                       "finished_at TEXT,failure_category TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS agent_requirements("
                       "run_id TEXT NOT NULL REFERENCES agent_runs(run_id) ON DELETE CASCADE,"
                       "claim_id TEXT,domain TEXT,resource_summary TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS agent_evidence("
                       "run_id TEXT NOT NULL REFERENCES agent_runs(run_id) ON DELETE CASCADE,"
                       "tool_call_id TEXT,domain TEXT,resource_summary TEXT,outcome INTEGER)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS agent_claims("
                       "run_id TEXT NOT NULL REFERENCES agent_runs(run_id) ON DELETE CASCADE,"
                       "claim_id TEXT NOT NULL,assertion_value INTEGER NOT NULL,verdict INTEGER NOT NULL,"
                       "resource_summary TEXT,"
                       "supporting_tool_calls TEXT,PRIMARY KEY(run_id,claim_id))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS agent_authorizations("
                       "tool_call_id TEXT NOT NULL REFERENCES agent_tool_calls(tool_call_id) "
                       "ON DELETE CASCADE,domain TEXT NOT NULL,access TEXT NOT NULL,"
                       "resource_summary TEXT,decision TEXT)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS agent_runs_recent ON agent_runs(started_at DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS agent_runs_session ON agent_runs(session_id,started_at DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS agent_runs_parent ON agent_runs(parent_run_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS agent_runs_state ON agent_runs(state)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS agent_steps_run ON agent_steps(run_id,sequence)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS agent_tool_calls_step ON agent_tool_calls(step_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS agent_requirements_run ON agent_requirements(run_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS agent_evidence_run ON agent_evidence(run_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS agent_authorizations_call ON agent_authorizations(tool_call_id)")};
    QSqlQuery query(connection.db);
    for (const auto& sql : schema)
        if (!query.exec(sql)) {
            lastError_ = query.lastError().text();
            return false;
        }
    if (!ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("capability_snapshot"),
                      QStringLiteral("capability_snapshot TEXT"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("role"), QStringLiteral("role TEXT"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("provider_error_category"),
                      QStringLiteral("provider_error_category TEXT"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("provider_http_status"),
                      QStringLiteral("provider_http_status INTEGER"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("provider_attempts"),
                      QStringLiteral("provider_attempts INTEGER"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("provider_retry_occurred"),
                      QStringLiteral("provider_retry_occurred INTEGER"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("provider_request_lifecycle"),
                      QStringLiteral("provider_request_lifecycle TEXT"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("terminal_reason"),
                      QStringLiteral("terminal_reason TEXT"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("delegation_purpose"),
                      QStringLiteral("delegation_purpose TEXT"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("provider_recovery_attempts"),
                      QStringLiteral("provider_recovery_attempts INTEGER"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("workspace_id"), QStringLiteral("workspace_id TEXT"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("workspace_name"), QStringLiteral("workspace_name TEXT"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("preset_id"), QStringLiteral("preset_id TEXT"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_runs"),
                      QStringLiteral("profile_version"), QStringLiteral("profile_version TEXT"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_tool_calls"),
                      QStringLiteral("mutation_summary"),
                      QStringLiteral("mutation_summary TEXT"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_tool_calls"),
                      QStringLiteral("batch_id"),
                      QStringLiteral("batch_id TEXT"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_tool_calls"),
                      QStringLiteral("sandbox_summary"),
                      QStringLiteral("sandbox_summary TEXT"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_evidence"),
                      QStringLiteral("freshness"), QStringLiteral("freshness INTEGER"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_claims"),
                      QStringLiteral("claim_type"), QStringLiteral("claim_type INTEGER"), lastError_) ||
        !ensureColumn(connection.db, QStringLiteral("agent_authorizations"),
                      QStringLiteral("scope"), QStringLiteral("scope TEXT"), lastError_))
        return false;
    if (!query.exec(QStringLiteral("INSERT INTO agent_run_schema_metadata(key,value) "
                                   "VALUES('schema_version',7) ON CONFLICT(key) "
                                   "DO UPDATE SET value=excluded.value"))) {
        lastError_ = query.lastError().text();
        return false;
    }
    if (!query.exec(QStringLiteral("UPDATE agent_runs SET state='Interrupted',"
                                   "finished_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                                   "WHERE state IN ('Running','AwaitingApproval')"))) {
        lastError_ = query.lastError().text();
        return false;
    }
    if (!query.exec(QStringLiteral("UPDATE agent_steps SET status='Interrupted',"
                                   "finished_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                                   "WHERE finished_at IS NULL AND run_id IN ("
                                   "SELECT run_id FROM agent_runs WHERE state='Interrupted')"))) {
        lastError_ = query.lastError().text();
        return false;
    }
    if (!query.exec(QStringLiteral("UPDATE agent_tool_calls SET status='Interrupted',"
                                   "finished_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                                   "WHERE finished_at IS NULL AND step_id IN ("
                                   "SELECT step_id FROM agent_steps WHERE run_id IN ("
                                   "SELECT run_id FROM agent_runs WHERE state='Interrupted'))"))) {
        lastError_ = query.lastError().text();
        return false;
    }
    lastError_.clear();
    return true;
}

bool SQLiteAgentRunStore::clearHistory() {
    std::lock_guard lock(mutex_);
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return false;
    }
    if (!ready_) return false;
    Connection connection(databasePath_);
    if (!connection.db.isOpen() || !connection.db.transaction()) {
        lastError_ = QStringLiteral("Agent history store unavailable.");
        return false;
    }
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("SELECT 1 FROM agent_runs WHERE finished_at IS NULL LIMIT 1")) ||
        query.next()) {
        lastError_ = QStringLiteral(
            "Agent history clear refused while execution evidence is active or unavailable.");
        query.finish();
        connection.db.rollback();
        return false;
    }
    query.finish();
    if (!query.exec(QStringLiteral("DELETE FROM agent_runs")) || !connection.db.commit()) {
        lastError_ = query.lastError().isValid() ? query.lastError().text()
                                                : connection.db.lastError().text();
        connection.db.rollback();
        return false;
    }
    lastError_.clear();
    return true;
}

int SQLiteAgentRunStore::pruneCompletedBefore(const QDateTime& cutoffUtc, int limit) {
    std::lock_guard lock(mutex_);
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return -1;
    }
    if (!ready_ || !cutoffUtc.isValid() || limit < 1 || limit > 100) return -1;
    Connection connection(databasePath_);
    if (!connection.db.isOpen() || !connection.db.transaction()) return -1;
    QSqlQuery select(connection.db);
    select.prepare(QStringLiteral("SELECT run_id FROM agent_runs WHERE finished_at IS NOT NULL "
        "AND finished_at<? AND state NOT IN ('Running','Queued') "
        "AND NOT EXISTS (SELECT 1 FROM agent_runs child WHERE child.parent_run_id=agent_runs.run_id "
        "AND child.finished_at IS NULL) ORDER BY finished_at LIMIT ?"));
    select.addBindValue(timeText(cutoffUtc));
    select.addBindValue(limit);
    if (!select.exec()) { connection.db.rollback(); lastError_ = select.lastError().text(); return -1; }
    QStringList ids;
    while (select.next()) ids.append(select.value(0).toString());
    select.finish();
    QSqlQuery remove(connection.db);
    remove.prepare(QStringLiteral("DELETE FROM agent_runs WHERE run_id=?"));
    for (const auto& id : ids) {
        remove.bindValue(0, id);
        if (!remove.exec()) { connection.db.rollback(); lastError_ = remove.lastError().text(); return -1; }
    }
    if (!connection.db.commit()) { connection.db.rollback(); lastError_ = connection.db.lastError().text(); return -1; }
    lastError_.clear();
    return ids.size();
}

bool SQLiteAgentRunStore::record(const AgentEvent& event) {
    std::lock_guard lock(mutex_);
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return false;
    }
    if (event.turnId.isEmpty())
        return true;
    if (!ready_)
        return false;
    switch (event.type) {
    case AgentEventType::RunStarted:
    case AgentEventType::ModelRequestStarted:
    case AgentEventType::ModelRequestCompleted:
    case AgentEventType::ContextSnapshot:
    case AgentEventType::ToolRequested:
    case AgentEventType::ToolApprovalRequired:
    case AgentEventType::ToolApprovalResolved:
    case AgentEventType::ToolExecutionStarted:
    case AgentEventType::ToolExecutionCompleted:
    case AgentEventType::ToolExecutionFailed:
    case AgentEventType::AgentStepCompleted:
    case AgentEventType::AgentCompleted:
    case AgentEventType::AgentFailed:
    case AgentEventType::AgentCancelled:
    case AgentEventType::RuntimeStateChanged:
        break;
    default:
        return true;
    }
    Connection connection(databasePath_);
    if (!connection.db.isOpen()) {
        lastError_ = connection.db.lastError().text();
        return false;
    }
    auto& db = connection.db;
    if (!db.transaction()) {
        lastError_ = db.lastError().text();
        return false;
    }
    if (event.type != AgentEventType::RunStarted) {
        QSqlQuery existing(db);
        existing.prepare(QStringLiteral("SELECT 1 FROM agent_runs WHERE run_id=?"));
        existing.addBindValue(event.turnId);
        if (!existing.exec() || !existing.next()) {
            lastError_ = QStringLiteral("Agent evidence refused: owning run is missing.");
            existing.finish();
            db.rollback();
            return false;
        }
    }
    // Event IDs are runtime-owned; reject accidental cross-session/run redirection before mutation.
    auto owns = [&](const QString& sql, const QString& id, const QString& expected) {
        if (id.isEmpty())
            return true;
        QSqlQuery owner(db);
        owner.prepare(sql);
        owner.addBindValue(id);
        if (!owner.exec()) {
            lastError_ = owner.lastError().text();
            return false;
        }
        if (owner.next() && owner.value(0).toString() != expected) {
            lastError_ = QStringLiteral("Agent evidence ownership mismatch.");
            return false;
        }
        return true;
    };
    if (!owns(QStringLiteral("SELECT session_id FROM agent_runs WHERE run_id=?"), event.turnId,
              event.sessionId) ||
        !owns(QStringLiteral("SELECT run_id FROM agent_steps WHERE step_id=?"), event.stepId,
              event.turnId) ||
        !owns(QStringLiteral("SELECT step_id FROM agent_tool_calls WHERE tool_call_id=?"),
              event.toolCallId, event.stepId)) {
        db.rollback();
        return false;
    }
    const QString at = timeText(event.timestamp);
    bool ok = true;
    auto exec = [&](const QString& sql, const QVariantList& values) {
        if (ok)
            ok = apply(db, sql, values, lastError_);
    };
    if (event.type == AgentEventType::RunStarted) {
        const auto* start = std::get_if<AgentRunStartedEvent>(&event.payload);
        exec(QStringLiteral("INSERT OR IGNORE INTO agent_runs(run_id,session_id,parent_run_id,"
                            "parent_tool_call_id,run_type,started_at,state,provider_id,model_id,goal_summary,capability_snapshot,role,delegation_purpose,workspace_id,workspace_name,preset_id,profile_version) "
                            "VALUES(?,?,?,?,?,?,'Running',?,?,?,?,?,?,?,?,?,?)"),
             {event.turnId, event.sessionId, start ? start->parentRunId : QString{},
              start ? start->parentToolCallId : QString{},
              start ? start->runType : QStringLiteral("interactive"), at,
              event.providerId, event.modelId,
              bounded(start ? start->goalSummary : QString{}, 180),
              event.capabilitySnapshot.left(160), start ? start->role.left(40) : QString{},
              start ? start->delegationPurpose.left(40) : QString{},
              start ? start->workspaceId.left(120) : QString{},
              start ? start->workspaceName.left(160) : QString{},
              start ? start->presetId.left(120) : QString{},
              start ? start->profileVersion.left(32) : QString{}});
        exec(QStringLiteral("DELETE FROM agent_runs WHERE run_id IN ("
                            "SELECT run_id FROM agent_runs WHERE finished_at IS NOT NULL ORDER BY "
                            "started_at DESC,run_id DESC "
                            "LIMIT -1 OFFSET 500)"),
             {});
    } else if (event.type == AgentEventType::ModelRequestStarted) {
        exec(QStringLiteral("UPDATE agent_runs SET provider_id=?,model_id=? WHERE run_id=?"),
             {event.providerId, event.modelId, event.turnId});
        exec(QStringLiteral("INSERT OR IGNORE INTO agent_steps(step_id,run_id,sequence,step_type,"
                            "status,started_at) VALUES(?,?,?,'planning','Running',?)"),
             {event.stepId, event.turnId, event.stepIndex, at});
    } else if (event.type == AgentEventType::ModelRequestCompleted) {
        exec(QStringLiteral("UPDATE agent_steps SET status='Planned',finished_at=? "
                            "WHERE step_id=? AND status='Running'"), {at, event.stepId});
    } else if (event.type == AgentEventType::ContextSnapshot) {
        if (const auto* context = std::get_if<AgentContextEvent>(&event.payload))
            exec(QStringLiteral("UPDATE agent_runs SET context_tokens=?,context_items=?,"
                                "context_omitted=?,context_compacted=? WHERE run_id=?"),
                 {context->estimatedTokens, context->includedItems, context->omittedItems,
                  context->compacted ? 1 : 0, event.turnId});
    } else if (event.type == AgentEventType::AgentStepCompleted) {
        if (const auto* payload = std::get_if<AgentStepEvent>(&event.payload)) {
            const auto& step = payload->step;
            exec(QStringLiteral("INSERT INTO agent_steps(step_id,run_id,sequence,step_type,status,"
                                "started_at,finished_at,tool_call_id,observation_kind,filesystem_failure) "
                                "VALUES(?,?,?,'tool',?,?,?,?,?,?) ON CONFLICT(step_id) DO UPDATE SET "
                                "sequence=excluded.sequence,step_type=excluded.step_type,"
                                "status=excluded.status,"
                                "finished_at=excluded.finished_at,tool_call_id=excluded.tool_call_id,"
                                "observation_kind=excluded.observation_kind,"
                                "filesystem_failure=excluded.filesystem_failure"),
                 {event.stepId, event.turnId, step.index,
                  step.succeeded ? QStringLiteral("Succeeded") : QStringLiteral("Failed"),
                  at, at, event.toolCallId,
                  step.structuredObservation ? static_cast<int>(step.structuredObservation->kind) : 0,
                  step.structuredObservation ? static_cast<int>(step.structuredObservation->fileSystemFailure) : 0});
            if (!event.toolCallId.isEmpty())
                exec(QStringLiteral("UPDATE agent_tool_calls SET status=?,finished_at=?,"
                                    "failure_category=?,observation_summary=?,mutation_summary=?,"
                                    "sandbox_summary=? "
                                    "WHERE tool_call_id=?"),
                     {step.succeeded ? QStringLiteral("Succeeded") : QStringLiteral("Failed"), at,
                      step.failureCategory != ToolFailureCategory::None
                          ? toolFailureCategoryName(step.failureCategory)
                          : step.structuredObservation
                              ? QString::number(static_cast<int>(step.structuredObservation->fileSystemFailure))
                              : QString{},
                      step.structuredObservation
                          ? QStringLiteral("structured kind=%1").arg(
                                static_cast<int>(step.structuredObservation->kind))
                          : QStringLiteral("no structured observation"),
                      step.structuredObservation && !step.structuredObservation->patchPaths.isEmpty()
                          ? QStringLiteral("%1 path outcome(s), %2 committed")
                                .arg(step.structuredObservation->patchPaths.size())
                                .arg(std::count_if(step.structuredObservation->patchPaths.cbegin(),
                                                   step.structuredObservation->patchPaths.cend(),
                                                   [](const PatchPathOutcome& path) {
                                                       return path.committed;
                                                   }))
                          : QString{},
                      step.sandbox.backend.isEmpty()
                          ? QString{}
                          : (QStringLiteral("%1 | %2 | Network: %3 | Filesystem: %4 | Tree: %5 | Env: %6 | Token: %7 | Mitigations: %8")
                                .arg(sandboxEnforcementName(step.sandbox.enforcement),
                                     step.sandbox.backend,
                                     step.sandbox.networkDenied ? QStringLiteral("Denied")
                                                                : QStringLiteral("Not denied"),
                                     step.sandbox.filesystemRestricted
                                         ? QStringLiteral("Restricted")
                                         : QStringLiteral("Not restricted"),
                                     step.sandbox.processTreeControlled ? QStringLiteral("Owned")
                                                                        : QStringLiteral("Uncontrolled"),
                                     step.sandbox.environmentRestricted ? QStringLiteral("Filtered")
                                                                         : QStringLiteral("Unrestricted"),
                                     step.sandbox.restrictedToken ? QStringLiteral("Restricted")
                                                                  : QStringLiteral("None"),
                                     step.sandbox.mitigationsApplied ? QStringLiteral("Applied")
                                                                      : QStringLiteral("None")) +
                             (step.sandbox.failureCategory.isEmpty()
                                  ? QString{} : QStringLiteral(" | %1").arg(
                                        step.sandbox.failureCategory)))
                                .left(300),
                      event.toolCallId});
        }
    } else if (event.type == AgentEventType::ToolRequested ||
               event.type == AgentEventType::ToolApprovalRequired ||
               event.type == AgentEventType::ToolApprovalResolved ||
               event.type == AgentEventType::ToolExecutionStarted ||
               event.type == AgentEventType::ToolExecutionCompleted ||
               event.type == AgentEventType::ToolExecutionFailed) {
        exec(QStringLiteral("INSERT OR IGNORE INTO agent_steps(step_id,run_id,sequence,step_type,"
                            "status,started_at,tool_call_id) VALUES(?,?,?,'tool','Requested',?,?)"),
             {event.stepId, event.turnId, event.stepIndex, at, event.toolCallId});
        if (event.type == AgentEventType::ToolRequested) {
            exec(QStringLiteral("UPDATE agent_steps SET step_type='tool',status='Requested',"
                                "tool_call_id=?,finished_at=NULL WHERE step_id=?"),
                 {event.toolCallId, event.stepId});
            if (const auto* tool = std::get_if<AgentToolEvent>(&event.payload))
                exec(QStringLiteral("INSERT OR IGNORE INTO agent_tool_calls(tool_call_id,step_id,"
                                    "tool_id,source,status,started_at,batch_id) "
                                    "VALUES(?,?,?,?,'Requested',?,?)"),
                     {event.toolCallId, event.stepId, tool->toolId, tool->source, at,
                      tool->batchId});
        } else {
            QString status;
            switch (event.type) {
            case AgentEventType::ToolApprovalRequired: status = QStringLiteral("AwaitingApproval"); break;
            case AgentEventType::ToolApprovalResolved: status = QStringLiteral("ApprovalResolved"); break;
            case AgentEventType::ToolExecutionStarted: status = QStringLiteral("Running"); break;
            case AgentEventType::ToolExecutionCompleted: status = QStringLiteral("Succeeded"); break;
            default: status = QStringLiteral("Failed"); break;
            }
            exec(QStringLiteral("UPDATE agent_tool_calls SET status=?,finished_at=CASE WHEN ? "
                                "IN ('Succeeded','Failed') THEN ? ELSE finished_at END "
                                "WHERE tool_call_id=?"),
                 {status, status, at, event.toolCallId});
            if (event.type == AgentEventType::ToolApprovalRequired) {
                if (const auto* tool = std::get_if<AgentToolEvent>(&event.payload)) {
                    exec(QStringLiteral("DELETE FROM agent_authorizations WHERE tool_call_id=?"),
                         {event.toolCallId});
                    int authorizationCount = 0;
                    for (const auto& request : tool->authorizationRequests) {
                        if (++authorizationCount > 32) break;
                        exec(QStringLiteral("UPDATE agent_tool_calls SET resource_summary=? "
                                            "WHERE tool_call_id=?"),
                             {bounded(securityDomainName(request.domain) + QLatin1Char('/') +
                                      accessModeName(request.access) + QLatin1Char(' ') +
                                      request.resource, 180), event.toolCallId});
                        exec(QStringLiteral("INSERT INTO agent_authorizations(tool_call_id,domain,"
                                            "access,resource_summary,decision) VALUES(?,?,?,?,?)"),
                             {event.toolCallId, securityDomainName(request.domain),
                              accessModeName(request.access), bounded(request.resource, 180),
                              QStringLiteral("Pending")});
                    }
                }
            } else if (event.type == AgentEventType::ToolApprovalResolved) {
                const auto* decision = std::get_if<AgentTextEvent>(&event.payload);
                exec(QStringLiteral("UPDATE agent_authorizations SET decision=? WHERE tool_call_id=?"),
                     {decision && decision->text == QLatin1String("Approved")
                          ? QStringLiteral("Approved") : QStringLiteral("Denied"),
                      event.toolCallId});
            }
        }
    } else if (event.type == AgentEventType::AgentCompleted ||
               event.type == AgentEventType::AgentFailed ||
               event.type == AgentEventType::AgentCancelled) {
        const QString state = event.type == AgentEventType::AgentCompleted
                                  ? QStringLiteral("Completed")
                                  : event.type == AgentEventType::AgentCancelled
                                        ? QStringLiteral("Cancelled") : QStringLiteral("Failed");
        const auto* text = std::get_if<AgentTextEvent>(&event.payload);
        exec(QStringLiteral("UPDATE agent_runs SET state=?,finished_at=?,final_answer=?,failure=? "
                            "WHERE run_id=?"),
             {state, at,
              state == QLatin1String("Completed") && text ? bounded(text->text, 2000) : QString{},
              state != QLatin1String("Completed") && text ? bounded(text->text, 240) : QString{},
              event.turnId});
        exec(QStringLiteral("UPDATE agent_steps SET status=?,finished_at=? "
                            "WHERE run_id=? AND finished_at IS NULL"),
             {state, at, event.turnId});
        exec(QStringLiteral("UPDATE agent_tool_calls SET status=?,finished_at=? "
                            "WHERE tool_call_id IN (SELECT tool_call_id FROM agent_steps "
                            "WHERE run_id=?) AND finished_at IS NULL"),
             {state, at, event.turnId});
    } else if (event.type == AgentEventType::RuntimeStateChanged) {
        if (const auto* state = std::get_if<AgentStateEvent>(&event.payload)) {
            if (state->phase == AgentLoopPhase::Running)
                exec(QStringLiteral("UPDATE agent_runs SET state='Running' WHERE run_id=?"),
                     {event.turnId});
        }
        if (const auto* run = std::get_if<AgentRunEvent>(&event.payload)) {
            if (run->terminalReason != AgentTerminalReason::None)
                exec(QStringLiteral("UPDATE agent_runs SET terminal_reason=? WHERE run_id=?"),
                     {agentTerminalReasonName(run->terminalReason), event.turnId});
            exec(QStringLiteral("UPDATE agent_runs SET provider_recovery_attempts=? WHERE run_id=?"),
                 {qBound(0, run->providerRecoveryAttempts, 32), event.turnId});
            if (run->providerFailure) {
                const auto& failure = *run->providerFailure;
                exec(QStringLiteral("UPDATE agent_runs SET provider_error_category=?,"
                                    "provider_http_status=?,provider_attempts=?,"
                                    "provider_retry_occurred=?,provider_request_lifecycle=? "
                                    "WHERE run_id=?"),
                     {chatProviderErrorCategoryName(failure.category), failure.httpStatus,
                      failure.attempts, failure.attempts > 1 ? 1 : 0,
                      lifecycleName(failure.lifecycle), event.turnId});
            }
            if (run->phase == AgentLoopPhase::AwaitingApproval)
                exec(QStringLiteral("UPDATE agent_runs SET state='AwaitingApproval' WHERE run_id=?"),
                     {event.turnId});
            else {
                exec(QStringLiteral("UPDATE agent_runs SET grounding_mode=? WHERE run_id=?"),
                     {groundingModeName(run->grounding.mode), event.turnId});
                exec(QStringLiteral("DELETE FROM agent_requirements WHERE run_id=?"), {event.turnId});
                exec(QStringLiteral("DELETE FROM agent_evidence WHERE run_id=?"), {event.turnId});
                exec(QStringLiteral("DELETE FROM agent_claims WHERE run_id=?"), {event.turnId});
                int count = 0;
                for (const auto& requirement : run->observationIntent.requirements) {
                    if (++count > 64) break;
                    exec(QStringLiteral("INSERT INTO agent_requirements(run_id,claim_id,domain,"
                                        "resource_summary) VALUES(?,?,?,?)"),
                         {event.turnId, bounded(requirement.claimId, 80),
                          observationDomainName(requirement.domain),
                          bounded(requirement.resourceHint, 180)});
                }
                count = 0;
                for (const auto& evidence : run->evidence) {
                    if (++count > 128) break;
                    exec(QStringLiteral("INSERT INTO agent_evidence(run_id,tool_call_id,domain,"
                                        "resource_summary,outcome,freshness) VALUES(?,?,?,?,?,?)"),
                         {event.turnId, evidence.toolCallId,
                          observationDomainName(evidence.domain),
                          bounded(evidence.resource, 180), static_cast<int>(evidence.outcome),
                          static_cast<int>(evidence.freshness)});
                }
                count = 0;
                for (const auto& assertion : run->finalClaims) {
                    if (++count > 64) break;
                    for (const auto& requirement : run->observationIntent.requirements) {
                        if (requirement.claimId != assertion.id)
                            continue;
                        const auto resolved = ClaimGroundingResolver::resolve(
                            requirement, run->evidence, assertion);
                        QJsonArray support;
                        for (const auto& id : resolved.fact.evidenceCallIds)
                            support.append(id);
                        exec(QStringLiteral("INSERT OR REPLACE INTO agent_claims(run_id,claim_id,"
                                            "assertion_value,verdict,resource_summary,"
                                            "supporting_tool_calls,claim_type) VALUES(?,?,?,?,?,?,?)"),
                             {event.turnId, bounded(assertion.id, 80), assertion.value ? 1 : 0,
                              static_cast<int>(resolved.verdict),
                              bounded(resolved.fact.resource, 180),
                              QString::fromUtf8(QJsonDocument(support).toJson(QJsonDocument::Compact)),
                              static_cast<int>(requirement.claimType)});
                        break;
                    }
                }
            }
        }
    }
    if (ok && db.commit()) {
        lastError_.clear();
        return true;
    }
    if (ok)
        lastError_ = db.lastError().text();
    db.rollback();
    return false;
}

QList<StoredAgentRun> SQLiteAgentRunStore::recentRuns(int limit) const {
    std::lock_guard lock(mutex_);
    QList<StoredAgentRun> result;
    if (!ready_ || limit <= 0) return result;
    Connection connection(databasePath_, true);
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT %1 FROM agent_runs ORDER BY started_at DESC,run_id DESC LIMIT ?")
                      .arg(runColumns));
    query.addBindValue(qBound(1, limit, 100));
    if (!query.exec()) lastError_ = query.lastError().text();
    else { lastError_.clear(); while (query.next()) result.append(readRun(query)); }
    return result;
}

QList<StoredAgentRun> SQLiteAgentRunStore::runsBefore(const QDateTime& startedAt,
                                                      const QString& runId, int limit) const {
    std::lock_guard lock(mutex_);
    QList<StoredAgentRun> result;
    if (!ready_ || limit <= 0 || !startedAt.isValid() || runId.isEmpty()) return result;
    Connection connection(databasePath_, true);
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT %1 FROM agent_runs WHERE started_at < ? "
                                 "OR (started_at = ? AND run_id < ?) "
                                 "ORDER BY started_at DESC,run_id DESC LIMIT ?").arg(runColumns));
    const QString at = timeText(startedAt);
    query.addBindValue(at);
    query.addBindValue(at);
    query.addBindValue(runId);
    query.addBindValue(qBound(1, limit, 100));
    if (!query.exec()) lastError_ = query.lastError().text();
    else { lastError_.clear(); while (query.next()) result.append(readRun(query)); }
    return result;
}

StoredAgentRun SQLiteAgentRunStore::runById(const QString& runId) const {
    std::lock_guard lock(mutex_);
    if (!ready_) return {};
    Connection connection(databasePath_, true);
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT %1 FROM agent_runs WHERE run_id=?").arg(runColumns));
    query.addBindValue(runId);
    if (!query.exec()) { lastError_ = query.lastError().text(); return {}; }
    lastError_.clear();
    return query.next() ? readRun(query) : StoredAgentRun{};
}

QList<StoredAgentRun> SQLiteAgentRunStore::childRuns(const QString& parentRunId, int limit) const {
    std::lock_guard lock(mutex_);
    QList<StoredAgentRun> result;
    if (!ready_ || parentRunId.isEmpty() || limit <= 0) return result;
    Connection connection(databasePath_, true);
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT %1 FROM agent_runs WHERE parent_run_id=? "
                                 "ORDER BY started_at DESC,run_id DESC LIMIT ?").arg(runColumns));
    query.addBindValue(parentRunId);
    query.addBindValue(qBound(1, limit, 50));
    if (!query.exec()) lastError_ = query.lastError().text();
    else { lastError_.clear(); while (query.next()) result.append(readRun(query)); }
    return result;
}

QList<StoredAgentStep> SQLiteAgentRunStore::stepsForRun(const QString& runId, int limit) const {
    std::lock_guard lock(mutex_);
    QList<StoredAgentStep> result;
    if (!ready_ || limit <= 0) return result;
    Connection connection(databasePath_, true);
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT step_id,run_id,sequence,status,tool_call_id,started_at,"
                                 "finished_at,observation_kind,filesystem_failure,step_type "
                                 "FROM agent_steps WHERE run_id=? "
                                 "ORDER BY sequence ASC LIMIT ?"));
    query.addBindValue(runId);
    query.addBindValue(qBound(1, limit, 200));
    if (query.exec()) while (query.next())
        result.append({query.value(0).toString(), query.value(1).toString(),
                       query.value(2).toInt(), query.value(3).toString(),
                       query.value(4).toString(), parsedTime(query.value(5)),
                       parsedTime(query.value(6)), query.value(7).toInt(),
                       query.value(8).toInt(), query.value(9).toString()});
    return result;
}

QList<StoredAgentToolCall> SQLiteAgentRunStore::toolCallsForRun(const QString& runId, int limit) const {
    std::lock_guard lock(mutex_);
    QList<StoredAgentToolCall> result;
    if (!ready_ || limit <= 0) return result;
    Connection connection(databasePath_, true);
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT t.tool_call_id,t.step_id,t.tool_id,t.source,t.status,"
                                 "t.resource_summary,t.observation_summary,t.started_at,t.finished_at,"
                                 "t.failure_category,t.mutation_summary,t.batch_id,t.sandbox_summary "
                                 "FROM agent_tool_calls t JOIN agent_steps s ON s.step_id=t.step_id "
                                 "WHERE s.run_id=? ORDER BY s.sequence ASC LIMIT ?"));
    query.addBindValue(runId);
    query.addBindValue(qBound(1, limit, 200));
    if (query.exec()) while (query.next())
        result.append({query.value(0).toString(), query.value(1).toString(),
                       query.value(2).toString(), query.value(3).toString(),
                       query.value(4).toString(), query.value(5).toString(),
                       query.value(6).toString(), parsedTime(query.value(7)),
                       parsedTime(query.value(8)), query.value(9).toString(),
                       query.value(10).toString(), query.value(11).toString(),
                       query.value(12).toString()});
    return result;
}

QList<StoredAgentEvidence> SQLiteAgentRunStore::evidenceForRun(const QString& runId,
                                                               int limit) const {
    std::lock_guard lock(mutex_);
    QList<StoredAgentEvidence> result;
    if (!ready_ || limit <= 0) return result;
    Connection connection(databasePath_, true);
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT tool_call_id,domain,resource_summary,outcome,freshness "
                                 "FROM agent_evidence WHERE run_id=? ORDER BY rowid ASC LIMIT ?"));
    query.addBindValue(runId);
    query.addBindValue(qBound(1, limit, 128));
    if (query.exec()) while (query.next())
        result.append({query.value(0).toString(), query.value(1).toString(),
                       query.value(2).toString(), query.value(3).toInt(),
                       query.value(4).toInt()});
    return result;
}

QList<StoredAgentClaim> SQLiteAgentRunStore::claimsForRun(const QString& runId,
                                                          int limit) const {
    std::lock_guard lock(mutex_);
    QList<StoredAgentClaim> result;
    if (!ready_ || limit <= 0) return result;
    Connection connection(databasePath_, true);
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT claim_id,assertion_value,verdict,resource_summary,"
                                 "supporting_tool_calls,claim_type FROM agent_claims WHERE run_id=? "
                                 "ORDER BY claim_id ASC LIMIT ?"));
    query.addBindValue(runId);
    query.addBindValue(qBound(1, limit, 64));
    if (query.exec()) while (query.next()) {
        QStringList support;
        const auto parsed = QJsonDocument::fromJson(query.value(4).toByteArray());
        for (const auto& id : parsed.array())
            support.append(id.toString());
        result.append({query.value(0).toString(), query.value(1).toBool(),
                       query.value(2).toInt(), query.value(3).toString(), support,
                       query.value(5).toInt()});
    }
    return result;
}

QList<StoredAgentAuthorization> SQLiteAgentRunStore::authorizationsForRun(
    const QString& runId, int limit) const {
    std::lock_guard lock(mutex_);
    QList<StoredAgentAuthorization> result;
    if (!ready_ || limit <= 0) return result;
    Connection connection(databasePath_, true);
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT a.tool_call_id,a.domain,a.access,a.resource_summary,"
                                 "a.decision,a.scope FROM agent_authorizations a "
                                 "JOIN agent_tool_calls t ON t.tool_call_id=a.tool_call_id "
                                 "JOIN agent_steps s ON s.step_id=t.step_id WHERE s.run_id=? "
                                 "ORDER BY s.sequence ASC LIMIT ?"));
    query.addBindValue(runId);
    query.addBindValue(qBound(1, limit, 128));
    if (query.exec()) while (query.next())
        result.append({query.value(0).toString(), query.value(1).toString(),
                       query.value(2).toString(), query.value(3).toString(),
                       query.value(4).toString(), query.value(5).toString()});
    return result;
}

QString SQLiteAgentRunStore::lastError() const {
    std::lock_guard lock(mutex_);
    return lastError_;
}

} // namespace sentinel::core
