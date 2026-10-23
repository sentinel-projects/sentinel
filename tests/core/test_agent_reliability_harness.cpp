// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// P1-01 deterministic Agent reliability harness (AC-P1-01).
//
// Executes the COR-01..COR-18 corpus from docs/roadmaps/ACCEPTANCE_MATRIX.md
// against the existing AgentRuntime / AgentLoop / ToolExecutionGateway seams
// with injected planners, chat providers, approval policies and tool handlers.
// Runtime path reuses AgentRuntime with a fixture metadata seam and the loop
// path reuses AgentLoop directly; no second runtime is introduced.
//
// Determinism rules:
// - No arbitrary sleeps. Synchronization uses QSemaphore barriers with bounded
//   deadlock guards and QtTest QTRY_* polling on terminal conditions.
// - Every fixture uses a disposable QTemporaryDir workspace with synthetic
//   data and a trusted baseline manifest.
// - An independent verifier outside any model/tool authority compares a fresh
//   filesystem manifest against an exact per-scenario allowlist.
// - Tool side effects are prescribed exactly per scenario on the fixture
//   handler; any invocation outside the prescription is counted as an
//   unauthorized effect and fails the case.

#include "../support/DeterministicChatFixture.h"
#include "../support/IndependentCompileOracle.h"
#include "sentinel/core/chat/OllamaChatProvider.h"
#include "sentinel/core/security/SQLitePermissionGrantStore.h"
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <mutex>

#include "sentinel/core/agent/AgentEvent.h"
#include "sentinel/core/agent/AgentLoop.h"
#include "sentinel/core/agent/AgentRuntime.h"
#include "sentinel/core/agent/ClaimGroundingResolver.h"
#include "sentinel/core/agent/IAgentRuntime.h"
#include "sentinel/core/agent/LlmAgentRuntime.h"
#include "sentinel/core/agent/ObservationPolicy.h"
#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/model/ModelRouting.h"
#include "sentinel/core/runtime/BuiltInToolProvider.h"
#include "sentinel/core/runtime/IToolExecutor.h"
#include "sentinel/core/runtime/InMemoryToolRegistry.h"
#include "sentinel/core/runtime/ToolApproval.h"
#include "sentinel/core/security/IApprovalPolicy.h"
#include "sentinel/core/security/StaticSandboxPolicy.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

using namespace sentinel::core;

namespace {

// Capture diagnostics without forwarding private fixture text to the test log.
std::mutex plannerPrivacyLogMutex;
QStringList plannerPrivacyLogs;
void capturePlannerPrivacyLog(QtMsgType, const QMessageLogContext&, const QString& text) {
    std::lock_guard lock(plannerPrivacyLogMutex);
    plannerPrivacyLogs.append(text);
}

constexpr int kBarrierTimeoutMs = 20000;

// --------------------------------------------------------------------------
// Independent verification (outside any model/tool authority)
// --------------------------------------------------------------------------

QString sha256OfFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("<unreadable>");
    return QString::fromUtf8(
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
}

struct WorkspaceState {
    QHash<QString, QString> files; // relative path -> content digest
};

WorkspaceState stateOf(const QString& root) {
    WorkspaceState state;
    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        state.files.insert(QDir(root).relativeFilePath(path), sha256OfFile(path));
    }
    return state;
}

// Walks the disposable workspace directly from the filesystem (never through
// tool output) and diffs it against an exact expected allowlist.
class WorkspaceVerifier {
public:
    explicit WorkspaceVerifier(const QString& root)
        : root_(QFileInfo(root).canonicalFilePath()), baseline_(stateOf(root_)) {}

    struct Expected {
        QStringList changed; // relative paths whose content may differ
        QStringList added;   // relative paths that may be created
        QStringList removed; // relative paths that may be deleted
    };

    struct Diff {
        QStringList changed, added, removed;
        bool empty() const {
            return changed.isEmpty() && added.isEmpty() && removed.isEmpty();
        }
        QString describe() const {
            return QStringLiteral("changed=[%1] added=[%2] removed=[%3]")
                .arg(changed.join(QLatin1Char(',')), added.join(QLatin1Char(',')),
                     removed.join(QLatin1Char(',')));
        }
    };

    Diff delta() const {
        return deltaFrom(baseline_);
    }

    bool untouched() const {
        return delta().empty();
    }

    // True only for the exact expected set: nothing changed/created/removed
    // beyond the allowlist entries.
    bool matches(const Expected& expected, QString* actual = nullptr) const {
        const Diff diff = delta();
        if (actual)
            *actual = diff.describe();
        const auto sameSet = [](const QStringList& left, const QStringList& right) {
            return QSet<QString>(left.cbegin(), left.cend()) ==
                   QSet<QString>(right.cbegin(), right.cend());
        };
        return sameSet(diff.changed, expected.changed) && sameSet(diff.added, expected.added) &&
               sameSet(diff.removed, expected.removed);
    }

    QString fileContent(const QString& relativePath) const {
        QFile file(root_ + QLatin1Char('/') + relativePath);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(file.readAll());
    }

    const QString& root() const {
        return root_;
    }

private:
    Diff deltaFrom(const WorkspaceState& before) const {
        const auto after = stateOf(root_);
        Diff diff;
        for (auto it = after.files.cbegin(); it != after.files.cend(); ++it) {
            if (!before.files.contains(it.key()))
                diff.added.append(it.key());
            else if (before.files.value(it.key()) != it.value())
                diff.changed.append(it.key());
        }
        for (auto it = before.files.cbegin(); it != before.files.cend(); ++it)
            if (!after.files.contains(it.key()))
                diff.removed.append(it.key());
        std::sort(diff.changed.begin(), diff.changed.end());
        std::sort(diff.added.begin(), diff.added.end());
        std::sort(diff.removed.begin(), diff.removed.end());
        return diff;
    }

    QString root_;
    WorkspaceState baseline_;
};

// Terminal-state audit over the typed runtime event history.
class TerminalAudit {
public:
    void scan(const QList<AgentEvent>& events) {
        for (const auto& event : events) {
            if (event.type == AgentEventType::AgentCompleted)
                ++completed;
            else if (event.type == AgentEventType::AgentFailed)
                ++failed;
            else if (event.type == AgentEventType::AgentCancelled)
                ++cancelled;
        }
    }
    int total() const {
        return completed + failed + cancelled;
    }
    bool singleTerminal() const {
        return total() == 1;
    }
    QString describe() const {
        return QStringLiteral("completed=%1 failed=%2 cancelled=%3")
            .arg(QString::number(completed), QString::number(failed), QString::number(cancelled));
    }
    int completed = 0, failed = 0, cancelled = 0;
};

// --------------------------------------------------------------------------
// Deterministic planner fixture
// --------------------------------------------------------------------------

class ScriptedPlanner final : public IAgentStepPlanner {
public:
    QVector<AgentStepDecision> decisions;
    bool repeatLast = false;
    bool canonicalFinal = false;
    // Canonical finals must be derived from the same intent the loop
    // classified; otherwise the recompute-fence rejects the representation.
    ObservationIntent canonicalIntent;
    std::function<void(int)> onCall;
    mutable int calls = 0;
    // Deterministic planning barrier. When enabled, nextStep announces entry
    // and blocks until the test thread releases it, so cancellation can be
    // requested while planning is genuinely on the stack. No sleeps are used;
    // the timeout only guards a hung fixture.
    mutable bool barrierPlanning = false;
    mutable QSemaphore planningEntered{0};
    mutable QSemaphore planningRelease{0};
    mutable std::atomic_bool planningBarrierTimedOut{false};

    void resetPlanningIndex() {
        indexBase_ = calls;
    }

    void setEvidence(const QList<EvidenceRecord>& value) override {
        evidence_ = value;
    }

    AgentStepDecision nextStep(const QString& goal,
                               const QList<AgentStepRecord>& history) const override {
        Q_UNUSED(goal);
        Q_UNUSED(history);
        if (barrierPlanning) {
            planningEntered.release();
            if (!planningRelease.tryAcquire(1, kBarrierTimeoutMs))
                planningBarrierTimedOut.store(true);
        }
        const int index = (calls++) - indexBase_;
        if (onCall)
            onCall(index);
        if (index >= 0 && index < decisions.size()) {
            auto decision = decisions.at(index);
            if (canonicalFinal && decision.kind == AgentStepDecision::Kind::FinalAnswer) {
                if (auto answer = ClaimGroundingResolver::filesystemFinalAnswer(
                        canonicalIntent, evidence_, decision.answer))
                    decision.answer = *answer;
            }
            return decision;
        }
        if (repeatLast && !decisions.isEmpty()) {
            auto decision = decisions.last();
            if (canonicalFinal && decision.kind == AgentStepDecision::Kind::FinalAnswer) {
                if (auto answer = ClaimGroundingResolver::filesystemFinalAnswer(
                        canonicalIntent, evidence_, decision.answer))
                    decision.answer = *answer;
            }
            return decision;
        }
        AgentStepDecision fallback;
        fallback.kind = AgentStepDecision::Kind::FinalAnswer;
        fallback.answer = QStringLiteral("fallback final answer");
        return fallback;
    }

private:
    mutable int indexBase_ = 0;
    mutable QList<EvidenceRecord> evidence_;
};

AgentStepDecision toolDecision(const QString& toolId, ToolRiskLevel risk = ToolRiskLevel::Low) {
    AgentStepDecision decision;
    decision.kind = AgentStepDecision::Kind::ToolCall;
    decision.toolId = toolId;
    decision.toolName = toolId;
    decision.riskLevel = risk;
    decision.thought = QStringLiteral("use %1").arg(toolId);
    return decision;
}

void appendArg(AgentStepDecision& decision, const QString& id, const QString& value) {
    decision.arguments.append(ToolInvocationArgument{id, value});
}

AgentStepDecision finalDecision(const QString& answer) {
    AgentStepDecision decision;
    decision.kind = AgentStepDecision::Kind::FinalAnswer;
    decision.answer = answer;
    return decision;
}

AgentStepDecision giveUpDecision(const QString& reason) {
    AgentStepDecision decision;
    decision.kind = AgentStepDecision::Kind::GiveUp;
    decision.reason = reason;
    return decision;
}

class FixedIntentPolicy final : public IObservationIntentPolicy {
public:
    ObservationIntent intent;
    ObservationIntent classify(const QString&, const QString&,
                               const QList<ToolDescriptor>&) const override {
        return intent;
    }
};

class CallbackIntentPolicy final : public IObservationIntentPolicy {
public:
    std::function<void()> onClassify;
    ObservationIntent classify(const QString&, const QString&,
                               const QList<ToolDescriptor>&) const override {
        if (onClassify)
            onClassify();
        return {};
    }
};

class FixtureApprovalPolicy final : public IApprovalPolicy {
public:
    enum class Mode { ApproveAll, RequireApproval, DenyAll };
    Mode mode = Mode::ApproveAll;
    mutable QStringList requestedTools;

    ApprovalDecision evaluate(const ToolInvocationPlan& plan) const override {
        requestedTools.append(plan.invocations.isEmpty() ? QString()
                                                         : plan.invocations.first().toolId);
        switch (mode) {
        case Mode::ApproveAll:
            return {ApprovalStatus::Approved, QStringLiteral("Fixture pre-approval"), {}};
        case Mode::DenyAll:
            return {ApprovalStatus::Denied, QStringLiteral("Fixture denies every action"), {}};
        case Mode::RequireApproval:
            return {
                ApprovalStatus::RequiresApproval, QStringLiteral("Fixture approval required"), {}};
        }
        return {ApprovalStatus::RequiresApproval, QStringLiteral("unreachable"), {}};
    }
};

// Executor seam that must never be consulted once the registry carries
// handler registrations; any use indicates a lost registry seam.
class NoOpExecutor final : public IToolExecutor {
public:
    mutable int calls = 0;
    ToolExecutionResult execute(const ToolExecutionRequest&) const override {
        ++calls;
        return {ToolExecutionStatus::Blocked, QStringLiteral("Executor seam used unexpectedly.")};
    }
};

// --------------------------------------------------------------------------
// Deterministic tool handler with exact side-effect prescriptions
// --------------------------------------------------------------------------

class FixtureHandler final : public IToolHandler {
public:
    QString root;
    QSet<QString> allowedCommands;         // exact authorized run-command strings
    bool testReceiptFails = false;         // deterministic failing-test receipt
    QHash<QString, QString> allowedWrites; // rel path -> exact canonical content
    QString unavailableReasonOverride;     // registered capability missing
    // Force structured coverage metadata to advertise an incomplete/truncated
    // observation. The observation content is real; only the typed completeness
    // contract is degraded, which must prevent unsupported absence conclusions.
    bool observationIncomplete = false;
    bool emptyFileContent = false; // read-file reports empty (missing) content
    bool blockSlowBuild = false;   // cooperative blocking command flag
    QSemaphore slowBuildStarted{0};
    std::atomic<qint64> childPid{0};
    std::atomic_bool childReleased{false};
    std::atomic_bool childDrainTimedOut{false};

    QHash<QString, int> counts; // receipt issuance per toolId
    QStringList order;          // execution order receipts
    std::atomic_bool sealed{false};
    int postSealCalls = 0;
    int unauthorizedCalls = 0;

    QString unavailableReason() const override {
        return unavailableReasonOverride;
    }

    void seal() {
        sealed.store(true);
    }

    IToolExecutor::Cancel execute(const ToolExecutionRequest& request, const QString& sessionId,
                                  const QString& toolCallId, IToolExecutor::Output,
                                  IToolExecutor::Completion completion) override {
        Q_UNUSED(sessionId);
        Q_UNUSED(toolCallId);
        if (sealed.load()) {
            ++postSealCalls;
            completion({ToolExecutionStatus::Blocked, QStringLiteral("Fixture sealed.")});
            return IToolExecutor::Cancel{};
        }
        const auto& invocation = request.plan.invocations.first();
        const auto toolId = invocation.toolId;
        const auto arg = [&invocation](const QString& id) {
            for (const auto& item : invocation.arguments)
                if (item.id == id)
                    return item.value;
            return QString{};
        };

        if (invocation.arguments.isEmpty())
            ++unauthorizedCalls; // no-argument calls are never prescribed

        if (toolId == QLatin1String("run-command")) {
            const QString command = arg(QStringLiteral("command"));
            if (!allowedCommands.contains(command)) {
                ++unauthorizedCalls;
                completion({ToolExecutionStatus::Blocked,
                            QStringLiteral("Command not in fixture allowlist: %1").arg(command)});
                return IToolExecutor::Cancel{};
            }
            order.append(QStringLiteral("run-command(%1)").arg(command));
        } else {
            order.append(QStringLiteral("%1(%2)").arg(toolId, arg(QStringLiteral("path"))));
        }

        auto observation = std::make_shared<StructuredObservation>();
        observation->data = {{"complete", !observationIncomplete},
                             {"truncated", observationIncomplete},
                             {"cancelled", false}};
        ToolExecutionResult result{ToolExecutionStatus::Succeeded, QStringLiteral("fixture ok")};
        counts[toolId]++;

        if (toolId == QLatin1String("write-file") || toolId == QLatin1String("edit-file")) {
            const QString path = resolved(arg(QStringLiteral("path")));
            const QString content = arg(QStringLiteral("content"));
            if (!authorizedWrite(path, content)) {
                ++unauthorizedCalls;
                completion({ToolExecutionStatus::Blocked,
                            QStringLiteral("Fixture write outside authorized set: %1").arg(path)});
                return IToolExecutor::Cancel{};
            }
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(content.toUtf8()) < 0) {
                completion({ToolExecutionStatus::Failed, QStringLiteral("fixture write failed")});
                return IToolExecutor::Cancel{};
            }
            result.mutations.append({path});
        } else if (toolId == QLatin1String("read-file") || toolId == QLatin1String("stat-path")) {
            const QString path = resolved(arg(QStringLiteral("path")));
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                completion({ToolExecutionStatus::Failed, QStringLiteral("fixture read failed")});
                return IToolExecutor::Cancel{};
            }
            observation->kind = StructuredObservationKind::FileContent;
            observation->data.insert("path", QFileInfo(path).canonicalFilePath());
            observation->data.insert(
                "content", emptyFileContent ? QString{} : QString::fromUtf8(file.readAll()));
        } else if (toolId == QLatin1String("list-directory")) {
            observation->kind = StructuredObservationKind::DirectoryListing;
            QJsonArray listing;
            QDirIterator it(resolved(arg(QStringLiteral("path"))), QDir::NoDotAndDotDot);
            while (it.hasNext())
                listing.append(QFileInfo(it.next()).fileName());
            observation->data.insert("entries", listing);
            observation->data.insert("includeHidden", false);
        } else if (toolId == QLatin1String("git-diff") || toolId == QLatin1String("git-status")) {
            observation->kind = StructuredObservationKind::Generic;
            observation->data.insert("stdout", QStringLiteral("- return a - b\n+ return a + b"));
            observation->data.insert("exitCode", 0);
        } else if (toolId == QLatin1String("run-command")) {
            observation->kind = StructuredObservationKind::Generic;
            observation->data.insert("exitCode", testReceiptFails ? 1 : 0);
            observation->data.insert("stdout", testReceiptFails
                                                   ? QStringLiteral("TEST_FAILED")
                                                   : QStringLiteral("ALL_TESTS_PASSED"));
            observation->data.insert("stderr", QString{});
            if (testReceiptFails)
                result.status = ToolExecutionStatus::Failed;
        } else if (toolId == QLatin1String("delete-file") || toolId == QLatin1String("move-file")) {
            ++unauthorizedCalls;
            completion({ToolExecutionStatus::Blocked,
                        QStringLiteral("Fixture forbids destructive receipts.")});
            return IToolExecutor::Cancel{};
        } else {
            ++unauthorizedCalls;
            completion({ToolExecutionStatus::Blocked,
                        QStringLiteral("Tool not prescribed by fixture: %1").arg(toolId)});
            return IToolExecutor::Cancel{};
        }

        if (blockSlowBuild && toolId == QLatin1String("run-command") &&
            arg(QStringLiteral("command")) == QStringLiteral("echo slow-build")) {
            // Run only this test executable's fixed synthetic child mode. The
            // worker context owns ProcessExecutor and its child until release.
            auto* processes = new ProcessExecutor(request.callbackContext);
            ProcessRequest launch;
            launch.program = QCoreApplication::applicationFilePath();
            launch.arguments = {QStringLiteral("--cooperative-child"),
                                QDir(root).filePath(QStringLiteral("build/partial.log"))};
            launch.workingDirectory = root;
            launch.sessionId = sessionId;
            launch.toolCallId = toolCallId;
            launch.timeoutMs = kBarrierTimeoutMs; // test watchdog, not an acceptance budget
            launch.unconfinedPermitted = true;    // explicit test helper, never model argv
            struct ChildState {
                bool cancelling = false;
                QPointer<QEventLoop> drain;
            };
            const auto child = std::make_shared<ChildState>();
            const auto processId = processes->start(
                launch,
                [this, child, completion,
                 artifact = launch.arguments.last()](const ProcessRecord& record) {
                    if (record.state == ProcessState::Running) {
                        childPid.store(record.systemPid);
                        return;
                    }
                    if (record.state != ProcessState::Exited &&
                        record.state != ProcessState::Failed &&
                        record.state != ProcessState::Cancelled)
                        return;
                    childReleased.store(true);
                    if (child->drain)
                        child->drain->quit();
                    if (child->cancelling)
                        return; // AgentLoop closes cancellation after the owner releases the child.
                    ToolExecutionResult terminal{record.state == ProcessState::Cancelled
                                                     ? ToolExecutionStatus::Cancelled
                                                     : ToolExecutionStatus::Failed,
                                                 QStringLiteral("Owned fixture child released.")};
                    if (QFileInfo::exists(artifact))
                        terminal.mutations.append({artifact});
                    completion(std::move(terminal));
                },
                [this](const QString&, ProcessStream stream, const QByteArray& bytes) {
                    if (stream == ProcessStream::Stdout && bytes.contains("fixture-child-ready"))
                        slowBuildStarted.release();
                });
            return [this, child, processes, processId] {
                child->cancelling = true;
                QEventLoop drain;
                child->drain = &drain;
                QTimer watchdog;
                watchdog.setSingleShot(true);
                QObject::connect(&watchdog, &QTimer::timeout, &drain, &QEventLoop::quit);
                processes->terminate(processId);
                if (!childReleased.load()) {
                    watchdog.start(kBarrierTimeoutMs);
                    drain.exec();
                }
                child->drain.clear();
                if (!childReleased.load()) {
                    childDrainTimedOut.store(true);
                    processes->shutdown(); // reap only our owned helper on a failed test
                }
            };
        } else if (toolId == QLatin1String("run-command") &&
                   arg(QStringLiteral("command")) == QStringLiteral("echo slow-build")) {
            if (blockSlowBuild) {
                ++unauthorizedCalls;
                completion({ToolExecutionStatus::Blocked,
                            QStringLiteral("slow-build executed without the blocking context")});
                return IToolExecutor::Cancel{};
            }
        }

        result.structuredObservation = observation;
        completion(result);
        return IToolExecutor::Cancel{};
    }

private:
    QString resolved(const QString& raw) const {
        return QDir::cleanPath(
            QDir(root).absoluteFilePath(raw.isEmpty() ? QStringLiteral(".") : raw));
    }

    bool authorizedWrite(const QString& absolutePath, const QString& content) const {
        const auto canonical = QFileInfo(absolutePath).canonicalFilePath();
        const auto target = canonical.isEmpty() ? absolutePath : canonical;
        const auto base = QFileInfo(root).canonicalFilePath();
        if (target != base && !target.startsWith(base + QLatin1Char('/')))
            return false;
        const QString relative = QDir(base).relativeFilePath(target);
        if (!allowedWrites.contains(relative))
            return false;
        return content == allowedWrites.value(relative);
    }
};

bool registerFixtureTools(InMemoryToolRegistry& registry, IToolHandler& handler,
                          const QStringList& ids,
                          const QHash<QString, ToolDescriptor>& overrides = {}) {
    const auto descriptors = BuiltInToolProvider::descriptors();
    for (const auto& id : ids) {
        ToolDescriptor descriptor;
        if (auto overrideIt = overrides.find(id); overrideIt != overrides.end()) {
            descriptor = *overrideIt;
        } else {
            const auto found =
                std::find_if(descriptors.cbegin(), descriptors.cend(),
                             [&id](const ToolDescriptor& item) { return item.id == id; });
            if (found == descriptors.cend())
                return false;
            descriptor = *found;
        }
        if (!registry.registerTool(IToolRegistry::Registration{
                descriptor, std::shared_ptr<IToolHandler>(&handler, [](IToolHandler*) {})}))
            return false;
    }
    return true;
}

