// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/desktop/viewmodels/AgentInspectorViewModel.h"
#include "sentinel/core/app/ApplicationController.h"

#include "sentinel/core/agent/ObservationEvidence.h"
#include "sentinel/core/agent/ClaimGroundingResolver.h"

#include <QClipboard>
#include <QGuiApplication>
#include <algorithm>

namespace sentinel::desktop {
namespace {

QString duration(const QDateTime& start, const QDateTime& end) {
    if (!start.isValid()) return {};
    const auto milliseconds = start.msecsTo(end.isValid() ? end : QDateTime::currentDateTimeUtc());
    if (milliseconds < 0) return {};
    if (milliseconds < 1000) return QStringLiteral("%1 ms").arg(milliseconds);
    if (milliseconds < 60000) return QStringLiteral("%1 s").arg(milliseconds / 1000);
    return QStringLiteral("%1 min %2 s").arg(milliseconds / 60000).arg(milliseconds / 1000 % 60);
}

QVariantMap runRow(const sentinel::core::StoredAgentRun& run) {
    return {{QStringLiteral("runId"), run.runId},
            {QStringLiteral("sessionId"), run.sessionId},
            {QStringLiteral("parentRunId"), run.parentRunId},
            {QStringLiteral("parentToolCallId"), run.parentToolCallId},
            {QStringLiteral("runType"), run.runType},
            {QStringLiteral("role"), run.role},
            {QStringLiteral("state"), run.state},
            {QStringLiteral("completionStatus"),
             sentinel::core::agentCompletionStatusName(run.state, run.terminalReason)},
            {QStringLiteral("terminalReason"), run.terminalReason},
            {QStringLiteral("provider"), run.providerId},
            {QStringLiteral("model"), run.modelId},
            {QStringLiteral("goal"), run.goalSummary},
            {QStringLiteral("answer"), run.finalAnswer},
            {QStringLiteral("failure"), run.failure},
            {QStringLiteral("providerErrorCategory"), run.providerErrorCategory},
            {QStringLiteral("providerHttpStatus"), run.providerHttpStatus},
            {QStringLiteral("providerAttempts"), run.providerAttempts},
            {QStringLiteral("providerRetryOccurred"), run.providerRetryOccurred},
            {QStringLiteral("providerRequestLifecycle"), run.providerRequestLifecycle},
            {QStringLiteral("startedAt"), run.startedAt},
            {QStringLiteral("finishedAt"), run.finishedAt},
            {QStringLiteral("duration"), duration(run.startedAt, run.finishedAt)},
            {QStringLiteral("stepCount"), run.stepCount},
            {QStringLiteral("grounding"), run.groundingMode},
            {QStringLiteral("contextTokens"), run.contextTokens},
            {QStringLiteral("contextItems"), run.contextItems},
            {QStringLiteral("contextOmitted"), run.contextOmitted},
            {QStringLiteral("contextCompacted"), run.contextCompacted},
            {QStringLiteral("capabilitySnapshot"), run.capabilitySnapshot}};
}

QString outcomeName(int outcome) {
    using sentinel::core::EvidenceOutcome;
    switch (static_cast<EvidenceOutcome>(outcome)) {
    case EvidenceOutcome::Verified: return QStringLiteral("Verified");
    case EvidenceOutcome::Partial: return QStringLiteral("Partial");
    case EvidenceOutcome::Unavailable: return QStringLiteral("Unavailable");
    case EvidenceOutcome::Denied: return QStringLiteral("Denied");
    case EvidenceOutcome::Failed: return QStringLiteral("Failed");
    case EvidenceOutcome::Stale: return QStringLiteral("Stale");
    }
    return QStringLiteral("Unknown");
}

QString freshnessName(int freshness) {
    using sentinel::core::EvidenceFreshness;
    switch (static_cast<EvidenceFreshness>(freshness)) {
    case EvidenceFreshness::Live: return QStringLiteral("Live");
    case EvidenceFreshness::TurnScoped: return QStringLiteral("Turn");
    case EvidenceFreshness::SessionStable: return QStringLiteral("Session");
    }
    return QStringLiteral("Not recorded");
}

QString claimTypeName(int type) {
    using sentinel::core::ClaimType;
    switch (static_cast<ClaimType>(type)) {
    case ClaimType::None:
        return QStringLiteral("Claim");
    case ClaimType::PathExists:
        return QStringLiteral("Path exists");
    case ClaimType::FileExists:
        return QStringLiteral("File exists");
    case ClaimType::DirectoryExists:
        return QStringLiteral("Directory exists");
    case ClaimType::TextContains:
        return QStringLiteral("Text contains");
    case ClaimType::SearchHasMatches:
        return QStringLiteral("Search has matches");
    case ClaimType::HiddenEntriesExist:
        return QStringLiteral("Hidden entries exist");
    case ClaimType::PathPatternExists:
        return QStringLiteral("Path pattern exists");
    }
    return QStringLiteral("Claim");
}

QString verdictName(int verdict) {
    using sentinel::core::ClaimVerdict;
    switch (static_cast<ClaimVerdict>(verdict)) {
    case ClaimVerdict::Supported:
        return QStringLiteral("Supported");
    case ClaimVerdict::Contradicted:
        return QStringLiteral("Contradicted");
    case ClaimVerdict::Unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

} // namespace

int InspectorRowsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant InspectorRowsModel::data(const QModelIndex& index, int role) const {
    return index.isValid() && index.row() >= 0 && index.row() < rows_.size() && role == EntryRole
               ? QVariant(rows_.at(index.row())) : QVariant{};
}

QHash<int, QByteArray> InspectorRowsModel::roleNames() const {
    return {{EntryRole, "entry"}};
}

void InspectorRowsModel::setRows(QList<QVariantMap> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

AgentInspectorViewModel::AgentInspectorViewModel(sentinel::core::AgentInspectorService& service,
                                                 QObject* parent)
    : QObject(parent), service_(service) {
    refreshTimer_.setInterval(2500);
    connect(&refreshTimer_, &QTimer::timeout, this, &AgentInspectorViewModel::poll);
}

void AgentInspectorViewModel::setError(QString error) {
    if (errorMessage_ == error) return;
    errorMessage_ = std::move(error);
    emit errorMessageChanged();
}

void AgentInspectorViewModel::setActive(bool value) {
    if (active_ == value) return;
    active_ = value;
    if (active_) {
        if (loadedRuns_.isEmpty()) refresh();
        else poll();
        refreshTimer_.start();
    } else refreshTimer_.stop();
    emit activeChanged();
}

void AgentInspectorViewModel::refresh() {
    const auto page = service_.recentRuns(26);
    setError(service_.error());
    loadedRuns_ = page.mid(0, 25);
    hasMore_ = page.size() > 25;
    applyFilters();
    emit runsChanged();
    if (!selectedRunId_.isEmpty()) selectRun(selectedRunId_);
}

void AgentInspectorViewModel::loadMore() {
    if (!hasMore_ || loadedRuns_.isEmpty()) return;
    const auto& last = loadedRuns_.last();
    const auto page = service_.runsBefore(last.startedAt, last.runId, 26);
    setError(service_.error());
    for (const auto& run : page.mid(0, 25))
        loadedRuns_.append(run);
    hasMore_ = page.size() > 25;
    applyFilters();
    emit runsChanged();
}

void AgentInspectorViewModel::poll() {
    const auto recent = service_.recentRuns(25);
    setError(service_.error());
    for (int i = static_cast<int>(recent.size()) - 1; i >= 0; --i) {
        const auto& run = recent.at(i);
        auto found = std::find_if(loadedRuns_.begin(), loadedRuns_.end(),
                                  [&](const auto& current) { return current.runId == run.runId; });
        if (found != loadedRuns_.end()) *found = run;
        else loadedRuns_.prepend(run);
    }
    while (loadedRuns_.size() > 500) loadedRuns_.removeLast();
    applyFilters();
    if (!selectedRunId_.isEmpty()) selectRun(selectedRunId_);
}

void AgentInspectorViewModel::applyFilters() {
    QList<QVariantMap> rows;
    for (const auto& run : loadedRuns_) {
        if (!stateFilter_.isEmpty() && run.state != stateFilter_) continue;
        if (!typeFilter_.isEmpty() && run.runType != typeFilter_) continue;
        if (!providerFilter_.isEmpty() && !run.providerId.contains(providerFilter_, Qt::CaseInsensitive) &&
            !run.modelId.contains(providerFilter_, Qt::CaseInsensitive)) continue;
        if (!searchText_.isEmpty() && !run.goalSummary.contains(searchText_, Qt::CaseInsensitive) &&
            !run.finalAnswer.contains(searchText_, Qt::CaseInsensitive)) continue;
        rows.append(runRow(run));
    }
    runs_.setRows(std::move(rows));
}

void AgentInspectorViewModel::setStateFilter(QString value) {
    if (stateFilter_ == value) return;
    stateFilter_ = std::move(value); applyFilters(); emit filtersChanged();
}
void AgentInspectorViewModel::setTypeFilter(QString value) {
    if (typeFilter_ == value) return;
    typeFilter_ = std::move(value); applyFilters(); emit filtersChanged();
}
void AgentInspectorViewModel::setProviderFilter(QString value) {
    if (providerFilter_ == value) return;
    providerFilter_ = std::move(value); applyFilters(); emit filtersChanged();
}
void AgentInspectorViewModel::setSearchText(QString value) {
    value = value.left(100);
    if (searchText_ == value) return;
    searchText_ = std::move(value); applyFilters(); emit filtersChanged();
}

void AgentInspectorViewModel::selectRun(const QString& runId) {
    const auto detail = service_.detail(runId);
    setError(service_.error());
    selectedRunId_ = runId;
    selectedRun_ = detail.run.runId.isEmpty() ? QVariantMap{} : runRow(detail.run);
    if (!detail.run.runId.isEmpty() && controller_) {
        selectedRun_.insert(QStringLiteral("providerCatalogStatus"),
                            controller_->providerCatalogStatus(detail.run.providerId,
                                                               detail.run.modelId));
        selectedRun_.insert(QStringLiteral("providerCatalogDetail"),
                            controller_->providerCatalogDetail(detail.run.providerId,
                                                               detail.run.modelId));
    }
    emit selectedRunChanged();
    QList<QVariantMap> timeline;
    for (const auto& step : detail.steps) {
        timeline.append({{QStringLiteral("kind"), step.stepType == QLatin1String("planning")
                                                  ? QStringLiteral("Planning") : QStringLiteral("Step")},
                         {QStringLiteral("title"), QStringLiteral("Step %1 - %2").arg(step.sequence).arg(step.status)},
                         {QStringLiteral("detail"), step.stepId},
                         {QStringLiteral("time"), step.startedAt},
                         {QStringLiteral("duration"), duration(step.startedAt, step.finishedAt)},
                         {QStringLiteral("toolCallId"), step.toolCallId},
                         {QStringLiteral("observationKind"), step.observationKind},
                         {QStringLiteral("filesystemFailure"), step.filesystemFailure}});
    }
    for (const auto& tool : detail.tools) {
        timeline.append({{QStringLiteral("kind"), QStringLiteral("Tool")},
                         {QStringLiteral("title"), tool.toolId + QStringLiteral(" - ") + tool.status},
                         {QStringLiteral("detail"), tool.resourceSummary},
                         {QStringLiteral("time"), tool.startedAt},
                         {QStringLiteral("duration"), duration(tool.startedAt, tool.finishedAt)},
                         {QStringLiteral("toolCallId"), tool.toolCallId},
                         {QStringLiteral("source"), tool.source},
                         {QStringLiteral("observation"), tool.observationSummary},
                         {QStringLiteral("failure"), tool.failureCategory},
                         {QStringLiteral("mutation"), tool.mutationSummary},
                         {QStringLiteral("sandbox"), tool.sandboxSummary}});
        if (!tool.batchId.isEmpty())
            timeline.last().insert(QStringLiteral("batchId"), tool.batchId);
    }
    for (const auto& approval : detail.authorizations)
        timeline.append({{QStringLiteral("kind"), QStringLiteral("Approval")},
                         {QStringLiteral("title"), approval.domain + QLatin1Char('/') + approval.access},
                         {QStringLiteral("detail"), approval.decision + QStringLiteral(" - ") + approval.resourceSummary},
                         {QStringLiteral("toolCallId"), approval.toolCallId}});
    for (const auto& item : detail.evidence)
        timeline.append({{QStringLiteral("kind"), QStringLiteral("Evidence")},
                         {QStringLiteral("title"), item.domain + QStringLiteral(" - ") + outcomeName(item.outcome)},
                         {QStringLiteral("detail"), item.resourceSummary},
                         {QStringLiteral("toolCallId"), item.toolCallId}});
    for (const auto& item : detail.claims)
        timeline.append({{QStringLiteral("kind"), QStringLiteral("Claim")},
                         {QStringLiteral("title"), item.claimId + QStringLiteral(" - ") + verdictName(item.verdict)},
                         {QStringLiteral("detail"), item.resourceSummary}});
    if (!detail.run.state.isEmpty())
        timeline.append({{QStringLiteral("kind"), QStringLiteral("Result")},
                         {QStringLiteral("title"), detail.run.state},
                         {QStringLiteral("detail"), detail.run.failure.isEmpty()
                              ? detail.run.groundingMode : detail.run.failure},
                         {QStringLiteral("time"), detail.run.finishedAt}});
    std::stable_sort(timeline.begin(), timeline.end(), [](const QVariantMap& left, const QVariantMap& right) {
        const auto a = left.value(QStringLiteral("time")).toDateTime();
        const auto b = right.value(QStringLiteral("time")).toDateTime();
        if (a.isValid() != b.isValid()) return a.isValid();
        return a.isValid() && a < b;
    });
    timeline_.setRows(std::move(timeline));
    QList<QVariantMap> evidence;
    for (const auto& item : detail.evidence)
        evidence.append({{QStringLiteral("domain"), item.domain},
                         {QStringLiteral("resource"), item.resourceSummary},
                         {QStringLiteral("outcome"), outcomeName(item.outcome)},
                         {QStringLiteral("freshness"), freshnessName(item.freshness)},
                         {QStringLiteral("toolCallId"), item.toolCallId}});
    evidence_.setRows(std::move(evidence));
    QList<QVariantMap> claims;
    for (const auto& item : detail.claims)
        claims.append({{QStringLiteral("claimId"), item.claimId},
                       {QStringLiteral("type"), claimTypeName(item.claimType)},
                       {QStringLiteral("resource"), item.resourceSummary},
                       {QStringLiteral("assertion"), item.assertionValue},
                       {QStringLiteral("verdict"), verdictName(item.verdict)},
                       {QStringLiteral("support"), item.supportingToolCallIds.join(QStringLiteral(", "))}});
    claims_.setRows(std::move(claims));
    QList<QVariantMap> approvals;
    for (const auto& item : detail.authorizations)
        approvals.append({{QStringLiteral("domain"), item.domain},
                          {QStringLiteral("access"), item.access},
                          {QStringLiteral("resource"), item.resourceSummary},
                          {QStringLiteral("decision"), item.decision},
                          {QStringLiteral("scope"), item.scope.isEmpty()
                              ? QStringLiteral("Not recorded") : item.scope},
                          {QStringLiteral("toolCallId"), item.toolCallId}});
    approvals_.setRows(std::move(approvals));
    QList<QVariantMap> children;
    for (const auto& child : detail.children)
        children.append(runRow(child));
    children_.setRows(std::move(children));
}

void AgentInspectorViewModel::copySafe(const QString& field, const QString& value) {
    if (field == QLatin1String("runId") || field == QLatin1String("sessionId") ||
        field == QLatin1String("toolCallId") || field == QLatin1String("failure"))
        QGuiApplication::clipboard()->setText(value.left(240));
}

} // namespace sentinel::desktop
