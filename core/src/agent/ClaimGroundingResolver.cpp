// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/agent/ClaimGroundingResolver.h"
#include "sentinel/core/agent/ObservationPolicy.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <optional>

namespace sentinel::core {
namespace {
QString literal(const QString& value) {
    const auto encoded =
        QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
    return encoded.mid(1, encoded.size() - 2);
}
QString path(const QString& value) {
    return normalizedObservationResource(value, ObservationDomain::FileSystem);
}
std::optional<bool> observed(const ObservationRequirement& claim,
                             const StructuredObservation& observation) {
    const auto data = observation.data;
    const auto target = path(claim.resourceHint);
    const auto coverage = FileSystemCoverage::fromObservation(observation);
    const bool complete = coverage.supportsAbsence(false, false);
    if (claim.claimType == ClaimType::HiddenEntriesExist ||
        claim.claimType == ClaimType::PathPatternExists) {
        if (path(coverage.scope) != target || target.isEmpty())
            return {};
        if (claim.claimType == ClaimType::HiddenEntriesExist) {
            for (const auto& entry : data.value(QStringLiteral("hiddenEntries")).toArray()) {
                const auto observedPath = path(entry.toString());
                if ((claim.recursive && (observedPath.startsWith(target + QLatin1Char('/')))) ||
                    (!claim.recursive && QFileInfo(observedPath).absolutePath() == target))
                    return true;
            }
            if (observation.kind == StructuredObservationKind::DirectoryListing) {
                for (const auto& entry : data.value(QStringLiteral("entries")).toArray())
                    if (entry.toObject()
                            .value(QStringLiteral("name"))
                            .toString()
                            .startsWith(QLatin1Char('.')))
                        return true;
            } else if (observation.kind != StructuredObservationKind::PathMatches)
                return {};
            // Recursive traversal reports hidden directories too, including empty ones.
            // Filtered matches alone cannot prove the absence of all hidden entries.
            if (!data.value(QStringLiteral("hiddenEntries")).isArray())
                return {};
            return coverage.supportsAbsence(true, claim.recursive) ? std::optional<bool>{false}
                                                                   : std::optional<bool>{};
        }
        if (observation.kind == StructuredObservationKind::DirectoryListing) {
            for (const auto& entry : data.value(QStringLiteral("entries")).toArray()) {
                const auto object = entry.toObject();
                if (object.value(QStringLiteral("type")).toString() == QLatin1String("file") &&
                    QDir::match(claim.claimQuery, object.value(QStringLiteral("name")).toString()))
                    return true;
            }
            return coverage.supportsAbsence(true, claim.recursive) ? std::optional<bool>{false}
                                                                   : std::optional<bool>{};
        }
        if (observation.kind != StructuredObservationKind::PathMatches ||
            data.value(QStringLiteral("pattern")).toString() != claim.claimQuery)
            return {};
        if (!data.value(QStringLiteral("matches")).toArray().isEmpty())
            return true;
        return coverage.supportsAbsence(true, claim.recursive) ? std::optional<bool>{false}
                                                               : std::optional<bool>{};
    }
    const bool existence = claim.claimType == ClaimType::PathExists ||
                           claim.claimType == ClaimType::FileExists ||
                           claim.claimType == ClaimType::DirectoryExists;
    if (existence) {
        if (observation.kind == StructuredObservationKind::FileSystemFact &&
            path(data.value(QStringLiteral("path")).toString()) == target) {
            if (claim.claimType == ClaimType::PathExists)
                return data.value(QStringLiteral("exists")).toBool();
            if (claim.claimType == ClaimType::FileExists)
                return data.value(QStringLiteral("file")).toBool();
            if (claim.claimType == ClaimType::DirectoryExists)
                return data.value(QStringLiteral("directory")).toBool();
        }
        if (observation.kind == StructuredObservationKind::FileSystemFailure &&
            path(observation.failureResource) == target) {
            switch (observation.fileSystemFailure) {
            case FileSystemFailure::NotFound:
                return false;
            case FileSystemFailure::NotFile:
                if (claim.claimType == ClaimType::PathExists)
                    return true;
                if (claim.claimType == ClaimType::FileExists)
                    return false;
                return {};
            case FileSystemFailure::NotDirectory:
                if (claim.claimType == ClaimType::PathExists)
                    return true;
                if (claim.claimType == ClaimType::DirectoryExists)
                    return false;
                return {};
            case FileSystemFailure::AlreadyExists:
                if (claim.claimType == ClaimType::PathExists)
                    return true;
                return {};
            default:
                return {};
            }
        }
        if (observation.kind == StructuredObservationKind::DirectoryListing) {
            const auto listed = path(data.value(QStringLiteral("path")).toString());
            if (listed == target)
                return claim.claimType != ClaimType::FileExists;
            if (listed != QFileInfo(target).absolutePath())
                return {};
            for (const auto& entry : data.value(QStringLiteral("entries")).toArray()) {
                const auto object = entry.toObject();
                if (object.value(QStringLiteral("name")).toString() != QFileInfo(target).fileName())
                    continue;
                if (claim.claimType == ClaimType::PathExists)
                    return true;
                const auto type = object.value(QStringLiteral("type")).toString();
                return claim.claimType == ClaimType::DirectoryExists
                           ? type == QLatin1String("directory")
                           : type == QLatin1String("file");
            }
            if (QFileInfo(target).fileName().startsWith(QLatin1Char('.')) &&
                coverage.hiddenEntriesPolicy != HiddenEntriesPolicy::Included)
                return {};
            return complete ? std::optional<bool>{false} : std::optional<bool>{};
        }
        if (observation.kind == StructuredObservationKind::FileContent &&
            path(data.value(QStringLiteral("path")).toString()) == target)
            return claim.claimType != ClaimType::DirectoryExists;
        if (observation.kind == StructuredObservationKind::PathMatches &&
            claim.claimType != ClaimType::DirectoryExists) {
            const auto pattern = data.value(QStringLiteral("pattern")).toString();
            if (pattern != QFileInfo(target).fileName() ||
                path(data.value(QStringLiteral("root")).toString()) !=
                    QFileInfo(target).absolutePath())
                return {};
            for (const auto& item : data.value(QStringLiteral("matches")).toArray())
                if (path(item.toString()) == target)
                    return true;
            if (QFileInfo(target).fileName().startsWith(QLatin1Char('.')) &&
                coverage.hiddenEntriesPolicy != HiddenEntriesPolicy::Included)
                return {};
            return complete ? std::optional<bool>{false} : std::optional<bool>{};
        }
    }
    if (claim.claimType == ClaimType::TextContains &&
        observation.kind == StructuredObservationKind::FileContent &&
        path(data.value(QStringLiteral("path")).toString()) == target) {
        if (data.value(QStringLiteral("content")).toString().contains(claim.claimQuery))
            return true;
        return complete ? std::optional<bool>{false} : std::optional<bool>{};
    }
    if (claim.claimType == ClaimType::SearchHasMatches &&
        observation.kind == StructuredObservationKind::TextSearch &&
        path(data.value(QStringLiteral("scope")).toString()) == target &&
        data.value(QStringLiteral("query")).toString() == claim.claimQuery) {
        if (data.value(QStringLiteral("matchCount")).toInt() > 0)
            return true;
        return coverage.supportsAbsence(data.value(QStringLiteral("recursive")).toBool(),
                                        claim.recursive)
                   ? std::optional<bool>{false}
                   : std::optional<bool>{};
    }
    return {};
}
} // namespace
FileSystemCoverage FileSystemCoverage::fromObservation(const StructuredObservation& observation) {
    const auto& data = observation.data;
    FileSystemCoverage result;
    result.enumeration = observation.kind == StructuredObservationKind::DirectoryListing ||
                         observation.kind == StructuredObservationKind::PathMatches ||
                         observation.kind == StructuredObservationKind::TextSearch;
    result.scope = data.value(QStringLiteral("scope")).toString();
    if (result.scope.isEmpty())
        result.scope = data.value(QStringLiteral("root")).toString();
    if (result.scope.isEmpty())
        result.scope = data.value(QStringLiteral("path")).toString();
    const auto hidden = data.value(QStringLiteral("hiddenEntriesPolicy")).toString();
    if (hidden == QLatin1String("included"))
        result.hiddenEntriesPolicy = HiddenEntriesPolicy::Included;
    else if (hidden == QLatin1String("excluded"))
        result.hiddenEntriesPolicy = HiddenEntriesPolicy::Excluded;
    result.complete = data.value(QStringLiteral("complete")).toBool(false);
    result.truncated = data.value(QStringLiteral("truncated")).toBool(true);
    result.cancelled = data.value(QStringLiteral("cancelled")).toBool(true);
    result.permissionLimited = data.value(QStringLiteral("permissionLimited")).toBool(true) ||
                               !data.value(QStringLiteral("issues")).isArray() ||
                               !data.value(QStringLiteral("issues")).toArray().isEmpty();
    result.recursive = data.value(QStringLiteral("recursive")).toBool(false);
    result.followSymlinks = data.value(QStringLiteral("followSymlinks")).toBool(false);
    result.symlinkPolicyKnown = data.value(QStringLiteral("followSymlinks")).isBool() &&
                                data.value(QStringLiteral("skippedSymlinks")).isDouble() &&
                                data.value(QStringLiteral("skippedSymlinks")).toDouble(-1) ==
                                    data.value(QStringLiteral("skippedSymlinks")).toInt(-2) &&
                                data.value(QStringLiteral("skippedSymlinks")).toInt(-1) >= 0;
    result.skippedSymlinks = data.value(QStringLiteral("skippedSymlinks")).toInt(0);
    result.maxDepth = data.value(QStringLiteral("maxDepth")).toInt(0);
    result.resultLimit = data.value(QStringLiteral("resultLimit")).toInt(0);
    return result;
}
bool FileSystemCoverage::supportsAbsence(bool includeHidden, bool requireRecursive) const {
    return complete && !truncated && !cancelled && !permissionLimited &&
           (!enumeration || (symlinkPolicyKnown && skippedSymlinks == 0)) &&
           (!recursive || (symlinkPolicyKnown && skippedSymlinks == 0)) &&
           (!includeHidden ||
            (hiddenEntriesPolicy == HiddenEntriesPolicy::Included && resultLimit > 0)) &&
           (!requireRecursive || (recursive && maxDepth > 0 && resultLimit > 0 &&
                                  symlinkPolicyKnown && skippedSymlinks == 0)) &&
           !scope.isEmpty();
}
ClaimResolution ClaimGroundingResolver::resolve(const ObservationRequirement& claim,
                                                const QList<EvidenceRecord>& evidence,
                                                const ClaimAssertion& assertion) {
    ClaimResolution result;
    result.fact.id = claim.claimId;
    result.fact.resource = claim.resourceHint;
    if (claim.claimType == ClaimType::None || assertion.id != claim.claimId)
        return result;
    int latest = -1;
    for (const auto& item : evidence) {
        if ((item.outcome != EvidenceOutcome::Verified &&
             item.outcome != EvidenceOutcome::Partial &&
             !(item.outcome == EvidenceOutcome::Failed && item.structuredObservation &&
               item.structuredObservation->kind == StructuredObservationKind::FileSystemFailure)) ||
            !item.structuredObservation || item.stepIndex < latest)
            continue;
        const auto value = observed(claim, *item.structuredObservation);
        if (!value) {
            const auto coverage = FileSystemCoverage::fromObservation(*item.structuredObservation);
            if ((claim.claimType == ClaimType::HiddenEntriesExist ||
                 claim.claimType == ClaimType::PathPatternExists) &&
                path(coverage.scope) == path(claim.resourceHint) && item.stepIndex >= latest) {
                latest = item.stepIndex;
                result.fact.evidenceCallIds.clear();
                result.verdict = ClaimVerdict::Unknown;
            }
            continue;
        }
        if (item.stepIndex > latest)
            result.fact.evidenceCallIds.clear();
        latest = item.stepIndex;
        result.fact.value = *value;
        result.fact.evidenceCallIds.append(item.toolCallId);
    }
    if (latest >= 0 && !result.fact.evidenceCallIds.isEmpty())
        result.verdict = result.fact.value == assertion.value ? ClaimVerdict::Supported
                                                              : ClaimVerdict::Contradicted;
    return result;
}
QList<StructuredFact> ClaimGroundingResolver::facts(const ObservationIntent& intent,
                                                    const QList<EvidenceRecord>& evidence) {
    QList<StructuredFact> result;
    for (const auto& claim : intent.requirements) {
        if (claim.claimType == ClaimType::None)
            continue;
        auto resolved = resolve(claim, evidence, {claim.claimId, true});
        if (resolved.verdict != ClaimVerdict::Unknown)
            result.append(resolved.fact);
    }
    return result;
}
std::optional<EvidenceExplanation>
ClaimGroundingResolver::filesystemExplanation(const ObservationIntent& intent,
                                              const QList<EvidenceRecord>& evidence,
                                              const QString& proposed) {
    if (intent.indeterminate || proposed.size() > 16384)
        return {};
    // Symbolic existence/absence claims retain the stricter fact projection.
    for (const auto& requirement : intent.requirements)
        if (requirement.claimType != ClaimType::None ||
            requirement.purpose == ObservationPurpose::Operate)
            return {};
    for (const auto& item : evidence)
        if (item.scope == EvidenceScope::Operation)
            return {};
    const auto document = QJsonDocument::fromJson(proposed.toUtf8());
    if (!document.isObject() || document.object().size() != 1 ||
        !document.object().value(QStringLiteral("explanations")).isArray())
        return {};
    const auto blocks = document.object().value(QStringLiteral("explanations")).toArray();
    if (blocks.isEmpty() || blocks.size() > 8)
        return {};
    QJsonArray canonical;
    QStringList rendered;
    for (const auto& value : blocks) {
        const auto block = value.toObject();
        const auto interpretation =
            block.value(QStringLiteral("interpretation")).toString().trimmed();
        const auto sources = block.value(QStringLiteral("evidence")).toArray();
        if (block.size() != 2 || interpretation.isEmpty() || interpretation.size() > 1500 ||
            sources.isEmpty() || sources.size() > 4)
            return {};
        for (const auto ch : interpretation)
            if (ch.unicode() < 32 && ch != QLatin1Char('\n') && ch != QLatin1Char('\t'))
                return {};
        QJsonArray citations;
        QStringList sourceText;
        for (const auto& sourceValue : sources) {
            const auto source = sourceValue.toObject();
            const auto id = source.value(QStringLiteral("call_id")).toString();
            auto quote = source.value(QStringLiteral("quote")).toString();
            if (source.size() < 2 || source.size() > 3 || id.isEmpty() ||
                quote.trimmed().isEmpty() || quote.size() > 512)
                return {};
            const EvidenceRecord* matched = nullptr;
            for (const auto& item : evidence)
                if (item.toolCallId == id && item.outcome == EvidenceOutcome::Verified &&
                    item.structuredObservation &&
                    item.structuredObservation->kind == StructuredObservationKind::FileContent &&
                    (item.domain == ObservationDomain::FileSystem ||
                     item.domain == ObservationDomain::Workspace))
                    matched = &item;
            if (!matched)
                return {};
            // Do not cite an old read when a later observation changed that resource.
            for (const auto& item : evidence)
                if (item.resource == matched->resource && item.stepIndex > matched->stepIndex)
                    return {};
            const auto data = matched->structuredObservation->data;
            const auto resource = data.value(QStringLiteral("path")).toString();
            const auto observed = data.value(QStringLiteral("content")).toString();
            if (!observed.contains(quote)) {
                // Some native model replies double-escape whitespace inside JSON
                // quotes. Repair representation only when the decoded quote is
                // still an exact substring of this current verified observation.
                auto decoded = quote;
                decoded.replace(QStringLiteral("\\r"), QStringLiteral("\r"));
                decoded.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
                decoded.replace(QStringLiteral("\\t"), QStringLiteral("\t"));
                if (!observed.contains(decoded))
                    return {};
                quote = decoded;
            }
            if (resource.isEmpty() || !observed.contains(quote) ||
                (source.contains(QStringLiteral("source")) &&
                 source.value(QStringLiteral("source")).toString() != resource))
                return {};
            citations.append(QJsonObject{{QStringLiteral("call_id"), id},
                                         {QStringLiteral("source"), resource},
                                         {QStringLiteral("quote"), quote}});
            sourceText.append(QStringLiteral("Source %1 [%2], observed quote: %3")
                                  .arg(literal(resource), literal(id), literal(quote)));
        }
        canonical.append(QJsonObject{{QStringLiteral("interpretation"), interpretation},
                                     {QStringLiteral("evidence"), citations}});
        rendered.append(QStringLiteral("Interpretation: %1\n%2")
                            .arg(interpretation, sourceText.join(QLatin1Char('\n'))));
    }
    return EvidenceExplanation{
        QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("explanations"), canonical}})
                              .toJson(QJsonDocument::Compact)),
        rendered.join(QStringLiteral("\n\n"))};
}