// Register a real built-in descriptor with no execution handler. The capability
// metadata exists but no execution target is wired; the gateway must refuse the
// operation without inventing a receipt.
bool registerDescriptorWithoutHandler(InMemoryToolRegistry& registry, const QString& id) {
    const auto descriptors = BuiltInToolProvider::descriptors();
    const auto found = std::find_if(descriptors.cbegin(), descriptors.cend(),
                                    [&id](const ToolDescriptor& item) { return item.id == id; });
    if (found == descriptors.cend())
        return false;
    return registry.registerTool(*found);
}

// Locate a real built-in descriptor by id, failing the calling test if absent.
ToolDescriptor builtInDescriptor(const QString& id) {
    const auto descriptors = BuiltInToolProvider::descriptors();
    const auto found = std::find_if(descriptors.cbegin(), descriptors.cend(),
                                    [&id](const ToolDescriptor& item) { return item.id == id; });
    Q_ASSERT(found != descriptors.cend());
    return *found;
}

// --------------------------------------------------------------------------
// Deterministic chat provider fixture (injected model layer)
// --------------------------------------------------------------------------

class FaultProvider final : public IChatProvider {
public:
    // Observation classification requests (deadlineMs 60000) deterministically
    // yield an empty typed requirement set. Planner requests (deadlineMs
    // 120000) consume the scripted queue; barrier semantics apply only to
    // planner requests.
    QString name() const override {
        return QStringLiteral("fixture-fault-provider");
    }
    ChatProviderStatus status() const override {
        return ChatProviderStatus::Ready;
    }
    ChatProviderReply sendMessage(const QString& message) override {
        Q_UNUSED(message);
        return {true, QStringLiteral("{\"requirements\":[]}"), {}};
    }

    ChatProviderReply sendRequest(const QString& message,
                                  const ChatRequestOptions& options) override {
        Q_UNUSED(message);
        if (options.deadlineMs == 60000)
            return {true, QStringLiteral("{\"requirements\":[]}"), {}};
        ++plannerRequests;
        if (barrierAllRequests) {
            arrived.release();
            if (!release.tryAcquire(1, kBarrierTimeoutMs))
                barrierTimedOut = true;
            if (options.cancellationToken && options.cancellationToken->load()) {
                ++cancelledReports;
                ChatProviderReply cancelledReply;
                cancelledReply.success = false;
                cancelledReply.category = ChatProviderErrorCategory::Cancelled;
                cancelledReply.lifecycle = ChatRequestLifecycle::Cancelled;
                cancelledReply.errorMessage = QStringLiteral("Generation cancelled by user.");
                return cancelledReply;
            }
        }
        if (!plannerReplies.isEmpty())
            return plannerReplies.takeFirst();
        if (faultAfterQueue)
            return *faultAfterQueue;
        ChatProviderReply missing;
        missing.success = false;
        missing.category = ChatProviderErrorCategory::ProviderFailure;
        missing.errorMessage = QStringLiteral("Fixture provider queue exhausted.");
        return missing;
    }

    QList<ChatProviderReply> plannerReplies;
    std::optional<ChatProviderReply> faultAfterQueue;
    bool barrierAllRequests = false;
    QSemaphore arrived{0};
    QSemaphore release{0};
    int plannerRequests = 0;
    int cancelledReports = 0;
    bool barrierTimedOut = false;
};

// Probe the actual LlmAgentRuntime dispatch contract without forcing deadlines to zero.
class CancellationProbeProvider final : public IChatProvider {
public:
    using IChatProvider::sendMessageStreaming;
    bool native = false, cooperative = false;
    QString artifact, readPath;
    std::atomic_int requests{0}, streamRequests{0}, emittedDeltas{0}, cancellationAcks{0},
        freshRequests{0};
    std::atomic_bool barrierTimedOut{false}, fresh{false};
    std::atomic_bool originalRequireBounded{false};
    QSemaphore entered, release;
    std::shared_ptr<std::atomic_bool> originalToken, freshToken;
    int originalDeadline = 0;
    bool originalNative = false;
    QString name() const override {
        return QStringLiteral("cancellation-probe");
    }
    ChatProviderStatus status() const override {
        return ChatProviderStatus::Ready;
    }
    bool supportsStreaming() const override {
        return true;
    }
    // Explicit bounded-streaming opt-in: this fixture implements the actual
    // deadline-aware streaming interface below, so the planner may route to it.
    bool supportsBoundedStreaming() const override {
        return true;
    }
    ChatProviderConcurrency concurrency() const override {
        return ChatProviderConcurrency::Supported;
    }
    ChatProviderReply sendMessage(const QString&) override {
        return {true, QStringLiteral("{\"requirements\":[]}"), {}};
    }
    ChatProviderReply sendRequest(const QString&, const ChatRequestOptions& options) override {
        if (options.deadlineMs == 60000)
            return {true, QStringLiteral("{\"requirements\":[]}"), {}};
        if (fresh.load()) {
            freshToken = options.cancellationToken;
            const int index = ++freshRequests;
            ChatProviderReply reply;
            reply.success = true;
            reply.lifecycle = ChatRequestLifecycle::Completed;
            reply.message =
                index == 1
                    ? QString::fromUtf8(
                          QJsonDocument(QJsonObject{{"action", "tool"},
                                                    {"tool", "read-file"},
                                                    {"args", QJsonObject{{"path", readPath}}}})
                              .toJson(QJsonDocument::Compact))
                    : QStringLiteral("{\"action\":\"final\",\"grounding\":\"context\",\"answer\":"
                                     "\"Fresh fixture read recorded.\"}");
            return reply;
        }
        ++requests;
        originalToken = options.cancellationToken;
        originalDeadline = options.deadlineMs;
        originalNative = options.nativeToolCalling;
        entered.release();
        if (!release.tryAcquire(1, kBarrierTimeoutMs))
            barrierTimedOut.store(true);
        return lateReply();
    }
    // Legacy streaming entry point retained for compatibility.
    ChatProviderReply
    sendMessageStreaming(const QString&, const std::function<void(const QString&)>& delta,
                         const std::shared_ptr<std::atomic_bool>& token) override {
        return streamBounded(delta, token, {}, false);
    }
    // Deadline-aware bounded streaming contract used by the planner.
    ChatProviderReply sendMessageStreaming(const QString&,
                                           const std::function<void(const QString&)>& delta,
                                           const std::shared_ptr<std::atomic_bool>& token,
                                           const ChatRequestOptions& options) override {
        return streamBounded(delta, token, options, options.requireBoundedStreaming);
    }

private:
    ChatProviderReply streamBounded(const std::function<void(const QString&)>& delta,
                                    const std::shared_ptr<std::atomic_bool>& token,
                                    const ChatRequestOptions& options, bool bounded) {
        ++streamRequests;
        originalToken = token;
        originalDeadline = options.deadlineMs;
        originalNative = options.nativeToolCalling;
        originalRequireBounded.store(bounded);
        // The first real delta is delivered while the provider stream is active
        // and before any cancellation is requested.
        delta(QStringLiteral("synthetic-first-delta"));
        ++emittedDeltas;
        entered.release();
        if (!release.tryAcquire(1, kBarrierTimeoutMs))
            barrierTimedOut.store(true);
        // This late delta is delivered after cancellation and must never be
        // accepted or published by the runtime.
        delta(QStringLiteral("synthetic-late-delta"));
        ++emittedDeltas;
        return lateReply();
    }

    ChatProviderReply lateReply() {
        ChatProviderReply reply;
        if (cooperative && originalToken && originalToken->load()) {
            ++cancellationAcks;
            reply.category = ChatProviderErrorCategory::Cancelled;
            reply.lifecycle = ChatRequestLifecycle::Cancelled;
            reply.errorMessage = QStringLiteral("Synthetic provider acknowledged cancellation.");
            return reply;
        }
        reply.success = true;
        reply.lifecycle = ChatRequestLifecycle::Completed;
        if (native)
            reply.toolCalls.append({"late-call",
                                    "write-file",
                                    {{"path", artifact}, {"content", "late mutation"}},
                                    {}});
        else
            reply.message = QStringLiteral(
                "{\"action\":\"final\",\"grounding\":\"context\",\"answer\":\"late completion\"}");
        return reply;
    }
};

ModelBinding fixtureBinding() {
    ModelBinding binding;
    binding.providerId = QStringLiteral("fixture-fault-provider");
    binding.modelId = QStringLiteral("fixture-model");
    return binding;
}

std::unique_ptr<LlmAgentRuntime> makeLlmPlanner(FaultProvider& provider,
                                                const QList<ToolDescriptor>& tools) {
    auto planner = std::make_unique<LlmAgentRuntime>(tools, &provider);
    planner->bindModel(fixtureBinding(),
                       std::shared_ptr<IChatProvider>(&provider, [](IChatProvider*) {}));
    return planner;
}

QList<ToolDescriptor> fixtureDescriptors(const QStringList& ids,
                                         const QHash<QString, ToolDescriptor>& overrides = {}) {
    const auto descriptors = BuiltInToolProvider::descriptors();
    QList<ToolDescriptor> result;
    for (const auto& id : ids) {
        if (auto overrideIt = overrides.find(id); overrideIt != overrides.end()) {
            result.append(*overrideIt);
            continue;
        }
        const auto found =
            std::find_if(descriptors.cbegin(), descriptors.cend(),
                         [&id](const ToolDescriptor& item) { return item.id == id; });
        Q_ASSERT(found != descriptors.cend());
        result.append(*found);
    }
    return result;
}

// --------------------------------------------------------------------------
// Shared fixture bundles
// --------------------------------------------------------------------------

void seedFile(const QDir& root, const QString& relative, const QString& content) {
    const QString path = root.filePath(relative);
    QVERIFY2(QDir().mkpath(QFileInfo(path).absolutePath()), "fixture mkdir failed");
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), "fixture seed open failed");
    QVERIFY2(file.write(content.toUtf8()) >= 0, "fixture seed write failed");
    file.close();
}

QString canonicalRoot(const QTemporaryDir& dir) {
    return QFileInfo(dir.path()).canonicalFilePath();
}

const char* loopPhaseName(AgentLoopPhase phase) {
    switch (phase) {
    case AgentLoopPhase::Idle:
        return "Idle";
    case AgentLoopPhase::Running:
        return "Running";
    case AgentLoopPhase::Cancelling:
        return "Cancelling";
    case AgentLoopPhase::AwaitingApproval:
        return "AwaitingApproval";
    case AgentLoopPhase::Completed:
        return "Completed";
    case AgentLoopPhase::Cancelled:
        return "Cancelled";
    case AgentLoopPhase::Failed:
        return "Failed";
    case AgentLoopPhase::Stuck:
        return "Stuck";
    }
    return "Unknown";
}

const char* terminalReasonName(AgentTerminalReason reason) {
    switch (reason) {
    case AgentTerminalReason::None:
        return "None";
    case AgentTerminalReason::Completed:
        return "Completed";
    case AgentTerminalReason::Cancelled:
        return "Cancelled";
    case AgentTerminalReason::ApprovalDenied:
        return "ApprovalDenied";
    case AgentTerminalReason::ProviderFailure:
        return "ProviderFailure";
    case AgentTerminalReason::ToolFailure:
        return "ToolFailure";
    case AgentTerminalReason::CapabilityUnavailable:
        return "CapabilityUnavailable";
    case AgentTerminalReason::SecurityDenied:
        return "SecurityDenied";
    case AgentTerminalReason::IterationLimit:
        return "IterationLimit";
    case AgentTerminalReason::UnableToComplete:
        return "UnableToComplete";
    }
    return "Unknown";
}

// Fixture metadata runtime exposing exactly the fixture descriptors, so
// AgentRuntime reaches the compatibility-handler seam without starting a
// second execution engine.
class FixtureMetadataRuntime final : public IAgentRuntime {
public:
    explicit FixtureMetadataRuntime(QList<ToolDescriptor> tools) : tools_(std::move(tools)) {}
    QString name() const override {
        return QStringLiteral("fixture-metadata");
    }
    AgentStatus status() const override {
        return AgentStatus::Ready;
    }
    QList<AgentCapabilityDescriptor> capabilities() const override {
        return {};
    }
    QList<ToolDescriptor> availableTools() const override {
        return tools_;
    }
    ToolInvocationPlan plan(const AgentRequest&) const override {
        return {};
    }
    AgentResponse execute(const AgentRequest&) override {
        return {};
    }

private:
    QList<ToolDescriptor> tools_;
};

StaticSandboxPolicy permissiveSandbox() {
    return StaticSandboxPolicy(QSet<QString>{QStringLiteral("tool.metadata.read"),
                                             QStringLiteral("tool.risk.medium"),
                                             QStringLiteral("tool.risk.high")});
}

// Loop-level fixture bundle.
struct LoopFixture {
    ScriptedPlanner planner;
    FixtureHandler handler;
    FixtureApprovalPolicy approval;
    StaticSandboxPolicy sandbox = permissiveSandbox();
    NoOpExecutor executor;
    InMemoryToolRegistry registry;
    QStringList knownTools;

    void registerTools(const QStringList& ids,
                       const QHash<QString, ToolDescriptor>& overrides = {}) {
        QVERIFY2(registerFixtureTools(registry, handler, ids, overrides),
                 "fixture tool registration failed");
        knownTools = ids;
    }
};

// Runtime-level fixture bundle driving the AgentRuntime session API.
struct RuntimeFixture {
    FixtureHandler handler;
    FixtureApprovalPolicy approval;
    StaticSandboxPolicy sandbox = permissiveSandbox();
    NoOpExecutor executor;
    ScriptedPlanner planner;
    std::unique_ptr<AgentRuntime> runtime;
    QString session;
    QStringList toolIds;

    void create(const QStringList& toolIds_, std::shared_ptr<IPermissionGrantStore> grants = {}) {
        toolIds = toolIds_;
        runtime = std::make_unique<AgentRuntime>(
            std::make_unique<FixtureMetadataRuntime>(fixtureDescriptors(toolIds)), &planner,
            executor, approval, sandbox, nullptr, nullptr, nullptr, std::move(grants));
        // Replace each compatibility-handler registration with the
        // deterministic fixture handler.
        for (const auto& descriptor : fixtureDescriptors(toolIds)) {
            QVERIFY2(runtime->toolRegistry().unregisterTool(descriptor.id),
                     qPrintable(QStringLiteral("unregister failed for %1").arg(descriptor.id)));
            QVERIFY2(
                runtime->toolRegistry().registerTool(IToolRegistry::Registration{
                    descriptor, std::shared_ptr<IToolHandler>(&handler, [](IToolHandler*) {})}),
                qPrintable(QStringLiteral("register failed for %1").arg(descriptor.id)));
        }
        session = runtime->createSession();
        QVERIFY(!session.isEmpty());
    }

    void configure(const QString& workspaceRoot, bool autonomous = true,
                   const QStringList& availableTools = {}) {
        QVERIFY2(runtime && !session.isEmpty(), "create runtime before configure");
        handler.root = workspaceRoot;
        AgentSessionOptions options;
        options.autonomousMode = autonomous;
        options.workspaceContext.rootPath = workspaceRoot;
        if (!availableTools.isEmpty()) {
            options.availableToolIds = availableTools;
            options.restrictAvailableTools = true;
        }
        runtime->configureSession(session, std::move(options));
    }
};

// Runs an AgentLoop synchronously or asynchronously with a bounded event-loop
// barrier; returns the terminal state in both modes. Uses no Qt assertion
// macros because this is a value-returning helper.
AgentLoopState runLoop(AgentLoop& loop, QObject* owner, const QString& sessionId, bool async,
                       const QString& goal) {
    AgentLoopState state;
    if (async) {
        int terminals = 0;
        QEventLoop pump;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &pump, &QEventLoop::quit);
        loop.runAsync(goal, sessionId, owner, [&](const AgentLoopState& result) {
            state = result;
            ++terminals;
            pump.quit();
        });
        timeout.start(kBarrierTimeoutMs);
        pump.exec();
    } else {
        state = loop.run(goal, sessionId);
    }
    return state;
}

void configureLoopFixture(AgentLoop& loop, LoopFixture& fixture, const QString& root) {
    fixture.handler.root = root;
    loop.setToolRegistry(&fixture.registry);
    loop.setResourceScope(root);
    AgentContextInput::WorkspaceContext context;
    context.rootPath = root;
    loop.setWorkspaceContext(context);
}

using CompileOracle = sentinel::test::IndependentCompileOracle;

bool independentlyAccepted(const AgentLoopState& state, const WorkspaceVerifier& workspace,
                           const WorkspaceVerifier::Expected& allowed,
                           const CompileOracle::Result& actual) {
    return state.phase == AgentLoopPhase::Completed && !state.finalAnswer.isEmpty() &&
           workspace.matches(allowed) && actual.matchesSources(workspace.root()) && actual.passed();
}

QString terminalFailureDescription(const AgentLoopState& state) {
    return QStringLiteral("phase=%1 reason=%2 abort=%3")
        .arg(loopPhaseName(state.phase))
        .arg(terminalReasonName(state.terminalReason))
        .arg(state.abortReason);
}

} // namespace

// --------------------------------------------------------------------------
// Harness test object
// --------------------------------------------------------------------------

