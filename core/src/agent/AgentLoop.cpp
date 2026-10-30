// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/agent/AgentLoop.h"
#include "sentinel/core/agent/AgentExecutionScheduler.h"
#include "sentinel/core/agent/ClaimGroundingResolver.h"
#include "sentinel/core/chat/IChatHistoryStore.h"
#include "sentinel/core/interfaces/IMemoryStore.h"

#include "sentinel/core/runtime/IToolExecutor.h"
#include "sentinel/core/runtime/IToolRegistry.h"
#include "sentinel/core/security/AuthorizationResolver.h"
#include "sentinel/core/security/ExternalDirectoryGate.h"
#include "sentinel/core/security/IApprovalPolicy.h"
#include "sentinel/core/security/ISandboxPolicy.h"
#include "sentinel/core/security/PathGuard.h"
#include "sentinel/core/security/PermissionService.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QScopeGuard>

#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <optional>
#include <utility>

namespace sentinel::core {
namespace {
Q_LOGGING_CATEGORY(loopDiagnostics, "sentinel.agent.loop.diagnostics", QtWarningMsg)
QJsonObject traceArguments(const QList<ToolInvocationArgument>& arguments,
                           const QString& workspace) {
    QJsonObject result;
    for (const auto& argument : arguments) {
        if (argument.id == QLatin1String("path") || argument.id == QLatin1String("source") ||
            argument.id == QLatin1String("destination")) {
            const auto path = ExternalDirectoryGate{}.resolvePath(argument.value, workspace);
            result.insert(argument.id, !workspace.isEmpty() && PathGuard::contains(workspace, path)
                                           ? QStringLiteral("<workspace>/") +
                                                 QDir(workspace).relativeFilePath(path)
                                           : QStringLiteral("<redacted-path>"));
        } else if (argument.id == QLatin1String("command")) {
            result.insert(QStringLiteral("command_sha256"),
                          QString::fromLatin1(QCryptographicHash::hash(argument.value.toUtf8(),
                                                                       QCryptographicHash::Sha256)
                                                  .toHex()));
        } else if (argument.id == QLatin1String("offset") ||
                   argument.id == QLatin1String("limit") ||
                   argument.id == QLatin1String("timeoutMs")) {
            bool valid = false;
            const auto number = argument.value.toInt(&valid);
            if (valid) {
                result.insert(argument.id, number);
            }
        } else if (argument.id == QLatin1String("staged")) {
            result.insert(argument.id, argument.value == QLatin1String("true"));
        }
    }
    return result;
}

void traceStage(const AgentLoopState& state, const char* stage, const char* event,
                const AgentStepDecision* decision = nullptr, const QString& workspace = {}) {
    QJsonObject record{{"session_id", state.sessionId},
                       {"stage", stage},
                       {"event", event},
                       {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                       {"step", state.steps.size() + 1},
                       {"classification_indeterminate", state.observationIntent.indeterminate}};
    QJsonArray requirements, evidence;
    for (const auto& item : state.observationIntent.requirements) {
        requirements.append(QJsonObject{{"domain", observationDomainName(item.domain)},
                                        {"purpose", int(item.purpose)},
                                        {"tool", item.toolId}});
    }
    for (const auto& item : state.evidence) {
        evidence.append(QJsonObject{{"tool", item.toolId},
                                    {"step", item.stepIndex},
                                    {"outcome", int(item.outcome)},
                                    {"scope", int(item.scope)}});
    }
    record.insert("requirements", requirements);
    record.insert("evidence", evidence);
    record.insert("phase", agentLoopPhaseName(state.phase));
    record.insert("rejected_finals", state.rejectedFinalAnswers);
    record.insert("blocked_steps", state.blockedSteps);
    record.insert("consecutive_blocked_steps", state.consecutiveBlockedSteps);
    record.insert("read_repairs", state.repeatedReadRepairs);
    record.insert("repeated_action_repairs", state.repeatedActionRepairs);
    record.insert("failed_repeat_repairs", state.repeatedFailureRepairs);
    record.insert("verification_recoveries", state.finalVerificationRecoveries);
    if (decision) {
        record.insert("decision", int(decision->kind));
        record.insert("tool", decision->toolId);
        QJsonArray calls;
        if (decision->toolBatch.isEmpty() && !decision->toolId.isEmpty()) {
            calls.append(
                QJsonObject{{"tool", decision->toolId},
                            {"arguments", traceArguments(decision->arguments, workspace)}});
        }
        for (const auto& call : decision->toolBatch) {
            calls.append(QJsonObject{{"tool", call.toolId},
                                     {"provider_call_id", call.providerCallId.left(128)},
                                     {"arguments", traceArguments(call.arguments, workspace)}});
        }
        record.insert("calls", calls);
    }
    if (QLatin1String(stage) == QLatin1String("evidence") && !state.steps.isEmpty()) {
        const auto& step = state.steps.last();
        record.insert("executed_tool", step.toolId);
        record.insert("executed_arguments", traceArguments(step.arguments, workspace));
        record.insert("tool_status", step.statusText);
    }
    if (decision)
        record.insert("tool_requested",
                      !decision->toolId.isEmpty() || !decision->toolBatch.isEmpty());
    qCInfo(loopDiagnostics).noquote() << QJsonDocument(record).toJson(QJsonDocument::Compact);
}
} // namespace

QString AgentLoop::workingDirectory() const {
    if (!workspaceContext_.rootPath.trimmed().isEmpty())
        return PathGuard::canonicalPath(workspaceContext_.rootPath);
    return QDir::currentPath();
}

QString AgentLoop::truncatedObservation(const QByteArray& output, const QString& toolName) const {
    const auto result = truncator_.truncate(output, toolName);
    if (!result.truncated)
        return result.preview;
    QString preview = result.preview;
    // Keep the observation actionable: a model that cannot see the omitted middle must know
    // where the complete output went and that repeating the same call will not help.
    if (!result.fullOutputPath.isEmpty()) {
        preview += QStringLiteral("\n\n[Output truncated: %1 bytes total. Full output: %2. "
                                  "Request a narrower range (for example grep -n or sed -n) "
                                  "instead of repeating the same command unchanged.]")
                       .arg(result.totalBytes)
                       .arg(result.fullOutputPath);
    } else {
        preview += QStringLiteral("\n\n[Output truncated: %1 bytes total. Request a narrower "
                                  "range; do not repeat the same command unchanged.]")
                       .arg(result.totalBytes);
    }
    return preview;
}

void AgentLoop::preparePlanningContext(const AgentLoopState& state) {
    AgentContextInput input;
    input.goal = state.goal;
    input.responseProfileInstructions = responseProfileInstructions_;
    input.workspace = workingDirectory();
    input.workspaceContext = workspaceContext_;
    input.skills = skills_;
    input.steps = state.steps;
    input.evidence = state.evidence;
    input.intent = state.observationIntent;
    input.facts = ClaimGroundingResolver::facts(state.observationIntent, state.evidence);
    input.contextWindowTokens = contextWindowTokens_;
    input.maxOutputTokens = maxOutputTokens_;
    input.memoryStore = memoryStore_;
    input.memoryAccess.sessionId = state.sessionId;
    input.memoryAccess.workspaceId = workspaceContext_.id;
    if (!workspaceContext_.id.isEmpty())
        input.memoryAccess.authorizedWorkspaceIds.insert(workspaceContext_.id);
    input.chatHistoryStore = chatHistoryStore_;
    if (toolRegistry_) {
        for (const auto& tool : toolRegistry_->enabledTools())
            if (knownToolIds_.contains(tool.id))
                input.tools.append(tool);
    }
    const auto context = contextEngine_.build(input);
    if (contextCallback_)
        contextCallback_(static_cast<int>(state.steps.size()) + 1, context);
    planner_.setPlanningContext(context);
}

namespace {

// A GiveUp decision carries a typed stop reason. Prefer it over the generic
// fallback so a context-budget refusal or an indeterminate observation is
// reported as itself rather than as an ordinary inability to complete.
AgentTerminalReason giveUpTerminalReason(const AgentStepDecision& decision) {
    if (decision.failure != AgentStepFailure::None)
        return agentTerminalReasonFor(decision.failure);
    return decision.providerFailure ? AgentTerminalReason::ProviderFailure
                                    : AgentTerminalReason::UnableToComplete;
}

// Turns a typed classification failure into a message the user can act on. No
// step ever ran in this state, so the message says so instead of implying work
// was done and lost.
QString classificationAbortMessage(const ObservationIntent& intent) {
    const QString detail = intent.error.trimmed().isEmpty()
                               ? QString{}
                               : QStringLiteral(" %1").arg(intent.error.trimmed());
    switch (intent.failure) {
    case ObservationClassificationFailure::OutputTruncated:
        return QStringLiteral(
                   "Observation classification failed: the model reached its output limit before "
                   "a complete classification result existed, so no step was planned or executed. "
                   "Raise the model's output limit or disable reasoning for this model, then "
                   "retry.") +
               detail;
    case ObservationClassificationFailure::InvalidJson:
    case ObservationClassificationFailure::ContractViolation:
        return QStringLiteral(
                   "Observation classification failed: the model did not return a valid "
                   "classification result, so no step was planned or executed. Invalid output is "
                   "never treated as a decision. Retry, or select a model that can return "
                   "structured output.") +
               detail;
    case ObservationClassificationFailure::BudgetRefused:
        return QStringLiteral(
                   "Observation classification failed: the classification request does not fit the "
                   "effective context window, so no step was planned or executed. Reduce the task "
                   "size or select a model with a larger context window.") +
               detail;
    case ObservationClassificationFailure::Cancelled:
        return QStringLiteral("Observation classification was cancelled before it completed.") +
               detail;
    case ObservationClassificationFailure::ProviderFailure:
    case ObservationClassificationFailure::None:
        break;
    }
    return QStringLiteral(
               "Observation classification failed; retry after checking the provider. No step "
               "was planned or executed.") +
           detail;
}

// The exact predicate `acceptFinalAnswer` enforces for a mutation receipt: a
// later evidence record that reports the resulting state of the same resource.
// Keeping one implementation means the read-back recovery aims at precisely the
// evidence that is missing rather than at an approximation of it.
bool mutationConfirmedBy(const AgentLoopState& state, const EvidenceRecord& operation) {
    const auto resource =
        normalizedObservationResource(operation.resource, ObservationDomain::FileSystem);
    // Only the last approved mutation determines this resource's final revision.
    // An earlier receipt is not a second requirement to restore an obsolete value.
    if (std::any_of(state.evidence.cbegin(), state.evidence.cend(), [&](const auto& later) {
            return later.stepIndex > operation.stepIndex &&
                   later.scope == EvidenceScope::Operation &&
                   later.outcome == EvidenceOutcome::Verified &&
                   normalizedObservationResource(later.resource, ObservationDomain::FileSystem) ==
                       resource;
        }))
        return true;
    return std::any_of(
        state.evidence.cbegin(), state.evidence.cend(), [&](const EvidenceRecord& observation) {
            if (observation.stepIndex <= operation.stepIndex ||
                observation.scope == EvidenceScope::Operation ||
                (observation.domain != ObservationDomain::FileSystem &&
                 observation.domain != ObservationDomain::Workspace) ||
                (!operation.resource.isEmpty() &&
                 normalizedObservationResource(observation.resource,
                                               ObservationDomain::FileSystem) != resource))
                return false;
            if (observation.outcome == EvidenceOutcome::Failed) {
                // Absence is a verified deletion outcome, never proof that a write succeeded.
                return (operation.toolId == QLatin1String("delete-file") ||
                        operation.toolId == QLatin1String("move-file")) &&
                       observation.structuredObservation &&
                       observation.structuredObservation->kind ==
                           StructuredObservationKind::FileSystemFailure &&
                       observation.structuredObservation->fileSystemFailure ==
                           FileSystemFailure::NotFound;
            }
            if (observation.outcome != EvidenceOutcome::Verified)
                return false;
            if (operation.toolId == QLatin1String("write-file") ||
                operation.toolId == QLatin1String("edit-file")) {
                if (!observation.structuredObservation ||
                    observation.structuredObservation->kind !=
                        StructuredObservationKind::FileContent)
                    return false; // Presence alone cannot verify a file write.
                const auto& data = observation.structuredObservation->data;
                if (!data.value("complete").toBool() || data.value("truncated").toBool() ||
                    data.value("cancelled").toBool())
                    return false;
                if (operation.toolId == QLatin1String("write-file")) {
                    const auto step = std::find_if(
                        state.steps.cbegin(), state.steps.cend(),
                        [&](const auto& step) { return step.index == operation.stepIndex; });
                    if (step == state.steps.cend())
                        return false;
                    const auto content = std::find_if(
                        step->arguments.cbegin(), step->arguments.cend(), [](const auto& argument) {
                            return argument.id == QLatin1String("content");
                        });
                    if (content == step->arguments.cend() ||
                        data.value("content").toString() != content->value)
                        return false;
                }
            }
            return true;
        });
}

// A mutation records an Operation receipt: an event that already happened, not
// an observation of the state it produced. A verified claim therefore needs a
// later observation of the affected resource. When the planner cannot choose
// that observation itself, dispatch exactly one safe, read-only read-back of the
// mutated resource. The mutation is never executed again on this path: it only
// reads, and the resulting call still passes plan validation, approval,
// permission and sandbox like any other plan.
std::optional<AgentStepDecision> postMutationReadBack(AgentLoopState& state,
                                                      const QString& workspace,
                                                      const QStringList& knownTools,
                                                      const IToolRegistry* registry) {
    const auto target =
        std::find_if(state.evidence.cbegin(), state.evidence.cend(), [&](const auto& item) {
            if (item.scope != EvidenceScope::Operation ||
                (item.domain != ObservationDomain::FileSystem &&
                 item.domain != ObservationDomain::Workspace) ||
                item.outcome != EvidenceOutcome::Verified)
                return false;
            const auto resource =
                normalizedObservationResource(item.resource, ObservationDomain::FileSystem);
            // Never read back a mutation twice, and never one that is already
            // confirmed by a later observation.
            return !state.mutationReadBacks.contains(resource) && !mutationConfirmedBy(state, item);
        });
    if (target == state.evidence.cend())
        return std::nullopt;

    const bool bareWorkspaceTarget = target->resource.trimmed().isEmpty();
    const auto path = bareWorkspaceTarget
                          ? workspace
                          : ExternalDirectoryGate{}.resolvePath(target->resource, workspace);
    if (path.isEmpty())
        return std::nullopt;

    // Choose the narrowest safe read-only observation that can report the
    // resulting state. A directory is listed; everything else, including a
    // resource that no longer exists, is read so a typed absence is observed
    // rather than assumed.
    const QString toolId = (!bareWorkspaceTarget && QFileInfo(path).isDir())
                               ? QStringLiteral("list-directory")
                               : QStringLiteral("read-file");
    if (!knownTools.contains(toolId))
        return std::nullopt;
    const auto tool = registry->findToolById(toolId);
    if (!tool || tool->source != ToolSource::BuiltIn || !tool->enabled || !tool->exposedToModel ||
        !toolSupportedOnCurrentPlatform(*tool)) {
        return std::nullopt;
    }

    state.mutationReadBacks.append(
        normalizedObservationResource(target->resource, ObservationDomain::FileSystem));
    AgentStepDecision decision;
    decision.kind = AgentStepDecision::Kind::ToolCall;
    decision.toolId = tool->id;
    decision.toolName = tool->name;
    decision.riskLevel = tool->riskLevel;
    decision.executionMode = tool->executionMode;
    decision.arguments = {{QStringLiteral("path"), path}};
    decision.thought = QStringLiteral(
        "Read back the resource the mutation changed so its resulting state is observed. "
        "This is a read-only verification of an already executed change; it does not repeat "
        "the change.");
    ++state.finalVerificationRecoveries;
    return decision;
}

std::optional<AgentStepDecision> finalVerificationRecovery(AgentLoopState& state,
                                                           const QString& workspace,
                                                           const QStringList& knownTools,
                                                           const IToolRegistry* registry) {
    if (!registry || state.rejectedFinalAnswers != 1 || state.finalVerificationRecoveries >= 3 ||
        !state.pendingApprovalPlan.invocations.isEmpty()) {
        return std::nullopt;
    }
    // Do not begin a partial repair when the required checklist already exceeds
    // the remaining recovery budget, including a possible mutation read-back.
    int missingRequirements = 0;
    for (const auto& requirement : state.observationIntent.requirements) {
        ObservationIntent single;
        single.requirements = {requirement};
        if (!EvidencePolicy::evaluate(single, state.evidence, GroundingMode::Verified, true)
                 .accepted)
            ++missingRequirements;
    }
    if (missingRequirements > 3 - state.finalVerificationRecoveries)
        return std::nullopt;
    // A rejected verified final over an unconfirmed mutation is the common case
    // for a plain file task: there is no shell command to satisfy a `tested`
    // precondition and no inspect requirement to drive a checklist. Observe the
    // mutation's result first, then fall through to the requirement checklist.
    if (auto readBack = postMutationReadBack(state, workspace, knownTools, registry))
        return readBack;
    const bool wrote =
        std::any_of(state.evidence.cbegin(), state.evidence.cend(), [](const auto& item) {
            return item.scope == EvidenceScope::Operation &&
                   item.outcome == EvidenceOutcome::Verified &&
                   (item.domain == ObservationDomain::FileSystem ||
                    item.domain == ObservationDomain::Workspace);
        });
    const bool tested =
        std::any_of(state.evidence.cbegin(), state.evidence.cend(), [](const auto& item) {
            return item.toolId == QLatin1String("run-command") &&
                   item.domain == ObservationDomain::ProcessExecution &&
                   item.outcome == EvidenceOutcome::Verified && item.structuredObservation &&
                   item.structuredObservation->data.value(QStringLiteral("exitCode")).toInt(-1) ==
                       0;
        });
    if (!wrote || !tested) {
        return std::nullopt;
    }
    QList<ObservationRequirement> pending;
    for (const auto& requirement : state.observationIntent.requirements) {
        ObservationIntent single;
        single.requirements = {requirement};
        if (EvidencePolicy::evaluate(single, state.evidence, GroundingMode::Verified, true)
                .accepted) {
            continue;
        }
        if (requirement.purpose != ObservationPurpose::Inspect ||
            (requirement.toolId != QLatin1String("read-file") &&
             requirement.toolId != QLatin1String("git-diff") &&
             requirement.toolId != QLatin1String("git-status"))) {
            return std::nullopt;
        }
        pending.append(requirement);
    }
    if (pending.isEmpty() || pending.size() > 3 - state.finalVerificationRecoveries) {
        return std::nullopt;
    }
    // Vet the complete remaining read-only checklist before dispatching any of it.
    for (const auto& requirement : pending) {
        if (!knownTools.contains(requirement.toolId) || requirement.resourceHint.isEmpty()) {
            return std::nullopt;
        }
        const auto path = ExternalDirectoryGate{}.resolvePath(requirement.resourceHint, workspace);
        if (!PathGuard::contains(workspace, path)) {
            return std::nullopt;
        }
        const auto tool = registry->findToolById(requirement.toolId);
        if (!tool || tool->source != ToolSource::BuiltIn || !tool->enabled ||
            !tool->exposedToModel || !toolSupportedOnCurrentPlatform(*tool)) {
            return std::nullopt;
        }
        if (std::any_of(state.steps.cbegin(), state.steps.cend(), [&](const auto& step) {
                return step.toolId == requirement.toolId && !step.succeeded;
            })) {
            return std::nullopt;
        }
    }
    const auto& missing = pending.first();
    const auto path = ExternalDirectoryGate{}.resolvePath(missing.resourceHint, workspace);
    const auto tool = registry->findToolById(missing.toolId);
    AgentStepDecision decision;
    decision.kind = AgentStepDecision::Kind::ToolCall;
    decision.toolId = tool->id;
    decision.toolName = tool->name;
    decision.riskLevel = tool->riskLevel;
    decision.executionMode = tool->executionMode;
    decision.arguments = {{QStringLiteral("path"), path}};
    decision.thought =
        QStringLiteral("Observe the explicitly requested remaining read-only verification.");
    ++state.finalVerificationRecoveries;
    return decision;
}

QString decisionActionKey(const AgentStepDecision& decision) {
    auto argumentsKey = [](const QList<ToolInvocationArgument>& arguments) {
        QStringList parts;
        for (const auto& argument : arguments)
            parts.append(QStringLiteral("%1=%2").arg(argument.id, argument.value));
        parts.sort();
        return parts.join(QLatin1Char(';'));
    };
    QString key = decision.toolId + QLatin1Char('|') + argumentsKey(decision.arguments);
    for (const auto& call : decision.toolBatch) {
        key += QLatin1Char('|') + call.toolId + QLatin1Char(';') + argumentsKey(call.arguments);
    }
    return key;
}

QString normalizedActionKey(const QString& id, QList<ToolInvocationArgument> arguments,
                            const QString& workspace, const IToolRegistry* registry) {
    for (auto& argument : arguments) {
        if (argument.id == QLatin1String("path") || argument.id == QLatin1String("source") ||
            argument.id == QLatin1String("destination")) {
            argument.value = ExternalDirectoryGate{}.resolvePath(argument.value, workspace);
        }
    }
    std::sort(arguments.begin(), arguments.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    QJsonObject values;
    for (const auto& argument : arguments) {
        values.insert(argument.id, argument.value);
    }
    if (registry) {
        const auto descriptor = registry->findToolById(id);
        if (descriptor) {
            const auto properties =
                descriptor->inputSchema.value(QStringLiteral("properties")).toObject();
            for (auto it = properties.begin(); it != properties.end(); ++it) {
                const auto field = it.value().toObject();
                if (!values.contains(it.key()) && field.contains(QStringLiteral("default"))) {
                    values.insert(it.key(),
                                  field.value(QStringLiteral("default")).toVariant().toString());
                }
            }
        }
    }
    return id + QLatin1Char('|') +
           QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact));
}

bool repeatsUnchangedStep(const AgentLoopState& state, const ToolInvocationPlan& plan,
                          const QString& workspace, const IToolRegistry* registry) {
    if (plan.invocations.size() != 1 || state.steps.isEmpty())
        return false;
    const auto& call = plan.invocations.first();
    const auto requested = normalizedActionKey(call.toolId, call.arguments, workspace, registry);
    for (auto it = state.steps.crbegin(); it != state.steps.crend(); ++it) {
        if (normalizedActionKey(it->toolId, it->arguments, workspace, registry) != requested) {
            continue;
        }
        // The immediately previous identical call made no intervening progress.
        if (it == state.steps.crbegin()) {
            return true;
        }
        // An identical successful mutation followed by its independent read-back
        // is not productive work. Ask for a final decision without replaying it.
        if (it->succeeded &&
            std::any_of(state.evidence.cbegin(), state.evidence.cend(), [&](const auto& item) {
                return item.stepIndex == it->index && item.scope == EvidenceScope::Operation &&
                       item.outcome == EvidenceOutcome::Verified &&
                       mutationConfirmedBy(state, item) &&
                       !std::any_of(
                           state.evidence.cbegin(), state.evidence.cend(), [&](const auto& later) {
                               return later.stepIndex > item.stepIndex &&
                                      later.scope == EvidenceScope::Operation &&
                                      normalizedObservationResource(
                                          later.resource, ObservationDomain::FileSystem) ==
                                          normalizedObservationResource(
                                              item.resource, ObservationDomain::FileSystem);
                           });
            }))
            return true;
        // A stale read is legitimately repeatable after mutation. A still-current
        // successful observation/command must not be executed again just to finish.
        if (it->succeeded &&
            std::any_of(state.evidence.cbegin(), state.evidence.cend(), [&](const auto& item) {
                return item.stepIndex == it->index &&
                       (item.scope != EvidenceScope::Operation ||
                        item.domain == ObservationDomain::ProcessExecution) &&
                       item.outcome == EvidenceOutcome::Verified;
            })) {
            return true;
        }
    }
    return false;
}

bool repeatsFailedCycle(const AgentLoopState& state, const ToolInvocationPlan& plan) {
    if (plan.invocations.size() != 1 || state.steps.size() < 4)
        return false;
    auto key = [](const QString& toolId, const QList<ToolInvocationArgument>& arguments) {
        QStringList parts;
        for (const auto& argument : arguments)
            parts.append(argument.id + QLatin1Char('=') + argument.value);
        parts.sort();
        return toolId + QLatin1Char('|') + parts.join(QLatin1Char(';'));
    };
    const int end = state.steps.size();
    for (int i = end - 4; i < end; ++i)
        if (state.steps.at(i).succeeded)
            return false;
    const auto first = key(state.steps.at(end - 4).toolId, state.steps.at(end - 4).arguments);
    const auto second = key(state.steps.at(end - 3).toolId, state.steps.at(end - 3).arguments);
    return first != second &&
           first == key(state.steps.at(end - 2).toolId, state.steps.at(end - 2).arguments) &&
           second == key(state.steps.at(end - 1).toolId, state.steps.at(end - 1).arguments) &&
           first == key(plan.invocations.first().toolId, plan.invocations.first().arguments);
}

void stopRepeatedCall(AgentLoopState& state) {
    state.phase = AgentLoopPhase::Stuck;
    const auto& last = state.steps.last();
    if (!last.succeeded && last.failureCategory == ToolFailureCategory::SecurityDenied) {
        state.terminalReason = AgentTerminalReason::SecurityDenied;
        state.abortReason =
            QStringLiteral(
                "Repeated tool call was not executed: the previous %1 attempt was blocked by "
                "security policy%2. Approval does not override sandbox restrictions.")
                .arg(last.toolId, last.sandbox.failureCategory.isEmpty()
                                      ? QString{}
                                      : QStringLiteral(" (%1)").arg(last.sandbox.failureCategory));
        return;
    }
    state.terminalReason = last.succeeded ? AgentTerminalReason::CompletionIndeterminate
                                          : AgentTerminalReason::ToolFailure;
    state.abortReason = QStringLiteral("Agent repeated the same tool call without a "
                                       "changed argument or intervening observation.");
}

bool repairRepeatedObservation(AgentLoopState& state, const ToolInvocationPlan& plan,
                               IAgentStepPlanner& planner) {
    if (plan.invocations.size() != 1 || state.steps.isEmpty())
        return false;
    if (!state.steps.last().succeeded) {
        // One bounded repair is granted only when the previous attempt actually
        // executed and returned a result the planner can act on: NotFound is a
        // definitive observation (the target does not exist) and Timeout is a
        // transient failure. The identical call is never executed again, and a
        // second repeat still stops fail-closed. Invalid, unknown, denied or
        // capability-unavailable blocks keep their immediate fail-closed stop:
        // they can never gain an observation through repair, and a final answer
        // must not paper over them.
        const auto& last = state.steps.last();
        const bool actionableFailure = last.statusText == QLatin1String("Failed") &&
                                       (last.failureCategory == ToolFailureCategory::NotFound ||
                                        last.failureCategory == ToolFailureCategory::Timeout);
        if (!actionableFailure || state.repeatedFailureRepairs != 0)
            return false;
        ++state.repeatedFailureRepairs;
        QString semantics =
            last.failureCategory == ToolFailureCategory::NotFound
                ? QStringLiteral(
                      "A NotFound result is definitive: that target does not currently exist. ")
                : QStringLiteral("A Timeout may resolve on a different approach. ");
        planner.setPlannerFeedback(
            QStringLiteral("The identical %1 call was not executed again because its previous "
                           "attempt already returned %2. %3Do not repeat the failed call: choose "
                           "a different action with changed arguments, report the blocker, or "
                           "provide a final answer grounded in the observations already "
                           "collected.")
                .arg(last.toolId, last.statusText, semantics));
        return true;
    }
    const auto& id = plan.invocations.first().toolId;
    const bool read = id == QLatin1String("read-file") || id == QLatin1String("glob") ||
                      id == QLatin1String("grep") || id == QLatin1String("list-directory");
    QString command;
    for (const auto& argument : plan.invocations.first().arguments) {
        if (argument.id == QLatin1String("command")) {
            command = argument.value;
        }
    }
    const bool verifiedCommand =
        id == QLatin1String("run-command") &&
        std::any_of(state.evidence.cbegin(), state.evidence.cend(), [&](const auto& item) {
            return item.toolId == id && item.domain == ObservationDomain::ProcessExecution &&
                   item.resource == command && item.outcome == EvidenceOutcome::Verified;
        });
    if (read || verifiedCommand) {
        if (state.repeatedReadRepairs != 0)
            return false;
        ++state.repeatedReadRepairs;
        // Nothing is executed or assumed successful here. One planning-only repair
        // preserves the existing evidence/approval gates and the next repeat stops.
        planner.setPlannerFeedback(QStringLiteral(
            "Repeated observation or already successful command was not executed. Do not rerun a "
            "successful test for the same file revision. Review the PENDING requirements and all "
            "current tool observations, not only "
            "the latest result. If they satisfy the task, provide a verified final with the "
            "required structured claims. Otherwise choose a different missing observation or "
            "report the blocker. Do not repeat equivalent reads using another spelling of the "
            "same path."));
        return true;
    }
    // A repeated non-observation action after a successful step (for example a
    // mutation) gets one bounded repair below; the next repeat stops fail-closed.
    if (state.repeatedActionRepairs != 0)
        return false;
    ++state.repeatedActionRepairs;
    state.repeatedActionRecoveryPending = true;
    // Mutations and other non-observation actions also get one bounded repair.
    // The repeat itself is never executed, so no side effect is skipped silently.
    planner.setPlannerFeedback(QStringLiteral(
        "The repeated action was not executed because it was identical to the previous action. "
        "Inspect the previous result and choose a different action with changed arguments. A "
        "final answer is not accepted until the repeated mutation is resolved."));
    return true;
}

void fillRecordFromPlan(AgentStepRecord& record, const ToolInvocationPlan& plan) {
    if (plan.invocations.isEmpty()) {
        return;
    }
    const auto& invocation = plan.invocations.first();
    record.toolId = invocation.toolId;
    record.toolName = invocation.toolName;
    record.arguments = invocation.arguments;
}

StructuredObservationPtr resourceFailureObservation(const ResourceAuthorizationResult& result) {
    auto observation = std::make_shared<StructuredObservation>();
    observation->kind = StructuredObservationKind::FileSystemFailure;
    observation->fileSystemFailure = result.failure;
    observation->failureResource = result.resource;
    observation->data.insert(QStringLiteral("resource"), result.resource);
    return observation;
}

bool securityFailure(FileSystemFailure failure) {
    return failure == FileSystemFailure::PermissionDenied ||
           failure == FileSystemFailure::ResourceChanged ||
           failure == FileSystemFailure::SymlinkEscape ||
           failure == FileSystemFailure::UnsafeParent ||
           failure == FileSystemFailure::SecurityBoundaryViolation;
}

} // namespace

QStringList AgentLoop::externalPathsRequiringApproval(const ToolInvocationPlan& plan,
                                                      const QString& sessionId) const {
    if (!externalDirectoryGate_ || plan.invocations.isEmpty())
        return {};
    QStringList paths;
    for (const auto& invocation : plan.invocations) {
        const auto registration = toolRegistry_ ? toolRegistry_->findRegistration(invocation.toolId)
                                                : std::optional<IToolRegistry::Registration>{};
        const auto* descriptor = invocation.descriptorSnapshot
                                     ? invocation.descriptorSnapshot.get()
                                     : (registration ? &registration->descriptor : nullptr);
        if (!descriptor)
            continue;
        for (const auto& request : AuthorizationResolver::resolve(
                 *descriptor, invocation, externalDirectoryGate_, workingDirectory())) {
            if (request.domain != SecurityDomain::FileSystem || request.resource.isEmpty())
                continue;
            if (permissionService_ && permissionService_->evaluateAuthorization(
                                          request, sessionId) == PermissionEffect::Allow)
                continue;
            if (externalDirectoryGate_->canRequestPermission(request.resource, workingDirectory()))
                paths.append(request.resource);
        }
    }
    paths.removeDuplicates();
    return paths;
}

void AgentLoop::grantExternalPaths(const ToolInvocationPlan& plan, const QString& sessionId) {
    for (const auto& invocation : plan.invocations) {
        const auto registration = toolRegistry_ ? toolRegistry_->findRegistration(invocation.toolId)
                                                : std::optional<IToolRegistry::Registration>{};
        const auto* descriptor = invocation.descriptorSnapshot
                                     ? invocation.descriptorSnapshot.get()
                                     : (registration ? &registration->descriptor : nullptr);
        if (!descriptor)
            continue;
        for (const auto& request : AuthorizationResolver::resolve(
                 *descriptor, invocation, externalDirectoryGate_, workingDirectory())) {
            if (permissionService_ && externalDirectoryGate_ &&
                request.domain == SecurityDomain::FileSystem && !request.resource.isEmpty() &&
                externalDirectoryGate_->canRequestPermission(request.resource, workingDirectory()))
                permissionService_->grantAuthorization(request, sessionId, false);
        }
    }
}

bool AgentLoop::hasAuthorizationGrants(const ToolInvocationPlan& plan,
                                       const QString& sessionId) const {
    if (!permissionService_ || !toolRegistry_ || plan.invocations.isEmpty())
        return false;
    bool found = false;
    for (const auto& invocation : plan.invocations) {
        const auto registration = toolRegistry_->findRegistration(invocation.toolId);
        const auto* descriptor = invocation.descriptorSnapshot
                                     ? invocation.descriptorSnapshot.get()
                                     : (registration ? &registration->descriptor : nullptr);
        if (!descriptor)
            return false;
        const auto requests = AuthorizationResolver::resolve(
            *descriptor, invocation, externalDirectoryGate_, workingDirectory());
        for (const auto& request : requests) {
            found = true;
            if (permissionService_->evaluateAuthorization(request, sessionId) !=
                PermissionEffect::Allow)
                return false;
        }
    }
    return found;
}

bool AgentLoop::hasAuthorizationDeny(const ToolInvocationPlan& plan,
                                     const QString& sessionId) const {
    if (!permissionService_ || !toolRegistry_)
        return false;
    for (const auto& invocation : plan.invocations) {
        const auto registration = toolRegistry_->findRegistration(invocation.toolId);
        const auto* descriptor = invocation.descriptorSnapshot
                                     ? invocation.descriptorSnapshot.get()
                                     : (registration ? &registration->descriptor : nullptr);
        if (!descriptor)
            return true;
        for (const auto& request : AuthorizationResolver::resolve(
                 *descriptor, invocation, externalDirectoryGate_, workingDirectory())) {
            if (permissionService_->evaluateAuthorization(request, sessionId) ==
                PermissionEffect::Deny)
                return true;
        }
    }
    return false;
}

QList<AuthorizationRequest>
AgentLoop::resolveAuthorizationRequests(const ToolInvocationPlan& plan) const {
    QList<AuthorizationRequest> requests;
    for (const auto& invocation : plan.invocations) {
        const auto registration = toolRegistry_ ? toolRegistry_->findRegistration(invocation.toolId)
                                                : std::optional<IToolRegistry::Registration>{};
        const auto* descriptor = invocation.descriptorSnapshot
                                     ? invocation.descriptorSnapshot.get()
                                     : (registration ? &registration->descriptor : nullptr);
        if (descriptor)
            requests.append(AuthorizationResolver::resolve(
                *descriptor, invocation, externalDirectoryGate_, workingDirectory()));
    }
    return requests;
}

ResourceAuthorizationResult AgentLoop::prepareResources(ToolInvocationPlan& plan) const {
    for (auto& invocation : plan.invocations) {
        if (!invocation.descriptorSnapshot)
            continue;
        auto resolved = ResourceAuthorizationResolver::resolve(
            *invocation.descriptorSnapshot, invocation, workingDirectory(), externalDirectoryGate_);
        if (!resolved.ok())
            return resolved;
        if (!resourceScope_.isEmpty()) {
            for (const auto& file : resolved.snapshot.files)
                if (!PathGuard::contains(resourceScope_, file.path.canonicalPath)) {
                    resolved.failure = FileSystemFailure::SecurityBoundaryViolation;
                    resolved.resource = file.path.canonicalPath;
                    resolved.reason =
                        QStringLiteral("Resource is outside this agent's assigned workspace.");
                    return resolved;
                }
        }
        invocation.resourceSnapshot =
            std::make_shared<const ResourceAuthorizationSnapshot>(std::move(resolved.snapshot));
    }
    return {};
}

ResourceAuthorizationResult AgentLoop::authorizeResources(ToolInvocationPlan& plan,
                                                          const QString& sessionId) const {
    for (auto& invocation : plan.invocations) {
        if (!invocation.resourceSnapshot)
            continue;
        auto result = ResourceAuthorizationResolver::authorize(
            *invocation.resourceSnapshot, externalDirectoryGate_, permissionService_, sessionId);
        if (!result.ok())
            return result;
        invocation.resourceSnapshot =
            std::make_shared<const ResourceAuthorizationSnapshot>(std::move(result.snapshot));
    }
    return {};
}

void AgentLoop::applyPermissionPolicy(const ToolInvocationPlan& plan, const QString& sessionId,
                                      ApprovalDecision& approval) const {
    if (!permissionPolicy_ || !toolRegistry_)
        return;
    for (const auto& invocation : plan.invocations) {
        const auto registration = toolRegistry_->findRegistration(invocation.toolId);
        const auto* descriptor = invocation.descriptorSnapshot
                                     ? invocation.descriptorSnapshot.get()
                                     : (registration ? &registration->descriptor : nullptr);
        if (!descriptor) {
            approval.status = ApprovalStatus::Denied;
            approval.summary = QStringLiteral("Tool authorization metadata is unavailable.");
            return;
        }
        const auto requests = AuthorizationResolver::resolve(
            *descriptor, invocation, externalDirectoryGate_, workingDirectory());
        for (const auto& request : requests) {
            const auto effect = permissionPolicy_->defaultEffect(request, defaultPermissionState_);
            if (effect == PermissionEffect::Deny) {
                approval.status = ApprovalStatus::Denied;
                approval.summary = QStringLiteral("Denied by the configured authorization policy.");
                return;
            }
            if (permissionService_) {
                const auto granted = permissionService_->evaluateAuthorization(request, sessionId);
                if (granted == PermissionEffect::Deny) {
                    approval.status = ApprovalStatus::Denied;
                    approval.summary =
                        QStringLiteral("Denied by an explicit authorization decision.");
                    return;
                }
                if (granted == PermissionEffect::Allow)
                    continue;
            }
            if (effect == PermissionEffect::Ask && approval.status != ApprovalStatus::Approved) {
                approval.status = ApprovalStatus::RequiresApproval;
                approval.summary =
                    QStringLiteral("External service access requires user approval.");
            }
        }
    }
}

AgentLoop::AgentLoop(IAgentStepPlanner& planner, const IToolExecutor& executor,
                     const IApprovalPolicy& approvalPolicy, const ISandboxPolicy& sandboxPolicy,
                     QStringList knownToolIds, Config config)
    : planner_(planner), executor_(executor), approvalPolicy_(approvalPolicy),
      sandboxPolicy_(sandboxPolicy), knownToolIds_(std::move(knownToolIds)), config_(config) {
    TruncationConfig truncationConfig;
    truncationConfig.enabled = true;
    truncationConfig.maxLines = 400;
    truncationConfig.maxBytes = config_.observationMaxBytes;
    truncationConfig.previewLines = config_.observationPreviewLines;
    truncator_ = ToolOutputTruncator(truncationConfig);
}

void AgentLoop::setStepCallback(StepCallback callback) {
    stepCallback_ = std::move(callback);
}

void AgentLoop::setStatusCallback(StatusCallback callback) {
    statusCallback_ = std::move(callback);
}

void AgentLoop::setCancelQuery(CancelQuery query) {
    cancelQuery_ = std::move(query);
}

void AgentLoop::setCancellationToken(std::shared_ptr<std::atomic_bool> token) {
    cancellationToken_ = std::move(token);
}

void AgentLoop::setToolCallback(ToolCallback callback) {
    toolCallback_ = std::move(callback);
}

void AgentLoop::setPlanningCallback(PlanningCallback callback) {
    planningCallback_ = std::move(callback);
}

void AgentLoop::setOutputCallback(OutputCallback callback) {
    outputCallback_ = std::move(callback);
}

void AgentLoop::setToolCallIdProvider(ToolCallIdProvider provider) {
    toolCallIdProvider_ = std::move(provider);
}

void AgentLoop::runAsync(const QString& goal, const QString& sessionId, QObject* context,
                         CompletionCallback completion) {
    asyncContext_ = context;
    completionCallback_ = std::move(completion);
    asyncState_ = {};
    asyncState_.sessionId = sessionId;
    asyncState_.goal = goal;
    asyncState_.phase = AgentLoopPhase::Running;
    asyncFinished_ = false;
    initializeObservationIntent(asyncState_);
    // Classification can enter the provider's event loop and process cancellation.
    // Never resurrect a run whose terminal callback has already been delivered.
    if (asyncFinished_)
        return;
    if (statusCallback_)
        statusCallback_(QStringLiteral("Agent loop running for goal: %1").arg(goal));
    if (asyncState_.observationIntent.indeterminate) {
        asyncState_.phase =
            cancellationRequested() ? AgentLoopPhase::Cancelled : AgentLoopPhase::Failed;
        asyncState_.terminalReason = cancellationRequested()
                                         ? AgentTerminalReason::Cancelled
                                         : AgentTerminalReason::ObservationIndeterminate;
        asyncState_.abortReason = classificationAbortMessage(asyncState_.observationIntent);
        if (asyncState_.observationIntent.providerFailure)
            asyncState_.providerFailure = *asyncState_.observationIntent.providerFailure;
        completeAsync();
        return;
    }
    scheduleAsyncAdvance();
}

void AgentLoop::resumeAsync(AgentLoopState state, bool approved, QObject* context,
                            CompletionCallback completion) {
    asyncContext_ = context;
    completionCallback_ = std::move(completion);
    asyncState_ = std::move(state);
    planner_.setObservationIntent(asyncState_.observationIntent);
    asyncState_.phase = AgentLoopPhase::Running;
    asyncFinished_ = false;
    if (statusCallback_)
        statusCallback_(QStringLiteral("Agent loop running for goal: %1").arg(asyncState_.goal));
    const auto plan = asyncState_.pendingApprovalPlan;
    const auto thought = asyncState_.pendingApprovalThought;
    asyncState_.pendingApprovalPlan = {};
    asyncState_.pendingApprovalThought.clear();
    asyncState_.pendingAuthorizationRequests.clear();
    if (approved) {
        grantExternalPaths(plan, asyncState_.sessionId);
        executeStepAsync(
            plan, thought,
            {ApprovalStatus::Approved, QStringLiteral("User approved execution in chat."), {}});
    } else {
        appendBlockedStep(asyncState_, plan, thought, QStringLiteral("Denied"),
                          QStringLiteral("User denied execution in chat."));
        asyncState_.phase = AgentLoopPhase::Failed;
        asyncState_.terminalReason = AgentTerminalReason::ApprovalDenied;
        asyncState_.abortReason = QStringLiteral("User denied the required action.");
        completeAsync();
    }
}

void AgentLoop::cancelAsync() {
    cancelled_ = true;
    if (cancellationToken_)
        cancellationToken_->store(true);
    if (asyncFinished_)
        return;
    if (scheduler_)
        scheduler_->cancel();
    if (waitingForTool_ && cancelTool_) {
        // The completion path clears cancelTool_ from inside the functor, so
        // run it from local storage instead of the member it can invalidate.
        auto cancelTool = std::move(cancelTool_);
        cancelTool_ = {};
        cancelTool();
        asyncState_.phase = AgentLoopPhase::Cancelled;
        asyncState_.abortReason = QStringLiteral("Agent run cancelled by user.");
        completeAsync();
        return;
    }
    asyncState_.phase = AgentLoopPhase::Cancelled;
    asyncState_.abortReason = QStringLiteral("Agent run cancelled by user.");
    completeAsync();
}

void AgentLoop::scheduleAsyncAdvance() {
    QTimer::singleShot(0, asyncContext_, [this] {
        if (!asyncFinished_)
            advanceAsync();
    });
}

void AgentLoop::completeAsync() {
    if (asyncFinished_)
        return;
    if (asyncState_.phase == AgentLoopPhase::Cancelled)
        asyncState_.terminalReason = AgentTerminalReason::Cancelled;
    asyncFinished_ = true;
    waitingForTool_ = false;
    cancelTool_ = {};
    scheduler_.reset();
    if (completionCallback_)
        completionCallback_(asyncState_);
}

void AgentLoop::advanceAsync() {
    if (cancellationRequested()) {
        asyncState_.phase = AgentLoopPhase::Cancelled;
        asyncState_.abortReason = QStringLiteral("Agent run cancelled by user.");
        completeAsync();
        return;
    }
    const int productiveSteps =
        qMax(0, static_cast<int>(asyncState_.steps.size()) - asyncState_.blockedSteps);
    if (asyncState_.blockedSteps >= config_.maxBlockedSteps) {
        asyncState_.phase = AgentLoopPhase::Failed;
        asyncState_.terminalReason = AgentTerminalReason::Blocked;
        asyncState_.abortReason =
            QStringLiteral("Too many blocked tool requests (%1). Review the tool contract and "
                           "choose a permitted action.")
                .arg(asyncState_.blockedSteps);
        completeAsync();
        return;
    }
    if (asyncState_.consecutiveBlockedSteps >= config_.maxConsecutiveBlockedSteps) {
        asyncState_.phase = AgentLoopPhase::Failed;
        asyncState_.terminalReason = AgentTerminalReason::Blocked;
        asyncState_.abortReason =
            QStringLiteral("Too many consecutive blocked tool requests (%1). The planner did not "
                           "recover after the previous rejection; inspect the last observation and "
                           "choose a different permitted action.")
                .arg(asyncState_.consecutiveBlockedSteps);
        completeAsync();
        return;
    }

    const int stepIndex = asyncState_.steps.size() + 1;
    if (planningCallback_)
        planningCallback_(stepIndex, true);
    AgentStepDecision decision;
    try {
        planner_.setObservationIntent(asyncState_.observationIntent);
        planner_.setStructuredFacts(
            ClaimGroundingResolver::facts(asyncState_.observationIntent, asyncState_.evidence));
        preparePlanningContext(asyncState_);
        planner_.setEvidence(asyncState_.evidence);
        traceStage(asyncState_, "planning", "enter");
        if (const auto recovery = finalVerificationRecovery(asyncState_, workingDirectory(),
                                                            knownToolIds_, toolRegistry_)) {
            decision = *recovery;
            if (statusCallback_) {
                statusCallback_(QStringLiteral("Planning requested final verification."));
            }
            traceStage(asyncState_, "verification_recovery", "selected", &decision,
                       workingDirectory());
        } else {
            decision = planner_.nextStep(asyncState_.goal, asyncState_.steps);
            asyncState_.providerRecoveryAttempts += planner_.lastProviderRecoveryAttempts();
        }
        traceStage(asyncState_, "planning", "exit", &decision, workingDirectory());
    } catch (...) {
        if (planningCallback_)
            planningCallback_(stepIndex, false);
        asyncState_.phase = AgentLoopPhase::Failed;
        asyncState_.abortReason = QStringLiteral("Agent execution failed.");
        completeAsync();
        return;
    }
    if (planningCallback_)
        planningCallback_(stepIndex, false);
    if (cancellationRequested()) {
        asyncState_.phase = AgentLoopPhase::Cancelled;
        asyncState_.abortReason = QStringLiteral("Agent run cancelled by user.");
        completeAsync();
        return;
    }
    if (decision.kind == AgentStepDecision::Kind::FinalAnswer) {
        if (acceptFinalAnswer(asyncState_, decision))
            completeAsync();
        else
            scheduleAsyncAdvance();
        return;
    }
    if (decision.kind == AgentStepDecision::Kind::GiveUp) {
        asyncState_.providerFailure = decision.providerFailure;
        asyncState_.phase = AgentLoopPhase::Failed;
        asyncState_.terminalReason = giveUpTerminalReason(decision);
        asyncState_.abortReason =
            decision.reason.trimmed().isEmpty() ? decision.thought : decision.reason;
        completeAsync();
        return;
    }
    if (productiveSteps >= config_.maxIterations) {
        asyncState_.phase = AgentLoopPhase::Failed;
        asyncState_.terminalReason = AgentTerminalReason::IterationLimit;
        asyncState_.abortReason = QStringLiteral("Iteration limit reached (%1 productive steps).")
                                      .arg(config_.maxIterations);
        completeAsync();
        return;
    }

    auto plan = planFromDecision(decision);
    if (std::any_of(plan.invocations.cbegin(), plan.invocations.cend(),
                    [this](const auto& call) { return !knownToolIds_.contains(call.toolId); })) {
        if (repeatsUnchangedStep(asyncState_, plan, workingDirectory(), toolRegistry_)) {
            asyncState_.phase = AgentLoopPhase::Failed;
            asyncState_.terminalReason = AgentTerminalReason::CapabilityUnavailable;
            asyncState_.abortReason = QStringLiteral("Required tool is unavailable.");
            completeAsync();
            return;
        }
        appendBlockedStep(asyncState_, plan, decision.thought, QStringLiteral("Unknown Tool"),
                          QStringLiteral("Unknown tool requested: %1").arg(decision.toolId));
        scheduleAsyncAdvance();
        return;
    }
    if (repeatsUnchangedStep(asyncState_, plan, workingDirectory(), toolRegistry_) ||
        repeatsFailedCycle(asyncState_, plan)) {
        if (repairRepeatedObservation(asyncState_, plan, planner_)) {
            scheduleAsyncAdvance();
            return;
        }
        stopRepeatedCall(asyncState_);
        completeAsync();
        return;
    }
    doomDetector_.recordAction(asyncState_.sessionId, decisionActionKey(decision));
    if (doomDetector_.isStuck(asyncState_.sessionId)) {
        if (repairRepeatedObservation(asyncState_, plan, planner_)) {
            scheduleAsyncAdvance();
            return;
        }
        asyncState_.phase = AgentLoopPhase::Stuck;
        asyncState_.abortReason = QStringLiteral(
            "Doom loop detected: the agent repeated the same action without progress.");
        completeAsync();
        return;
    }
    if (toolCallback_)
        for (int i = 0; i < plan.invocations.size(); ++i) {
            auto one = plan;
            one.invocations = {plan.invocations.at(i)};
            toolCallback_(ToolTransition::Requested, stepIndex + i, one, nullptr);
        }
    const auto validation = gateway_.validatePlan(plan);
    if (validation.status != ToolExecutionStatus::Succeeded) {
        appendBlockedStep(asyncState_, plan, decision.thought,
                          toolExecutionStatusName(validation.status), validation.summary);
        if (toolCallback_)
            toolCallback_(ToolTransition::ExecutionFinished, stepIndex, plan,
                          &asyncState_.steps.last());
        scheduleAsyncAdvance();
        return;
    }
    const auto resources = prepareResources(plan);
    if (!resources.ok()) {
        appendBlockedStep(asyncState_, plan, decision.thought,
                          securityFailure(resources.failure) ? QStringLiteral("Denied")
                                                             : QStringLiteral("Invalid Arguments"),
                          resources.reason, resourceFailureObservation(resources));
        if (toolCallback_)
            toolCallback_(ToolTransition::ExecutionFinished, stepIndex, plan,
                          &asyncState_.steps.last());
        scheduleAsyncAdvance();
        return;
    }
    if (externalDirectoryGate_)
        externalDirectoryGate_->setSecuritySessionId(asyncState_.sessionId);
    ApprovalDecision approval;
    if (hasAuthorizationDeny(plan, asyncState_.sessionId))
        approval = {
            ApprovalStatus::Denied, QStringLiteral("Denied by an explicit permission grant."), {}};
    else if (config_.autonomousMode)
        approval = {ApprovalStatus::Approved,
                    QStringLiteral("Autonomous Mode is enabled: per-step approval is bypassed."),
                    {}};
    else {
        approval = hasAuthorizationGrants(plan, asyncState_.sessionId)
                       ? ApprovalDecision{ApprovalStatus::Approved,
                                          QStringLiteral("An explicit semantic permission grant "
                                                         "covers this invocation."),
                                          {}}
                       : approvalPolicy_.evaluate(plan);
    }
    const auto externalPaths = externalPathsRequiringApproval(plan, asyncState_.sessionId);
    applyPermissionPolicy(plan, asyncState_.sessionId, approval);
    if (!externalPaths.isEmpty() && approval.status != ApprovalStatus::Denied) {
        const auto toolName =
            plan.invocations.isEmpty() ? QStringLiteral("tool") : plan.invocations.first().toolName;
        approval.status = ApprovalStatus::RequiresApproval;
        const auto displayed = externalPaths.mid(0, 3).join(QStringLiteral(", "));
        approval.summary =
            QStringLiteral("Allow %1 to access %2%3?")
                .arg(toolName, displayed,
                     externalPaths.size() > 3
                         ? QStringLiteral(" and %1 more file(s)").arg(externalPaths.size() - 3)
                         : QString{});
    }
    if (approval.status == ApprovalStatus::RequiresApproval) {
        if (toolCallback_)
            toolCallback_(ToolTransition::ApprovalRequired, stepIndex, plan, nullptr);
        asyncState_.pendingApprovalPlan = plan;
        asyncState_.pendingApprovalThought = decision.thought;
        asyncState_.pendingAuthorizationRequests = resolveAuthorizationRequests(plan);
        asyncState_.phase = AgentLoopPhase::AwaitingApproval;
        completeAsync();
        return;
    }
    if (approval.status == ApprovalStatus::Denied) {
        appendBlockedStep(asyncState_, plan, decision.thought, QStringLiteral("Denied"),
                          QStringLiteral("Denied by approval policy: %1").arg(approval.summary));
        asyncState_.phase = AgentLoopPhase::Failed;
        asyncState_.terminalReason = AgentTerminalReason::SecurityDenied;
        asyncState_.abortReason = QStringLiteral("Required action denied by authorization policy.");
        completeAsync();
        return;
    }
    executeStepAsync(plan, decision.thought, approval);
}

void AgentLoop::executeStepAsync(const ToolInvocationPlan& originalPlan, const QString& thought,
                                 ApprovalDecision approval) {
    ToolInvocationPlan plan = originalPlan;
    const int index = asyncState_.steps.size() + 1;
    const auto descriptor = toolRegistry_ && !plan.invocations.isEmpty()
                                ? toolRegistry_->findToolById(plan.invocations.first().toolId)
                                : std::optional<ToolDescriptor>{};
    if (externalDirectoryGate_)
        externalDirectoryGate_->setSecuritySessionId(asyncState_.sessionId);
    const auto authorization = authorizeResources(plan, asyncState_.sessionId);
    if (!authorization.ok()) {
        appendBlockedStep(asyncState_, plan, thought, QStringLiteral("Denied"),
                          authorization.reason, resourceFailureObservation(authorization));
        if (toolCallback_)
            toolCallback_(ToolTransition::ExecutionFinished, index, plan,
                          &asyncState_.steps.last());
        scheduleAsyncAdvance();
        return;
    }
    if (plan.invocations.size() > 1) {
        executeBatchAsync(plan, thought, approval);
        return;
    }
    const auto sandbox = sandboxPolicy_.evaluate(plan, approval);
    if (toolCallback_)
        toolCallback_(ToolTransition::ExecutionStarted, index, plan, nullptr);
    if (persistenceCheck_ && !persistenceCheck_()) {
        asyncState_.phase = AgentLoopPhase::Failed;
        asyncState_.terminalReason = AgentTerminalReason::UnableToComplete;
        asyncState_.abortReason =
            QStringLiteral("Agent audit persistence failed; tool dispatch refused.");
        completeAsync();
        return;
    }
    waitingForTool_ = true;
    ToolInvocationPlan cancellablePlan = plan;
    for (auto& invocation : cancellablePlan.invocations)
        invocation.cancellation = cancellationToken_;
    cancelTool_ = gateway_.executeAsync(
        {cancellablePlan, approval, sandbox, knownToolIds_, asyncContext_}, executor_,
        asyncState_.sessionId,
        toolCallIdProvider_ ? toolCallIdProvider_(index) : QString::number(index),
        [this, index](const QString& processId, ProcessStream stream, const QByteArray& bytes) {
            if (!asyncFinished_ && !cancellationRequested() && outputCallback_)
                outputCallback_(index, processId, stream, bytes);
        },
        [this, index, plan, thought, descriptor](ToolExecutionResult result) {
            if (asyncFinished_)
                return;
            waitingForTool_ = false;
            cancelTool_ = {};
            if (cancellationRequested()) {
                AgentStepRecord partial;
                partial.index = index;
                partial.thought = thought;
                partial.batchId = plan.batchId;
                fillRecordFromPlan(partial, plan);
                partial.statusText = toolExecutionStatusName(ToolExecutionStatus::Cancelled);
                partial.failureCategory = ToolFailureCategory::Cancelled;
                partial.observation = truncatedObservation(result.summary.toUtf8(), partial.toolId);
                partial.structuredObservation = result.structuredObservation;
                asyncState_.steps.append(partial);
                if (descriptor && result.structuredObservation)
                    recordEvidence(asyncState_, *descriptor, plan, ToolExecutionStatus::Cancelled,
                                   result.summary, index, result.structuredObservation,
                                   result.mutations);
                if (toolCallback_)
                    toolCallback_(ToolTransition::ExecutionFinished, index, plan, &partial);
                if (stepCallback_)
                    stepCallback_(partial);
                asyncState_.phase = AgentLoopPhase::Cancelled;
                asyncState_.abortReason = QStringLiteral("Agent run cancelled by user.");
                completeAsync();
                return;
            }
            if (result.status == ToolExecutionStatus::Cancelled) {
                asyncState_.phase = AgentLoopPhase::Cancelled;
                asyncState_.abortReason = QStringLiteral("Active tool cancelled.");
            }
            AgentStepRecord record;
            record.index = index;
            record.thought = thought;
            record.batchId = plan.batchId;
            fillRecordFromPlan(record, plan);
            record.succeeded = result.status == ToolExecutionStatus::Succeeded ||
                               result.status == ToolExecutionStatus::PlaceholderSucceeded;
            record.statusText = toolExecutionStatusName(result.status);
            record.observation = truncatedObservation(result.summary.toUtf8(), record.toolId);
            record.structuredObservation = result.structuredObservation;
            record.sandbox = result.sandbox;
            record.failureCategory = result.failureCategory;
            asyncState_.steps.append(record);
            asyncState_.consecutiveBlockedSteps = 0;
            asyncState_.repeatedActionRecoveryPending = false;
            if (record.succeeded)
                doomDetector_.reset(asyncState_.sessionId);
            if (!record.succeeded && result.status != ToolExecutionStatus::Cancelled)
                planner_.setPlannerFeedback(
                    QStringLiteral(
                        "Tool %1 returned %2: %3. Replan using this observation; do not repeat "
                        "the same call unchanged.")
                        .arg(record.toolId, record.statusText, record.observation));
            if (descriptor)
                recordEvidence(asyncState_, *descriptor, plan, result.status, result.summary, index,
                               result.structuredObservation, result.mutations);
            if (toolCallback_)
                toolCallback_(ToolTransition::ExecutionFinished, index, plan, &record);
            if (stepCallback_)
                stepCallback_(record);
            if (statusCallback_)
                statusCallback_(QStringLiteral("Agent loop step %1 finished: %2 (%3)")
                                    .arg(index)
                                    .arg(record.toolName, record.statusText));
            if (result.status == ToolExecutionStatus::Cancelled)
                completeAsync();
            else
                scheduleAsyncAdvance();
        });
    if (!waitingForTool_)
        cancelTool_ = {};
}

void AgentLoop::executeBatchAsync(const ToolInvocationPlan& plan, const QString& thought,
                                  ApprovalDecision approval) {
    const int firstIndex = asyncState_.steps.size() + 1;
    QList<ToolExecutionRequest> requests;
    QStringList callIds;
    for (int i = 0; i < plan.invocations.size(); ++i) {
        auto one = plan;
        one.invocations = {plan.invocations.at(i)};
        one.invocations.first().cancellation = cancellationToken_;
        requests.append(
            {one, approval, sandboxPolicy_.evaluate(one, approval), knownToolIds_, asyncContext_});
        callIds.append(toolCallIdProvider_ ? toolCallIdProvider_(firstIndex + i)
                                           : QString::number(firstIndex + i));
    }
    waitingForTool_ = true;
    scheduler_ = std::make_shared<AgentExecutionScheduler>(
        gateway_, executor_, config_.maxParallelTools, config_.executionBudget);
    scheduler_->setPersistenceCheck(persistenceCheck_);
    scheduler_->start(
        std::move(requests), asyncState_.sessionId, std::move(callIds),
        [this, plan, firstIndex](int i) {
            if (!toolCallback_)
                return;
            auto one = plan;
            one.invocations = {plan.invocations.at(i)};
            toolCallback_(ToolTransition::ExecutionStarted, firstIndex + i, one, nullptr);
        },
        [this, firstIndex](int i, const QString& processId, ProcessStream stream,
                           const QByteArray& bytes) {
            if (!asyncFinished_ && !cancellationRequested() && outputCallback_)
                outputCallback_(firstIndex + i, processId, stream, bytes);
        },
        [this, plan, thought, firstIndex](QList<ToolExecutionResult> results) {
            if (asyncFinished_)
                return;
            waitingForTool_ = false;
            bool cancelled = cancellationRequested();
            for (int i = 0; i < results.size(); ++i) {
                auto one = plan;
                one.invocations = {plan.invocations.at(i)};
                auto& result = results[i];
                AgentStepRecord record;
                record.index = firstIndex + i;
                record.thought = thought;
                record.batchId = plan.batchId;
                fillRecordFromPlan(record, one);
                record.succeeded = result.status == ToolExecutionStatus::Succeeded ||
                                   result.status == ToolExecutionStatus::PlaceholderSucceeded;
                record.statusText = toolExecutionStatusName(result.status);
                record.observation = truncatedObservation(result.summary.toUtf8(), record.toolId);
                record.structuredObservation = result.structuredObservation;
                record.sandbox = result.sandbox;
                record.failureCategory = result.failureCategory;
                asyncState_.steps.append(record);
                asyncState_.consecutiveBlockedSteps = 0;
                asyncState_.repeatedActionRecoveryPending = false;
                if (record.succeeded)
                    doomDetector_.reset(asyncState_.sessionId);
                if (!record.succeeded && result.status != ToolExecutionStatus::Cancelled)
                    planner_.setPlannerFeedback(
                        QStringLiteral(
                            "Tool %1 returned %2: %3. Replan using this observation; do not repeat "
                            "the same call unchanged.")
                            .arg(record.toolId, record.statusText, record.observation));
                if (one.invocations.first().descriptorSnapshot)
                    recordEvidence(asyncState_, *one.invocations.first().descriptorSnapshot, one,
                                   result.status, result.summary, record.index,
                                   result.structuredObservation, result.mutations);
                if (toolCallback_)
                    toolCallback_(ToolTransition::ExecutionFinished, record.index, one, &record);
                if (stepCallback_)
                    stepCallback_(record);
                cancelled |= result.status == ToolExecutionStatus::Cancelled;
            }
            scheduler_.reset();
            if (cancelled) {
                asyncState_.phase = AgentLoopPhase::Cancelled;
                asyncState_.abortReason = QStringLiteral("Agent batch cancelled.");
                completeAsync();
            } else {
                scheduleAsyncAdvance();
            }
        });
}

bool AgentLoop::cancellationRequested() const {
    return cancelled_.load() || (cancelQuery_ && cancelQuery_());
}

AgentLoopState AgentLoop::run(const QString& goal, const QString& sessionId) {
    AgentLoopState state;
    state.sessionId = sessionId.trimmed().isEmpty()
                          ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                          : sessionId;
    state.goal = goal;
    state.phase = AgentLoopPhase::Running;
    initializeObservationIntent(state);
    if (state.observationIntent.indeterminate) {
        state.phase = cancellationRequested() ? AgentLoopPhase::Cancelled : AgentLoopPhase::Failed;
        state.terminalReason = cancellationRequested()
                                   ? AgentTerminalReason::Cancelled
                                   : AgentTerminalReason::ObservationIndeterminate;
        state.abortReason = classificationAbortMessage(state.observationIntent);
        if (state.observationIntent.providerFailure)
            state.providerFailure = *state.observationIntent.providerFailure;
        return state;
    }
    return advance(std::move(state));
}

AgentLoopState AgentLoop::resume(AgentLoopState state, bool approved) {
    planner_.setObservationIntent(state.observationIntent);
    state.phase = AgentLoopPhase::Running;

    auto pendingPlan = state.pendingApprovalPlan;
    const QString thought = state.pendingApprovalThought;
    state.pendingApprovalPlan = ToolInvocationPlan{};
    state.pendingApprovalThought.clear();
    state.pendingAuthorizationRequests.clear();

    if (!approved) {
        appendBlockedStep(state, pendingPlan, thought, QStringLiteral("Denied"),
                          QStringLiteral("User denied execution in chat."));
        state.phase = AgentLoopPhase::Failed;
        state.terminalReason = AgentTerminalReason::ApprovalDenied;
        state.abortReason = QStringLiteral("User denied the required action.");
        return state;
    }

    grantExternalPaths(pendingPlan, state.sessionId);
    executeStep(state, pendingPlan, thought,
                ApprovalDecision{
                    ApprovalStatus::Approved,
                    QStringLiteral("User approved execution in chat."),
                    {},
                });
    return advance(std::move(state));
}

AgentLoopState AgentLoop::advance(AgentLoopState state) {
    if (statusCallback_) {
        statusCallback_(QStringLiteral("Agent loop running for goal: %1").arg(state.goal));
    }

    while (true) {
        if (cancellationRequested()) {
            state.phase = AgentLoopPhase::Cancelled;
            state.abortReason = QStringLiteral("Agent run cancelled by user.");
            return state;
        }

        const int productiveSteps =
            qMax(0, static_cast<int>(state.steps.size()) - state.blockedSteps);
        if (state.blockedSteps >= config_.maxBlockedSteps) {
            state.phase = AgentLoopPhase::Failed;
            state.terminalReason = AgentTerminalReason::Blocked;
            state.abortReason =
                QStringLiteral("Too many blocked tool requests (%1). Review the tool contract and "
                               "choose a permitted action.")
                    .arg(state.blockedSteps);
            return state;
        }
        if (state.consecutiveBlockedSteps >= config_.maxConsecutiveBlockedSteps) {
            state.phase = AgentLoopPhase::Failed;
            state.terminalReason = AgentTerminalReason::Blocked;
            state.abortReason =
                QStringLiteral("Too many consecutive blocked tool requests (%1). The planner did "
                               "not recover after the previous rejection; inspect the last "
                               "observation and choose a different permitted action.")
                    .arg(state.consecutiveBlockedSteps);
            return state;
        }

        const int stepIndex = static_cast<int>(state.steps.size()) + 1;
        if (planningCallback_) {
            planningCallback_(stepIndex, true);
        }
        AgentStepDecision decision;
        try {
            planner_.setObservationIntent(state.observationIntent);
            planner_.setStructuredFacts(
                ClaimGroundingResolver::facts(state.observationIntent, state.evidence));
            preparePlanningContext(state);
            planner_.setEvidence(state.evidence);
            traceStage(state, "planning", "enter");
            if (const auto recovery = finalVerificationRecovery(state, workingDirectory(),
                                                                knownToolIds_, toolRegistry_)) {
                decision = *recovery;
                if (statusCallback_) {
                    statusCallback_(QStringLiteral("Planning requested final verification."));
                }
                traceStage(state, "verification_recovery", "selected", &decision,
                           workingDirectory());
            } else {
                decision = planner_.nextStep(state.goal, state.steps);
                state.providerRecoveryAttempts += planner_.lastProviderRecoveryAttempts();
            }
            traceStage(state, "planning", "exit", &decision, workingDirectory());
        } catch (...) {
            if (planningCallback_)
                planningCallback_(stepIndex, false);
            throw;
        }
        if (planningCallback_) {
            planningCallback_(stepIndex, false);
        }
        if (cancellationRequested()) {
            state.phase = AgentLoopPhase::Cancelled;
            state.abortReason = QStringLiteral("Agent run cancelled by user.");
            return state;
        }

        if (decision.kind == AgentStepDecision::Kind::FinalAnswer) {
            if (acceptFinalAnswer(state, decision))
                return state;
            continue;
        }

        if (decision.kind == AgentStepDecision::Kind::GiveUp) {
            state.providerFailure = decision.providerFailure;
            state.phase = AgentLoopPhase::Failed;
            state.terminalReason = giveUpTerminalReason(decision);
            state.abortReason =
                decision.reason.trimmed().isEmpty() ? decision.thought : decision.reason;
            return state;
        }

        if (productiveSteps >= config_.maxIterations) {
            state.phase = AgentLoopPhase::Failed;
            state.terminalReason = AgentTerminalReason::IterationLimit;
            state.abortReason = QStringLiteral("Iteration limit reached (%1 productive steps).")
                                    .arg(config_.maxIterations);
            return state;
        }

        const auto known = decision.toolBatch.isEmpty()
                               ? knownToolIds_.contains(decision.toolId)
                               : std::all_of(decision.toolBatch.cbegin(), decision.toolBatch.cend(),
                                             [this](const auto& call) {
                                                 return knownToolIds_.contains(call.toolId);
                                             });
        if (!known) {
            auto unavailablePlan = planFromDecision(decision);
            if (repeatsUnchangedStep(state, unavailablePlan, workingDirectory(), toolRegistry_)) {
                state.phase = AgentLoopPhase::Failed;
                state.terminalReason = AgentTerminalReason::CapabilityUnavailable;
                state.abortReason = QStringLiteral("Required tool is unavailable.");
                return state;
            }
            appendBlockedStep(state, unavailablePlan, decision.thought,
                              QStringLiteral("Unknown Tool"),
                              QStringLiteral("Unknown tool requested: %1").arg(decision.toolId));
            continue;
        }

        auto plan = planFromDecision(decision);
        if (repeatsUnchangedStep(state, plan, workingDirectory(), toolRegistry_) ||
            repeatsFailedCycle(state, plan)) {
            if (repairRepeatedObservation(state, plan, planner_)) {
                continue;
            }
            stopRepeatedCall(state);
            return state;
        }
        doomDetector_.recordAction(state.sessionId, decisionActionKey(decision));
        if (doomDetector_.isStuck(state.sessionId)) {
            if (repairRepeatedObservation(state, plan, planner_)) {
                continue;
            }
            state.phase = AgentLoopPhase::Stuck;
            state.abortReason = QStringLiteral(
                "Doom loop detected: the agent repeated the same action without progress.");
            return state;
        }
        if (toolCallback_) {
            for (int i = 0; i < plan.invocations.size(); ++i) {
                auto one = plan;
                one.invocations = {plan.invocations.at(i)};
                toolCallback_(ToolTransition::Requested, stepIndex + i, one, nullptr);
            }
        }
        const auto validation = gateway_.validatePlan(plan);
        if (validation.status != ToolExecutionStatus::Succeeded) {
            appendBlockedStep(state, plan, decision.thought,
                              toolExecutionStatusName(validation.status), validation.summary);
            if (toolCallback_)
                toolCallback_(ToolTransition::ExecutionFinished, stepIndex, plan,
                              &state.steps.last());
            continue;
        }
        const auto resources = prepareResources(plan);
        if (!resources.ok()) {
            appendBlockedStep(state, plan, decision.thought,
                              securityFailure(resources.failure)
                                  ? QStringLiteral("Denied")
                                  : QStringLiteral("Invalid Arguments"),
                              resources.reason, resourceFailureObservation(resources));
            if (toolCallback_)
                toolCallback_(ToolTransition::ExecutionFinished, stepIndex, plan,
                              &state.steps.last());
            continue;
        }
        if (externalDirectoryGate_)
            externalDirectoryGate_->setSecuritySessionId(state.sessionId);

        ApprovalDecision approval;
        if (hasAuthorizationDeny(plan, state.sessionId)) {
            approval = {ApprovalStatus::Denied,
                        QStringLiteral("Denied by an explicit permission grant."),
                        {}};
        } else if (config_.autonomousMode) {
            approval = ApprovalDecision{
                ApprovalStatus::Approved,
                QStringLiteral("Autonomous Mode is enabled: per-step approval is bypassed."),
                {},
            };
        } else {
            approval = hasAuthorizationGrants(plan, state.sessionId)
                           ? ApprovalDecision{ApprovalStatus::Approved,
                                              QStringLiteral("An explicit semantic permission "
                                                             "grant covers this invocation."),
                                              {}}
                           : approvalPolicy_.evaluate(plan);
        }

        const auto externalPaths = externalPathsRequiringApproval(plan, state.sessionId);
        applyPermissionPolicy(plan, state.sessionId, approval);
        if (!externalPaths.isEmpty() && approval.status != ApprovalStatus::Denied) {
            const auto toolName = plan.invocations.isEmpty() ? QStringLiteral("tool")
                                                             : plan.invocations.first().toolName;
            approval.status = ApprovalStatus::RequiresApproval;
            const auto displayed = externalPaths.mid(0, 3).join(QStringLiteral(", "));
            approval.summary =
                QStringLiteral("Allow %1 to access %2%3?")
                    .arg(toolName, displayed,
                         externalPaths.size() > 3
                             ? QStringLiteral(" and %1 more file(s)").arg(externalPaths.size() - 3)
                             : QString{});
        }
        if (approval.status == ApprovalStatus::RequiresApproval) {
            if (toolCallback_) {
                toolCallback_(ToolTransition::ApprovalRequired, stepIndex, plan, nullptr);
            }
            state.pendingApprovalPlan = plan;
            state.pendingApprovalThought = decision.thought;
            state.pendingAuthorizationRequests = resolveAuthorizationRequests(plan);
            state.phase = AgentLoopPhase::AwaitingApproval;
            if (statusCallback_) {
                statusCallback_(QStringLiteral("Agent loop paused: approval required for %1.")
                                    .arg(decision.toolId));
            }
            return state;
        }

        if (approval.status == ApprovalStatus::Denied) {
            appendBlockedStep(
                state, plan, decision.thought, QStringLiteral("Denied"),
                QStringLiteral("Denied by approval policy: %1").arg(approval.summary));
            state.phase = AgentLoopPhase::Failed;
            state.terminalReason = AgentTerminalReason::SecurityDenied;
            state.abortReason = QStringLiteral("Required action denied by authorization policy.");
            return state;
        }

        executeStep(state, plan, decision.thought, approval);
    }
}

void AgentLoop::executeStep(AgentLoopState& state, const ToolInvocationPlan& originalPlan,
                            const QString& thought, ApprovalDecision approval) {
    if (originalPlan.invocations.size() > 1) {
        for (const auto& invocation : originalPlan.invocations) {
            auto one = originalPlan;
            one.invocations = {invocation};
            executeStep(state, one, thought, approval);
            if (cancellationRequested())
                break;
        }
        return;
    }
    ToolInvocationPlan plan = originalPlan;
    if (externalDirectoryGate_)
        externalDirectoryGate_->setSecuritySessionId(state.sessionId);
    const auto authorization = authorizeResources(plan, state.sessionId);
    if (!authorization.ok()) {
        appendBlockedStep(state, plan, thought, QStringLiteral("Denied"), authorization.reason,
                          resourceFailureObservation(authorization));
        if (toolCallback_)
            toolCallback_(ToolTransition::ExecutionFinished, state.steps.size(), plan,
                          &state.steps.last());
        return;
    }
    const auto sandbox = sandboxPolicy_.evaluate(plan, approval);
    const int stepIndex = static_cast<int>(state.steps.size()) + 1;
    const auto descriptor = toolRegistry_ && !plan.invocations.isEmpty()
                                ? toolRegistry_->findToolById(plan.invocations.first().toolId)
                                : std::optional<ToolDescriptor>{};
    if (toolCallback_) {
        toolCallback_(ToolTransition::ExecutionStarted, stepIndex, plan, nullptr);
    }

    if (persistenceCheck_ && !persistenceCheck_()) {
        state.phase = AgentLoopPhase::Failed;
        state.terminalReason = AgentTerminalReason::UnableToComplete;
        state.abortReason =
            QStringLiteral("Agent audit persistence failed; tool dispatch refused.");
        return;
    }
    const auto result = gateway_.execute(
        ToolExecutionRequest{
            plan,
            approval,
            sandbox,
            knownToolIds_,
        },
        executor_);

    AgentStepRecord record;
    record.index = static_cast<int>(state.steps.size()) + 1;
    record.thought = thought;
    record.batchId = plan.batchId;
    fillRecordFromPlan(record, plan);
    record.succeeded = result.status == ToolExecutionStatus::Succeeded ||
                       result.status == ToolExecutionStatus::PlaceholderSucceeded;
    record.statusText = toolExecutionStatusName(result.status);
    record.observation = truncatedObservation(result.summary.toUtf8(), record.toolId);

    record.structuredObservation = result.structuredObservation;
    record.sandbox = result.sandbox;
    record.failureCategory = result.failureCategory;
    state.steps.append(record);
    state.consecutiveBlockedSteps = 0;
    state.repeatedActionRecoveryPending = false;
    if (record.succeeded)
        doomDetector_.reset(state.sessionId);
    if (!record.succeeded && result.status != ToolExecutionStatus::Cancelled)
        planner_.setPlannerFeedback(
            QStringLiteral("Tool %1 returned %2: %3. Replan using this observation; do not repeat "
                           "the same call unchanged.")
                .arg(record.toolId, record.statusText, record.observation));
    if (descriptor)
        recordEvidence(state, *descriptor, plan, result.status, result.summary, stepIndex,
                       result.structuredObservation, result.mutations);
    if (toolCallback_) {
        toolCallback_(ToolTransition::ExecutionFinished, stepIndex, plan, &record);
    }
    if (stepCallback_) {
        stepCallback_(record);
    }
    if (statusCallback_) {
        statusCallback_(
            QStringLiteral("Agent loop step %1 finished: %2 (%3)")
                .arg(QString::number(record.index), record.toolName, record.statusText));
    }
}

void AgentLoop::appendBlockedStep(AgentLoopState& state, const ToolInvocationPlan& plan,
                                  const QString& thought, const QString& statusText,
                                  const QString& observation,
                                  StructuredObservationPtr structuredObservation) {
    if (plan.invocations.size() > 1) {
        for (const auto& invocation : plan.invocations) {
            auto one = plan;
            one.invocations = {invocation};
            appendBlockedStep(state, one, thought, statusText, observation, structuredObservation);
        }
        return;
    }
    AgentStepRecord record;
    record.index = static_cast<int>(state.steps.size()) + 1;
    record.thought = thought;
    record.batchId = plan.batchId;
    fillRecordFromPlan(record, plan);
    record.succeeded = false;
    record.statusText = statusText;
    record.failureCategory =
        statusText == QLatin1String("Denied")              ? ToolFailureCategory::SecurityDenied
        : statusText == QLatin1String("Unknown Tool")      ? ToolFailureCategory::Unsupported
        : statusText == QLatin1String("Invalid Arguments") ? ToolFailureCategory::InvalidArguments
                                                           : ToolFailureCategory::None;
    record.observation = observation;
    record.structuredObservation = std::move(structuredObservation);

    state.steps.append(record);
    ++state.blockedSteps;
    ++state.consecutiveBlockedSteps;
    planner_.setPlannerFeedback(
        QStringLiteral(
            "Tool %1 was blocked (%2). %3 Do not retry the same action unchanged or request "
            "broader permission to bypass a denial.")
            .arg(record.toolId, record.statusText, record.observation));
    if (statusText == QLatin1String("Denied") && toolRegistry_ && !plan.invocations.isEmpty()) {
        const auto descriptor = toolRegistry_->findToolById(record.toolId);
        if (descriptor)
            recordEvidence(state, *descriptor, plan, ToolExecutionStatus::Blocked, observation,
                           record.index, record.structuredObservation);
    }
    if (statusText == QLatin1String("Unknown Tool") &&
        (record.toolId.startsWith(QLatin1String("mcp.")) ||
         record.toolId.startsWith(QLatin1String("plugin.")))) {
        EvidenceRecord unavailable;
        unavailable.toolId = record.toolId;
        unavailable.toolCallId =
            toolCallIdProvider_ ? toolCallIdProvider_(record.index)
                                : QStringLiteral("%1:%2").arg(state.sessionId).arg(record.index);
        unavailable.stepIndex = record.index;
        unavailable.domain = ObservationDomain::ExternalService;
        unavailable.scope = EvidenceScope::Provider;
        unavailable.outcome = EvidenceOutcome::Unavailable;
        unavailable.observedAtUtc = QDateTime::currentDateTimeUtc();
        state.evidence.append(std::move(unavailable));
    }
    if (stepCallback_) {
        stepCallback_(record);
    }
}

void AgentLoop::initializeObservationIntent(AgentLoopState& state) {
    traceStage(state, "classification", "enter");
    const auto traceExit = qScopeGuard([&] { traceStage(state, "classification", "exit"); });
    if (observationIntentPolicy_) {
        if (statusCallback_)
            statusCallback_(QStringLiteral("Determining required observations."));
        state.observationIntent = observationIntentPolicy_->classify(
            state.goal,
            observationContext_ + QStringLiteral("\nACTIVE WORKSPACE ROOT: ") +
                workspaceContext_.rootPath,
            toolRegistry_ ? toolRegistry_->enabledTools() : QList<ToolDescriptor>{});
    }
    planner_.setObservationIntent(state.observationIntent);
}

bool AgentLoop::acceptFinalAnswer(AgentLoopState& state, const AgentStepDecision& decision) {
    state.lastFinalRejection = AgentStepFailure::None;
    if (state.repeatedActionRecoveryPending && decision.grounding != GroundingMode::Verified) {
        state.phase = AgentLoopPhase::Stuck;
        state.terminalReason = AgentTerminalReason::CompletionIndeterminate;
        state.abortReason =
            QStringLiteral("Agent repeated a previously executed action and did not choose a "
                           "different action after the bounded repair.");
        return true;
    }
    planner_.setStructuredFacts(
        ClaimGroundingResolver::facts(state.observationIntent, state.evidence));
    auto gate = EvidencePolicy::evaluate(state.observationIntent, state.evidence,
                                         decision.grounding, decision.groundingDeclared);
    if (!gate.accepted)
        state.lastFinalRejection = AgentStepFailure::VerificationFailed;
    if (gate.accepted && !state.pendingApprovalPlan.invocations.isEmpty()) {
        gate.accepted = false;
        gate.repair = QStringLiteral("Required approval is still unresolved.");
    }
    if (gate.accepted && decision.grounding == GroundingMode::Verified) {
        for (const auto& operation : state.evidence) {
            if (operation.scope != EvidenceScope::Operation ||
                (operation.domain != ObservationDomain::FileSystem &&
                 operation.domain != ObservationDomain::Workspace) ||
                operation.outcome != EvidenceOutcome::Verified)
                continue;
            if (!mutationConfirmedBy(state, operation)) {
                gate.accepted = false;
                gate.repair = QStringLiteral(
                    "Observe the affected filesystem state after the mutation before "
                    "claiming a verified result.");
                state.lastFinalRejection = AgentStepFailure::VerificationFailed;
                break;
            }
        }
    }
    if (gate.accepted && decision.grounding == GroundingMode::Verified) {
        for (const auto& assertion : decision.claims) {
            const bool known = std::any_of(state.observationIntent.requirements.cbegin(),
                                           state.observationIntent.requirements.cend(),
                                           [&](const ObservationRequirement& requirement) {
                                               return requirement.claimType != ClaimType::None &&
                                                      requirement.claimId == assertion.id;
                                           });
            if (!known) {
                gate.accepted = false;
                gate.repair = QStringLiteral("Final references an unknown claim ID.");
                state.lastFinalRejection = AgentStepFailure::VerificationFailed;
                break;
            }
        }
        for (const auto& requirement : state.observationIntent.requirements) {
            if (!gate.accepted)
                break;
            if (requirement.claimType == ClaimType::None)
                continue;
            const auto assertion = std::find_if(
                decision.claims.cbegin(), decision.claims.cend(),
                [&](const ClaimAssertion& item) { return item.id == requirement.claimId; });
            if (assertion == decision.claims.cend()) {
                gate.accepted = false;
                gate.repair =
                    QStringLiteral("Verified final requires a structured assertion for %1.")
                        .arg(requirement.claimId);
                state.lastFinalRejection = AgentStepFailure::VerificationFailed;
                break;
            }
            const auto resolved =
                ClaimGroundingResolver::resolve(requirement, state.evidence, *assertion);
            if (resolved.verdict != ClaimVerdict::Supported) {
                gate.accepted = false;
                gate.repair = resolved.verdict == ClaimVerdict::Contradicted
                                  ? QStringLiteral("The observed result contradicts %1. Use the "
                                                   "verified fact when answering.")
                                        .arg(requirement.claimId)
                                  : QStringLiteral("The observed result cannot determine %1. "
                                                   "Observe more or report inability to verify.")
                                        .arg(requirement.claimId);
                state.lastFinalRejection = AgentStepFailure::VerificationFailed;
                break;
            }
            gate.grounding.evidenceCallIds.append(resolved.fact.evidenceCallIds);
        }
        gate.grounding.evidenceCallIds.removeDuplicates();
    }
    if (gate.accepted && decision.grounding != GroundingMode::UnableToVerify &&
        gate.answerOverride.isEmpty()) {
        const auto canonical = ClaimGroundingResolver::filesystemFinalAnswer(
            state.observationIntent, state.evidence, decision.answer);
        if (canonical && decision.answer != *canonical) {
            // Narrow safety net: an observation whose coverage cannot support an absence
            // conclusion must keep the evidence-derived representation. Productive coding
            // finals and ordinary file explanations are published as natural language.
            if (ClaimGroundingResolver::canonicalObservationRequired(state.evidence)) {
                gate.accepted = false;
                gate.repair =
                    QStringLiteral("The current observation scope cannot support that conclusion. "
                                   "Use the evidence-derived scoped representation or obtain "
                                   "complete coverage.");
                state.lastFinalRejection = AgentStepFailure::VerificationFailed;
            } else if (decision.answer.trimmed().isEmpty()) {
                gate.answerOverride = *canonical;
            }
        }
    }
    if (gate.accepted && decision.observationRequirementDeclared && decision.requiresObservation &&
        state.evidence.isEmpty()) {
        gate.accepted = false;
        gate.repair = QStringLiteral("You declared that observation is needed. Use an "
                                     "appropriate tool before answering.");
        state.lastFinalRejection = AgentStepFailure::VerificationFailed;
    }
    traceStage(state, "final_gate", gate.accepted ? "accepted" : "rejected");
    if (gate.accepted) {
        const bool mutated =
            std::any_of(state.evidence.cbegin(), state.evidence.cend(), [](const auto& e) {
                return e.scope == EvidenceScope::Operation &&
                       e.outcome == EvidenceOutcome::Verified &&
                       (e.domain == ObservationDomain::FileSystem ||
                        e.domain == ObservationDomain::Workspace);
            });
        if (mutated && decision.grounding == GroundingMode::UnableToVerify) {
            state.phase = AgentLoopPhase::Failed;
            state.terminalReason = AgentTerminalReason::VerificationFailed;
            state.abortReason =
                gate.answerOverride.isEmpty() ? decision.answer : gate.answerOverride;
            state.abortReason += QStringLiteral(
                " Applied changes remain in place; task completion was not verified.");
            return true;
        }
        state.repeatedActionRecoveryPending = false;
        state.finalGrounding = std::move(gate.grounding);
        state.finalClaims = decision.claims;
        state.finalAnswer = gate.answerOverride.isEmpty() ? decision.answer : gate.answerOverride;
        if (const auto explanation = ClaimGroundingResolver::filesystemExplanation(
                state.observationIntent, state.evidence, state.finalAnswer))
            state.finalAnswer = explanation->rendered;
        state.phase = AgentLoopPhase::Completed;
        state.terminalReason = AgentTerminalReason::Completed;
        return true;
    }
    ++state.rejectedFinalAnswers;
    if (state.rejectedFinalAnswers >= 2) {
        state.phase = AgentLoopPhase::Failed;
        // A rejection that is specifically about verification or an unconfirmed
        // mutation reports that, so the user is not told a generic "could not
        // complete" when the run actually stopped over missing proof.
        state.terminalReason = agentTerminalReasonFor(state.lastFinalRejection);
        // This is a bounded policy diagnostic, not model reasoning or raw output.
        // Keep the failed requirement observable instead of hiding every grounding
        // failure behind the same generic terminal message.
        state.abortReason =
            QStringLiteral(
                "Agent could not determine a grounded next action. %1 No verified result was "
                "published and no success is claimed; any tool effects already applied remain in "
                "place. Inspect the affected resources, then retry with a narrower goal or supply "
                "the missing evidence.")
                .arg(gate.repair);
        return true;
    }
    if (statusCallback_)
        statusCallback_(QStringLiteral("Verification Pending"));
    planner_.setPlannerFeedback(gate.repair);
    return false;
}

void AgentLoop::recordEvidence(AgentLoopState& state, const ToolDescriptor& descriptor,
                               const ToolInvocationPlan& plan, ToolExecutionStatus status,
                               const QString& summary, int stepIndex,
                               StructuredObservationPtr structuredObservation,
                               const QList<FileMutation>& mutations) {
    if (plan.invocations.isEmpty())
        return;
    const auto callId = toolCallIdProvider_
                            ? toolCallIdProvider_(stepIndex)
                            : QStringLiteral("%1:%2").arg(state.sessionId).arg(stepIndex);
    const bool trustedMutations =
        descriptor.source == ToolSource::BuiltIn || descriptor.filesystemFailureSemanticContract;
    if ((trustedMutations && !mutations.isEmpty()) ||
        (status == ToolExecutionStatus::Succeeded &&
         descriptor.id == QLatin1String("run-command"))) {
        QStringList changed;
        for (const auto& mutation : trustedMutations ? mutations : QList<FileMutation>{})
            changed.append(
                normalizedObservationResource(mutation.path, ObservationDomain::FileSystem));
        if (descriptor.id == QLatin1String("run-command"))
            changed.append(workingDirectory());
        for (auto& prior : state.evidence) {
            // A test of an earlier file revision cannot verify a later edit.
            if (trustedMutations && !mutations.isEmpty() &&
                prior.domain == ObservationDomain::ProcessExecution &&
                prior.outcome == EvidenceOutcome::Verified) {
                prior.outcome = EvidenceOutcome::Stale;
            }
            // Operation receipts describe an event that already happened. Later
            // commands invalidate state observations, not that historical event.
            // acceptFinalAnswer still requires a fresh post-mutation observation.
            if (prior.scope == EvidenceScope::Operation ||
                (prior.domain != ObservationDomain::FileSystem &&
                 prior.domain != ObservationDomain::Workspace)) {
                continue;
            }
            const auto resource =
                normalizedObservationResource(prior.resource, ObservationDomain::FileSystem);
            for (const auto& change : changed)
                if (resource == change || resource.startsWith(change + QLatin1Char('/')) ||
                    change.startsWith(resource + QLatin1Char('/'))) {
                    prior.outcome = EvidenceOutcome::Stale;
                    break;
                }
        }
    }
    auto recorded = EvidencePolicy::record(descriptor, plan.invocations.first(), status, summary,
                                           stepIndex, callId, structuredObservation);
    for (auto& item : recorded) {
        if (descriptor.source == ToolSource::BuiltIn && !item.resource.isEmpty() &&
            (item.domain == ObservationDomain::FileSystem ||
             item.domain == ObservationDomain::Workspace)) {
            item.resource = ExternalDirectoryGate{}.resolvePath(item.resource, workingDirectory());
        }
    }
    state.evidence.append(recorded);
    if (trustedMutations && !mutations.isEmpty() && status == ToolExecutionStatus::Succeeded)
        planner_.setPlannerFeedback(QStringLiteral(
            "The mutation was reported successful. Observe the affected path before claiming "
            "its final filesystem state."));
    traceStage(state, "evidence", "updated", nullptr, workingDirectory());
    while (state.evidence.size() > 96)
        state.evidence.removeFirst();
}

ToolInvocationPlan AgentLoop::planFromDecision(const AgentStepDecision& decision) const {
    ToolInvocationPlan plan;
    plan.status = ToolInvocationPlanStatus::Planned;
    plan.summary = QStringLiteral("Agent loop step: %1").arg(decision.toolId);
    if (!decision.toolBatch.isEmpty()) {
        plan.batchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (const auto& action : decision.toolBatch) {
            PlannedToolInvocation invocation;
            invocation.toolId = action.toolId;
            invocation.toolName = action.toolName.isEmpty() ? action.toolId : action.toolName;
            invocation.summary = QStringLiteral("Agent batch invocation for %1").arg(action.toolId);
            invocation.rationale = decision.thought;
            invocation.riskLevel = action.riskLevel;
            invocation.executionMode = action.executionMode;
            invocation.arguments = action.arguments;
            invocation.dependsOn = action.dependsOn;
            invocation.providerCallId = action.providerCallId;
            plan.invocations.append(std::move(invocation));
        }
        return plan;
    }
    plan.invocations.append(PlannedToolInvocation{
        decision.toolId,
        decision.toolName.isEmpty() ? decision.toolId : decision.toolName,
        QStringLiteral("Agent loop invocation for %1").arg(decision.toolId),
        decision.thought,
        decision.riskLevel,
        decision.executionMode,
        decision.arguments,
        {},
    });
    return plan;
}

} // namespace sentinel::core