bool ClaimGroundingResolver::canonicalObservationRequired(const QList<EvidenceRecord>& evidence) {
    for (const auto& item : evidence) {
        if (!item.structuredObservation)
            continue;
        const auto kind = item.structuredObservation->kind;
        if (kind != StructuredObservationKind::DirectoryListing &&
            kind != StructuredObservationKind::PathMatches &&
            kind != StructuredObservationKind::TextSearch)
            continue;
        const auto coverage = FileSystemCoverage::fromObservation(*item.structuredObservation);
        if (kind == StructuredObservationKind::DirectoryListing &&
            coverage.hiddenEntriesPolicy != HiddenEntriesPolicy::Included)
            return true;
        if (!coverage.supportsAbsence(coverage.hiddenEntriesPolicy == HiddenEntriesPolicy::Included,
                                      coverage.recursive))
            return true;
    }
    return false;
}

std::optional<QString>
ClaimGroundingResolver::filesystemFinalAnswer(const ObservationIntent& intent,
                                              const QList<EvidenceRecord>& evidence,
                                              const QString& proposed) {
    // An explanation cites a read, but it cannot widen a listing/search scope
    // whose hidden, recursive, or completion coverage is insufficient. In that
    // case the evidence-derived observation representation is authoritative.
    if (!canonicalObservationRequired(evidence)) {
        if (const auto explanation = filesystemExplanation(intent, evidence, proposed))
            return explanation->canonical;
    }
    const bool codingOperation =
        !intent.indeterminate &&
        std::any_of(intent.requirements.cbegin(), intent.requirements.cend(),
                    [](const auto& r) {
                        return r.domain == ObservationDomain::FileSystem ||
                               r.domain == ObservationDomain::Workspace;
                    }) &&
        std::any_of(intent.requirements.cbegin(), intent.requirements.cend(),
                    [](const auto& r) { return r.purpose == ObservationPurpose::Operate; });
    if (codingOperation) {
        QJsonArray operations;
        QJsonArray executions;
        QJsonArray observations;
        QJsonArray inspections;
        QSet<QString> seen;
        for (auto it = evidence.crbegin(); it != evidence.crend(); ++it) {
            if (it->scope == EvidenceScope::Operation && it->outcome == EvidenceOutcome::Verified &&
                (it->domain == ObservationDomain::FileSystem ||
                 it->domain == ObservationDomain::Workspace) &&
                operations.size() < 8)
                operations.append(
                    QJsonObject{{QStringLiteral("tool"), it->toolId},
                                {QStringLiteral("call_id"), it->toolCallId},
                                {QStringLiteral("resource"), it->resource},
                                {QStringLiteral("outcome"), QStringLiteral("verified_operation")}});
            if (!it->structuredObservation)
                continue;
            const auto& data = it->structuredObservation->data;
            if (it->toolId == QLatin1String("run-command") &&
                it->domain == ObservationDomain::ProcessExecution && executions.size() < 8 &&
                it->structuredObservation->kind == StructuredObservationKind::Generic &&
                data.value(QStringLiteral("exitCode")).isDouble()) {
                const auto out = data.value(QStringLiteral("stdout")).toString();
                const auto err = data.value(QStringLiteral("stderr")).toString();
                executions.append(QJsonObject{
                    {QStringLiteral("call_id"), it->toolCallId},
                    {QStringLiteral("command"), it->resource},
                    {QStringLiteral("exit_code"), data.value(QStringLiteral("exitCode"))},
                    {QStringLiteral("stdout"), out.left(4096)},
                    {QStringLiteral("stderr"), err.left(4096)},
                    {QStringLiteral("timed_out"), data.value(QStringLiteral("timedOut"))},
                    {QStringLiteral("cancelled"), data.value(QStringLiteral("cancelled"))},
                    {QStringLiteral("complete"), data.value(QStringLiteral("complete")).toBool() &&
                                                     out.size() <= 4096 && err.size() <= 4096}});
            }
            if (it->domain == ObservationDomain::Workspace &&
                it->outcome == EvidenceOutcome::Verified &&
                (it->toolId == QLatin1String("git-diff") ||
                 it->toolId == QLatin1String("git-status")) &&
                inspections.size() < 8) {
                const auto output = data.value(QStringLiteral("stdout")).toString();
                inspections.append(QJsonObject{
                    {QStringLiteral("tool"), it->toolId},
                    {QStringLiteral("call_id"), it->toolCallId},
                    {QStringLiteral("resource"), it->resource},
                    {QStringLiteral("observed_output"), output.left(4096)},
                    {QStringLiteral("complete"),
                     data.value(QStringLiteral("complete")).toBool() && output.size() <= 4096}});
            }
            if (it->domain != ObservationDomain::FileSystem &&
                it->domain != ObservationDomain::Workspace)
                continue;
            if (it->scope == EvidenceScope::Operation || seen.contains(it->resource))
                continue;
            seen.insert(it->resource);
            if (it->outcome == EvidenceOutcome::Verified && observations.size() < 8 &&
                it->structuredObservation->kind == StructuredObservationKind::FileContent)
                observations.append(
                    QJsonObject{{QStringLiteral("call_id"), it->toolCallId},
                                {QStringLiteral("source"), data.value(QStringLiteral("path"))},
                                {QStringLiteral("observed_text"),
                                 data.value(QStringLiteral("content")).toString().left(4096)}});
        }
        if (!operations.isEmpty() || !executions.isEmpty())
            return QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("operations"), operations},
                                          {QStringLiteral("executions"), executions},
                                          {QStringLiteral("observations"), observations},
                                          {QStringLiteral("inspections"), inspections}})
                    .toJson(QJsonDocument::Compact));
    }
    QStringList facts;
    QStringList supportingCalls;
    for (const auto& requirement : intent.requirements) {
        if (requirement.claimType == ClaimType::None ||
            (requirement.domain != ObservationDomain::FileSystem &&
             requirement.domain != ObservationDomain::Workspace))
            continue;
        const auto resolved = resolve(requirement, evidence, {requirement.claimId, true});
        if (resolved.verdict == ClaimVerdict::Unknown)
            continue;
        supportingCalls.append(resolved.fact.evidenceCallIds);
        QString subject;
        switch (requirement.claimType) {
        case ClaimType::HiddenEntriesExist:
            subject = QStringLiteral("Hidden entries");
            break;
        case ClaimType::PathPatternExists:
            subject = QStringLiteral("Files matching %1").arg(literal(requirement.claimQuery));
            break;
        case ClaimType::SearchHasMatches:
            subject = QStringLiteral("Matches for %1").arg(literal(requirement.claimQuery));
            break;
        case ClaimType::TextContains:
            subject = QStringLiteral("Text %1").arg(literal(requirement.claimQuery));
            break;
        case ClaimType::FileExists:
            subject = QStringLiteral("File");
            break;
        case ClaimType::DirectoryExists:
            subject = QStringLiteral("Directory");
            break;
        default:
            subject = QStringLiteral("Path");
            break;
        }
        facts.append(
            QStringLiteral("%1: %2 in %3 (%4; symbolic link targets excluded).")
                .arg(subject,
                     resolved.fact.value ? QStringLiteral("found") : QStringLiteral("not found"),
                     literal(requirement.resourceHint),
                     resolved.fact.value
                         ? QStringLiteral("positive observation; no exhaustive absence inference")
                     : requirement.recursive ? QStringLiteral("complete recursive scope")
                                             : QStringLiteral("complete exact scope")));
    }
    if (!facts.isEmpty()) {
        for (const auto& item : evidence) {
            if (!item.structuredObservation || !supportingCalls.contains(item.toolCallId))
                continue;
            const auto& data = item.structuredObservation->data;
            if (item.structuredObservation->kind == StructuredObservationKind::FileContent)
                facts.append(QString::fromUtf8(
                    QJsonDocument(
                        QJsonObject{{QStringLiteral("source"), data.value(QStringLiteral("path"))},
                                    {QStringLiteral("observed_text"),
                                     data.value(QStringLiteral("content")).toString().left(4096)}})
                        .toJson(QJsonDocument::Compact)));
            if (item.structuredObservation->kind == StructuredObservationKind::DirectoryListing)
                for (const auto& entry : data.value(QStringLiteral("entries")).toArray()) {
                    const auto name = entry.toObject().value(QStringLiteral("name")).toString();
                    if (facts.size() < 604 && !facts.contains(literal(name)))
                        facts.append(literal(name));
                }
            if (item.structuredObservation->kind == StructuredObservationKind::PathMatches)
                for (const auto& entry : data.value(QStringLiteral("matches")).toArray())
                    if (facts.size() < 204 && !facts.contains(literal(entry.toString())))
                        facts.append(literal(entry.toString()));
            for (const auto& hidden : data.value(QStringLiteral("hiddenEntries")).toArray())
                if (facts.size() < 604 && !facts.contains(literal(hidden.toString())))
                    facts.append(literal(hidden.toString()));
        }
        return facts.join(QLatin1Char('\n'));
    }
    for (auto it = evidence.crbegin(); it != evidence.crend(); ++it) {
        if (!it->structuredObservation ||
            (it->outcome != EvidenceOutcome::Verified && it->outcome != EvidenceOutcome::Partial))
            continue;
        const auto& observation = *it->structuredObservation;
        const auto& data = observation.data;
        if (observation.kind == StructuredObservationKind::DirectoryListing ||
            observation.kind == StructuredObservationKind::PathMatches) {
            const auto coverage = FileSystemCoverage::fromObservation(observation);
            QStringList entries;
            if (observation.kind == StructuredObservationKind::DirectoryListing) {
                for (const auto& item : data.value(QStringLiteral("entries")).toArray())
                    entries.append(
                        literal(item.toObject().value(QStringLiteral("name")).toString()));
            } else {
                for (const auto& item : data.value(QStringLiteral("matches")).toArray())
                    entries.append(literal(item.toString()));
                for (const auto& item : data.value(QStringLiteral("hiddenEntries")).toArray())
                    if (!entries.contains(literal(item.toString())))
                        entries.append(literal(item.toString()));
            }
            return QStringLiteral("Observed entries in %1 (%2; hidden entries %3; symbolic link "
                                  "targets excluded; completeness %4):\n%5")
                .arg(literal(coverage.scope),
                     coverage.recursive ? QStringLiteral("recursive") : QStringLiteral("root only"),
                     coverage.hiddenEntriesPolicy == HiddenEntriesPolicy::Included
                         ? QStringLiteral("included")
                     : coverage.hiddenEntriesPolicy == HiddenEntriesPolicy::Excluded
                         ? QStringLiteral("excluded")
                         : QStringLiteral("unknown"),
                     coverage.supportsAbsence(false, coverage.recursive)
                         ? QStringLiteral("complete")
                         : QStringLiteral("limited"),
                     entries.isEmpty()
                         ? QStringLiteral(
                               "No entries returned within this observation's stated scope.")
                         : entries.join(QLatin1Char('\n')));
        }
        if (observation.kind == StructuredObservationKind::FileContent) {
            const auto content = data.value(QStringLiteral("content")).toString();
            const auto source = data.value(QStringLiteral("path")).toString();
            QString excerpt = content.left(4096);
            const auto candidate = proposed.size() <= 16384
                                       ? QJsonDocument::fromJson(proposed.toUtf8())
                                       : QJsonDocument{};
            if (candidate.isObject() && candidate.object().size() == 2 &&
                candidate.object().value(QStringLiteral("source")).toString() == source &&
                candidate.object().value(QStringLiteral("observed_text")).isString()) {
                const auto text =
                    candidate.object().value(QStringLiteral("observed_text")).toString();
                if (text.size() <= 4096 && content.contains(text))
                    excerpt = text;
            } else if (!proposed.trimmed().isEmpty() && proposed.size() <= 4096 &&
                       content.contains(proposed)) {
                excerpt = proposed;
            }
            // This is a literal extract, not an assertion about the directory or
            // workspace. Preserve that distinction even for adversarial file text.
            return QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("source"), source},
                                          {QStringLiteral("observed_text"), excerpt}})
                    .toJson(QJsonDocument::Compact));
        }
        if (observation.kind == StructuredObservationKind::TextSearch ||
            observation.kind == StructuredObservationKind::CodeDefinitions)
            return QStringLiteral("Observed tool result (no broader absence claim):\n%1")
                .arg(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)));
    }
    for (const auto& item : evidence) {
        if ((item.domain == ObservationDomain::FileSystem ||
             item.domain == ObservationDomain::Workspace) &&
            item.scope != EvidenceScope::Operation &&
            (item.outcome == EvidenceOutcome::Verified || item.outcome == EvidenceOutcome::Partial))
            return QStringLiteral("Filesystem observation from %1 at %2: typed coverage "
                                  "unavailable; no absence conclusion is supported.")
                .arg(literal(item.toolId), literal(item.resource));
    }
    return {};
}
} // namespace sentinel::core