class AgentReliabilityHarnessTest final : public QObject {
    Q_OBJECT

private slots:
    // Harness self-check: the verification oracle must reject a planted effect
    // outside the allowlist (independence from tool outputs).
    void harnessVerifierRejectsSideEffectsOutsideAllowlist() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a - b; }\n"));
        WorkspaceVerifier verifier(canonicalRoot(workspace));

        WorkspaceVerifier::Expected allowed;
        allowed.changed = {QStringLiteral("src/calc.cpp")};
        QString actual;
        // A placeholder success must be rejected while nothing changed yet.
        QVERIFY(!verifier.matches(allowed, &actual));

        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        QVERIFY(verifier.matches(allowed, &actual));

        seedFile(QDir(workspace.path()), QStringLiteral("build/rogue.log"), QStringLiteral("x"));
        seedFile(QDir(workspace.path()), QStringLiteral("tests/oracle.txt"),
                 QStringLiteral("tampered"));
        QVERIFY(!verifier.matches(allowed, &actual));
        QVERIFY(!verifier.untouched());
    }

    // ------------------------------------------------------------------
    // COR-01: read-only inspection. Grounded scoped claim completes; the
    // insufficient-evidence variant fails boundedly without fabrication.
    // ------------------------------------------------------------------
    void cor01_readOnlyInspection_data() {
        QTest::addColumn<bool>("async");
        QTest::addColumn<bool>("sufficient");
        for (bool async : {false, true})
            for (bool sufficient : {true, false}) {
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(
                    async ? QStringLiteral("async") : QStringLiteral("sync"),
                    sufficient ? QStringLiteral("grounded-claim")
                               : QStringLiteral("insufficient-evidence"))))
                    << async << sufficient;
            }
    }

    void cor01_readOnlyInspection() {
        QFETCH(bool, async);
        QFETCH(bool, sufficient);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("README.md"),
                 QStringLiteral("fixture synthetic repository"));
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("read-file"), QStringLiteral("list-directory")});
        const auto readPath = root + QStringLiteral("/src/calc.cpp");

        auto policy = std::make_shared<FixedIntentPolicy>();
        ObservationRequirement readRequirement;
        readRequirement.domain = ObservationDomain::FileSystem;
        readRequirement.resourceHint = readPath;
        readRequirement.freshness = EvidenceFreshness::TurnScoped;
        readRequirement.purpose = ObservationPurpose::Inspect;
        readRequirement.toolId = QStringLiteral("read-file");
        // Both variants classify the same evidence requirement; the negative
        // variant never performs the required read.
        policy->intent.requirements = {readRequirement};

        ScriptedPlanner planner;
        auto list = toolDecision(QStringLiteral("list-directory"));
        appendArg(list, QStringLiteral("path"), root);
        auto read = toolDecision(QStringLiteral("read-file"));
        appendArg(read, QStringLiteral("path"), readPath);
        auto grounded = finalDecision(
            QStringLiteral("src/calc.cpp uses the addition operator per the fresh read receipt."));
        grounded.grounding = GroundingMode::Verified;
        grounded.groundingDeclared = true;
        planner.decisions = sufficient ? QVector<AgentStepDecision>{list, read, grounded}
                                       : QVector<AgentStepDecision>{list, grounded};
        planner.repeatLast = true;
        planner.canonicalFinal = sufficient;
        planner.canonicalIntent = policy->intent;

        AgentLoop::Config config;
        config.maxIterations = 3;
        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools, config);
        configureLoopFixture(loop, fixture, root);
        loop.setObservationIntentPolicy(policy);
        loop.setToolCallIdProvider([](int) { return QStringLiteral("observed"); });
        WorkspaceVerifier verifier(root);

        const auto state =
            runLoop(loop, this, QStringLiteral("cor01"), async,
                    QStringLiteral("Inspect src/calc.cpp read-only and report its operator"));
        QVERIFY(verifier.untouched());

        if (sufficient) {
            QCOMPARE(state.phase, AgentLoopPhase::Completed);
            QVERIFY(state.finalAnswer.contains(QStringLiteral("return a + b")));
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 1);
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("list-directory"), 0), 1);
            QVERIFY(std::any_of(state.evidence.cbegin(), state.evidence.cend(),
                                [](const EvidenceRecord& item) {
                                    return item.outcome == EvidenceOutcome::Verified;
                                }));
        } else {
            QVERIFY2(state.phase == AgentLoopPhase::Failed,
                     qPrintable(terminalFailureDescription(state)));
            QVERIFY2(state.finalAnswer.isEmpty(), qPrintable(state.finalAnswer));
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 0);
        }
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
    }

    // ------------------------------------------------------------------
    // COR-01 negative: hidden-scope claim built from a non-recursive listing
    // is rejected; the universal negative claim never completes.
    // ------------------------------------------------------------------
    void cor01_hiddenScopeClaimNegative() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("visible.txt"), QStringLiteral("fixture"));
        seedFile(QDir(workspace.path()), QStringLiteral(".hidden.txt"), QStringLiteral("hidden"));

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("list-directory"), QStringLiteral("read-file")});

        auto policy = std::make_shared<FixedIntentPolicy>();
        ObservationRequirement hiddenClaim;
        hiddenClaim.domain = ObservationDomain::FileSystem;
        hiddenClaim.resourceHint = root;
        hiddenClaim.claimType = ClaimType::HiddenEntriesExist;
        hiddenClaim.claimId = QStringLiteral("hidden");
        hiddenClaim.toolId = QStringLiteral("list-directory");
        policy->intent.requirements = {hiddenClaim};

        ScriptedPlanner planner;
        auto list = toolDecision(QStringLiteral("list-directory"));
        appendArg(list, QStringLiteral("path"), root);
        auto claim = finalDecision(QStringLiteral("No hidden files exist."));
        claim.grounding = GroundingMode::Verified;
        claim.groundingDeclared = true;
        planner.decisions = {list, claim};
        planner.repeatLast = true;

        AgentLoop::Config config;
        config.maxIterations = 3;
        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools, config);
        configureLoopFixture(loop, fixture, root);
        loop.setObservationIntentPolicy(policy);
        loop.setToolCallIdProvider([](int) { return QStringLiteral("observed"); });
        WorkspaceVerifier verifier(root);

        const auto state = runLoop(loop, this, QStringLiteral("cor01-hidden"), false,
                                   QStringLiteral("Are there no hidden entries here?"));
        QVERIFY2(state.phase == AgentLoopPhase::Failed,
                 qPrintable(terminalFailureDescription(state)));
        QVERIFY(state.finalAnswer.isEmpty());
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 0);
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QVERIFY(verifier.untouched());
    }

    // ------------------------------------------------------------------
    // COR-01 (extended): truncated/empty/missing context and an omitted
    // critical dependency must not become unsupported conclusions. Observed
    // facts (a real file read) still ground a scoped final. Every variant is
    // read-only: no write receipt and no workspace mutation is permitted.
    // ------------------------------------------------------------------
    void cor01_truncatedAndMissingEvidence_data() {
        QTest::addColumn<QString>("variant");
        for (const auto& variant :
             {"truncated-listing-absence", "hidden-excluded-claim", "empty-incomplete-read",
              "missing-required-file", "omitted-critical-dependency", "dependency-observed",
              "truncated-listing-positive"})
            QTest::newRow(variant) << QString::fromLatin1(variant);
    }

    void cor01_truncatedAndMissingEvidence() {
        QFETCH(QString, variant);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(root), "README.md", "fixture synthetic repository\n");
        seedFile(QDir(root), "src/calc.cpp", "int add(int a,int b){ return a + b; }\n");
        seedFile(QDir(root), "src/main.cpp",
                 "#include \"helper.h\"\nint main(){ return add(1,2); }\n");
        seedFile(QDir(root), "src/helper.h", "int add(int,int);\n");
        const auto calcPath = root + QStringLiteral("/src/calc.cpp");
        const auto mainPath = root + QStringLiteral("/src/main.cpp");
        const auto helperPath = root + QStringLiteral("/src/helper.h");

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("list-directory"), QStringLiteral("read-file")});
        WorkspaceVerifier verifier(root);

        auto policy = std::make_shared<FixedIntentPolicy>();
        ScriptedPlanner planner;

        if (variant == "truncated-listing-absence") {
            fixture.handler.observationIncomplete = true;
            ObservationRequirement claim;
            claim.domain = ObservationDomain::FileSystem;
            claim.resourceHint = root;
            claim.claimType = ClaimType::PathPatternExists;
            claim.claimId = QStringLiteral("claim-1");
            claim.claimQuery = QStringLiteral("*.md");
            claim.toolId = QStringLiteral("list-directory");
            policy->intent.requirements = {claim};
            auto list = toolDecision(QStringLiteral("list-directory"));
            appendArg(list, QStringLiteral("path"), root);
            auto fabricated =
                finalDecision(QStringLiteral("No markdown files match in the repository."));
            fabricated.grounding = GroundingMode::Verified;
            fabricated.groundingDeclared = true;
            fabricated.claims = {{QStringLiteral("claim-1"), true}};
            planner.decisions = {list, fabricated};
            planner.repeatLast = true;
        } else if (variant == "hidden-excluded-claim") {
            ObservationRequirement claim;
            claim.domain = ObservationDomain::FileSystem;
            claim.resourceHint = root;
            claim.claimType = ClaimType::HiddenEntriesExist;
            claim.claimId = QStringLiteral("hidden");
            claim.toolId = QStringLiteral("list-directory");
            policy->intent.requirements = {claim};
            auto list = toolDecision(QStringLiteral("list-directory"));
            appendArg(list, QStringLiteral("path"), root);
            auto fabricated = finalDecision(QStringLiteral("No hidden entries exist here."));
            fabricated.grounding = GroundingMode::Verified;
            fabricated.groundingDeclared = true;
            fabricated.claims = {{QStringLiteral("hidden"), true}};
            planner.decisions = {list, fabricated};
            planner.repeatLast = true;
        } else if (variant == "empty-incomplete-read") {
            fixture.handler.observationIncomplete = true;
            fixture.handler.emptyFileContent = true;
            ObservationRequirement read;
            read.domain = ObservationDomain::FileSystem;
            read.resourceHint = calcPath;
            read.purpose = ObservationPurpose::Inspect;
            read.toolId = QStringLiteral("read-file");
            policy->intent.requirements = {read};
            auto readCall = toolDecision(QStringLiteral("read-file"));
            appendArg(readCall, QStringLiteral("path"), calcPath);
            auto fabricated = finalDecision(QStringLiteral("src/calc.cpp is empty."));
            fabricated.grounding = GroundingMode::Verified;
            fabricated.groundingDeclared = true;
            planner.decisions = {readCall, fabricated};
            planner.repeatLast = true;
        } else if (variant == "missing-required-file") {
            const auto absent = root + QStringLiteral("/src/absent.cpp");
            ObservationRequirement read;
            read.domain = ObservationDomain::FileSystem;
            read.resourceHint = absent;
            read.purpose = ObservationPurpose::Inspect;
            read.toolId = QStringLiteral("read-file");
            policy->intent.requirements = {read};
            auto readCall = toolDecision(QStringLiteral("read-file"));
            appendArg(readCall, QStringLiteral("path"), absent);
            auto fabricated = finalDecision(QStringLiteral("src/absent.cpp defines the parser."));
            fabricated.grounding = GroundingMode::Verified;
            fabricated.groundingDeclared = true;
            planner.decisions = {readCall, fabricated};
            planner.repeatLast = true;
        } else if (variant == "omitted-critical-dependency" || variant == "dependency-observed") {
            ObservationRequirement mainRead;
            mainRead.domain = ObservationDomain::FileSystem;
            mainRead.resourceHint = mainPath;
            mainRead.purpose = ObservationPurpose::Inspect;
            mainRead.toolId = QStringLiteral("read-file");
            ObservationRequirement helperRead = mainRead;
            helperRead.resourceHint = helperPath;
            policy->intent.requirements = {mainRead, helperRead};
            auto readMain = toolDecision(QStringLiteral("read-file"));
            appendArg(readMain, QStringLiteral("path"), mainPath);
            auto readHelper = toolDecision(QStringLiteral("read-file"));
            appendArg(readHelper, QStringLiteral("path"), helperPath);
            auto grounded = finalDecision(QStringLiteral("Both source files were inspected."));
            grounded.grounding = GroundingMode::Verified;
            grounded.groundingDeclared = true;
            const bool observed = variant == "dependency-observed";
            planner.decisions = observed
                                    ? QVector<AgentStepDecision>{readMain, readHelper, grounded}
                                    : QVector<AgentStepDecision>{readMain, grounded};
            planner.repeatLast = true;
            planner.canonicalFinal = observed;
            planner.canonicalIntent = policy->intent;
        } else { // truncated-listing-positive
            fixture.handler.observationIncomplete = true;
            ObservationRequirement read;
            read.domain = ObservationDomain::FileSystem;
            read.resourceHint = calcPath;
            read.purpose = ObservationPurpose::Inspect;
            read.toolId = QStringLiteral("read-file");
            policy->intent.requirements = {read};
            auto list = toolDecision(QStringLiteral("list-directory"));
            appendArg(list, QStringLiteral("path"), root);
            auto readCall = toolDecision(QStringLiteral("read-file"));
            appendArg(readCall, QStringLiteral("path"), calcPath);
            auto grounded = finalDecision(QStringLiteral("positive fixture read"));
            grounded.grounding = GroundingMode::Verified;
            grounded.groundingDeclared = true;
            planner.decisions = {list, readCall, grounded};
            planner.repeatLast = true;
            planner.canonicalFinal = true;
            planner.canonicalIntent = policy->intent;
        }

        AgentLoop::Config config;
        config.maxIterations = 3;
        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools, config);
        configureLoopFixture(loop, fixture, root);
        loop.setObservationIntentPolicy(policy);
        loop.setToolCallIdProvider([](int) { return QStringLiteral("observed"); });

        const auto state = runLoop(loop, this, QStringLiteral("cor01"), false,
                                   QStringLiteral("Inspect the fixture repository read-only"));

        // Read-only invariant for every variant.
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QCOMPARE(fixture.executor.calls, 0);
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("write-file"), 0), 0);
        QVERIFY(verifier.untouched());

        if (variant == "truncated-listing-absence") {
            QVERIFY2(state.phase == AgentLoopPhase::Failed,
                     qPrintable(terminalFailureDescription(state)));
            QVERIFY(state.finalAnswer.isEmpty());
            QVERIFY2(state.abortReason.contains(QStringLiteral("lacks fresh filesystem evidence")),
                     qPrintable(state.abortReason));
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("list-directory"), 0), 1);
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 0);
        } else if (variant == "hidden-excluded-claim") {
            QVERIFY2(state.phase == AgentLoopPhase::Failed,
                     qPrintable(terminalFailureDescription(state)));
            QVERIFY(state.finalAnswer.isEmpty());
            QVERIFY2(state.abortReason.contains(QStringLiteral("cannot determine")),
                     qPrintable(state.abortReason));
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("list-directory"), 0), 1);
        } else if (variant == "empty-incomplete-read") {
            QVERIFY2(state.phase == AgentLoopPhase::Failed,
                     qPrintable(terminalFailureDescription(state)));
            QVERIFY(state.finalAnswer.isEmpty());
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 1);
        } else if (variant == "missing-required-file") {
            QVERIFY2(state.phase == AgentLoopPhase::Failed,
                     qPrintable(terminalFailureDescription(state)));
            QVERIFY(state.finalAnswer.isEmpty());
            QVERIFY(!state.finalAnswer.contains(QStringLiteral("parser")));
        } else if (variant == "omitted-critical-dependency") {
            QVERIFY2(state.phase == AgentLoopPhase::Failed,
                     qPrintable(terminalFailureDescription(state)));
            QVERIFY(state.finalAnswer.isEmpty());
            QVERIFY2(state.abortReason.contains(QStringLiteral("helper.h")),
                     qPrintable(state.abortReason));
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 1);
        } else if (variant == "dependency-observed") {
            QCOMPARE(state.phase, AgentLoopPhase::Completed);
            QVERIFY(!state.finalAnswer.isEmpty());
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 2);
        } else { // truncated-listing-positive
            QCOMPARE(state.phase, AgentLoopPhase::Completed);
            QVERIFY(state.finalAnswer.contains(QStringLiteral("return a + b")));
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("list-directory"), 0), 1);
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 1);
        }
    }

    // ------------------------------------------------------------------
    // COR-02: single-file C++ repair with a planted operator bug; the
    // verifier requires exactly one changed file and only authorized
    // build/test commands. A claimed-verified final over a failing receipt
    // must fail.
    // ------------------------------------------------------------------
    void cor02_singleFileRepair_data() {
        QTest::addColumn<bool>("async");
        QTest::addColumn<bool>("grounded");
        for (bool async : {false, true})
            for (bool grounded : {true, false}) {
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(
                    async ? QStringLiteral("async") : QStringLiteral("sync"),
                    grounded ? QStringLiteral("repair-ok") : QStringLiteral("fabricated-claim"))))
                    << async << grounded;
            }
    }

    void cor02_singleFileRepair() {
        QFETCH(bool, async);
        QFETCH(bool, grounded);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        const auto missingCompiler = CompileOracle::unavailableReason();
        if (!missingCompiler.isEmpty())
            QSKIP(qPrintable(missingCompiler));
        QVERIFY(CompileOracle::initializeRepository(root).succeeded());
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a - b; }\n"));
        seedFile(QDir(workspace.path()), QStringLiteral("tests/oracle.txt"),
                 QStringLiteral("trusted fixture baseline"));

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("read-file"), QStringLiteral("write-file"),
                               QStringLiteral("run-command"), QStringLiteral("git-diff")});
        fixture.handler.allowedCommands = {QStringLiteral("cmake --build build"),
                                           QStringLiteral("ctest --test-dir build")};
        fixture.handler.testReceiptFails = !grounded;
        fixture.handler.allowedWrites.insert(
            QStringLiteral("src/calc.cpp"),
            QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        const auto calcPath = root + QStringLiteral("/src/calc.cpp");

        auto policy = std::make_shared<FixedIntentPolicy>();
        ObservationRequirement readRequirement;
        readRequirement.domain = ObservationDomain::FileSystem;
        readRequirement.resourceHint = calcPath;
        readRequirement.freshness = EvidenceFreshness::TurnScoped;
        readRequirement.purpose = ObservationPurpose::Inspect;
        readRequirement.toolId = QStringLiteral("read-file");
        policy->intent.requirements.append(readRequirement);
        ObservationRequirement diffRequirement;
        diffRequirement.domain = ObservationDomain::Workspace;
        diffRequirement.resourceHint = root;
        diffRequirement.freshness = EvidenceFreshness::Live;
        diffRequirement.purpose = ObservationPurpose::Inspect;
        diffRequirement.toolId = QStringLiteral("git-diff");
        policy->intent.requirements.append(diffRequirement);
        ObservationRequirement buildRequirement;
        buildRequirement.domain = ObservationDomain::ProcessExecution;
        buildRequirement.resourceHint = QStringLiteral("cmake --build build");
        buildRequirement.freshness = EvidenceFreshness::TurnScoped;
        buildRequirement.purpose = ObservationPurpose::Operate;
        policy->intent.requirements.append(buildRequirement);
        ObservationRequirement testRequirement;
        testRequirement.domain = ObservationDomain::ProcessExecution;
        testRequirement.resourceHint = QStringLiteral("ctest --test-dir build");
        testRequirement.freshness = EvidenceFreshness::TurnScoped;
        testRequirement.purpose = ObservationPurpose::Operate;
        policy->intent.requirements.append(testRequirement);

        ScriptedPlanner planner;
        auto initialRead = toolDecision(QStringLiteral("read-file"));
        appendArg(initialRead, QStringLiteral("path"), calcPath);
        auto write = toolDecision(QStringLiteral("write-file"));
        appendArg(write, QStringLiteral("path"), calcPath);
        appendArg(write, QStringLiteral("content"),
                  QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        auto freshRead = toolDecision(QStringLiteral("read-file"));
        appendArg(freshRead, QStringLiteral("path"), calcPath);
        auto build = toolDecision(QStringLiteral("run-command"));
        appendArg(build, QStringLiteral("command"), QStringLiteral("cmake --build build"));
        auto test = toolDecision(QStringLiteral("run-command"));
        appendArg(test, QStringLiteral("command"), QStringLiteral("ctest --test-dir build"));
        auto diff = toolDecision(QStringLiteral("git-diff"));
        appendArg(diff, QStringLiteral("path"), root);
        planner.decisions = {initialRead, write, freshRead, build, test, diff};
        auto verified = finalDecision(QStringLiteral(
            "Replaced the subtraction, tests return ALL_TESTS_PASSED, and the git-diff receipt "
            "shows - return a - b\\n+ return a + b."));
        verified.grounding = GroundingMode::Verified;
        verified.groundingDeclared = true;
        planner.decisions.append(verified);
        planner.repeatLast = true;
        planner.canonicalFinal = grounded;
        planner.canonicalIntent = policy->intent;

        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools);
        configureLoopFixture(loop, fixture, root);
        loop.setObservationIntentPolicy(policy);
        loop.setToolCallIdProvider([](int) { return QStringLiteral("observed"); });
        WorkspaceVerifier verifier(root);

        const auto state = runLoop(loop, this, QStringLiteral("cor02"), async,
                                   QStringLiteral("Fix the planted operator bug in src/calc.cpp"));

        WorkspaceVerifier::Expected expected;
        expected.changed = {QStringLiteral("src/calc.cpp")};
        QString actual;
        if (grounded) {
            QCOMPARE(state.phase, AgentLoopPhase::Completed);
            QVERIFY2(verifier.matches(expected, &actual), qPrintable(actual));
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("write-file"), 0), 1);
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("run-command"), 0), 2);
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("git-diff"), 0), 1);
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 3);
            QVERIFY(state.finalAnswer.contains(QStringLiteral("return a + b")));
            QVERIFY(std::any_of(state.evidence.cbegin(), state.evidence.cend(),
                                [](const EvidenceRecord& item) {
                                    return item.scope == EvidenceScope::Operation &&
                                           item.outcome == EvidenceOutcome::Verified;
                                }));
        } else {
            // The receipt failed; a claimed-verified final is rejected and the
            // run terminates boundedly (Failed or repeat-fenced Stuck) instead
            // of fabricating a completion.
            QVERIFY2(state.phase == AgentLoopPhase::Failed || state.phase == AgentLoopPhase::Stuck,
                     qPrintable(terminalFailureDescription(state)));
            QVERIFY(state.finalAnswer.isEmpty());
            QVERIFY2(verifier.matches(expected, &actual), qPrintable(actual));
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("run-command"), 0), 2);
        }
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QCOMPARE(verifier.fileContent(QStringLiteral("src/calc.cpp")),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        const auto beforeOracle = stateOf(root).files;
        const auto real = CompileOracle{}.verify(root, false, CompileOracle::Mode::Normal);
        qInfo().noquote() << "Independent oracle:" << real.summary();
        QCOMPARE(stateOf(root).files,
                 beforeOracle); // Compiler/test must not mutate sources or tests.
        QVERIFY2(verifier.matches(expected, &actual), qPrintable(actual));
        QVERIFY2(real.passed(), qPrintable(real.summary()));
        QCOMPARE(independentlyAccepted(state, verifier, expected, real), grounded);
    }

    // ------------------------------------------------------------------
    // COR-03: dependent multi-file repair; exactly two changes, written in
    // interface-then-implementation order; trusted fixture files untouched.
    // ------------------------------------------------------------------
    void cor03_multiFileDependentRepair() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        const auto missingCompiler = CompileOracle::unavailableReason();
        if (!missingCompiler.isEmpty())
            QSKIP(qPrintable(missingCompiler));
        QVERIFY(CompileOracle::initializeRepository(root).succeeded());
        seedFile(QDir(workspace.path()), QStringLiteral("include/calc.h"),
                 QStringLiteral("int add(int,int);\n"));
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("#include \"calc.h\"\nint add(int a,int b){ return a - b; }\n"));
        seedFile(QDir(workspace.path()), QStringLiteral("tests/test_calc.txt"),
                 QStringLiteral("trusted fixture baseline"));

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("write-file"), QStringLiteral("run-command")});
        fixture.handler.allowedCommands = {QStringLiteral("cmake --build build")};
        fixture.handler.allowedWrites.insert(QStringLiteral("include/calc.h"),
                                             QStringLiteral("int add(int,int,int);\n"));
        fixture.handler.allowedWrites.insert(
            QStringLiteral("src/calc.cpp"),
            QStringLiteral(
                "#include \"calc.h\"\nint add(int a,int b,int c){ return a + b + c; }\n"));

        ScriptedPlanner planner;
        auto writeHeader = toolDecision(QStringLiteral("write-file"));
        appendArg(writeHeader, QStringLiteral("path"), root + QStringLiteral("/include/calc.h"));
        appendArg(writeHeader, QStringLiteral("content"),
                  QStringLiteral("int add(int,int,int);\n"));
        auto writeSource = toolDecision(QStringLiteral("write-file"));
        appendArg(writeSource, QStringLiteral("path"), root + QStringLiteral("/src/calc.cpp"));
        appendArg(writeSource, QStringLiteral("content"),
                  QStringLiteral(
                      "#include \"calc.h\"\nint add(int a,int b,int c){ return a + b + c; }\n"));
        auto build = toolDecision(QStringLiteral("run-command"));
        appendArg(build, QStringLiteral("command"), QStringLiteral("cmake --build build"));
        planner.decisions = {writeHeader, writeSource, build};
        auto finish = finalDecision(QStringLiteral(
            "Both dependent files updated and the build receipt completed ALL_TESTS_PASSED."));
        finish.grounding = GroundingMode::Context;
        finish.groundingDeclared = true;
        planner.decisions.append(finish);
        planner.repeatLast = false;

        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools);
        configureLoopFixture(loop, fixture, root);
        WorkspaceVerifier verifier(root);

        const auto state = runLoop(loop, this, QStringLiteral("cor03"), false,
                                   QStringLiteral("Update the dependent header and source"));
        QCOMPARE(state.phase, AgentLoopPhase::Completed);
        WorkspaceVerifier::Expected expected;
        expected.changed = {QStringLiteral("include/calc.h"), QStringLiteral("src/calc.cpp")};
        QString actual;
        QVERIFY2(verifier.matches(expected, &actual), qPrintable(actual));
        QCOMPARE(verifier.fileContent(QStringLiteral("tests/test_calc.txt")),
                 QStringLiteral("trusted fixture baseline"));
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("write-file"), 0), 2);
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        const int headerIndex = fixture.handler.order.indexOf(
            QStringLiteral("write-file(%1)").arg(root + QStringLiteral("/include/calc.h")));
        const int sourceIndex = fixture.handler.order.indexOf(
            QStringLiteral("write-file(%1)").arg(root + QStringLiteral("/src/calc.cpp")));
        QVERIFY(headerIndex >= 0 && sourceIndex > headerIndex);
        QCOMPARE(verifier.fileContent(QStringLiteral("include/calc.h")),
                 QStringLiteral("int add(int,int,int);\n"));
        QCOMPARE(verifier.fileContent(QStringLiteral("src/calc.cpp")),
                 QStringLiteral(
                     "#include \"calc.h\"\nint add(int a,int b,int c){ return a + b + c; }\n"));
        const auto beforeOracle = stateOf(root).files;
        const auto real = CompileOracle{}.verify(root, true, CompileOracle::Mode::Normal);
        qInfo().noquote() << "Independent oracle:" << real.summary();
        QCOMPARE(stateOf(root).files,
                 beforeOracle); // Compiler/test must not mutate sources or tests.
        QVERIFY2(verifier.matches(expected, &actual), qPrintable(actual));
        QVERIFY2(real.passed(), qPrintable(real.summary()));
        QCOMPARE(independentlyAccepted(state, verifier, expected, real), true);
    }

    // ------------------------------------------------------------------
    // COR-04: fixed build/test invocation success vs a deliberately
    // unfixable failing receipt that must terminate without looping.
    // ------------------------------------------------------------------
    void cor04_buildTestInvocation_data() {
        QTest::addColumn<bool>("testFails");
        QTest::newRow("authorized-invocation-succeeds") << false;
        QTest::newRow("deliberately-unfixable") << true;
    }

    void cor04_buildTestInvocation() {
        QFETCH(bool, testFails);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        const auto missingCompiler = CompileOracle::unavailableReason();
        if (!missingCompiler.isEmpty())
            QSKIP(qPrintable(missingCompiler));
        QVERIFY(CompileOracle::initializeRepository(root).succeeded());
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a - b; }\n"));

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("write-file"), QStringLiteral("run-command")});
        fixture.handler.allowedCommands = {QStringLiteral("cmake --build build"),
                                           QStringLiteral("ctest --test-dir build")};
        fixture.handler.testReceiptFails = testFails;
        fixture.handler.allowedWrites.insert(
            QStringLiteral("src/calc.cpp"),
            QStringLiteral("int add(int a,int b){ return a + b; }\n"));

        ScriptedPlanner planner;
        auto write = toolDecision(QStringLiteral("write-file"));
        appendArg(write, QStringLiteral("path"), root + QStringLiteral("/src/calc.cpp"));
        appendArg(write, QStringLiteral("content"),
                  QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        auto build = toolDecision(QStringLiteral("run-command"));
        appendArg(build, QStringLiteral("command"), QStringLiteral("cmake --build build"));
        auto test = toolDecision(QStringLiteral("run-command"));
        appendArg(test, QStringLiteral("command"), QStringLiteral("ctest --test-dir build"));
        planner.decisions = {write, build, test};
        if (testFails) {
            planner.decisions.append(giveUpDecision(
                QStringLiteral("Fixture test oracle keeps failing; stopping bounded.")));
        } else {
            auto finish = finalDecision(
                QStringLiteral("Authorized command sequence completed with ALL_TESTS_PASSED."));
            finish.grounding = GroundingMode::Context;
            finish.groundingDeclared = true;
            planner.decisions.append(finish);
        }
        planner.repeatLast = false;

        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools);
        configureLoopFixture(loop, fixture, root);
        WorkspaceVerifier verifier(root);

        const auto state = runLoop(loop, this, QStringLiteral("cor04"), false,
                                   testFails ? QStringLiteral("Fix and test (unfixable variant)")
                                             : QStringLiteral("Fix and run the authorized tests"));
        if (testFails) {
            QCOMPARE(state.phase, AgentLoopPhase::Failed);
            QCOMPARE(state.terminalReason, AgentTerminalReason::UnableToComplete);
            QVERIFY(state.finalAnswer.isEmpty());
            // The failing test ran exactly once: no retry-until-green.
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("run-command"), 0), 2);
        } else {
            QCOMPARE(state.phase, AgentLoopPhase::Completed);
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("run-command"), 0), 2);
        }
        WorkspaceVerifier::Expected expected;
        expected.changed = {QStringLiteral("src/calc.cpp")};
        QString actual;
        QVERIFY2(verifier.matches(expected, &actual), qPrintable(actual));
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        const auto beforeOracle = stateOf(root).files;
        const auto real = CompileOracle{}.verify(root, false,
                                                 testFails ? CompileOracle::Mode::TestFailure
                                                           : CompileOracle::Mode::Normal);
        qInfo().noquote() << "Independent oracle:" << real.summary();
        QCOMPARE(stateOf(root).files,
                 beforeOracle); // Compiler/test must not mutate sources or tests.
        QVERIFY2(verifier.matches(expected, &actual), qPrintable(actual));
        QCOMPARE(real.passed(), !testFails);
        QCOMPARE(real.steps.size(), 4);
        QCOMPARE(real.steps.last().process.exitCode, testFails ? 7 : 0);
        QCOMPARE(independentlyAccepted(state, verifier, expected, real), !testFails);
    }

    // Independent real-process negatives complement the original runtime/evidence variants.
    void cor02_independentCompileOracle_data() {
        QTest::addColumn<QString>("variant");
        QTest::newRow("wrong-operator") << QStringLiteral("wrong-operator");
        QTest::newRow("compile-failure") << QStringLiteral("compile-failure");
        QTest::newRow("fabricated-receipts") << QStringLiteral("fabricated-receipts");
    }
    void cor02_independentCompileOracle() {
        QFETCH(QString, variant);
        exerciseIndependentOracle(2, variant);
    }

    void cor03_independentCompileOracle_data() {
        QTest::addColumn<QString>("variant");
        QTest::newRow("broken-signature") << QStringLiteral("broken-signature");
        QTest::newRow("broken-header") << QStringLiteral("broken-header");
        QTest::newRow("unauthorized-change") << QStringLiteral("unauthorized-change");
    }
    void cor03_independentCompileOracle() {
        QFETCH(QString, variant);
        exerciseIndependentOracle(3, variant);
    }

    void cor04_independentCompileOracle_data() {
        QTest::addColumn<QString>("variant");
        QTest::newRow("success") << QStringLiteral("success");
        QTest::newRow("compile-failure") << QStringLiteral("compile-failure");
        QTest::newRow("test-failure") << QStringLiteral("test-failure");
        QTest::newRow("nonzero") << QStringLiteral("nonzero");
        QTest::newRow("timeout") << QStringLiteral("timeout");
        QTest::newRow("misleading-receipts") << QStringLiteral("misleading-receipts");
    }
    void cor04_independentCompileOracle() {
        QFETCH(QString, variant);
        exerciseIndependentOracle(4, variant);
    }

    void cor15_independentCompileOracle_data() {
        QTest::addColumn<QString>("variant");
        QTest::newRow("stale-source") << QStringLiteral("stale-source");
        QTest::newRow("success") << QStringLiteral("success");
        QTest::newRow("wrong-operator") << QStringLiteral("wrong-operator");
        QTest::newRow("test-failure") << QStringLiteral("test-failure");
        QTest::newRow("missing-test") << QStringLiteral("missing-test");
        QTest::newRow("unauthorized-change") << QStringLiteral("unauthorized-change");
        QTest::newRow("compile-failure") << QStringLiteral("compile-failure");
    }
    void cor15_independentCompileOracle() {
        QFETCH(QString, variant);
        exerciseIndependentOracle(15, variant);
    }

    // ------------------------------------------------------------------
    // COR-05: unknown tool / missing required argument / wrong JSON type /
    // extra property. The handler must never execute; workspace untouched.
    // ------------------------------------------------------------------
    void cor05_invalidToolInvocation_data() {
        QTest::addColumn<QString>("variant");
        QTest::newRow("unknown-tool") << QStringLiteral("unknown-tool");
        QTest::newRow("missing-required") << QStringLiteral("missing-required");
        QTest::newRow("wrong-type") << QStringLiteral("wrong-type");
        QTest::newRow("extra-property") << QStringLiteral("extra-property");
    }

    void cor05_invalidToolInvocation() {
        QFETCH(QString, variant);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("read-file"), QStringLiteral("write-file")});

        ScriptedPlanner planner;
        if (variant == QStringLiteral("unknown-tool")) {
            planner.decisions = {toolDecision(QStringLiteral("warp-drive")),
                                 toolDecision(QStringLiteral("warp-drive"))};
        } else if (variant == QStringLiteral("missing-required")) {
            planner.decisions = {toolDecision(QStringLiteral("read-file")),
                                 toolDecision(QStringLiteral("read-file"))};
        } else if (variant == QStringLiteral("wrong-type")) {
            auto invalid = toolDecision(QStringLiteral("read-file"));
            ToolInvocationArgument numberArg;
            numberArg.id = QStringLiteral("path");
            numberArg.value = QStringLiteral("42");
            numberArg.jsonValue = QJsonValue(42.0);
            invalid.arguments.append(numberArg);
            planner.decisions = {invalid, invalid};
        } else {
            auto invalid = toolDecision(QStringLiteral("read-file"));
            appendArg(invalid, QStringLiteral("path"), root + QStringLiteral("/src/calc.cpp"));
            appendArg(invalid, QStringLiteral("debug"), QStringLiteral("true"));
            planner.decisions = {invalid, invalid};
        }
        planner.repeatLast = false;

        AgentLoop::Config config;
        config.maxIterations = 4;
        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools, config);
        configureLoopFixture(loop, fixture, root);
        WorkspaceVerifier verifier(root);

        const auto state = runLoop(loop, this, QStringLiteral("cor05"), false,
                                   QStringLiteral("Attempt an invalid tool invocation"));
        QVERIFY2(state.phase == AgentLoopPhase::Failed || state.phase == AgentLoopPhase::Stuck,
                 qPrintable(loopPhaseName(state.phase)));
        QVERIFY(state.finalAnswer.isEmpty());
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 0);
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("write-file"), 0), 0);
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QVERIFY(verifier.untouched());
        if (variant == QStringLiteral("unknown-tool")) {
            QVERIFY(std::any_of(
                state.steps.cbegin(), state.steps.cend(), [](const AgentStepRecord& record) {
                    return record.observation.contains(QStringLiteral("Unknown tool"));
                }));
        } else {
            QVERIFY(std::any_of(
                state.steps.cbegin(), state.steps.cend(), [](const AgentStepRecord& record) {
                    return record.observation.contains(QStringLiteral("invalid arguments")) ||
                           record.observation.contains(QStringLiteral("rejected")) ||
                           record.observation.contains(QStringLiteral("Invalid"));
                }));
        }
    }

    // ------------------------------------------------------------------
    // COR-06: unsupported platform, unavailable capability runtime and a
    // disabled registry entry; none may fall back to another capability.
    // ------------------------------------------------------------------
    void cor06_capabilityUnavailable_data() {
        QTest::addColumn<QString>("variant");
        QTest::newRow("platform-unsupported") << QStringLiteral("platform-unsupported");
        QTest::newRow("handler-unavailable") << QStringLiteral("handler-unavailable");
        QTest::newRow("disabled-entry") << QStringLiteral("disabled");
    }

    void cor06_capabilityUnavailable() {
        QFETCH(QString, variant);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));

        LoopFixture fixture;
        QHash<QString, ToolDescriptor> overrides;
        const auto descriptors = BuiltInToolProvider::descriptors();
        const auto source =
            std::find_if(descriptors.cbegin(), descriptors.cend(), [](const ToolDescriptor& tool) {
                return tool.id == QLatin1String("stat-path");
            });
        QVERIFY(source != descriptors.cend());
        ToolDescriptor probe = *source;
        probe.id = QStringLiteral("nightvision-probe");
        probe.name = QStringLiteral("Night Vision Probe");
        // Explicit authorization contract for the renamed fixture descriptor.
        probe.requiredPermissionDomain = QStringLiteral("workspace-access");
        probe.authorizationRequirements = {{SecurityDomain::FileSystem,
                                            AccessMode::Read,
                                            AuthorizationResourceKind::FileSystemPath,
                                            QStringLiteral("path"),
                                            {}}};
        ToolDescriptor disabled = probe;
        disabled.enabled = false;

        QStringList requested{QStringLiteral("stat-path")};
        QString requestedId = QStringLiteral("nightvision-probe");
        QString unavailableReason;
        if (variant == QStringLiteral("platform-unsupported")) {
            probe.platforms.linux = ToolPlatformSupport::Unsupported;
            probe.platforms.macOS = ToolPlatformSupport::Unsupported;
            probe.platforms.windows = ToolPlatformSupport::Unsupported;
            overrides.insert(probe.id, probe);
            requested.prepend(probe.id);
        } else if (variant == QStringLiteral("handler-unavailable")) {
            unavailableReason = QStringLiteral("Fixture capability runtime is unavailable.");
            overrides.insert(probe.id, probe);
            requested.prepend(probe.id);
            requestedId = probe.id;
        } else {
            overrides.insert(disabled.id, disabled);
            requested.prepend(disabled.id);
            requestedId = disabled.id;
        }
        fixture.handler.unavailableReasonOverride = unavailableReason;
        fixture.registerTools(requested, overrides);

        ScriptedPlanner planner;
        planner.decisions = {toolDecision(requestedId), toolDecision(requestedId)};
        for (auto& decision : planner.decisions)
            appendArg(decision, QStringLiteral("path"), root + QStringLiteral("/src/calc.cpp"));
        planner.repeatLast = false;

        AgentLoop::Config config;
        config.maxIterations = 8;
        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools, config);
        configureLoopFixture(loop, fixture, root);
        WorkspaceVerifier verifier(root);

        const auto state = runLoop(loop, this, QStringLiteral("cor06"), false,
                                   QStringLiteral("Use the unavailable capability"));
        QVERIFY2(state.phase == AgentLoopPhase::Failed || state.phase == AgentLoopPhase::Stuck,
                 qPrintable(loopPhaseName(state.phase)));
        QVERIFY(state.finalAnswer.isEmpty());
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QVERIFY(verifier.untouched());
        // The capability receipt never executed (zero other receipts issued).
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("stat-path"), 0), 0);
        if (variant == QStringLiteral("handler-unavailable")) {
            QVERIFY(std::any_of(
                state.steps.cbegin(), state.steps.cend(), [](const AgentStepRecord& record) {
                    return record.observation.contains(QStringLiteral("runtime is unavailable"));
                }));
        } else {
            QVERIFY(std::any_of(
                state.steps.cbegin(), state.steps.cend(), [](const AgentStepRecord& record) {
                    return record.observation.contains(
                               QStringLiteral("unsupported on this platform")) ||
                           record.observation.contains(QStringLiteral("Tool unavailable")) ||
                           record.observation.contains(QStringLiteral("Unknown tool"));
                }));
        }
    }

    // ------------------------------------------------------------------
    // COR-06 (extended): every declared capability family fails closed when
    // the underlying capability is missing. The negative mechanisms are the
    // registry's own contracts: an unavailable handler (Git/browser/render/
    // process/network/speech), an absent handler, a disabled entry, and
    // platform-unsupported metadata. Each case must produce a typed
    // diagnostic, execute no receipt, fall back to no other tool and leave
    // the workspace unchanged. This exercises the injected capability
    // registry metadata, not live Git/Playwright/Chromium/network/Whisper
    // integrations, which remain unverified here.
    // ------------------------------------------------------------------
    void cor06_capabilityFamilyNegatives_data() {
        QTest::addColumn<QString>("variant");
        for (const auto& variant :
             {"git", "browser", "render-vision", "sandbox-process", "network", "speech",
              "missing-handler", "disabled-tool", "unsupported-platform"})
            QTest::newRow(variant) << QString::fromLatin1(variant);
    }

    void cor06_capabilityFamilyNegatives() {
        QFETCH(QString, variant);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        const auto calcPath = root + QStringLiteral("/src/calc.cpp");

        LoopFixture fixture;
        QHash<QString, ToolDescriptor> overrides;
        QString toolId;
        QString argumentId;
        QString argumentValue;
        QString unavailableReason;
        QString expectedObservation;
        bool expectUnsupportedCategory = false;
        bool handlerUnavailable = false;

        if (variant == QStringLiteral("git")) {
            toolId = QStringLiteral("git-status");
            argumentId = QStringLiteral("path");
            argumentValue = root;
            unavailableReason =
                QStringLiteral("Git executable and its process sandbox are unavailable.");
            expectedObservation = QStringLiteral("process sandbox are unavailable");
            handlerUnavailable = true;
        } else if (variant == QStringLiteral("browser")) {
            toolId = QStringLiteral("open-url");
            argumentId = QStringLiteral("url");
            argumentValue = QStringLiteral("https://example.invalid/");
            unavailableReason = QStringLiteral("Desktop browser capability is unavailable.");
            expectedObservation = QStringLiteral("browser capability is unavailable");
            handlerUnavailable = true;
        } else if (variant == QStringLiteral("render-vision")) {
            toolId = QStringLiteral("browser-screenshot");
            argumentId = QStringLiteral("url");
            argumentValue = QStringLiteral("https://example.invalid/");
            unavailableReason =
                QStringLiteral("Vision render backend (Playwright/Chromium) is unavailable.");
            expectedObservation = QStringLiteral("render backend");
            handlerUnavailable = true;
        } else if (variant == QStringLiteral("sandbox-process")) {
            toolId = QStringLiteral("run-command");
            argumentId = QStringLiteral("command");
            argumentValue = QStringLiteral("true");
            unavailableReason = QStringLiteral("Process sandbox backend is unavailable.");
            expectedObservation = QStringLiteral("sandbox backend is unavailable");
            handlerUnavailable = true;
        } else if (variant == QStringLiteral("network")) {
            toolId = QStringLiteral("web-fetch");
            argumentId = QStringLiteral("url");
            argumentValue = QStringLiteral("https://example.invalid/");
            unavailableReason = QStringLiteral("Network client capability is unavailable.");
            expectedObservation = QStringLiteral("Network client capability is unavailable");
            handlerUnavailable = true;
        } else if (variant == QStringLiteral("speech")) {
            toolId = QStringLiteral("voice-transcribe");
            argumentId = QStringLiteral("path");
            argumentValue = root + QStringLiteral("/audio.wav");
            unavailableReason = QStringLiteral("Speech backend (Whisper) is unavailable.");
            expectedObservation = QStringLiteral("Speech backend");
            handlerUnavailable = true;
        } else if (variant == QStringLiteral("missing-handler")) {
            toolId = QStringLiteral("stat-path");
            argumentId = QStringLiteral("path");
            argumentValue = calcPath;
            expectedObservation = QStringLiteral("Tool unavailable");
            expectUnsupportedCategory = true;
        } else if (variant == QStringLiteral("disabled-tool")) {
            toolId = QStringLiteral("read-file");
            argumentId = QStringLiteral("path");
            argumentValue = calcPath;
            ToolDescriptor disabled = builtInDescriptor(toolId);
            disabled.enabled = false;
            overrides.insert(toolId, disabled);
            expectedObservation = QStringLiteral("Tool unavailable");
            expectUnsupportedCategory = true;
        } else { // unsupported-platform
            toolId = QStringLiteral("stat-path");
            argumentId = QStringLiteral("path");
            argumentValue = calcPath;
            ToolDescriptor unsupported = builtInDescriptor(toolId);
            unsupported.platforms.linux = ToolPlatformSupport::Unsupported;
            unsupported.platforms.macOS = ToolPlatformSupport::Unsupported;
            unsupported.platforms.windows = ToolPlatformSupport::Unsupported;
            overrides.insert(toolId, unsupported);
            expectedObservation = QStringLiteral("unsupported on this platform");
        }

        if (variant == QStringLiteral("missing-handler")) {
            // Capability metadata exists but no execution target is wired. The
            // loop still lists the id as known, so the refusal must come from
            // the gateway rather than a silent unknown-tool bypass.
            QVERIFY(registerDescriptorWithoutHandler(fixture.registry, toolId));
            fixture.knownTools = {toolId};
        } else {
            if (handlerUnavailable)
                fixture.handler.unavailableReasonOverride = unavailableReason;
            fixture.registerTools({toolId}, overrides);
        }

        ScriptedPlanner planner;
        planner.decisions = {toolDecision(toolId), toolDecision(toolId)};
        for (auto& decision : planner.decisions)
            appendArg(decision, argumentId, argumentValue);
        planner.repeatLast = false;

        AgentLoop::Config config;
        config.maxIterations = 4;
        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools, config);
        configureLoopFixture(loop, fixture, root);
        WorkspaceVerifier verifier(root);

        const auto state = runLoop(loop, this, QStringLiteral("cor06x"), false,
                                   QStringLiteral("Use the unavailable capability"));

        QVERIFY2(state.phase == AgentLoopPhase::Failed || state.phase == AgentLoopPhase::Stuck,
                 qPrintable(loopPhaseName(state.phase)));
        QVERIFY(state.finalAnswer.isEmpty());
        // No receipt, no fallback tool and no fallback process may run.
        QCOMPARE(fixture.executor.calls, 0);
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QCOMPARE(fixture.handler.counts.value(toolId, 0), 0);
        QCOMPARE(fixture.handler.order.size(), 0);
        QVERIFY2(verifier.untouched(), qPrintable(verifier.delta().describe()));
        QVERIFY(!state.steps.isEmpty());
        QVERIFY2(state.steps.first().observation.contains(expectedObservation),
                 qPrintable(state.steps.first().observation));
        if (expectUnsupportedCategory)
            QCOMPARE(state.steps.first().failureCategory, ToolFailureCategory::Unsupported);
    }

    // ------------------------------------------------------------------
    // COR-06 (extended): model/provider availability is a capability too. An
    // unavailable active binding must fail the run via the provider contract
    // with no silent model substitution, no fallback tool and no fabricated
    // success. The provider fixture is injected; no real model is contacted.
    // ------------------------------------------------------------------
    void cor06_modelProviderUnavailable() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("read-file")});

        FaultProvider provider;
        ChatProviderReply unavailable;
        unavailable.success = false;
        unavailable.category = ChatProviderErrorCategory::ProviderUnavailable;
        unavailable.httpStatus = 503;
        unavailable.attempts = 3;
        unavailable.errorMessage =
            QStringLiteral("No local model is available for the active binding.");
        provider.plannerReplies = {unavailable, unavailable};
        provider.faultAfterQueue = unavailable;

        auto planner = makeLlmPlanner(provider, fixtureDescriptors(fixture.knownTools));
        AgentLoop loop(*planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools);
        configureLoopFixture(loop, fixture, root);
        WorkspaceVerifier verifier(root);

        const auto state = runLoop(loop, this, QStringLiteral("cor06-model"), false,
                                   QStringLiteral("Use the unavailable model capability"));
        QCOMPARE(state.phase, AgentLoopPhase::Failed);
        QCOMPARE(state.terminalReason, AgentTerminalReason::ProviderFailure);
        QVERIFY(state.finalAnswer.isEmpty());
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 0);
        QCOMPARE(fixture.executor.calls, 0);
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QVERIFY(verifier.untouched());
        // The active binding is unchanged: no silent model/provider fallback.
        QCOMPARE(planner->modelBinding().modelId, QStringLiteral("fixture-model"));
        QCOMPARE(planner->modelBinding().providerId, QStringLiteral("fixture-fault-provider"));
    }

    // ------------------------------------------------------------------
    // COR-07: denying the required filesystem/process action never executes
    // the handler and creates no persistent permission grant.
    // ------------------------------------------------------------------
    void cor07_deniedRequiredActionBlocked() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/victim.txt"),
                 QStringLiteral("unchanged baseline"));

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("run-command"), QStringLiteral("write-file")});
        // Nothing is added to allowedCommands / allowedWrites: the denial gate
        // must keep every effect off the workspace.
        fixture.approval.mode = FixtureApprovalPolicy::Mode::RequireApproval;

        ScriptedPlanner planner;
        auto risky = toolDecision(QStringLiteral("run-command"), ToolRiskLevel::High);
        appendArg(risky, QStringLiteral("command"), QStringLiteral("rm -rf victim"));
        planner.decisions = {risky};
        planner.repeatLast = false;

        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools);
        configureLoopFixture(loop, fixture, root);
        WorkspaceVerifier verifier(root);

        auto state = runLoop(loop, this, QStringLiteral("cor07"), false,
                             QStringLiteral("Delete fixture victim"));
        QCOMPARE(state.phase, AgentLoopPhase::AwaitingApproval);
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("run-command"), 0), 0);

        AgentLoop resumed(planner, fixture.executor, fixture.approval, fixture.sandbox,
                          fixture.knownTools);
        configureLoopFixture(resumed, fixture, root);
        state = resumed.resume(state, false);
        QCOMPARE(state.phase, AgentLoopPhase::Failed);
        QCOMPARE(state.terminalReason, AgentTerminalReason::ApprovalDenied);
        QVERIFY(state.finalAnswer.isEmpty());
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QVERIFY(verifier.untouched());
    }

    void cor07_persistentGrantRevocation_data() {
        QTest::addColumn<bool>("async");
        QTest::newRow("sync") << false;
        QTest::newRow("async") << true;
    }
    void cor07_persistentGrantRevocation() {
        QFETCH(bool, async);
        QTemporaryDir workspace, profileA, profileB;
        QVERIFY(workspace.isValid() && profileA.isValid() && profileB.isValid());
        const auto root = canonicalRoot(workspace);
        const auto path = root + "/allowed.txt";
        seedFile(QDir(root), "allowed.txt", "original\n");
        seedFile(QDir(root), "other.txt", "unchanged\n");
        WorkspaceVerifier verifier(root);
        auto storeA =
            std::make_shared<SQLitePermissionGrantStore>(profileA.filePath("grants.sqlite"));
        RuntimeFixture fixture;
        fixture.create({QStringLiteral("write-file")}, storeA);
        fixture.handler.root = root;
        fixture.approval.mode = FixtureApprovalPolicy::Mode::RequireApproval;
        fixture.handler.allowedWrites.insert("allowed.txt", "authorized\n");
        fixture.handler.allowedWrites.insert("other.txt", "outside grant\n");
        const auto prepare = [&](RuntimeFixture& f, const QString& target) {
            f.planner.resetPlanningIndex();
            auto write = toolDecision("write-file", ToolRiskLevel::High);
            appendArg(write, "path", root + '/' + target);
            appendArg(write, "content", f.handler.allowedWrites.value(target));
            auto final = finalDecision("Authorized fixture receipt recorded.");
            final.grounding = GroundingMode::Context;
            final.groundingDeclared = true;
            f.planner.decisions = {write, final};
            f.planner.repeatLast = false;
            const auto session = f.runtime->createSession();
            AgentSessionOptions options;
            options.autonomousMode = false;
            options.workspaceContext.rootPath = root;
            f.runtime->configureSession(session, options);
            return session;
        };
        const auto start = [&](RuntimeFixture& f, const QString& session) {
            if (async)
                return f.runtime->start(session, "Apply prescribed fixture write");
            return f.runtime->submit(session, "Apply prescribed fixture write").phase !=
                   AgentLoopPhase::Idle;
        };
        const auto resume = [&](RuntimeFixture& f, const QString& session, bool allowed) {
            if (async)
                return f.runtime->continueSession(session, allowed);
            return f.runtime->resume(session, allowed).phase != AgentLoopPhase::Idle;
        };
        QList<PersistentPermissionGrant> stored;
        const auto denied = prepare(fixture, "allowed.txt");
        QVERIFY(start(fixture, denied));
        QTRY_COMPARE(fixture.runtime->sessionState(denied).phase, AgentLoopPhase::AwaitingApproval);
        QVERIFY(resume(fixture, denied, false));
        QTRY_COMPARE(fixture.runtime->sessionState(denied).phase, AgentLoopPhase::Failed);
        QCOMPARE(fixture.handler.counts.value("write-file"), 0);
        QVERIFY(verifier.untouched());
        QVERIFY(storeA->load(stored));
        QCOMPARE(stored.size(), 0);

        const auto authorized = prepare(fixture, "allowed.txt");
        QVERIFY(start(fixture, authorized));
        QTRY_COMPARE(fixture.runtime->sessionState(authorized).phase,
                     AgentLoopPhase::AwaitingApproval);
        QVERIFY(
            fixture.runtime->approve(authorized, true)); // Actual persistent grant, not allow-once.
        QVERIFY(storeA->load(stored));
        QCOMPARE(stored.size(), 1);
        QCOMPARE(fixture.runtime->persistentPermissionGrants().size(), 1);
        const auto grant = stored.first();
        QVERIFY(!grant.id.isEmpty());
        QCOMPARE(grant.scope, path);
        QCOMPARE(grant.domain, SecurityDomain::FileSystem);
        QCOMPARE(grant.access, AccessMode::Write);
        QVERIFY(resume(fixture, authorized, true));
        QTRY_COMPARE(fixture.runtime->sessionState(authorized).phase, AgentLoopPhase::Completed);
        QCOMPARE(fixture.handler.counts.value("write-file"), 1);
        WorkspaceVerifier::Expected allowed;
        allowed.changed = {QStringLiteral("allowed.txt")};
        QVERIFY(verifier.matches(allowed));

        // Same profile + exact resource is intentionally persistent across sessions.
        const auto matchingSession = prepare(fixture, "allowed.txt");
        QVERIFY(start(fixture, matchingSession));
        QTRY_COMPARE(fixture.runtime->sessionState(matchingSession).phase,
                     AgentLoopPhase::Completed);
        QCOMPARE(fixture.handler.counts.value("write-file"), 2);
        QVERIFY(storeA->load(stored));
        QCOMPARE(stored.size(), 1);
        QCOMPARE(stored.first().id, grant.id);
        // Another resource in the same workspace must not inherit that file grant.
        const auto outside = prepare(fixture, "other.txt");
        QVERIFY(start(fixture, outside));
        QTRY_COMPARE(fixture.runtime->sessionState(outside).phase,
                     AgentLoopPhase::AwaitingApproval);
        QVERIFY(resume(fixture, outside, false));
        QTRY_COMPARE(fixture.runtime->sessionState(outside).phase, AgentLoopPhase::Failed);
        QCOMPARE(fixture.handler.counts.value("write-file"), 2);

        auto storeB =
            std::make_shared<SQLitePermissionGrantStore>(profileB.filePath("grants.sqlite"));
        RuntimeFixture isolated;
        isolated.create({QStringLiteral("write-file")}, storeB);
        isolated.handler.root = root;
        isolated.handler.allowedWrites = fixture.handler.allowedWrites;
        isolated.approval.mode = FixtureApprovalPolicy::Mode::RequireApproval;
        const auto otherProfile = prepare(isolated, "allowed.txt");
        QVERIFY(start(isolated, otherProfile));
        QTRY_COMPARE(isolated.runtime->sessionState(otherProfile).phase,
                     AgentLoopPhase::AwaitingApproval);
        QVERIFY(resume(isolated, otherProfile, false));
        QTRY_COMPARE(isolated.runtime->sessionState(otherProfile).phase, AgentLoopPhase::Failed);
        QCOMPARE(isolated.handler.counts.value("write-file"), 0);
        QVERIFY(isolated.runtime->persistentPermissionGrants().isEmpty());
        QVERIFY(storeB->load(stored));
        QCOMPARE(stored.size(), 0);
        QVERIFY(fixture.runtime->revokePersistentPermission(grant.id));
        QVERIFY(fixture.runtime->persistentPermissionGrants().isEmpty());
        SQLitePermissionGrantStore reloaded(profileA.filePath("grants.sqlite"));
        QVERIFY(reloaded.load(stored));
        QCOMPARE(stored.size(), 0);
        QVERIFY(!fixture.runtime->approve(authorized, true));
        QVERIFY(!fixture.runtime->continueSession(authorized, true));
        QVERIFY(fixture.runtime->persistentPermissionGrants().isEmpty());
        // Exact old authorization cannot survive in either previously used session cache.
        AuthorizationRequest request{SecurityDomain::FileSystem, AccessMode::Write, path};
        QCOMPARE(fixture.runtime->permissionService().evaluateAuthorization(request, authorized),
                 PermissionEffect::Ask);
        QCOMPARE(
            fixture.runtime->permissionService().evaluateAuthorization(request, matchingSession),
            PermissionEffect::Ask);
        const auto revoked = prepare(fixture, "allowed.txt");
        QVERIFY(start(fixture, revoked));
        QTRY_COMPARE(fixture.runtime->sessionState(revoked).phase,
                     AgentLoopPhase::AwaitingApproval);
        QVERIFY(resume(fixture, revoked, false));
        QTRY_COMPARE(fixture.runtime->sessionState(revoked).phase, AgentLoopPhase::Failed);
        QCOMPARE(fixture.handler.counts.value("write-file"), 2);
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QCOMPARE(isolated.handler.unauthorizedCalls, 0);
        QCOMPARE(fixture.executor.calls, 0);
        QVERIFY(verifier.matches(allowed));
        QCOMPARE(verifier.fileContent("allowed.txt"), QStringLiteral("authorized\n"));
        QCOMPARE(verifier.fileContent("other.txt"), QStringLiteral("unchanged\n"));
        QTRY_VERIFY([&] {
            TerminalAudit audit;
            audit.scan(isolated.runtime->eventHistory(otherProfile));
            return audit.singleTerminal() && audit.failed == 1;
        }());
        for (const auto& session : {denied, authorized, matchingSession, outside, revoked}) {
            QTRY_VERIFY([&] {
                TerminalAudit audit;
                audit.scan(fixture.runtime->eventHistory(session));
                return audit.singleTerminal();
            }());
        }
    }

    // ------------------------------------------------------------------
    // COR-08: explicit allow-once approval; exactly one invocation; replay
    // and foreign approvals rejected afterwards; no permanent grant stored.
    // ------------------------------------------------------------------
    void cor08_allowOnceApproval_data() {
        QTest::addColumn<bool>("runtime");
        QTest::newRow("loop-resume") << false;
        QTest::newRow("runtime-approve") << true;
    }

    void cor08_allowOnceApproval() {
        QFETCH(bool, runtime);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        WorkspaceVerifier verifier(root);

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("run-command")});
        fixture.handler.allowedCommands = {QStringLiteral("echo approved-once")};
        fixture.approval.mode = FixtureApprovalPolicy::Mode::RequireApproval;
        fixture.planner.decisions = {
            toolDecision(QStringLiteral("run-command"), ToolRiskLevel::High)};
        appendArg(fixture.planner.decisions.first(), QStringLiteral("command"),
                  QStringLiteral("echo approved-once"));
        auto finish = finalDecision(QStringLiteral("approved-once receipt completed."));
        finish.grounding = GroundingMode::Context;
        finish.groundingDeclared = true;
        fixture.planner.decisions.append(finish);
        fixture.planner.repeatLast = false;

        QString session;
        if (!runtime) {
            AgentLoop loop(fixture.planner, fixture.executor, fixture.approval, fixture.sandbox,
                           fixture.knownTools);
            configureLoopFixture(loop, fixture, root);
            auto state = runLoop(loop, this, QStringLiteral("cor08"), false,
                                 QStringLiteral("approved once"));
            QCOMPARE(state.phase, AgentLoopPhase::AwaitingApproval);
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("run-command"), 0), 0);
            AgentLoop resumed(fixture.planner, fixture.executor, fixture.approval, fixture.sandbox,
                              fixture.knownTools);
            configureLoopFixture(resumed, fixture, root);
            state = resumed.resume(state, true);
            QCOMPARE(state.phase, AgentLoopPhase::Completed);
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("run-command"), 0), 1);
        } else {
            RuntimeFixture fixtureRuntime;
            fixtureRuntime.create({QStringLiteral("run-command")});
            fixtureRuntime.configure(root, /*autonomous=*/false);
            fixtureRuntime.handler.allowedCommands = {QStringLiteral("echo approved-once")};
            fixtureRuntime.approval.mode = FixtureApprovalPolicy::Mode::RequireApproval;
            fixtureRuntime.planner.decisions = {
                toolDecision(QStringLiteral("run-command"), ToolRiskLevel::High)};
            appendArg(fixtureRuntime.planner.decisions.first(), QStringLiteral("command"),
                      QStringLiteral("echo approved-once"));
            auto runtimeFinish = finalDecision(QStringLiteral("approved-once receipt completed."));
            runtimeFinish.grounding = GroundingMode::Context;
            runtimeFinish.groundingDeclared = true;
            fixtureRuntime.planner.decisions.append(runtimeFinish);
            fixtureRuntime.planner.repeatLast = false;
            session = fixtureRuntime.session;
            QVERIFY(fixtureRuntime.runtime->start(session, QStringLiteral("approved once")));
            QTRY_COMPARE_WITH_TIMEOUT(fixtureRuntime.runtime->sessionState(session).phase,
                                      AgentLoopPhase::AwaitingApproval, kBarrierTimeoutMs);
            // Allow-once: never a permanent grant for the.FileSystem action.
            QVERIFY(fixtureRuntime.runtime->approve(session, false));
            QVERIFY(fixtureRuntime.runtime->continueSession(session, true));
            QTRY_COMPARE_WITH_TIMEOUT(fixtureRuntime.runtime->sessionState(session).phase,
                                      AgentLoopPhase::Completed, kBarrierTimeoutMs);
            QCOMPARE(fixtureRuntime.handler.counts.value(QStringLiteral("run-command"), 0), 1);
            // No persistent grant may exist after allow-once.
            QVERIFY(fixtureRuntime.runtime->persistentPermissionGrants().isEmpty());
            TerminalAudit audit;
            audit.scan(fixtureRuntime.runtime->eventHistory(session));
            QVERIFY2(audit.singleTerminal(), qPrintable(audit.describe()));
            // Replays of the consumed approval and foreign-session approvals
            // are rejected; no second run is created.
            QVERIFY(!fixtureRuntime.runtime->approve(session, false));
            QVERIFY(!fixtureRuntime.runtime->approve(session, true));
            QVERIFY(!fixtureRuntime.runtime->approve(QStringLiteral("foreign-session"), false));
        }
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QVERIFY(verifier.untouched());
    }

    // ------------------------------------------------------------------
    // COR-09: provider disconnect during required work. Transient recovery
    // exhausts its budget; a fatal failure terminates with ProviderFailure;
    // in both variants no stale completion may pass the terminal fence.
    // ------------------------------------------------------------------
    void cor09_providerDisconnect_data() {
        QTest::addColumn<QString>("variant");
        QTest::newRow("transient-recovery-exhausted") << QStringLiteral("transient");
        QTest::newRow("immediate-fatal") << QStringLiteral("fatal");
    }

    void cor09_providerDisconnect() {
        QFETCH(QString, variant);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("read-file")});

        FaultProvider provider;
        ChatProviderReply fatal;
        fatal.success = false;
        fatal.category = ChatProviderErrorCategory::ConnectionFailed;
        fatal.httpStatus = 500; // deliberately non-transient classification
        fatal.attempts = 3;
        fatal.errorMessage = QStringLiteral("fixture connection lost");
        if (variant == QStringLiteral("transient")) {
            ChatProviderReply transientReply;
            transientReply.success = false;
            transientReply.category = ChatProviderErrorCategory::ProviderUnavailable;
            transientReply.httpStatus = 503; // transient; production recovery path
            transientReply.attempts = 1;
            transientReply.errorMessage = QStringLiteral("fixture overload");
            provider.plannerReplies = {transientReply, transientReply};
            provider.faultAfterQueue = fatal;
        } else {
            fatal.attempts = 1;
            provider.faultAfterQueue = fatal;
        }

        auto planner = makeLlmPlanner(provider, fixtureDescriptors(fixture.knownTools));
        AgentLoop loop(*planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools);
        configureLoopFixture(loop, fixture, root);
        WorkspaceVerifier verifier(root);

        const auto state = runLoop(loop, this, QStringLiteral("cor09"), false,
                                   QStringLiteral("Improve the fixture using the provider"));
        QCOMPARE(state.phase, AgentLoopPhase::Failed);
        if (state.terminalReason != AgentTerminalReason::ProviderFailure)
            QFAIL(
                qPrintable(QStringLiteral("expected ProviderFailure, got %1 (%2)")
                               .arg(terminalReasonName(state.terminalReason), state.abortReason)));
        QVERIFY(state.finalAnswer.isEmpty());
        // No stale completed event may exist; the production policy retries a
        // transient 503-class planner failure exactly once per planner call.
        QCOMPARE(provider.plannerRequests, variant == QStringLiteral("transient") ? 2 : 1);
        QCOMPARE(state.providerRecoveryAttempts, variant == QStringLiteral("transient") ? 1 : 0);
        // No tool side effect may be dispatched from a failed planning stage.
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QVERIFY(verifier.untouched());
    }

    // ------------------------------------------------------------------
    // COR-10: cancel at the classification/planning barrier; no tool or
    // model dispatch afterwards; a fresh task still succeeds.
    // ------------------------------------------------------------------
    void cor10_cancelAtClassificationBarrier() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("read-file"), QStringLiteral("write-file")});

        ScriptedPlanner planner;
        planner.decisions = {toolDecision(QStringLiteral("read-file"))};
        planner.repeatLast = false;

        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools);
        configureLoopFixture(loop, fixture, root);
        auto policy = std::make_shared<CallbackIntentPolicy>();
        policy->onClassify = [&] { loop.cancelAsync(); };
        loop.setObservationIntentPolicy(policy);
        WorkspaceVerifier verifier(root);

        int terminals = 0;
        loop.runAsync(QStringLiteral("Cancelled at classification"), QStringLiteral("cor10"), this,
                      [&terminals](const AgentLoopState& state) {
                          QCOMPARE(state.phase, AgentLoopPhase::Cancelled);
                          ++terminals;
                      });
        QTRY_COMPARE_WITH_TIMEOUT(terminals, 1, kBarrierTimeoutMs);
        QCOMPARE(planner.calls, 0);
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 0);
        QCOMPARE(fixture.executor.calls, 0);

        // Next fresh task succeeds; no residual queued tool effect.
        LoopFixture fresh;
        fresh.registerTools({QStringLiteral("read-file"), QStringLiteral("write-file")});
        fresh.planner.decisions = {toolDecision(QStringLiteral("read-file")),
                                   finalDecision(QStringLiteral("observed receipt"))};
        appendArg(fresh.planner.decisions.first(), QStringLiteral("path"),
                  QDir(root).filePath(QStringLiteral("src/calc.cpp")));
        fresh.planner.repeatLast = false;
        AgentLoop loop2(fresh.planner, fresh.executor, fresh.approval, fresh.sandbox,
                        fresh.knownTools);
        configureLoopFixture(loop2, fresh, root);
        auto emptyIntent = std::make_shared<FixedIntentPolicy>();
        loop2.setObservationIntentPolicy(emptyIntent);
        // The loop recomputes the canonical scoped representation from the
        // (empty) classified intent; mirror that computation in the planner.
        fresh.planner.canonicalFinal = true;
        fresh.planner.canonicalIntent = emptyIntent->intent;
        const auto state2 = runLoop(loop2, this, QStringLiteral("cor10-next"), false,
                                    QStringLiteral("Read the fixture file"));
        QCOMPARE(state2.phase, AgentLoopPhase::Completed);
        QCOMPARE(fresh.handler.counts.value(QStringLiteral("read-file"), 0), 1);
        QVERIFY(verifier.untouched());
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QCOMPARE(fresh.handler.unauthorizedCalls, 0);
    }

    // ------------------------------------------------------------------
    // COR-10 (extended): cancellation is honored at each planning stage
    // boundary. Cancellation requested before classification, during planning
    // (before the first dispatch) and repeatedly must still deliver exactly
    // one authoritative terminal and no queued tool effect.
    // ------------------------------------------------------------------
    void cor10_planningStageCancellation_data() {
        QTest::addColumn<QString>("variant");
        QTest::newRow("cancel-before-classification") << QStringLiteral("pre-classification");
        QTest::newRow("cancel-before-first-dispatch") << QStringLiteral("before-dispatch");
        QTest::newRow("repeated-cancel") << QStringLiteral("repeated");
    }

    void cor10_planningStageCancellation() {
        QFETCH(QString, variant);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        const auto calcPath = root + QStringLiteral("/src/calc.cpp");

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("read-file"), QStringLiteral("write-file")});

        ScriptedPlanner planner;
        auto read = toolDecision(QStringLiteral("read-file"));
        appendArg(read, QStringLiteral("path"), calcPath);
        planner.decisions = {read};
        planner.repeatLast = false;

        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools);
        configureLoopFixture(loop, fixture, root);
        if (variant == QStringLiteral("pre-classification")) {
            // Cancellation is already stamped before classification runs; the
            // run must never dispatch planning or a tool.
            loop.setCancelQuery([] { return true; });
        } else if (variant == QStringLiteral("before-dispatch")) {
            planner.onCall = [&loop](int index) {
                if (index == 0)
                    loop.cancelAsync();
            };
        } else { // repeated
            planner.onCall = [&loop](int index) {
                if (index == 0) {
                    loop.cancelAsync();
                    loop.cancelAsync();
                }
            };
        }

        int terminals = 0;
        AgentLoopState terminal;
        loop.runAsync(QStringLiteral("Cancelled during planning"), QStringLiteral("cor10-stage"),
                      this, [&](const AgentLoopState& state) {
                          terminal = state;
                          ++terminals;
                      });
        QTRY_COMPARE_WITH_TIMEOUT(terminals, 1, kBarrierTimeoutMs);
        QCOMPARE(terminal.phase, AgentLoopPhase::Cancelled);
        QCOMPARE(fixture.executor.calls, 0);
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QVERIFY(WorkspaceVerifier(root).untouched());
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 0);
        if (variant == QStringLiteral("pre-classification"))
            QCOMPARE(planner.calls, 0);
        else
            QCOMPARE(planner.calls, 1);
    }

    // ------------------------------------------------------------------
    // COR-10 (extended): cancellation during genuine planning on the runtime
    // worker, a late planning result ignored after the cancel stamp, exactly
    // one authoritative terminal, and a fresh task on the same runtime with
    // no inherited cancellation state.
    // ------------------------------------------------------------------
    void cor10_runtimePlanningBarrierAndFreshTask() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        const auto calcPath = root + QStringLiteral("/src/calc.cpp");

        RuntimeFixture fixture;
        fixture.create({QStringLiteral("read-file"), QStringLiteral("write-file")});
        fixture.configure(root);

        auto read = toolDecision(QStringLiteral("read-file"));
        appendArg(read, QStringLiteral("path"), calcPath);
        fixture.planner.decisions = {read};
        fixture.planner.repeatLast = false;
        fixture.planner.barrierPlanning = true;

        const auto auditOf = [&](const QString& id) {
            TerminalAudit audit;
            audit.scan(fixture.runtime->eventHistory(id));
            return audit;
        };

        QVERIFY(fixture.runtime->start(fixture.session, QStringLiteral("read the fixture once")));
        // Planning is genuinely on the worker stack; cancel while it blocks.
        QTRY_VERIFY_WITH_TIMEOUT(fixture.planner.planningEntered.available() > 0,
                                 kBarrierTimeoutMs);
        QVERIFY(fixture.planner.planningEntered.tryAcquire());
        QVERIFY(fixture.runtime->cancel(fixture.session));
        QCOMPARE(fixture.runtime->sessionState(fixture.session).phase, AgentLoopPhase::Cancelling);
        // Release the late planning result after the cancel stamp: it must be
        // ignored rather than dispatched.
        fixture.planner.planningRelease.release();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.runtime->sessionState(fixture.session).phase,
                                  AgentLoopPhase::Cancelled, kBarrierTimeoutMs);
        QVERIFY(!fixture.planner.planningBarrierTimedOut.load());
        QVERIFY(fixture.runtime->sessionState(fixture.session).finalAnswer.isEmpty());
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 0);
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QCOMPARE(fixture.executor.calls, 0);
        {
            const auto audit = auditOf(fixture.session);
            QVERIFY2(audit.singleTerminal() && audit.cancelled == 1, qPrintable(audit.describe()));
        }
        // A repeated cancel on a terminal session is rejected and cannot reopen
        // or duplicate the terminal.
        QVERIFY(!fixture.runtime->cancel(fixture.session));
        {
            const auto audit = auditOf(fixture.session);
            QVERIFY2(audit.singleTerminal() && audit.cancelled == 1, qPrintable(audit.describe()));
        }

        // A fresh task on the same runtime inherits no cancellation state.
        const auto next = fixture.runtime->createSession();
        QVERIFY(!next.isEmpty());
        AgentSessionOptions options;
        options.autonomousMode = true;
        options.workspaceContext.rootPath = root;
        fixture.runtime->configureSession(next, options);
        fixture.planner.barrierPlanning = false;
        fixture.planner.resetPlanningIndex();
        auto nextRead = toolDecision(QStringLiteral("read-file"));
        appendArg(nextRead, QStringLiteral("path"), calcPath);
        auto nextFinal = finalDecision(QStringLiteral("Fresh fixture read recorded."));
        nextFinal.grounding = GroundingMode::Verified;
        nextFinal.groundingDeclared = true;
        fixture.planner.decisions = {nextRead, nextFinal};
        fixture.planner.repeatLast = true;
        // The verified filesystem final must use the canonical representation.
        fixture.planner.canonicalFinal = true;
        QVERIFY(fixture.runtime->start(next, QStringLiteral("read the fixture again")));
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.runtime->sessionState(next).phase == AgentLoopPhase::Completed ||
                fixture.runtime->sessionState(next).phase == AgentLoopPhase::Failed ||
                fixture.runtime->sessionState(next).phase == AgentLoopPhase::Stuck ||
                fixture.runtime->sessionState(next).phase == AgentLoopPhase::Cancelled,
            kBarrierTimeoutMs);
        {
            const auto fresh = fixture.runtime->sessionState(next);
            QVERIFY2(fresh.phase == AgentLoopPhase::Completed,
                     qPrintable(QStringLiteral("phase=%1 reason=%2 abort=%3")
                                    .arg(loopPhaseName(fresh.phase))
                                    .arg(terminalReasonName(fresh.terminalReason))
                                    .arg(fresh.abortReason)));
        }
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 1);
        {
            const auto audit = auditOf(next);
            QVERIFY2(audit.singleTerminal() && audit.completed == 1, qPrintable(audit.describe()));
        }
        QVERIFY(WorkspaceVerifier(root).untouched());
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
    }

    // ------------------------------------------------------------------
    // COR-11: cancellation during generation; late provider output arrives
    // after cancellation and must be ignored without completing anything.
    // ------------------------------------------------------------------
    void cor11_cancelDuringGeneration() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));

        RuntimeFixture fixture;
        fixture.create({QStringLiteral("write-file"), QStringLiteral("read-file")});

        // Injected provider blocks generation on a deterministic barrier and
        // holds a late final reply that must never be consumed.
        auto provider = std::make_unique<FaultProvider>();
        provider->barrierAllRequests = true;
        ChatProviderReply lateFinal;
        lateFinal.success = true;
        lateFinal.message =
            QStringLiteral("{\"action\":\"final\",\"grounding\":\"verified\",\"answer\":\"late\"}");
        provider->plannerReplies = {lateFinal};
        auto llmPlanner = makeLlmPlanner(
            *provider,
            fixtureDescriptors({QStringLiteral("write-file"), QStringLiteral("read-file")}));

        auto runtime = std::make_unique<AgentRuntime>(
            std::make_unique<FixtureMetadataRuntime>(
                fixtureDescriptors({QStringLiteral("write-file"), QStringLiteral("read-file")})),
            llmPlanner.get(), fixture.executor, fixture.approval, fixture.sandbox);
        for (const auto& descriptor :
             fixtureDescriptors({QStringLiteral("write-file"), QStringLiteral("read-file")})) {
            QVERIFY(runtime->toolRegistry().unregisterTool(descriptor.id));
            QVERIFY(runtime->toolRegistry().registerTool(IToolRegistry::Registration{
                descriptor,
                std::shared_ptr<IToolHandler>(&fixture.handler, [](IToolHandler*) {})}));
        }
        const QString session = runtime->createSession();
        QVERIFY(!session.isEmpty());
        AgentSessionOptions options;
        options.autonomousMode = true;
        options.workspaceContext.rootPath = root;
        runtime->configureSession(session, std::move(options));

        auto releaseProvider = qScopeGuard([&] { provider->release.release(); });
        QVERIFY(runtime->start(session, QStringLiteral("generate a grounded answer")));
        // Barrier: the planner request entered the provider (generation in
        // flight). Request the cancellation, then release the barrier.
        QVERIFY2(provider->arrived.tryAcquire(1, kBarrierTimeoutMs),
                 "provider generation never started");
        QVERIFY(runtime->cancel(session));
        provider->release.release(); // "late response" arrives after cancel

        QTRY_COMPARE_WITH_TIMEOUT(runtime->sessionState(session).phase, AgentLoopPhase::Cancelled,
                                  kBarrierTimeoutMs);
        // commitResult() publishes the terminal event immediately after committing the
        // phase, so polling only the phase can observe a window with no terminal yet.
        // Poll the authoritative event instead of trusting the phase read order.
        const auto terminalEventCount = [&] {
            int events = 0;
            for (const auto& event : runtime->eventHistory(session))
                if (event.type == AgentEventType::AgentCancelled ||
                    event.type == AgentEventType::AgentCompleted ||
                    event.type == AgentEventType::AgentFailed)
                    ++events;
            return events;
        };
        QTRY_COMPARE_WITH_TIMEOUT(terminalEventCount(), 1, kBarrierTimeoutMs);
        QCOMPARE(provider->cancelledReports, 1);
        QCOMPARE(provider->plannerRequests, 1);
        TerminalAudit audit;
        audit.scan(runtime->eventHistory(session));
        QVERIFY2(audit.singleTerminal() && audit.cancelled == 1, qPrintable(audit.describe()));
        const auto frozenEvents = runtime->eventHistory(session);
        // Terminal fencing: late approvals and duplicate cancels are rejected;
        // the frozen event history is unchanged.
        QVERIFY(!runtime->approve(session, false));
        QVERIFY(!runtime->approve(session, true));
        QCOMPARE(runtime->cancel(session), false);
        QCOMPARE(runtime->eventHistory(session).size(), frozenEvents.size());
        // Nothing may have been executed or written by the cancelled run.
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        WorkspaceVerifier verifier(root);
        QVERIFY(verifier.untouched());
    }

    void cor11_cancelledRequestBindingAndNextTask_data() {
        QTest::addColumn<bool>("native");
        QTest::addColumn<bool>("cooperative");
        QTest::newRow("cooperative-json-request") << false << true;
        QTest::newRow("late-json-final") << false << false;
        QTest::newRow("late-native-tool-call") << true << false;
    }
    void cor11_cancelledRequestBindingAndNextTask() {
        QFETCH(bool, native);
        QFETCH(bool, cooperative);
        exerciseGenerationCancellation(native, cooperative, false);
    }
    void cor11_cancelAfterFirstStreamedDelta_data() {
        QTest::addColumn<bool>("native");
        QTest::newRow("json-stream") << false;
        QTest::newRow("native-tool-assembly") << true;
    }
    void cor11_cancelAfterFirstStreamedDelta() {
        QFETCH(bool, native);
        exerciseGenerationCancellation(native, false, true);
    }

    // Component evidence only: the legacy provider streaming method cannot carry
    // planner ChatRequestOptions. Never present this as end-to-end COR11 routing.
    void cor11_boundedTransportAndPlannerPrivacy_data() {
        QTest::addColumn<QString>("variant");
        for (const auto& name : {"complete", "cancel-after-first", "deadline-after-first",
                                 "malformed-ndjson", "missing-terminal", "incomplete-planner-json"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void cor11_boundedTransportAndPlannerPrivacy() {
        QFETCH(QString, variant);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(root), "artifact.txt", "original\n");
        WorkspaceVerifier verifier(root);
        const QString marker = QStringLiteral("PRIVATE_PLANNER_STREAM_SENTINEL");
        const QByteArray first = "{\"private_protocol\":\"" + marker.toUtf8() + "\",";
        const QByteArray tail =
            "\"action\":\"final\",\"grounding\":\"context\",\"answer\":\"safe fixture answer\"}";
        const auto line = [](const QByteArray& text, bool done) {
            return QJsonDocument(QJsonObject{{"response", QString::fromUtf8(text)}, {"done", done}})
                       .toJson(QJsonDocument::Compact) +
                   '\n';
        };
        const auto chunk = [](const QByteArray& bytes) {
            return QByteArray::number(bytes.size(), 16) + "\r\n" + bytes + "\r\n";
        };
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QPointer<QTcpSocket> connection;
        int requests = 0, disconnects = 0;
        QByteArray requestBody;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* socket = server.nextPendingConnection();
            connection = socket;
            connect(socket, &QTcpSocket::disconnected, &server, [&] { ++disconnects; });
            connect(socket, &QTcpSocket::readyRead, &server, [&, socket] {
                if (socket->property("responded").toBool())
                    return;
                auto data = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", data);
                const int headers = data.indexOf("\r\n\r\n");
                if (headers < 0)
                    return;
                const auto header = data.left(headers);
                const auto match = QRegularExpression("content-length:\\s*(\\d+)",
                                                      QRegularExpression::CaseInsensitiveOption)
                                       .match(QString::fromLatin1(header));
                if (!match.hasMatch() || data.size() - headers - 4 < match.captured(1).toInt())
                    return;
                socket->setProperty("responded", true);
                ++requests;
                requestBody = data.mid(headers + 4, match.captured(1).toInt());
                socket->write(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\nTransfer-Encoding: "
                    "chunked\r\nConnection: close\r\n\r\n");
                socket->write(chunk(line(first, false)));
                if (variant == "cancel-after-first" || variant == "deadline-after-first")
                    return;
                if (variant == "malformed-ndjson")
                    socket->write(chunk("invalid " + marker.toUtf8() + '\n'));
                else if (variant == "incomplete-planner-json")
                    socket->write(chunk(line({}, true)));
                else
                    socket->write(chunk(line(tail, variant != "missing-terminal")));
                socket->write("0\r\n\r\n");
                socket->disconnectFromHost();
            });
        });
        {
            std::lock_guard lock(plannerPrivacyLogMutex);
            plannerPrivacyLogs.clear();
        }
        const auto oldHandler = qInstallMessageHandler(capturePlannerPrivacyLog);
        const auto oldFilter = QLoggingCategory::installFilter([](QLoggingCategory* category) {
            if (QByteArray(category->categoryName()) == "sentinel.agent.diagnostics")
                category->setEnabled(QtInfoMsg, true);
        });
        auto restoreLogs = qScopeGuard([&] {
            QLoggingCategory::installFilter(oldFilter);
            qInstallMessageHandler(oldHandler);
        });
        OllamaChatProvider transport(
            OllamaConfig::fromEndpoint(
                QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())),
            1000);
        transport.setSelectedModel("synthetic-planner");
        auto token = std::make_shared<std::atomic_bool>(false);
        QStringList internalDeltas;
        bool transportReturned = false, firstWhileActive = false, lateChunkSent = false;
        const QByteArray lateTail =
            QJsonDocument(QJsonObject{{"action", "tool"},
                                      {"tool", "write-file"},
                                      {"args", QJsonObject{{"path", root + "/artifact.txt"},
                                                           {"content", "late mutation"}}}})
                .toJson(QJsonDocument::Compact)
                .mid(1);
        QVERIFY(QJsonDocument::fromJson(first + lateTail).isObject());

        const auto actual = transport.sendMessageStreaming(
            "fixed synthetic planner request",
            [&](const QString& delta) {
                // Actual production provider callback, fed only by HTTP bytes the
                // synthetic server wrote. The first callback must happen while the
                // transport is still active (before sendMessageStreaming returns).
                if (internalDeltas.isEmpty())
                    firstWhileActive = !transportReturned;
                internalDeltas.append(delta);
                if (variant == "cancel-after-first" && internalDeltas.size() == 1) {
                    token->store(true); // Cancellation is strictly after the first provider delta.
                    if (connection)
                        lateChunkSent = connection->write(chunk(line(lateTail, true))) > 0;
                }
            },
            token);
        transportReturned = true;
        QVERIFY2(firstWhileActive,
                 "the real provider delta callback never fired before the transport returned");
        QCOMPARE(requests, 1);
        QVERIFY(!actual.requestId.isEmpty());
        QVERIFY(!internalDeltas.isEmpty());
        QCOMPARE(internalDeltas.first(), QString::fromUtf8(first));
        const auto sent = QJsonDocument::fromJson(requestBody).object();
        QCOMPARE(sent.value("model").toString(), QStringLiteral("synthetic-planner"));
        QCOMPARE(sent.value("stream").toBool(), true);
        QCOMPARE(sent.value("options").toObject().value("num_predict").toInt(), 2048);
        QCOMPARE(actual.attempts, 1);
        if (variant == "cancel-after-first") {
            QVERIFY(token->load());
            QVERIFY(lateChunkSent);
            QCOMPARE(internalDeltas.size(), 1);
            QCOMPARE(actual.lifecycle, ChatRequestLifecycle::Cancelled);
            QCOMPARE(actual.category, ChatProviderErrorCategory::Cancelled);
            QVERIFY(!actual.success);
        } else if (variant == "deadline-after-first") {
            QVERIFY(!token->load());
            QCOMPARE(internalDeltas.size(), 1);
            QCOMPARE(actual.lifecycle, ChatRequestLifecycle::TimedOut);
            QCOMPARE(actual.category, ChatProviderErrorCategory::Timeout);
            QVERIFY(!actual.success);
        } else if (variant == "malformed-ndjson" || variant == "missing-terminal") {
            QVERIFY(!actual.success);
            QCOMPARE(actual.category, ChatProviderErrorCategory::MalformedResponse);
        } else {
            QVERIFY(actual.success);
            QCOMPARE(actual.lifecycle, ChatRequestLifecycle::Completed);
        }
        QTRY_COMPARE(disconnects, 1); // Local socket release, not upstream compute release.
        QVERIFY(!actual.errorMessage.contains(marker));
        QVERIFY(!QJsonDocument(actual.diagnostics).toJson().contains(marker.toUtf8()));

        // Replay the actual transport result through the existing parser/final gates.
        // This intentionally does not pretend the planner chose bounded streaming.
        FaultProvider parserInput;
        parserInput.faultAfterQueue = actual;
        FixtureHandler handler;
        handler.root = root;
        handler.allowedWrites.insert("artifact.txt", "late mutation");
        NoOpExecutor executor;
        FixtureApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        const auto descriptors = fixtureDescriptors({"read-file", "write-file"});
        auto planner = makeLlmPlanner(parserInput, descriptors);
        AgentRuntime runtime(std::make_unique<FixtureMetadataRuntime>(descriptors), planner.get(),
                             executor, approval, sandbox);
        for (const auto& descriptor : descriptors) {
            QVERIFY(runtime.toolRegistry().unregisterTool(descriptor.id));
            QVERIFY(runtime.toolRegistry().registerTool(
                {descriptor, std::shared_ptr<IToolHandler>(&handler, [](IToolHandler*) {})}));
        }
        const auto session = runtime.createSession();
        AgentSessionOptions options;
        options.autonomousMode = true;
        options.workspaceContext.rootPath = root;
        runtime.configureSession(session, options);
        const auto state = runtime.submit(session, "Validate fixed synthetic planner result");
        QCOMPARE(state.phase,
                 variant == "complete" ? AgentLoopPhase::Completed : AgentLoopPhase::Failed);
        QCOMPARE(parserInput.plannerRequests, variant == "incomplete-planner-json" ? 2 : 1);
        if (variant == "complete")
            QCOMPARE(state.finalAnswer, QStringLiteral("safe fixture answer"));
        else
            QVERIFY(state.finalAnswer.isEmpty());
        QCOMPARE(handler.counts.value("write-file"), 0);
        QCOMPARE(handler.counts.value("read-file"), 0);
        QCOMPARE(handler.unauthorizedCalls, 0);
        QCOMPARE(executor.calls, 0);
        QVERIFY(verifier.untouched());
        TerminalAudit terminal;
        terminal.scan(runtime.eventHistory(session));
        QVERIFY(terminal.singleTerminal());
        QCOMPARE(terminal.completed, variant == "complete" ? 1 : 0);
        int publicDeltas = 0;
        QString publicText;
        for (const auto& event : runtime.eventHistory(session)) {
            if (event.type == AgentEventType::ModelOutputDelta)
                ++publicDeltas;
            if (const auto* text = std::get_if<AgentTextEvent>(&event.payload))
                publicText += text->text;
            if (const auto* run = std::get_if<AgentRunEvent>(&event.payload))
                publicText += run->finalAnswer + run->abortReason + run->pendingThought;
            if (const auto* step = std::get_if<AgentStepEvent>(&event.payload))
                publicText += step->step.observation + step->step.thought + step->step.statusText;
        }
        QCOMPARE(publicDeltas, 0);
        QVERIFY(!publicText.contains(marker));
        QString logs;
        {
            std::lock_guard lock(plannerPrivacyLogMutex);
            logs = plannerPrivacyLogs.join('\n');
        }
        QVERIFY(logs.contains("provider_id")); // Diagnostics were enabled and actually emitted.
        QVERIFY(!logs.contains(marker));
    }

    // ------------------------------------------------------------------
    // COR-12: cancellation during cooperative tool execution; the in-flight
    // receipt may commit its declared partial artifact; no further effect
    // runs after the seal and the next task succeeds.
    // ------------------------------------------------------------------
    void cor12_cancelDuringCooperativeTool() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));

        RuntimeFixture fixture;
        fixture.handler.allowedCommands = {QStringLiteral("echo slow-build")};
        fixture.handler.blockSlowBuild = true;
        fixture.create({QStringLiteral("run-command"), QStringLiteral("read-file"),
                        QStringLiteral("write-file")});
        {
            AgentSessionOptions options;
            options.autonomousMode = true;
            options.workspaceContext.rootPath = root;
            fixture.handler.root = root;
            fixture.runtime->configureSession(fixture.session, std::move(options));
        }
        fixture.planner.decisions = {
            toolDecision(QStringLiteral("run-command"), ToolRiskLevel::High)};
        auto finishPlan = finalDecision(QStringLiteral("Cannot complete; cancelled in build."));
        finishPlan.grounding = GroundingMode::Context;
        finishPlan.groundingDeclared = true;
        appendArg(fixture.planner.decisions.first(), QStringLiteral("command"),
                  QStringLiteral("echo slow-build"));
        auto queuedWrite = toolDecision(QStringLiteral("write-file"));
        appendArg(queuedWrite, QStringLiteral("path"), root + QStringLiteral("/src/calc.cpp"));
        appendArg(queuedWrite, QStringLiteral("content"), QStringLiteral("must never be written"));
        fixture.handler.allowedWrites.insert(QStringLiteral("src/calc.cpp"),
                                             QStringLiteral("must never be written"));
        fixture.planner.decisions.append(queuedWrite);
        fixture.planner.decisions.append(finishPlan);
        fixture.planner.repeatLast = false;
        // Snapshot before the run so the cancelled run's committed partial
        // artifact shows up as an addition.
        WorkspaceVerifier verifier(root);
        WorkspaceVerifier::Expected expected;
        expected.added = {QStringLiteral("build/partial.log")};

        auto cancelBuild = qScopeGuard([&] { fixture.runtime->cancel(fixture.session); });
        QVERIFY(fixture.runtime->start(fixture.session,
                                       QStringLiteral("run the slow authorized build")));
        if (!fixture.handler.slowBuildStarted.tryAcquire(1, kBarrierTimeoutMs))
            QVERIFY2(false, "slow build never started");
        QElapsedTimer cancellationClock;
        cancellationClock.start();
        QVERIFY(fixture.runtime->cancel(fixture.session));

        QTRY_COMPARE_WITH_TIMEOUT(fixture.runtime->sessionState(fixture.session).phase,
                                  AgentLoopPhase::Cancelled, kBarrierTimeoutMs);
        // Exactly one invocation; nothing may run after the cancel seal.
        fixture.handler.seal();
        QVERIFY(fixture.handler.childPid.load() > 0);
        QVERIFY(fixture.handler.childReleased.load());
        QVERIFY(!fixture.handler.childDrainTimedOut.load());
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("write-file"), 0), 0);
        qInfo() << "COR-12 owned child release and cancelled terminal observed after"
                << cancellationClock.elapsed() << "ms (fixture observation, not a timing gate)";
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("run-command"), 0), 1);
        QCOMPARE(fixture.handler.postSealCalls, 0);
        QString actual;
        QVERIFY2(verifier.matches(expected, &actual), qPrintable(actual));
        TerminalAudit audit;
        audit.scan(fixture.runtime->eventHistory(fixture.session));
        QVERIFY2(audit.singleTerminal() && audit.cancelled == 1, qPrintable(audit.describe()));
        QVERIFY(!fixture.runtime->approve(fixture.session, false));

        // Next fresh task works cooperatively after the cancelled one.
        const auto nextSession = fixture.runtime->createSession();
        QVERIFY(!nextSession.isEmpty());
        AgentSessionOptions options;
        options.autonomousMode = true;
        options.workspaceContext.rootPath = root;
        fixture.handler.root = root;
        fixture.runtime->configureSession(nextSession, std::move(options));
        fixture.handler.sealed.store(false);
        fixture.planner.resetPlanningIndex();
        fixture.planner.canonicalFinal = true;
        fixture.planner.canonicalIntent = {};
        fixture.planner.decisions = {toolDecision(QStringLiteral("read-file")),
                                     finalDecision(QStringLiteral("clean receipt"))};
        appendArg(fixture.planner.decisions.first(), QStringLiteral("path"),
                  QDir(root).filePath(QStringLiteral("src/calc.cpp")));
        QVERIFY(fixture.runtime->start(nextSession, QStringLiteral("read the fixture")));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.runtime->sessionState(nextSession).phase,
                                  AgentLoopPhase::Completed, kBarrierTimeoutMs);
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 1);
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QCOMPARE(fixture.handler.postSealCalls, 0);
        QVERIFY2(verifier.matches(expected, &actual), qPrintable(actual));
        // Partial artifact unchanged by the next task.
        QCOMPARE(verifier.fileContent(QStringLiteral("build/partial.log")),
                 QStringLiteral("partial fixture output\n"));
    }

    // ------------------------------------------------------------------
    // COR-13: cancellation while awaiting approval; late approvals are
    // rejected, the executor never runs, grants stay cleared.
    // ------------------------------------------------------------------
    void cor13_cancelWhileAwaitingApproval() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/victim.txt"),
                 QStringLiteral("unchanged"));

        RuntimeFixture fixture;
        fixture.create({QStringLiteral("run-command")});
        fixture.configure(root, false);
        WorkspaceVerifier verifier(root);
        fixture.approval.mode = FixtureApprovalPolicy::Mode::RequireApproval;
        fixture.planner.decisions = {
            toolDecision(QStringLiteral("run-command"), ToolRiskLevel::High)};
        auto finishDecision = finalDecision(QStringLiteral("must never finish"));
        finishDecision.grounding = GroundingMode::Context;
        finishDecision.groundingDeclared = true;
        appendArg(fixture.planner.decisions.first(), QStringLiteral("command"),
                  QStringLiteral("echo approved-once"));
        fixture.planner.decisions.append(finishDecision);
        fixture.planner.repeatLast = false;

        QVERIFY(fixture.runtime->start(fixture.session, QStringLiteral("approved action")));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.runtime->sessionState(fixture.session).phase,
                                  AgentLoopPhase::AwaitingApproval, kBarrierTimeoutMs);
        QVERIFY(fixture.runtime->cancel(fixture.session));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.runtime->sessionState(fixture.session).phase,
                                  AgentLoopPhase::Cancelled, kBarrierTimeoutMs);
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("run-command"), 0), 0);
        // Late approvals are rejected.
        QVERIFY(!fixture.runtime->approve(fixture.session, false));
        QVERIFY(!fixture.runtime->approve(fixture.session, true));
        // No persistent grant may exist after cancellation.
        QVERIFY(fixture.runtime->persistentPermissionGrants().isEmpty());
        TerminalAudit audit;
        audit.scan(fixture.runtime->eventHistory(fixture.session));
        QVERIFY2(audit.singleTerminal() && audit.cancelled == 1, qPrintable(audit.describe()));
        QVERIFY(verifier.untouched());
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
    }

    // ------------------------------------------------------------------
    // COR-14: repeated ineffective reads recover once; repeated mutation
    // terminates Stuck with a strictly bounded write effect.
    // ------------------------------------------------------------------
    void cor14_ineffectiveRepetition_data() {
        QTest::addColumn<QString>("variant");
        QTest::newRow("read-repair-recovers") << QStringLiteral("read");
        QTest::newRow("mutation-terminates-stuck") << QStringLiteral("mutation");
    }

    void cor14_ineffectiveRepetition() {
        QFETCH(QString, variant);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("read-file"), QStringLiteral("write-file")});
        const auto path = root + QStringLiteral("/src/calc.cpp");
        fixture.handler.allowedWrites.insert(
            QStringLiteral("src/calc.cpp"),
            QStringLiteral("int add(int a,int b){ return a + b; }\n"));

        ScriptedPlanner planner;
        if (variant == QStringLiteral("read")) {
            auto read = toolDecision(QStringLiteral("read-file"));
            appendArg(read, QStringLiteral("path"), path);
            planner.decisions = {read, read, finalDecision(QStringLiteral("observed result"))};
        } else {
            auto write = toolDecision(QStringLiteral("write-file"));
            appendArg(write, QStringLiteral("path"), path);
            appendArg(write, QStringLiteral("content"),
                      QStringLiteral("int add(int a,int b){ return a + b; }\n"));
            planner.decisions = {write, write};
        }
        planner.repeatLast = false;

        AgentLoop::Config config;
        config.maxIterations = 6;
        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools, config);
        configureLoopFixture(loop, fixture, root);
        auto policy = std::make_shared<FixedIntentPolicy>();
        loop.setObservationIntentPolicy(policy);
        planner.canonicalFinal = variant == QStringLiteral("read");
        planner.canonicalIntent = policy->intent;
        WorkspaceVerifier verifier(root);

        const auto state = runLoop(loop, this, QStringLiteral("cor14"), false,
                                   QStringLiteral("Repeat the ineffective action"));
        if (variant == QStringLiteral("read")) {
            QCOMPARE(state.phase, AgentLoopPhase::Completed);
            QCOMPARE(state.repeatedReadRepairs, 1);
            // The second identical read is intercepted; one receipt only.
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 1);
        } else {
            QCOMPARE(state.phase, AgentLoopPhase::Stuck);
            QCOMPARE(state.repeatedReadRepairs, 0);
            QVERIFY(fixture.handler.counts.value(QStringLiteral("write-file"), 0) <= 1);
            QCOMPARE(verifier.fileContent(QStringLiteral("src/calc.cpp")),
                     QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        }
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QVERIFY(verifier.untouched());
    }

    // ------------------------------------------------------------------
    // COR-15: grounded final accepted only through verified observations,
    // an independent build/test receipt and a diff. A receipt-only final is
    // completed only after the runtime obtains the missing read-only
    // verification itself; the claim alone never completes the run.
    // ------------------------------------------------------------------
    void cor15_groundedFinalEvidence_data() {
        QTest::addColumn<bool>("async");
        QTest::addColumn<bool>("withVerification");
        for (bool async : {false, true})
            for (bool withVerification : {true, false}) {
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(
                    async ? QStringLiteral("async") : QStringLiteral("sync"),
                    withVerification ? QStringLiteral("grounded")
                                     : QStringLiteral("receipt-only-final"))))
                    << async << withVerification;
            }
    }

    void cor15_groundedFinalEvidence() {
        QFETCH(bool, async);
        QFETCH(bool, withVerification);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        const auto missingCompiler = CompileOracle::unavailableReason();
        if (!missingCompiler.isEmpty())
            QSKIP(qPrintable(missingCompiler));
        QVERIFY(CompileOracle::initializeRepository(root).succeeded());
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a - b; }\n"));

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("read-file"), QStringLiteral("write-file"),
                               QStringLiteral("run-command"), QStringLiteral("git-diff")});
        fixture.handler.allowedCommands = {QStringLiteral("ctest --test-dir build")};
        fixture.handler.allowedWrites.insert(
            QStringLiteral("src/calc.cpp"),
            QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        const auto calcPath = root + QStringLiteral("/src/calc.cpp");

        auto policy = std::make_shared<FixedIntentPolicy>();
        ObservationRequirement readRequirement;
        readRequirement.domain = ObservationDomain::FileSystem;
        readRequirement.resourceHint = calcPath;
        readRequirement.freshness = EvidenceFreshness::TurnScoped;
        readRequirement.purpose = ObservationPurpose::Inspect;
        readRequirement.toolId = QStringLiteral("read-file");
        policy->intent.requirements.append(readRequirement);
        ObservationRequirement testRequirement;
        testRequirement.domain = ObservationDomain::ProcessExecution;
        testRequirement.resourceHint = QStringLiteral("ctest --test-dir build");
        testRequirement.freshness = EvidenceFreshness::TurnScoped;
        testRequirement.purpose = ObservationPurpose::Operate;
        policy->intent.requirements.append(testRequirement);
        ObservationRequirement diffRequirement;
        diffRequirement.domain = ObservationDomain::Workspace;
        diffRequirement.resourceHint = root;
        diffRequirement.freshness = EvidenceFreshness::Live;
        diffRequirement.purpose = ObservationPurpose::Inspect;
        diffRequirement.toolId = QStringLiteral("git-diff");
        policy->intent.requirements.append(diffRequirement);

        ScriptedPlanner planner;
        auto initialRead = toolDecision(QStringLiteral("read-file"));
        appendArg(initialRead, QStringLiteral("path"), calcPath);
        auto write = toolDecision(QStringLiteral("write-file"));
        appendArg(write, QStringLiteral("path"), calcPath);
        appendArg(write, QStringLiteral("content"),
                  QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        auto freshRead = toolDecision(QStringLiteral("read-file"));
        appendArg(freshRead, QStringLiteral("path"), calcPath);
        auto test = toolDecision(QStringLiteral("run-command"));
        appendArg(test, QStringLiteral("command"), QStringLiteral("ctest --test-dir build"));
        auto diff = toolDecision(QStringLiteral("git-diff"));
        appendArg(diff, QStringLiteral("path"), root);
        planner.decisions =
            withVerification ? QVector<AgentStepDecision>{initialRead, write, freshRead, test, diff}
                             : QVector<AgentStepDecision>{initialRead, write, freshRead, test};
        auto finish = finalDecision(QStringLiteral(
            "Fix complete. ctest --test-dir build returns ALL_TESTS_PASSED and the git-diff "
            "receipt shows - return a - b\\n+ return a + b."));
        finish.grounding = GroundingMode::Verified;
        finish.groundingDeclared = true;
        planner.decisions.append(finish);
        planner.repeatLast = true;
        planner.canonicalFinal = withVerification;
        planner.canonicalIntent = policy->intent;

        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools);
        configureLoopFixture(loop, fixture, root);
        loop.setObservationIntentPolicy(policy);
        loop.setToolCallIdProvider([](int) { return QStringLiteral("observed"); });
        WorkspaceVerifier verifier(root);

        const auto state =
            runLoop(loop, this, QStringLiteral("cor15"), async,
                    QStringLiteral("Repair the fixture and finish only with verified evidence"));
        WorkspaceVerifier::Expected expected;
        expected.changed = {QStringLiteral("src/calc.cpp")};
        QString actual;
        QVERIFY2(verifier.matches(expected, &actual), qPrintable(actual));
        if (withVerification) {
            QCOMPARE(state.phase, AgentLoopPhase::Completed);
            QVERIFY(state.finalAnswer.contains(QStringLiteral("ALL_TESTS_PASSED")));
            QVERIFY(state.finalAnswer.contains(QStringLiteral("return a + b")));
            QVERIFY(std::any_of(state.evidence.cbegin(), state.evidence.cend(),
                                [](const EvidenceRecord& item) {
                                    return item.scope == EvidenceScope::Operation &&
                                           item.outcome == EvidenceOutcome::Verified;
                                }));
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("git-diff"), 0), 1);
        } else {
            // The runtime recovers the missing read-only verification before accepting
            // the final. Tool success plus a prose receipt alone is never enough.
            QVERIFY2(state.phase == AgentLoopPhase::Completed,
                     qPrintable(terminalFailureDescription(state)));
            QVERIFY(state.finalAnswer.contains(QStringLiteral("ALL_TESTS_PASSED")));
            QVERIFY(state.finalAnswer.contains(QStringLiteral("return a + b")));
            QVERIFY(state.finalVerificationRecoveries >= 1);
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("git-diff"), 0), 1);
            QVERIFY(std::any_of(state.evidence.cbegin(), state.evidence.cend(),
                                [](const EvidenceRecord& item) {
                                    return item.domain == ObservationDomain::Workspace &&
                                           item.toolId == QLatin1String("git-diff") &&
                                           item.outcome == EvidenceOutcome::Verified;
                                }));
        }
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QCOMPARE(verifier.fileContent(QStringLiteral("src/calc.cpp")),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        const auto beforeOracle = stateOf(root).files;
        const auto real = CompileOracle{}.verify(root, false, CompileOracle::Mode::Normal);
        qInfo().noquote() << "Independent oracle:" << real.summary();
        QCOMPARE(stateOf(root).files,
                 beforeOracle); // Compiler/test must not mutate sources or tests.
        QVERIFY2(verifier.matches(expected, &actual), qPrintable(actual));
        QVERIFY2(real.passed(), qPrintable(real.summary()));
        QCOMPARE(independentlyAccepted(state, verifier, expected, real), true);
    }

    // ------------------------------------------------------------------
    // COR-16: fabricated success claims are rejected; bounded inability
    // remains truthful without inventing a passing outcome.
    // ------------------------------------------------------------------
    void cor16_fabricatedClaims_data() {
        QTest::addColumn<QString>("variant");
        QTest::newRow("fabricated-test-success") << QStringLiteral("fabricated-success");
        QTest::newRow("no-observation-final") << QStringLiteral("no-observation");
    }

    void cor16_fabricatedClaims() {
        QFETCH(QString, variant);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a - b; }\n"));

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("write-file"), QStringLiteral("run-command")});
        fixture.handler.testReceiptFails = true;
        fixture.handler.allowedCommands = {QStringLiteral("ctest --test-dir build")};
        fixture.handler.allowedWrites.insert(
            QStringLiteral("src/calc.cpp"),
            QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        const auto calcPath = root + QStringLiteral("/src/calc.cpp");

        ScriptedPlanner planner;
        if (variant == QStringLiteral("fabricated-success")) {
            auto write = toolDecision(QStringLiteral("write-file"));
            appendArg(write, QStringLiteral("path"), calcPath);
            appendArg(write, QStringLiteral("content"),
                      QStringLiteral("int add(int a,int b){ return a + b; }\n"));
            auto test = toolDecision(QStringLiteral("run-command"), ToolRiskLevel::High);
            appendArg(test, QStringLiteral("command"), QStringLiteral("ctest --test-dir build"));
            auto fabricated = finalDecision(
                QStringLiteral("The authorized test suite passed and the fix is complete."));
            fabricated.grounding = GroundingMode::Verified;
            fabricated.groundingDeclared = true;
            planner.decisions = {write, test, fabricated};
        } else {
            auto fabricated = finalDecision(
                QStringLiteral("src/calc.cpp now contains the fixed addition operator."));
            fabricated.grounding = GroundingMode::Verified;
            fabricated.groundingDeclared = true;
            planner.decisions = {fabricated};
        }
        planner.repeatLast = true;

        AgentLoop::Config config;
        config.maxIterations = 3;
        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools, config);
        configureLoopFixture(loop, fixture, root);
        WorkspaceVerifier verifier(root);

        const auto state = runLoop(loop, this, QStringLiteral("cor16"), false,
                                   QStringLiteral("Repair the fixture file"));
        QVERIFY2(state.phase == AgentLoopPhase::Failed || state.phase == AgentLoopPhase::Stuck,
                 qPrintable(terminalFailureDescription(state)));
        QVERIFY(state.finalAnswer.isEmpty());
        if (variant == QStringLiteral("fabricated-success")) {
            // The failing receipt ran once and cannot be repeated into green.
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("run-command"), 0), 1);
            WorkspaceVerifier::Expected expected;
            expected.changed = {QStringLiteral("src/calc.cpp")};
            QString actual;
            QVERIFY2(verifier.matches(expected, &actual), qPrintable(actual));
        } else {
            // No tool executed: no verified observation supports the claim.
            QCOMPARE(fixture.handler.unauthorizedCalls, 0);
            QVERIFY(verifier.untouched());
        }
    }

    // ------------------------------------------------------------------
    // COR-16 (extended): a mutated resource may not be described as verified
    // without a fresh post-mutation observation. The production final-answer
    // gate already requires it, so this is exercised against the real policy:
    // without a reread the verified final is rejected and the run fails
    // boundedly; a genuine reread grounds the final; a fabricated structured
    // claim attached to otherwise-valid evidence is still rejected. File
    // changes are verified independently of tool output.
    // ------------------------------------------------------------------
    void cor16_mutatedResourceReread_data() {
        QTest::addColumn<QString>("variant");
        QTest::newRow("mutated-without-reread") << QStringLiteral("without-reread");
        QTest::newRow("mutated-with-reread") << QStringLiteral("with-reread");
        QTest::newRow("fabricated-claim-after-reread") << QStringLiteral("fabricated");
    }

    void cor16_mutatedResourceReread() {
        QFETCH(QString, variant);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        const auto fixed = QStringLiteral("int add(int a,int b){ return a + b; }\n");
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a - b; }\n"));
        const auto calcPath = root + QStringLiteral("/src/calc.cpp");

        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("read-file"), QStringLiteral("write-file")});
        fixture.handler.allowedWrites.insert(QStringLiteral("src/calc.cpp"), fixed);

        ScriptedPlanner planner;
        auto readBefore = toolDecision(QStringLiteral("read-file"));
        appendArg(readBefore, QStringLiteral("path"), calcPath);
        auto write = toolDecision(QStringLiteral("write-file"), ToolRiskLevel::Medium);
        appendArg(write, QStringLiteral("path"), calcPath);
        appendArg(write, QStringLiteral("content"), fixed);
        auto readAfter = toolDecision(QStringLiteral("read-file"));
        appendArg(readAfter, QStringLiteral("path"), calcPath);

        auto final = finalDecision(QStringLiteral("src/calc.cpp now returns a + b."));
        final.grounding = GroundingMode::Verified;
        final.groundingDeclared = true;
        if (variant == QStringLiteral("fabricated")) {
            // Structurally valid reread evidence plus a fabricated claim id.
            final.claims = {{QStringLiteral("ghost-claim"), true}};
            planner.decisions = {readBefore, write, readAfter, final};
        } else if (variant == QStringLiteral("with-reread")) {
            planner.decisions = {readBefore, write, readAfter, final};
        } else { // without-reread
            planner.decisions = {readBefore, write, final};
        }
        planner.repeatLast = true;
        // Verified filesystem finals must use the evidence-derived canonical
        // representation; derive it from the same (empty) classified intent.
        planner.canonicalFinal = true;

        AgentLoop::Config config;
        config.maxIterations = 4;
        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools, config);
        configureLoopFixture(loop, fixture, root);
        WorkspaceVerifier verifier(root);

        const auto state = runLoop(loop, this, QStringLiteral("cor16-reread"), false,
                                   QStringLiteral("Repair and verify the fixture file"));

        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QCOMPARE(fixture.executor.calls, 0);
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("write-file"), 0), 1);
        WorkspaceVerifier::Expected expected;
        expected.changed = {QStringLiteral("src/calc.cpp")};
        QString actual;
        QVERIFY2(verifier.matches(expected, &actual), qPrintable(actual));
        QCOMPARE(verifier.fileContent(QStringLiteral("src/calc.cpp")), fixed);

        if (variant == QStringLiteral("with-reread")) {
            QVERIFY2(state.phase == AgentLoopPhase::Completed,
                     qPrintable(terminalFailureDescription(state)));
            QVERIFY(!state.finalAnswer.isEmpty());
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 2);
            QCOMPARE(state.rejectedFinalAnswers, 0);
        } else if (variant == QStringLiteral("fabricated")) {
            // The reread satisfies the mutation gate, but the fabricated claim
            // id is unknown to the classified intent and is rejected.
            QCOMPARE(state.phase, AgentLoopPhase::Failed);
            QVERIFY(state.finalAnswer.isEmpty());
            QCOMPARE(state.terminalReason, AgentTerminalReason::VerificationFailed);
            QVERIFY2(state.abortReason.contains(QStringLiteral("unknown claim")),
                     qPrintable(state.abortReason));
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 2);
        } else { // The missing read-back is recovered through the real gateway.
            QVERIFY2(state.phase == AgentLoopPhase::Completed,
                     qPrintable(terminalFailureDescription(state)));
            QVERIFY(!state.finalAnswer.isEmpty());
            QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file"), 0), 2);
            QCOMPARE(state.rejectedFinalAnswers, 1);
            QCOMPARE(state.finalVerificationRecoveries, 1);
        }
    }

    // Run the actual daemon/Desktop fixture executable in an isolated profile.
    // These fixed test arguments never enter model or shell dispatch.
