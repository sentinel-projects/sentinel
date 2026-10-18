// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/agent/ContextEngine.h"
#include "sentinel/core/agent/ObservationPolicy.h"
#include "sentinel/core/chat/IChatHistoryStore.h"
#include "sentinel/core/runtime/ToolArgumentValidator.h"
#include "sentinel/core/security/AuthorizationResolver.h"

#include <QFileInfo>
#include <QSet>
#include <algorithm>

namespace sentinel::core {
namespace {

QString bounded(QString text, int limit) {
    text = text.simplified();
    if (text.size() <= limit)
        return text;
    return text.left(limit) + QStringLiteral(" [excerpt; more available via tool]");
}

QString boundedProcessOutput(QString text, int limit) {
    text = text.simplified();
    if (text.size() <= limit)
        return text;
    return text.left(limit / 2) +
           QStringLiteral(" [excerpt; beginning and end; more available via tool] ") +
           text.right(limit - limit / 2);
}

int cost(const AgentContextItem& item) {
    return (item.content.size() + item.source.size() + 3) / 4 + 16;
}

} // namespace

AgentPlanningContext ContextEngine::build(const AgentContextInput& input) const {
    AgentPlanningContext result;
    const int window = input.contextWindowTokens > 0 ? input.contextWindowTokens : 8192;
    const int reserve = qMax(qMax(1024, window / 5), input.maxOutputTokens);
    const int budget = qMax(256, qMin(window - reserve, 24000));
    int remaining = budget - 550; // Planner contract and JSON response framing.
    auto add = [&](AgentContextItem item, int sectionCap) {
        const int allowed = qMin(sectionCap, remaining);
        if (allowed <= 24) {
            ++result.omittedItems;
            result.compacted = true;
            return false;
        }
        if (cost(item) > allowed) {
            if (item.priority == AgentContextPriority::Critical) {
                // The active goal is never removed or shortened.
                remaining -= cost(item);
                result.estimatedTokens += cost(item);
                result.items.append(std::move(item));
                return true;
            }
            ++result.omittedItems;
            result.compacted = true;
            return false;
        }
        remaining -= cost(item);
        result.estimatedTokens += cost(item);
        result.items.append(std::move(item));
        return true;
    };

    add({AgentContextKind::Goal, AgentContextPriority::Critical, QStringLiteral("active-run"),
         input.goal, false},
        budget);

    QStringList requirements;
    for (const auto& requirement : input.intent.requirements) {
        ObservationIntent single;
        single.requirements = {requirement};
        const auto gate =
            EvidencePolicy::evaluate(single, input.evidence, GroundingMode::Verified, true);
        requirements.append(
            QStringLiteral("%1 purpose=%2 tool=%3 ")
                .arg(gate.accepted ? QStringLiteral("observed") : QStringLiteral("PENDING"),
                     requirement.purpose == ObservationPurpose::Operate ? QStringLiteral("operate")
                                                                        : QStringLiteral("inspect"),
                     requirement.toolId) +
            requirement.claimId + QLatin1Char(' ') + observationDomainName(requirement.domain) +
            QLatin1Char(' ') + requirement.resourceHint +
            (requirement.recursive ? QStringLiteral(" recursive scope")
                                   : QStringLiteral(" exact scope")) +
            (requirement.claimType == ClaimType::HiddenEntriesExist
                 ? QStringLiteral(" hidden_entries; use includeHidden:true")
             : requirement.claimType == ClaimType::PathPatternExists
                 ? QStringLiteral(" path_pattern ") + requirement.claimQuery +
                       QStringLiteral("; includeHidden:true")
                 : QString{}));
    }
    if (input.intent.indeterminate)
        requirements.append(QStringLiteral("Live state is uncertain; obtain current evidence."));
    if (!requirements.isEmpty())
        add({AgentContextKind::Requirement, AgentContextPriority::Critical,
             QStringLiteral("observation-policy"), requirements.join(QStringLiteral("; ")), false},
            400);

    if (!input.responseProfileInstructions.trimmed().isEmpty())
        add({AgentContextKind::ResponseProfile, AgentContextPriority::Normal,
             QStringLiteral("user-response-profile"), input.responseProfileInstructions.left(2000), false}, 600);

    // Detailed contracts may not all fit. Preserve bounded discovery first, so
    // QMap ordering cannot silently hide late tools such as write-file. This is
    // metadata only; the registered schema still validates every invocation.
    QStringList catalog;
    for (const auto& tool : input.tools) {
        QStringList fields;
        for (const auto& parameter : tool.parameters)
            fields.append(parameter.id +
                          (parameter.required ? QStringLiteral("!") : QStringLiteral("?")));
        if (fields.isEmpty()) {
            const auto required = tool.inputSchema.value(QStringLiteral("required")).toArray();
            const auto properties = tool.inputSchema.value(QStringLiteral("properties")).toObject();
            for (auto it = properties.begin(); it != properties.end(); ++it)
                fields.append(it.key() + (required.contains(it.key()) ? QStringLiteral("!")
                                                                      : QStringLiteral("?")));
        }
        catalog.append(tool.id + QLatin1Char('(') + fields.join(QLatin1Char(',')) +
                       QLatin1Char(')'));
    }
    if (!catalog.isEmpty())
        add({AgentContextKind::Tool, AgentContextPriority::High,
             QStringLiteral("available-tool-index"),
             QStringLiteral("Available registered tools (! required, ? optional; index is not a "
                            "full schema): ") +
                 catalog.join(QStringLiteral("; ")),
             false},
            1000);

    int toolBudget = qMin(remaining / 2, 2600);
    for (const auto& tool : input.tools) {
        QStringList authorization;
        for (const auto& requirement : tool.authorizationRequirements)
            authorization.append(securityDomainName(requirement.domain) + QLatin1Char('/') +
                                 accessModeName(requirement.access));
        const QString contract = ToolArgumentValidator::compactContract(tool);
        AgentContextItem item{AgentContextKind::Tool, AgentContextPriority::High, tool.id,
                              QStringLiteral("%1 | risk=%2 | authorization=%3 | %4 | %5")
                                  .arg(tool.id, QString::number(static_cast<int>(tool.riskLevel)),
                                       authorization.join(QLatin1Char(',')),
                                       bounded(tool.description, 160), contract),
                              false};
        if (add(std::move(item), toolBudget))
            toolBudget -= cost(result.items.last());
    }

    // Instructions are context, never authorization. Preserve the existing
    // SkillService enable/requirements decision and additionally bind workspace
    // skills to this session's frozen workspace, not a later UI selection.
    auto skills = input.skills;
    std::stable_sort(skills.begin(), skills.end(),
                     [](const Skill& a, const Skill& b) { return a.name < b.name; });
    QSet<QString> skillNames;
    int skillBudget = qMin(remaining / 3, 1200);
    for (const auto& skill : skills) {
        if (!skill.isValid() || !skill.enabledPreference || skill.state != SkillState::Enabled ||
            (skill.scope == SkillScope::Workspace &&
             (skill.workspaceId.isEmpty() || skill.workspaceId != input.workspaceContext.id)) ||
            skillNames.contains(skill.name))
            continue;
        skillNames.insert(skill.name);
        AgentContextItem item{AgentContextKind::Skill, AgentContextPriority::Normal,
                              QStringLiteral("skill:") + skill.name, skill.content, false};
        if (add(std::move(item), skillBudget))
            skillBudget -= cost(result.items.last());
    }

    for (const auto& fact : input.facts)
        add({AgentContextKind::Fact, AgentContextPriority::High,
             fact.evidenceCallIds.join(QLatin1Char(',')),
             bounded(fact.id + QLatin1Char(' ') + fact.resource + QStringLiteral(" = ") +
                         (fact.value ? QStringLiteral("true") : QStringLiteral("false")),
                     360),
             false},
            1100);

    QSet<QString> seen;
    int observationBudget = qMin(remaining / 2, 1700);
    for (int i = input.steps.size() - 1; i >= 0; --i) {
        const auto& step = input.steps.at(i);
        const auto evidence = std::find_if(
            input.evidence.crbegin(), input.evidence.crend(),
            [&](const EvidenceRecord& record) { return record.stepIndex == step.index; });
        if (evidence != input.evidence.crend() && evidence->outcome == EvidenceOutcome::Stale)
            continue;
        const QString resource =
            evidence != input.evidence.crend()
                ? evidence->resource
                : (step.structuredObservation ? step.structuredObservation->failureResource
                                              : QString{});
        const QString key = step.toolId + QLatin1Char('|') + resource;
        const bool important = !step.succeeded || step.toolId.contains(QStringLiteral("write")) ||
                               step.toolId.contains(QStringLiteral("patch")) ||
                               step.toolId.contains(QStringLiteral("delete"));
        if (seen.contains(key) && !important) {
            ++result.omittedItems;
            result.compacted = true;
            continue;
        }
        seen.insert(key);
        QString excerpt = step.observation;
        if (step.structuredObservation &&
            step.structuredObservation->data.value(QStringLiteral("truncated")).toBool())
            excerpt += QStringLiteral(" [truncated]");
        if (step.structuredObservation &&
            (step.structuredObservation->kind == StructuredObservationKind::DirectoryListing ||
             step.structuredObservation->kind == StructuredObservationKind::PathMatches ||
             step.structuredObservation->kind == StructuredObservationKind::TextSearch)) {
            const auto coverage = FileSystemCoverage::fromObservation(*step.structuredObservation);
            excerpt.prepend(
                QStringLiteral("scope=%1 hidden=%2 recursive=%3 complete=%4 truncated=%5 "
                               "cancelled=%6 permissionLimited=%7 skippedSymlinks=%8; ")
                    .arg(coverage.scope)
                    .arg(static_cast<int>(coverage.hiddenEntriesPolicy))
                    .arg(coverage.recursive)
                    .arg(coverage.complete)
                    .arg(coverage.truncated)
                    .arg(coverage.cancelled)
                    .arg(coverage.permissionLimited)
                    .arg(coverage.skippedSymlinks));
        }
        AgentContextItem item{AgentContextKind::Observation, AgentContextPriority::High,
                              evidence != input.evidence.crend() ? evidence->toolCallId
                                                                 : QString::number(step.index),
                              QStringLiteral("tool=%1 resource=%2 status=%3 result=%4")
                                  .arg(step.toolId, resource, step.statusText,
                                       step.toolId == QLatin1String("run-command")
                                           ? boundedProcessOutput(excerpt, important ? 320 : 500)
                                           : bounded(excerpt, important ? 320 : 500)),
                              true};
        if (add(std::move(item), observationBudget))
            observationBudget -= cost(result.items.last());
    }

    if (!input.workspaceContext.id.isEmpty()) {
        const auto& workspace = input.workspaceContext;
        const auto summary =
            QStringLiteral("id=%1 root=%2 include=%3 exclude=%4 retrieval=%5 memory=%6")
                .arg(workspace.id, workspace.rootPath,
                     workspace.includeHints.join(QLatin1Char(',')),
                     workspace.excludeHints.join(QLatin1Char(',')), workspace.retrievalPreference,
                     workspace.memoryScope);
        add({AgentContextKind::Workspace, AgentContextPriority::Normal,
             QStringLiteral("workspace-profile"), bounded(summary, 400), true},
            180);
    } else if (!input.workspace.isEmpty())
        add({AgentContextKind::Workspace, AgentContextPriority::Normal,
             QStringLiteral("runtime-workspace"), input.workspace, false},
            120);

    int conversationBudget = qMin(remaining / 2, 650);
    const int recentLimit = qBound(2, conversationBudget / 90, 6);
    const auto conversation = input.chatHistoryStore && input.chatHistoryStore->isAvailable()
                                  ? input.chatHistoryStore->recentMessages(recentLimit)
                                  : QList<ChatMessage>{};
    for (const auto& message : conversation) {
        if (message.role == ChatRole::System || message.status == ChatMessageStatus::Error)
            continue;
        AgentContextItem item{
            AgentContextKind::Conversation, AgentContextPriority::High, QString::number(message.id),
            chatRoleName(message.role) + QStringLiteral(": ") + bounded(message.content, 420),
            true};
        if (add(std::move(item), conversationBudget))
            conversationBudget -= cost(result.items.last());
    }
    int historyBudget = qMin(remaining / 3, 350);
    const int beforeId = conversation.isEmpty() ? 0 : conversation.first().id;
    const auto historical = input.chatHistoryStore && input.chatHistoryStore->isAvailable()
                                ? input.chatHistoryStore->searchMessages(
                                      input.goal, qBound(1, historyBudget / 100, 3), beforeId)
                                : QList<ChatMessage>{};
    for (const auto& message : historical) {
        AgentContextItem item{
            AgentContextKind::History, AgentContextPriority::Normal, QString::number(message.id),
            chatRoleName(message.role) + QStringLiteral(": ") + bounded(message.content, 300),
            true};
        if (add(std::move(item), historyBudget)) {
            historyBudget -= cost(result.items.last());
        }
    }
    int memoryBudget = qMin(remaining / 3, 350);
    const auto memoryResult =
        input.memoryStore && input.memoryStore->isAvailable()
            ? input.memoryStore->retrieveMemory(input.goal.left(4096), input.memoryAccess,
                                                qBound(1, memoryBudget / 100, 3))
            : ScopedMemoryResult{};
    const auto memories = memoryResult.status == ScopedMemoryStatus::Ready
                              ? memoryResult.records
                              : QList<ScopedMemoryRecord>{};
    for (const auto& entry : memories) {
        AgentContextItem item{AgentContextKind::Memory, AgentContextPriority::Normal, entry.id,
                              scopedMemoryContextText({ScopedMemoryStatus::Ready, {entry}}), true};
        if (add(std::move(item), memoryBudget))
            memoryBudget -= cost(result.items.last());
    }
    return result;
}

} // namespace sentinel::core
