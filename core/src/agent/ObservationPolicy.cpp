// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/agent/ObservationPolicy.h"
#include "sentinel/core/agent/ClaimGroundingResolver.h"
#include "sentinel/core/context/ContextBudgetManager.h"

#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/security/ExternalDirectoryGate.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <algorithm>

namespace sentinel::core {
namespace {
Q_LOGGING_CATEGORY(classificationDiagnostics, "sentinel.classification.diagnostics", QtWarningMsg)
QString argumentValue(const PlannedToolInvocation& invocation, const QString& id) {
    for (const auto& argument : invocation.arguments)
        if (argument.id == id)
            return argument.value.trimmed();
    return {};
}

QString normalizedResource(QString resource, ObservationDomain domain) {
    resource = resource.trimmed();
    if (resource.isEmpty())
        return {};
    if (domain == ObservationDomain::FileSystem || domain == ObservationDomain::Workspace) {
        if (resource == QLatin1String("~"))
            resource = QDir::homePath();
        else if (resource.startsWith(QLatin1String("~/")))
            resource = QDir::home().filePath(resource.mid(2));
        if (!QDir::isAbsolutePath(resource)) {
            for (const auto location :
                 {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation,
                  QStandardPaths::DownloadLocation}) {
                const auto locationPath = QStandardPaths::writableLocation(location);
                const auto name = QFileInfo(locationPath).fileName();
                if (!name.isEmpty() &&
                    (resource.compare(name, Qt::CaseInsensitive) == 0 ||
                     resource.startsWith(name + QLatin1Char('/'), Qt::CaseInsensitive))) {
                    resource = locationPath + resource.mid(name.size());
                    break;
                }
            }
        }
        return ExternalDirectoryGate{}.resolvePath(resource, QDir::currentPath());
    }
    if (domain == ObservationDomain::Network || domain == ObservationDomain::Browser) {
        const QUrl url(resource);
        if (url.isValid() && !url.host().isEmpty())
            return url.adjusted(QUrl::NormalizePathSegments).toString();
    }
    if (domain == ObservationDomain::ProcessExecution) {
        return resource;
    }
    return resource.toCaseFolded();
}

std::optional<ObservationDomain> domainFromName(const QString& name) {
    static const QHash<QString, ObservationDomain> domains{
        {QStringLiteral("filesystem"), ObservationDomain::FileSystem},
        {QStringLiteral("workspace"), ObservationDomain::Workspace},
        {QStringLiteral("process"), ObservationDomain::Process},
        {QStringLiteral("system"), ObservationDomain::System},
        {QStringLiteral("clipboard"), ObservationDomain::Clipboard},
        {QStringLiteral("network"), ObservationDomain::Network},
        {QStringLiteral("browser"), ObservationDomain::Browser},
        {QStringLiteral("memory"), ObservationDomain::Memory},
        {QStringLiteral("history"), ObservationDomain::ConversationHistory},
        {QStringLiteral("external"), ObservationDomain::ExternalService},
        {QStringLiteral("application"), ObservationDomain::Application},
        {QStringLiteral("execution"), ObservationDomain::ProcessExecution}};
    const auto found = domains.constFind(name.toLower().trimmed());
    return found == domains.cend() ? std::nullopt : std::optional<ObservationDomain>(*found);
}

QString strictJsonObject(const QString& text) {
    const auto trimmed = text.trimmed();
    if (trimmed.startsWith(QLatin1Char('{')) && trimmed.endsWith(QLatin1Char('}')))
        return trimmed;

    // Some OpenAI-compatible local models wrap an otherwise valid structured
    // response in one Markdown JSON fence. This is presentation-only framing:
    // accept it only when the entire reply is that single fenced object, then
    // retain the normal JSON/schema validation below.
    if (!trimmed.startsWith(QStringLiteral("```")) || !trimmed.endsWith(QStringLiteral("```")))
        return {};
    const auto firstLineEnd = trimmed.indexOf(QLatin1Char('\n'));
    if (firstLineEnd < 0)
        return {};
    const auto fence = trimmed.left(firstLineEnd).trimmed().toLower();
    if (fence != QLatin1String("```") && fence != QLatin1String("```json"))
        return {};
    const auto object = trimmed.mid(firstLineEnd + 1, trimmed.size() - firstLineEnd - 4).trimmed();
    return object.startsWith(QLatin1Char('{')) && object.endsWith(QLatin1Char('}')) ? object
                                                                                    : QString{};
}

bool compatibleDomain(ObservationDomain required, ObservationDomain produced) {
    return required == produced ||
           (required == ObservationDomain::Workspace && produced == ObservationDomain::FileSystem);
}

bool resourceMatches(const ObservationRequirement& requirement, const EvidenceRecord& evidence) {
    if (requirement.purpose == ObservationPurpose::Inspect &&
        evidence.scope == EvidenceScope::Operation)
        return false;
    if (requirement.purpose == ObservationPurpose::Operate &&
        evidence.scope != EvidenceScope::Operation)
        return false;
    if (requirement.domain == ObservationDomain::Process ||
        requirement.domain == ObservationDomain::System ||
        requirement.domain == ObservationDomain::Clipboard)
        return true;
    const auto target = normalizedResource(requirement.resourceHint, requirement.domain);
    if (target.isEmpty())
        return true;
    const auto observed = normalizedResource(evidence.resource, requirement.domain);
    if (observed.isEmpty())
        return false;
    if (target == observed)
        return true;
    if (evidence.scope == EvidenceScope::Provider &&
        observed.endsWith(QStringLiteral(":") + target))
        return true;
    if (evidence.scope == EvidenceScope::DirectoryEntries &&
        (requirement.domain == ObservationDomain::FileSystem ||
         requirement.domain == ObservationDomain::Workspace))
        return QFileInfo(target).absolutePath() == observed;
    if (evidence.scope == EvidenceScope::SearchScope &&
        (requirement.domain == ObservationDomain::FileSystem ||
         requirement.domain == ObservationDomain::Workspace) &&
        target.startsWith(observed + QLatin1Char('/'))) {
        const auto name = QFileInfo(target).fileName();
        return !name.isEmpty() && evidence.qualifier.contains(name, Qt::CaseInsensitive);
    }
    return false;
}

bool matches(const ObservationRequirement& requirement, const EvidenceRecord& evidence) {
    return (requirement.toolId.isEmpty() || requirement.toolId == evidence.toolId) &&
           compatibleDomain(requirement.domain, evidence.domain) &&
           resourceMatches(requirement, evidence);
}

QList<ObservationRequirement> explicitResourceQuestions(const QString& goal) {
    QList<ObservationRequirement> requirements;
    // Concrete path/URL syntax is an independent runtime signal. Requiring a
    // question mark avoids treating a resource mentioned in a writing task as
    // a request to inspect it. Semantic cases remain with the intent classifier.
    if (!goal.contains(QLatin1Char('?')))
        return requirements;
    static const QRegularExpression path(
        QStringLiteral(R"((~/[^\s?]+|\./[^\s?]+|/(?:Users|home|tmp|etc|var)/[^\s?]+))"));
    static const QRegularExpression url(QStringLiteral(R"(https?://[^\s?]+)"),
                                        QRegularExpression::CaseInsensitiveOption);
    QString withoutUrl = goal;
    const auto urlMatch = url.match(goal);
    if (urlMatch.hasMatch())
        withoutUrl.replace(urlMatch.capturedStart(), urlMatch.capturedLength(), QString{});
    const auto pathMatch = path.match(withoutUrl);
    if (pathMatch.hasMatch())
        requirements.append({ObservationDomain::FileSystem, pathMatch.captured(1),
                             EvidenceFreshness::TurnScoped, ObservationPurpose::Inspect});
    if (urlMatch.hasMatch())
        requirements.append({ObservationDomain::Network, urlMatch.captured(),
                             EvidenceFreshness::Live, ObservationPurpose::Inspect});
    return requirements;
}
} // namespace

QString normalizedObservationResource(const QString& resource, ObservationDomain domain) {
    return normalizedResource(resource, domain);
}

QString observationDomainName(ObservationDomain domain) {
    switch (domain) {
    case ObservationDomain::FileSystem:
        return QStringLiteral("filesystem");
    case ObservationDomain::Workspace:
        return QStringLiteral("workspace");
    case ObservationDomain::Process:
        return QStringLiteral("process");
    case ObservationDomain::System:
        return QStringLiteral("system");
    case ObservationDomain::Clipboard:
        return QStringLiteral("clipboard");
    case ObservationDomain::Network:
        return QStringLiteral("network");
    case ObservationDomain::Browser:
        return QStringLiteral("browser");
    case ObservationDomain::Memory:
        return QStringLiteral("memory");
    case ObservationDomain::ConversationHistory:
        return QStringLiteral("history");
    case ObservationDomain::ExternalService:
        return QStringLiteral("external");
    case ObservationDomain::Application:
        return QStringLiteral("application");
    case ObservationDomain::ProcessExecution:
        return QStringLiteral("execution");
    }
    return QStringLiteral("external");
}

QString groundingModeName(GroundingMode mode) {
    switch (mode) {
    case GroundingMode::Context:
        return QStringLiteral("context");
    case GroundingMode::Verified:
        return QStringLiteral("verified");
    case GroundingMode::UnableToVerify:
        return QStringLiteral("unable_to_verify");
    }
    return QStringLiteral("context");
}

ObservationIntent ObservationIntentPolicy::classify(const QString& goal,
                                                    const QString& conversationContext,
                                                    const QList<ToolDescriptor>& tools) const {
    ObservationIntent intent;
    if (!provider_) {
        intent.indeterminate = true;
        intent.error = QStringLiteral("No model is configured for observation intent.");
        return intent;
    }
    QStringList capabilities;
    for (const auto& tool : tools)
        for (const auto& evidence : tool.evidenceProduced) {
            const auto name = observationDomainName(evidence.domain);
            if (!capabilities.contains(name))
                capabilities.append(name);
        }
    // AgentLoop appends trusted workspace context after recent conversation. Keep
    // that suffix even when the conversation exceeds the classifier's budget.
    const auto marker = QStringLiteral("\nACTIVE WORKSPACE ROOT: ");
    const auto rootOffset = conversationContext.lastIndexOf(marker);
    const auto context = rootOffset < 0 ? conversationContext.left(1500)
                                        : conversationContext.left(qMin(rootOffset, 1000)) +
                                              conversationContext.mid(rootOffset).left(600);
    QStringList availableToolIds;
    for (const auto& tool : tools) {
        availableToolIds.append(tool.id);
    }
    const auto prompt =
        QStringLiteral(
            "Classify the USER GOAL. Return ONLY JSON: "
            "{\"requirements\":[{\"domain\":\"filesystem\","
            "\"resource\":\"\",\"purpose\":\"inspect\",\"operation\":\"none\",\"query\":\"\","
            "\"recursive\":false,\"tool\":\"\"}]}. At most 8 entries. Domains: %1. "
            "Require evidence for current external state; [] for general knowledge, writing, "
            "hypotheticals or reasoning from visible conversation. Older history needs history. "
            "purpose is inspect or operate. Reading/explaining/comparing file CONTENT uses "
            "operation:none, never exists. Only existence questions use exists with full target "
            "path. "
            "Text checks use contains; search uses search_matches. Hidden-file questions use "
            "hidden_entries; named-file search/absence uses path_pattern. For workspace-wide "
            "search use the ACTIVE WORKSPACE ROOT and recursive:true. query is exact text/pattern. "
            "Resolve relative files against that root; never guess cwd/home. "
            "Separate every requested action and final verification. For an explicit requested "
            "tool such as git-diff, set tool to that registered ID; do not substitute a file read "
            "or git-status for a requested diff. For generic file changes leave tool empty; "
            "write/edit/patch are alternative implementations. For Git inspection use the "
            "requested repository directory; for active-workspace changes use ACTIVE WORKSPACE "
            "ROOT, never a source-file path. For execution resource is the exact requested "
            "command, not an empty string. Otherwise tool may be empty. Available tools: %4. "
            "CONTEXT: %2\nUSER GOAL: %3")
            .arg(capabilities.join(QLatin1Char(',')), context, goal,
                 availableToolIds.join(QLatin1Char(',')));
    const auto traceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QElapsedTimer elapsed;
    elapsed.start();
    qCInfo(classificationDiagnostics).noquote()
        << QJsonDocument(
               QJsonObject{{"stage", "classification"},
                           {"event", "enter"},
                           {"trace_id", traceId},
                           {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}})
               .toJson(QJsonDocument::Compact);
    ChatRequestOptions options;
    options.cancellationToken = cancellationToken_;
    // Reasoning models count hidden reasoning against the completion budget too.
    // Keep a hard bound, while leaving room for the required classification JSON.
    options.deadlineMs = 60000;
    options.maxOutputTokens = 2048;
    const auto budget = ContextBudgetManager{}.assemble(
        binding_, options, {{prompt, ContextBudgetKind::Instruction, 0, true}});
    if (!budget.accepted()) {
        intent.indeterminate = true;
        intent.error = QStringLiteral("Context budget refused observation classification.");
        return intent;
    }
    options.maxOutputTokens = budget.reservedOutput;
    const auto reply = provider_->sendRequest(budget.prompt, options);
    const auto recordOutcome = qScopeGuard([&] {
        QJsonObject diagnostic{{"stage", "classification"},
                               {"event", "exit"},
                               {"trace_id", traceId},
                               {"request_id", reply.requestId.left(128)},
                               {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                               {"wall_elapsed_ms", elapsed.elapsed()},
                               {"cancelled", cancellationToken_ && cancellationToken_->load()},
                               {"error_category", chatProviderErrorCategoryName(reply.category)}};
        for (const auto& key :
             {"provider_id", "model_id", "deadline_ms", "output_budget_tokens", "elapsed_ms",
              "http_status", "category", "upstream_cancellation", "reported_context_tokens",
              "request_bytes", "estimated_input_tokens", "estimate_method", "finish_reason",
              "prompt_tokens", "completion_tokens", "total_tokens"}) {
            const auto value = reply.diagnostics.value(QLatin1String(key));
            if (value.isDouble() || value.isBool() || value.isString())
                diagnostic.insert(QLatin1String(key), value.isString()
                                                          ? QJsonValue(value.toString().left(128))
                                                          : value);
        }
        if (reply.success && intent.indeterminate) {
            diagnostic.insert(QStringLiteral("parse_failure"), intent.error);
        }
        diagnostic.insert(QStringLiteral("classification_result"),
                          intent.indeterminate ? QStringLiteral("indeterminate")
                                               : QStringLiteral("classified"));
        qCInfo(classificationDiagnostics).noquote()
            << QJsonDocument(diagnostic).toJson(QJsonDocument::Compact);
    });
    if (!reply.success) {
        intent.indeterminate = true;
        intent.error = reply.errorMessage;
        return intent;
    }
    const auto candidate = strictJsonObject(reply.message);
    if (candidate.isEmpty()) {
        intent.error = QStringLiteral("Classifier response is not a standalone JSON object.");
        intent.indeterminate = true;
        return intent;
    }
    const auto document = QJsonDocument::fromJson(candidate.toUtf8());
    if (!document.isObject() ||
        !document.object().value(QStringLiteral("requirements")).isArray()) {
        intent.error = QStringLiteral("Classifier requirements JSON is malformed.");
        intent.indeterminate = true;
        return intent;
    }
    const auto entries = document.object().value(QStringLiteral("requirements")).toArray();
    if (entries.size() > 8) {
        intent.error = QStringLiteral("Classifier requirements exceed the eight-entry limit.");
        intent.indeterminate = true;
        return intent;
    }
    for (const auto& entry : entries) {
        if (!entry.isObject()) {
            intent.error = QStringLiteral("Classifier requirement is not an object.");
            intent.indeterminate = true;
            intent.requirements.clear();
            return intent;
        }
        const auto object = entry.toObject();
        const auto domain = domainFromName(object.value(QStringLiteral("domain")).toString());
        if (!domain) {
            intent.error = QStringLiteral("Classifier selected an unsupported domain.");
            intent.indeterminate = true;
            intent.requirements.clear();
            return intent;
        }
        ObservationRequirement requirement;
        requirement.domain = *domain;
        requirement.resourceHint = object.value(QStringLiteral("resource")).toString().left(512);
        requirement.toolId = object.value(QStringLiteral("tool")).toString();
        if (!requirement.toolId.isEmpty() && !availableToolIds.contains(requirement.toolId)) {
            intent.error = QStringLiteral("Classifier selected an unregistered tool.");
            intent.indeterminate = true;
            intent.requirements.clear();
            return intent;
        }
        requirement.purpose =
            object.value(QStringLiteral("purpose")).toString() == QLatin1String("operate")
                ? ObservationPurpose::Operate
                : ObservationPurpose::Inspect;
        if (!requirement.toolId.isEmpty()) {
            const auto tool = std::find_if(tools.cbegin(), tools.cend(), [&](const auto& item) {
                return item.id == requirement.toolId;
            });
            // A classifier label cannot change a registered tool's evidence type.
            // Resolve only unambiguous contracts; this grants no execution authority.
            if (tool != tools.cend() && tool->evidenceProduced.size() == 1) {
                const auto& produced = tool->evidenceProduced.first();
                requirement.domain = produced.domain;
                if (produced.scope == EvidenceScope::Operation) {
                    requirement.purpose = ObservationPurpose::Operate;
                }
            }
        }
        const auto workspace =
            rootOffset < 0
                ? QString{}
                : normalizedResource(conversationContext.mid(rootOffset + marker.size()).trimmed(),
                                     ObservationDomain::Workspace);
        if (!workspace.isEmpty() && !requirement.resourceHint.isEmpty() &&
            (requirement.domain == ObservationDomain::FileSystem ||
             requirement.domain == ObservationDomain::Workspace)) {
            requirement.resourceHint =
                ExternalDirectoryGate{}.resolvePath(requirement.resourceHint, workspace);
        }
        if ((requirement.toolId == QLatin1String("git-diff") ||
             requirement.toolId == QLatin1String("git-status")) &&
            !workspace.isEmpty() &&
            QFileInfo(requirement.resourceHint).absolutePath() == workspace) {
            requirement.resourceHint = workspace;
        }
        if (requirement.domain == ObservationDomain::ProcessExecution &&
            requirement.resourceHint.isEmpty()) {
            requirement.resourceHint = object.value(QStringLiteral("query")).toString().left(512);
        }
        // Generic file edits bind the verified effect, not a classifier-invented
        // implementation constraint. Explicitly named tool constraints remain exact.
        if (requirement.purpose == ObservationPurpose::Operate &&
            (requirement.toolId == QLatin1String("write-file") ||
             requirement.toolId == QLatin1String("edit-file") ||
             requirement.toolId == QLatin1String("apply-patch")) &&
            !goal.contains(requirement.toolId)) {
            requirement.toolId.clear();
        }
        requirement.freshness = requirement.domain == ObservationDomain::ConversationHistory
                                    ? EvidenceFreshness::SessionStable
                                    : EvidenceFreshness::TurnScoped;
        const auto operation = object.value(QStringLiteral("operation")).toString();
        if ((requirement.domain == ObservationDomain::FileSystem ||
             requirement.domain == ObservationDomain::Workspace) &&
            !requirement.resourceHint.isEmpty() &&
            requirement.purpose == ObservationPurpose::Inspect) {
            if (operation == QLatin1String("exists") || operation == QLatin1String("file_exists"))
                requirement.claimType = ClaimType::FileExists;
            else if (operation == QLatin1String("path_exists"))
                requirement.claimType = ClaimType::PathExists;
            else if (operation == QLatin1String("directory_exists"))
                requirement.claimType = ClaimType::DirectoryExists;
            else if (operation == QLatin1String("contains"))
                requirement.claimType = ClaimType::TextContains;
            else if (operation == QLatin1String("hidden_entries"))
                requirement.claimType = ClaimType::HiddenEntriesExist;
            else if (operation == QLatin1String("path_pattern"))
                requirement.claimType = ClaimType::PathPatternExists;
            else if (operation == QLatin1String("search_matches"))
                requirement.claimType = ClaimType::SearchHasMatches;
            if (requirement.claimType != ClaimType::None) {
                requirement.claimId =
                    QStringLiteral("claim-%1").arg(intent.requirements.size() + 1);
                requirement.recursive = object.value(QStringLiteral("recursive")).toBool(false);
                requirement.claimQuery = object.value(QStringLiteral("query")).toString().left(256);
                if (requirement.claimType != ClaimType::FileExists &&
                    requirement.claimType != ClaimType::PathExists &&
                    requirement.claimType != ClaimType::DirectoryExists &&
                    requirement.claimType != ClaimType::HiddenEntriesExist &&
                    requirement.claimQuery.isEmpty())
                    requirement.claimType = ClaimType::None;
            }
        }
        intent.requirements.append(std::move(requirement));
    }
    for (const auto& structural : explicitResourceQuestions(goal)) {
        bool alreadyRequired = false;
        for (const auto& semantic : intent.requirements)
            if (semantic.domain == structural.domain &&
                normalizedResource(semantic.resourceHint, semantic.domain) ==
                    normalizedResource(structural.resourceHint, structural.domain))
                alreadyRequired = true;
        if (!alreadyRequired)
            intent.requirements.append(structural);
    }
    return intent;
}

QList<EvidenceRecord> EvidencePolicy::record(const ToolDescriptor& descriptor,
                                             const PlannedToolInvocation& invocation,
                                             ToolExecutionStatus status, const QString& summary,
                                             int stepIndex, const QString& toolCallId,
                                             StructuredObservationPtr structuredObservation) {
    QList<EvidenceRecord> records;
    if (status == ToolExecutionStatus::InvalidArguments ||
        status == ToolExecutionStatus::InvalidToolContract ||
        status == ToolExecutionStatus::EmptyPlan || status == ToolExecutionStatus::NotRequested)
        return records;
    const bool succeeded = status == ToolExecutionStatus::Succeeded;
    const bool denied = status == ToolExecutionStatus::Blocked;
    const bool unavailable = status == ToolExecutionStatus::UnknownTool;
    for (const auto& produced : descriptor.evidenceProduced) {
        EvidenceRecord record;
        record.toolId = descriptor.id;
        record.toolCallId = toolCallId;
        record.stepIndex = stepIndex;
        record.domain = produced.domain;
        record.resource =
            produced.resourceArgument.isEmpty()
                ? (produced.scope == EvidenceScope::Provider ? descriptor.providerId : QString{})
                : argumentValue(invocation, produced.resourceArgument);
        if (record.resource.isEmpty() && structuredObservation) {
            record.resource = structuredObservation->data.value(QStringLiteral("scope")).toString();
            if (record.resource.isEmpty())
                record.resource =
                    structuredObservation->data.value(QStringLiteral("root")).toString();
            if (record.resource.isEmpty())
                record.resource =
                    structuredObservation->data.value(QStringLiteral("path")).toString();
        }
        // The built-in reader resolves a relative argument against the frozen
        // authorized workspace. Do not reinterpret it against daemon process cwd.
        // External tool payloads do not gain authority to rebind a requested path.
        if (descriptor.source == ToolSource::BuiltIn &&
            descriptor.id == QLatin1String("read-file") && structuredObservation &&
            structuredObservation->kind == StructuredObservationKind::FileContent) {
            const auto resolved =
                structuredObservation->data.value(QStringLiteral("path")).toString();
            if (QDir::isAbsolutePath(resolved))
                record.resource = resolved;
        }
        record.qualifier = argumentValue(invocation, produced.qualifierArgument);
        record.freshness = produced.freshness;
        record.scope = produced.scope;
        const bool partialObservation =
            structuredObservation &&
            (structuredObservation->data.value(QStringLiteral("partialEvidence")).toBool() ||
             (structuredObservation->data.contains(QStringLiteral("complete")) &&
              (!structuredObservation->data.value(QStringLiteral("complete")).toBool() ||
               structuredObservation->data.value(QStringLiteral("truncated")).toBool() ||
               structuredObservation->data.value(QStringLiteral("cancelled")).toBool())));
        record.outcome = succeeded && partialObservation            ? EvidenceOutcome::Partial
                         : succeeded                                ? EvidenceOutcome::Verified
                         : status == ToolExecutionStatus::Cancelled ? EvidenceOutcome::Partial
                         : denied                                   ? EvidenceOutcome::Denied
                         : unavailable                              ? EvidenceOutcome::Unavailable
                                                                    : EvidenceOutcome::Failed;
        if (structuredObservation &&
            (descriptor.structuredObservationKind == structuredObservation->kind ||
             ((descriptor.source == ToolSource::BuiltIn ||
               descriptor.filesystemFailureSemanticContract) &&
              (structuredObservation->kind == StructuredObservationKind::FileSystemFailure ||
               structuredObservation->kind == StructuredObservationKind::DirectoryListing ||
               (descriptor.source == ToolSource::BuiltIn &&
                (structuredObservation->kind == StructuredObservationKind::FileSystemFact ||
                 structuredObservation->kind == StructuredObservationKind::PatchResult))))))
            record.structuredObservation = structuredObservation;
        record.observedAtUtc = QDateTime::currentDateTimeUtc();
        records.append(std::move(record));
    }
    return records;
}

EvidenceGateResult EvidencePolicy::evaluate(const ObservationIntent& intent,
                                            const QList<EvidenceRecord>& evidence,
                                            GroundingMode mode, bool groundingDeclared) {
    EvidenceGateResult result;
    result.grounding.mode = mode;
    if (intent.indeterminate) {
        result.repair = QStringLiteral("Observation need is unclear. Choose an appropriate "
                                       "observation tool before answering, or report inability "
                                       "to determine a valid action.");
        return result;
    }
    if (intent.requirements.isEmpty()) {
        if (mode == GroundingMode::UnableToVerify) {
            result.repair = QStringLiteral("No failed observation supports unable_to_verify.");
            return result;
        }
        if (mode == GroundingMode::Verified) {
            for (const auto& item : evidence)
                if (item.outcome == EvidenceOutcome::Verified)
                    result.grounding.evidenceCallIds.append(item.toolCallId);
            if (result.grounding.evidenceCallIds.isEmpty()) {
                result.repair = QStringLiteral("Verified grounding requires a successful "
                                               "current-run observation.");
                return result;
            }
        }
        result.accepted = true;
        return result;
    }
    if (!groundingDeclared) {
        result.repair = QStringLiteral("Declare final grounding as verified or "
                                       "unable_to_verify.");
        return result;
    }
    if (mode == GroundingMode::Context) {
        result.repair = QStringLiteral("This goal requires fresh external evidence. Use an "
                                       "appropriate observation tool before answering.");
        return result;
    }
    QStringList failures;
    for (const auto& requirement : intent.requirements) {
        const EvidenceRecord* latest = nullptr;
        for (const auto& item : evidence) {
            if (!matches(requirement, item))
                continue;
            if (!latest || item.stepIndex >= latest->stepIndex)
                latest = &item;
        }
        const bool observedPartialEntries =
            latest && latest->structuredObservation && requirement.claimType == ClaimType::None &&
            ((latest->structuredObservation->kind == StructuredObservationKind::DirectoryListing &&
              !latest->structuredObservation->data.value(QStringLiteral("entries"))
                   .toArray()
                   .isEmpty()) ||
             (latest->structuredObservation->kind == StructuredObservationKind::PathMatches &&
              !latest->structuredObservation->data.value(QStringLiteral("matches"))
                   .toArray()
                   .isEmpty()) ||
             (latest->structuredObservation->kind == StructuredObservationKind::FileContent &&
              !latest->structuredObservation->data.value(QStringLiteral("content"))
                   .toString()
                   .isEmpty()));
        const bool supportedPartial =
            latest && latest->outcome == EvidenceOutcome::Partial &&
            requirement.claimType != ClaimType::None &&
            ClaimGroundingResolver::resolve(requirement, evidence, {requirement.claimId, true})
                    .verdict == ClaimVerdict::Supported;
        if (latest && (latest->outcome == EvidenceOutcome::Verified || supportedPartial ||
                       (latest->outcome == EvidenceOutcome::Partial && observedPartialEntries))) {
            result.grounding.evidenceCallIds.append(latest->toolCallId);
            continue;
        }
        if (mode == GroundingMode::Verified) {
            result.repair = QStringLiteral("Verified answer lacks fresh %1 evidence for %2. "
                                           "Use an appropriate tool.")
                                .arg(observationDomainName(requirement.domain),
                                     requirement.resourceHint.isEmpty()
                                         ? QStringLiteral("the requested resource")
                                         : requirement.resourceHint);
            return result;
        }
        if (!latest) {
            result.repair = QStringLiteral("No observation attempt exists for required %1 "
                                           "evidence. Use an appropriate tool.")
                                .arg(observationDomainName(requirement.domain));
            return result;
        }
        result.grounding.evidenceCallIds.append(latest->toolCallId);
        failures.append(requirement.resourceHint.isEmpty()
                            ? observationDomainName(requirement.domain)
                            : requirement.resourceHint);
    }
    if (mode == GroundingMode::UnableToVerify) {
        if (failures.isEmpty()) {
            bool unresolvedClaim = false;
            for (const auto& requirement : intent.requirements)
                if (requirement.claimType != ClaimType::None &&
                    ClaimGroundingResolver::resolve(requirement, evidence,
                                                    {requirement.claimId, true})
                            .verdict == ClaimVerdict::Unknown)
                    unresolvedClaim = true;
            if (!unresolvedClaim) {
                result.repair = QStringLiteral(
                    "All required observations succeeded; provide a verified answer.");
                return result;
            }
            result.accepted = true;
            result.answerOverride = QStringLiteral(
                "I could not verify the requested fact from the available observation.");
            result.grounding.evidenceCallIds.removeDuplicates();
            return result;
        }
        // Do not publish the model's wording after failed observations: it could
        // claim presence or absence while declaring unable_to_verify.
        result.answerOverride = QStringLiteral("I could not verify the current state of %1 "
                                               "because the observation did not succeed.")
                                    .arg(failures.join(QStringLiteral(", ")));
    }
    result.grounding.evidenceCallIds.removeDuplicates();
    result.accepted = true;
    return result;
}
} // namespace sentinel::core