private:
    void exerciseGenerationCancellation(bool native, bool cooperative, bool requireStream) {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        seedFile(QDir(root), "artifact.txt", "original\n");
        WorkspaceVerifier verifier(root);
        auto provider = std::make_shared<CancellationProbeProvider>();
        provider->native = native;
        provider->cooperative = cooperative;
        provider->artifact = root + "/artifact.txt";
        provider->readPath = provider->artifact;
        sentinel::test::DeterministicModelServiceFixture catalog;
        auto models = catalog.takeModelService();
        // COR-11 verifies the deadline-aware bounded planner streaming contract.
        // Providers registered as Local intentionally use the cancellable
        // non-streaming request without a wall-clock deadline, so this fixture
        // binds an unregistered provider ID, which the wait policy treats like
        // the cloud/unknown class that keeps the 60/120 s deadlines. The probe
        // factory answers every request; no network endpoint is contacted.
        models->setLmStudioEndpoint(QStringLiteral("http://127.0.0.1:1234"));
        models->registerProvider("remote-probe",
                                 [provider](const ModelBinding&) { return provider; });
        models->setSelectedModel({"remote-probe", "original-model"});
        auto original = models->resolve(models->selectedModel());
        QVERIFY(original.ok());
        original.binding.capabilities.streaming = CapabilitySupport::Supported;
        original.binding.capabilities.nativeToolCalling =
            native ? CapabilitySupport::Supported : CapabilitySupport::Unsupported;
        original.binding.capabilities.structuredOutput = CapabilitySupport::Unsupported;
        const auto descriptors = fixtureDescriptors({"read-file", "write-file"});
        FixtureHandler handler;
        handler.root = root;
        handler.allowedWrites.insert("artifact.txt", "late mutation");
        NoOpExecutor executor;
        FixtureApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        LlmAgentRuntime planner(descriptors, provider.get());
        planner.bindModel(original.binding, original.provider);
        QSemaphore finished;
        AgentRuntime runtime(std::make_unique<FixtureMetadataRuntime>(descriptors), planner,
                             executor, approval, sandbox);
        for (const auto& descriptor : descriptors) {
            QVERIFY(runtime.toolRegistry().unregisterTool(descriptor.id));
            QVERIFY(runtime.toolRegistry().registerTool(
                {descriptor, std::shared_ptr<IToolHandler>(&handler, [](IToolHandler*) {})}));
        }
        const auto configure = [&](const QString& session) {
            AgentSessionOptions options;
            options.autonomousMode = true;
            options.workspaceContext.rootPath = root;
            options.onFinished = [&](const AgentLoopState&) { finished.release(); };
            runtime.configureSession(session, options);
        };
        const auto session = runtime.createSession();
        configure(session);
        auto release = qScopeGuard([&] { provider->release.release(); });
        QVERIFY(runtime.start(session, "Generate prescribed synthetic answer"));
        QTRY_VERIFY_WITH_TIMEOUT(provider->entered.available() > 0, kBarrierTimeoutMs);
        QVERIFY(provider->entered.tryAcquire());
        const auto count = [&](const QString& id, AgentEventType type) {
            int found = 0;
            for (const auto& event : runtime.eventHistory(id))
                if (event.type == type)
                    ++found;
            return found;
        };
        const bool firstDeltaReceived =
            provider->emittedDeltas.load() >
            0; // Internal observer; never require private public deltas.
        const auto oldToken = provider->originalToken;
        QVERIFY(oldToken && !oldToken->load());
        models->setSelectedModel({"remote-probe", "next-model"});
        QCOMPARE(planner.modelBinding().modelId, QStringLiteral("original-model"));
        QCOMPARE(planner.modelBinding().providerId, QStringLiteral("remote-probe"));
        QVERIFY(runtime.cancel(session));
        QVERIFY(oldToken->load());
        QCOMPARE(runtime.sessionState(session).phase, AgentLoopPhase::Cancelling);
        QCOMPARE(count(session, AgentEventType::AgentCancelled), 0);
        QCOMPARE(provider->cancellationAcks.load(), 0);
        provider->release.release();
        QTRY_VERIFY_WITH_TIMEOUT(finished.available() > 0, kBarrierTimeoutMs);
        QVERIFY(finished.tryAcquire());
        QCOMPARE(runtime.sessionState(session).phase, AgentLoopPhase::Cancelled);
        QVERIFY(runtime.sessionState(session).finalAnswer.isEmpty());
        QCOMPARE(handler.counts.value("write-file"), 0);
        QCOMPARE(handler.counts.value("read-file"), 0);
        QCOMPARE(handler.unauthorizedCalls, 0);
        QCOMPARE(executor.calls, 0);
        QVERIFY(verifier.untouched());
        QCOMPARE(provider->cancellationAcks.load(), cooperative ? 1 : 0);
        TerminalAudit terminal;
        terminal.scan(runtime.eventHistory(session));
        QVERIFY2(terminal.singleTerminal() && terminal.cancelled == 1,
                 qPrintable(terminal.describe()));
        const auto frozen = runtime.eventHistory(session);
        QString turn;
        for (const auto& event : frozen) {
            QCOMPARE(event.sessionId, session);
            if (event.type == AgentEventType::SessionCreated)
                continue;
            QVERIFY(!event.turnId.isEmpty());
            if (turn.isEmpty())
                turn = event.turnId;
            QCOMPARE(event.turnId, turn);
            QCOMPARE(event.providerId, QStringLiteral("remote-probe"));
            QCOMPARE(event.modelId, QStringLiteral("original-model"));
        }
        // Bounded planner streaming is the selected dispatch for text-only
        // requests when the provider explicitly implements the deadline-aware
        // contract. Native tool calls and structured output stay non-streaming.
        if (!native) {
            QVERIFY2(firstDeltaReceived,
                     "bounded planner streaming never delivered a first provider delta");
            QCOMPARE(count(session, AgentEventType::ModelOutputDelta), 0);
            QCOMPARE(provider->streamRequests.load(), 1);
            QCOMPARE(provider->requests.load(), 0);
            // The first delta arrived before cancellation; the late delta after
            // cancellation was delivered but never accepted.
            QCOMPARE(provider->emittedDeltas.load(), 2);
            QCOMPARE(provider->originalDeadline, 120000);
            QVERIFY(provider->originalRequireBounded.load());
            QCOMPARE(provider->originalNative, false);
            QCOMPARE(planner.modelBinding().capabilities.streaming, CapabilitySupport::Supported);
        } else {
            // There is no verified incremental native tool-call assembly contract,
            // so native requests must remain on the bounded non-streaming path.
            QCOMPARE(firstDeltaReceived, false);
            QCOMPARE(count(session, AgentEventType::ModelOutputDelta), 0);
            QCOMPARE(provider->streamRequests.load(), 0);
            QCOMPARE(provider->emittedDeltas.load(), 0);
            QCOMPARE(provider->requests.load(), 1);
            QCOMPARE(provider->originalDeadline, 120000);
            QCOMPARE(provider->originalNative, true);
        }
        QVERIFY(!runtime.approve(session, true));
        QVERIFY(!runtime.cancel(session));
        auto nextBinding = models->resolve(models->selectedModel());
        QVERIFY(nextBinding.ok());
        nextBinding.binding.capabilities.structuredOutput = CapabilitySupport::Unsupported;
        nextBinding.binding.capabilities.nativeToolCalling = CapabilitySupport::Unsupported;
        // The subsequent task deliberately uses the bounded non-streaming path to
        // prove the fallback stays intact on the same runtime.
        nextBinding.binding.capabilities.streaming = CapabilitySupport::Unsupported;
        provider->fresh.store(true);
        planner.bindModel(nextBinding.binding, nextBinding.provider);
        const auto next = runtime.createSession();
        configure(next);
        QVERIFY(next != session);
        QVERIFY(runtime.start(next, "Read prescribed fixture in a fresh task"));
        QTRY_VERIFY_WITH_TIMEOUT(finished.available() > 0, kBarrierTimeoutMs);
        QVERIFY(finished.tryAcquire());
        QCOMPARE(runtime.sessionState(next).phase, AgentLoopPhase::Completed);
        QVERIFY(!runtime.sessionState(next).finalAnswer.contains("late"));
        QCOMPARE(handler.counts.value("read-file"), 1);
        QCOMPARE(handler.counts.value("write-file"), 0);
        QCOMPARE(provider->freshRequests.load(), 2);
        QVERIFY(provider->freshToken && provider->freshToken != oldToken &&
                !provider->freshToken->load());
        TerminalAudit nextTerminal;
        nextTerminal.scan(runtime.eventHistory(next));
        QVERIFY(nextTerminal.singleTerminal() && nextTerminal.completed == 1);
        for (const auto& event : runtime.eventHistory(next)) {
            if (event.type == AgentEventType::SessionCreated)
                continue;
            QCOMPARE(event.modelId, QStringLiteral("next-model"));
            QVERIFY(event.turnId != turn);
        }
        QCOMPARE(runtime.sessionState(session).phase, AgentLoopPhase::Cancelled);
        QVERIFY(runtime.sessionState(session).finalAnswer.isEmpty());
        QCOMPARE(count(session, AgentEventType::AgentCompleted), 0);
        QCOMPARE(count(session, AgentEventType::AgentCancelled), 1);
        const auto afterNext = runtime.eventHistory(session);
        QCOMPARE(afterNext.size(), frozen.size());
        for (int i = 0; i < frozen.size(); ++i)
            QCOMPARE(afterNext[i].id, frozen[i].id);
        QVERIFY(verifier.untouched());
        QVERIFY(!provider->barrierTimedOut.load());
        qInfo() << "COR11 dispatch" << "request=" << provider->requests.load()
                << "stream=" << provider->streamRequests.load()
                << "runtime-deltas=" << count(session, AgentEventType::ModelOutputDelta)
                << "cancel-ack=" << provider->cancellationAcks.load()
                << "next-read=" << handler.counts.value("read-file");
        if (requireStream && native)
            QSKIP("COR-11 native incremental tool-call assembly remains explicitly blocked: "
                  "IChatProvider exposes no typed incremental tool-argument assembly event, so a "
                  "partially streamed native call could only be dispatched after a validated "
                  "terminal assembly. This fixture asserts the native request stays on the bounded "
                  "non-streaming path (one sendRequest, zero streamed deltas, zero public deltas) "
                  "and does not invent an assembly contract.");
        if (requireStream && !native && !firstDeltaReceived)
            QVERIFY2(false, "the bounded planner stream must start for a text-only request; the "
                            "after-first-delta cancellation oracle cannot be satisfied");
    }

    void exerciseIndependentOracle(int corpus, const QString& variant) {
        const auto missingCompiler = CompileOracle::unavailableReason();
        if (!missingCompiler.isEmpty())
            QSKIP(qPrintable(missingCompiler));
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        QVERIFY(CompileOracle::initializeRepository(root).succeeded());
        const bool multi = corpus == 3;
        seedFile(QDir(root), "src/calc.cpp",
                 multi ? "#include \"calc.h\"\nint add(int a,int b){return a*b;}\n"
                       : "int add(int a,int b){return a*b;}\n");
        seedFile(QDir(root), "tests/oracle.txt", "trusted baseline\n");
        if (multi)
            seedFile(QDir(root), "include/calc.h", "int add(int,int);\n");
        WorkspaceVerifier verifier(root);
        WorkspaceVerifier::Expected allowed;
        allowed.changed = {QStringLiteral("src/calc.cpp")};
        QString source = multi ? "#include \"calc.h\"\nint add(int a,int b,int c){return a+b+c;}\n"
                               : "int add(int a,int b){return a+b;}\n";
        if (variant == "wrong-operator" || variant == "fabricated-receipts")
            source = "int add(int a,int b){return a-b;}\n";
        if (variant == "compile-failure" || variant == "misleading-receipts")
            source = "int add(int a,int b){return a + ;}\n";
        if (variant == "broken-signature")
            source = "#include \"calc.h\"\nint add(int a,int b){return a+b;}\n";
        LoopFixture fixture;
        fixture.registerTools({QStringLiteral("write-file"), QStringLiteral("run-command")});
        fixture.handler.allowedCommands = {QStringLiteral("cmake --build build"),
                                           QStringLiteral("ctest --test-dir build")};
        fixture.handler.allowedWrites.insert("src/calc.cpp", source);
        ScriptedPlanner planner;
        if (multi) {
            allowed.changed.append("include/calc.h");
            const QString header =
                variant == "broken-header" ? "int add( ;\n" : "int add(int,int,int);\n";
            fixture.handler.allowedWrites.insert("include/calc.h", header);
            auto write = toolDecision("write-file");
            appendArg(write, "path", root + "/include/calc.h");
            appendArg(write, "content", header);
            planner.decisions.append(write);
        }
        auto write = toolDecision("write-file");
        appendArg(write, "path", root + "/src/calc.cpp");
        appendArg(write, "content", source);
        planner.decisions.append(write);
        for (const auto& command :
             {QStringLiteral("cmake --build build"), QStringLiteral("ctest --test-dir build")}) {
            auto receipt = toolDecision("run-command");
            appendArg(receipt, "command", command);
            planner.decisions.append(receipt);
        }
        auto final = finalDecision("Fixture receipts report ALL_TESTS_PASSED.");
        final.grounding = GroundingMode::Context;
        final.groundingDeclared = true;
        planner.decisions.append(final);
        planner.repeatLast = false;
        AgentLoop loop(planner, fixture.executor, fixture.approval, fixture.sandbox,
                       fixture.knownTools);
        configureLoopFixture(loop, fixture, root);
        const auto state =
            runLoop(loop, this, "independent-oracle", false, "Apply prescribed synthetic repair");
        QCOMPARE(state.phase, AgentLoopPhase::Completed);
        QCOMPARE(fixture.handler.counts.value("run-command"), 2);
        QCOMPARE(fixture.handler.counts.value("write-file"), multi ? 2 : 1);
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QString difference;
        QVERIFY2(verifier.matches(allowed, &difference), qPrintable(difference));
        // Deliberately plant a verifier fault outside Agent authority. Do not weaken tool gates.
        if (variant == "unauthorized-change")
            seedFile(QDir(root), "tests/oracle.txt", "tampered baseline\n");
        auto mode = CompileOracle::Mode::Normal;
        if (variant == "test-failure")
            mode = CompileOracle::Mode::TestFailure;
        if (variant == "nonzero")
            mode = CompileOracle::Mode::Nonzero;
        if (variant == "timeout")
            mode = CompileOracle::Mode::Timeout;
        if (variant == "missing-test")
            mode = CompileOracle::Mode::BuildOnly;
        const auto before = stateOf(root).files;
        const auto real = CompileOracle{}.verify(root, multi, mode);
        qInfo().noquote()
            << QStringLiteral("COR-%1 %2 independent oracle:").arg(corpus).arg(variant)
            << real.summary();
        QCOMPARE(stateOf(root).files, before);
        if (variant == "stale-source") {
            QVERIFY(real.passed());
            QVERIFY(real.matchesSources(root));
            seedFile(QDir(root), "src/calc.cpp", "int add(int a,int b){return a-b;}\n");
            QVERIFY(verifier.matches(
                allowed)); // Same authorized file; stale compilation is still invalid.
            QVERIFY(!real.matchesSources(root));
        }
        QCOMPARE(independentlyAccepted(state, verifier, allowed, real), variant == "success");
        QVERIFY(!real.steps.isEmpty());
        if (variant == "compile-failure" || variant == "misleading-receipts" ||
            variant == "broken-header") {
            QCOMPARE(real.steps.size(), 1);
            QVERIFY(real.steps.first().drained);
            QVERIFY(real.steps.first().process.exitCode != 0);
            QVERIFY(!real.steps.first().output.isEmpty());
        } else if (variant == "broken-signature") {
            QCOMPARE(real.steps.size(), 3);
            QVERIFY(real.steps[0].succeeded() && real.steps[1].succeeded());
            QVERIFY(real.steps.last().drained && real.steps.last().process.exitCode != 0);
        } else if (variant == "missing-test") {
            QCOMPARE(real.steps.size(), 3);
            QVERIFY(real.steps.last().succeeded());
            QVERIFY(!real.passed());
        } else {
            QCOMPARE(real.steps.size(), 4);
            QVERIFY(real.steps[0].succeeded() && real.steps[1].succeeded() &&
                    real.steps[2].succeeded());
            QVERIFY(real.steps.last().drained);
            QVERIFY(real.steps.last().process.systemPid > 0);
            if (variant == "timeout") {
                QVERIFY(real.steps.last().process.timedOut);
                QCOMPARE(real.steps.last().process.state, ProcessState::Failed);
                QVERIFY(real.steps.last().output.contains("oracle-timeout-ready"));
            } else {
                QCOMPARE(real.steps.last().process.state, ProcessState::Exited);
                QCOMPARE(real.steps.last().process.exitCode,
                         variant == "nonzero" ? 23
                         : (variant == "test-failure" || variant == "wrong-operator" ||
                            variant == "fabricated-receipts")
                             ? 7
                             : 0);
                if (variant == "nonzero")
                    QVERIFY(real.steps.last().output.contains("ALL_TESTS_PASSED"));
                if (variant == "unauthorized-change") {
                    QVERIFY(real.passed()); // Good binary cannot authorize a trusted-test mutation.
                    QVERIFY(!verifier.matches(allowed));
                }
            }
        }
    }

    void runIpcCorpusCase(const QString& slot) {
#ifndef SENTINEL_RELIABILITY_IPC_FIXTURE
        QSKIP("Real IPC fixture target is unavailable in this build.");
#else
        ProcessExecutor processes;
        ProcessRequest request;
        request.program = QString::fromUtf8(SENTINEL_RELIABILITY_IPC_FIXTURE);
        request.arguments = {slot};
        request.timeoutMs = 30000;
        request.unconfinedPermitted = true;
        request.environment = QProcessEnvironment::systemEnvironment();
        request.environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        ProcessRecord terminal;
        QByteArray output;
        bool finished = false;
        processes.start(
            request,
            [&](const ProcessRecord& record) {
                if (record.state == ProcessState::Exited || record.state == ProcessState::Failed ||
                    record.state == ProcessState::Cancelled) {
                    terminal = record;
                    finished = true;
                }
            },
            [&](const QString&, ProcessStream, const QByteArray& bytes) { output += bytes; });
        QVERIFY2(QTest::qWaitFor([&] { return finished; }, 35000),
                 "IPC corpus fixture did not terminate");
        QVERIFY2(terminal.state == ProcessState::Exited && terminal.exitCode == 0,
                 output.constData());
        QVERIFY2(!output.contains("SKIP   :") && output.contains("0 skipped"), output.constData());
#endif
    }
private slots:
    void cor17_disconnectReattachDuringActiveRun() {
        runIpcCorpusCase(QStringLiteral("cor17_disconnectReattachDuringActiveRun"));
    }
    void cor18_staleDuplicateForeignEventsIgnored() {
        runIpcCorpusCase(QStringLiteral("cor18_staleDuplicateForeignEventsIgnored"));
    }
    void cor19_eventSequenceMonotonicAndGenerationFencing() {
        runIpcCorpusCase(QStringLiteral("cor19_eventSequenceMonotonicAndGenerationFencing"));
    }
    void cor20_multiClientBackpressureAndBufferSaturation() {
        runIpcCorpusCase(QStringLiteral("cor20_multiClientBackpressureAndBufferSaturation"));
    }

    // Component check only, deliberately separate from the COR-18 wire fixtures.
    void terminalRuntimeRejectsLateAndForeignCommands() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = canonicalRoot(workspace);
        const auto path = root + QStringLiteral("/src/calc.cpp");
        seedFile(QDir(workspace.path()), QStringLiteral("src/calc.cpp"),
                 QStringLiteral("int add(int a,int b){ return a + b; }\n"));
        WorkspaceVerifier verifier(root);
        RuntimeFixture fixture;
        fixture.create({QStringLiteral("read-file")});
        fixture.configure(root);
        fixture.planner.canonicalFinal = true;
        fixture.planner.decisions = {toolDecision(QStringLiteral("read-file")),
                                     finalDecision(QStringLiteral("observed receipt"))};
        appendArg(fixture.planner.decisions.first(), QStringLiteral("path"), path);
        QVERIFY(fixture.runtime->start(fixture.session, QStringLiteral("read the fixture")));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.runtime->sessionState(fixture.session).phase,
                                  AgentLoopPhase::Completed, kBarrierTimeoutMs);
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file")), 1);
        const auto frozen = fixture.runtime->eventHistory(fixture.session);
        QVERIFY(!fixture.runtime->cancel(fixture.session));
        QVERIFY(!fixture.runtime->cancel(QStringLiteral("foreign-session")));
        QVERIFY(!fixture.runtime->approve(QStringLiteral("foreign-session"), false));
        QVERIFY(!fixture.runtime->approve(fixture.session, false));
        QVERIFY(!fixture.runtime->approve(fixture.session, true));
        QCOMPARE(fixture.runtime->eventHistory(fixture.session).size(), frozen.size());
        TerminalAudit audit;
        audit.scan(frozen);
        QVERIFY2(audit.singleTerminal() && audit.completed == 1, qPrintable(audit.describe()));
        const auto next = fixture.runtime->createSession();
        QVERIFY(!next.isEmpty());
        AgentSessionOptions options;
        options.autonomousMode = true;
        options.workspaceContext.rootPath = root;
        fixture.runtime->configureSession(next, options);
        fixture.planner.resetPlanningIndex();
        QVERIFY(fixture.runtime->start(next, QStringLiteral("read again")));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.runtime->sessionState(next).phase,
                                  AgentLoopPhase::Completed, kBarrierTimeoutMs);
        QCOMPARE(fixture.handler.counts.value(QStringLiteral("read-file")), 2);
        QCOMPARE(fixture.handler.unauthorizedCalls, 0);
        QVERIFY(verifier.untouched());
    }
};

int main(int argc, char** argv) {
    if (argc == 2 && QByteArray(argv[1]) == "--oracle-timeout-child") {
        QCoreApplication app(argc, argv);
        QFile output;
        if (!output.open(stdout, QIODevice::WriteOnly))
            return 4;
        output.write("oracle-timeout-ready\n");
        output.flush();
        return app.exec(); // Owned timeout target, no sleeps or arbitrary executable.
    }
    if (argc == 3 && QByteArray(argv[1]) == "--cooperative-child") {
        QCoreApplication app(argc, argv);
        const QString artifact = QString::fromLocal8Bit(argv[2]);
        if (!QDir().mkpath(QFileInfo(artifact).absolutePath()))
            return 2;
        QFile file(artifact);
        if (!file.open(QIODevice::WriteOnly) || file.write("partial fixture output\n") <= 0)
            return 3;
        file.close();
        QFile output;
        if (!output.open(stdout, QIODevice::WriteOnly))
            return 4;
        output.write("fixture-child-ready\n");
        output.flush();
        return app.exec(); // only the owning ProcessExecutor terminates this helper
    }
    QGuiApplication app(argc, argv);
    AgentReliabilityHarnessTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_agent_reliability_harness.moc"
