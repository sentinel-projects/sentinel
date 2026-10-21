// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/app/OnboardingService.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/app/WorkspaceService.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace sentinel::core {
namespace {
const QStringList ids{QStringLiteral("welcome"), QStringLiteral("processing-mode"),
                      QStringLiteral("provider"), QStringLiteral("model"),
                      QStringLiteral("speech"), QStringLiteral("privacy"),
                      QStringLiteral("ready")};
}

OnboardingService::OnboardingService(AppSettings& settings, ModelService* models)
    : settings_(settings), models_(models) {}

OnboardingActionResult OnboardingService::persistedResult(const OnboardingSnapshot& state) const {
    const auto error = settings_.storageErrorCode();
    return error.isEmpty() ? OnboardingActionResult{true, {}, state}
                           : OnboardingActionResult{false, error, snapshot()};
}

QString OnboardingService::stepId(OnboardingStep step) {
    return ids.value(static_cast<int>(step), ids.first());
}

QStringList OnboardingService::stepIds() { return ids; }

OnboardingSnapshot OnboardingService::snapshot() const {
    OnboardingSnapshot state;
    state.complete = settings_.onboardingComplete();
    const auto document = QJsonDocument::fromJson(settings_.onboardingFlowJson().toUtf8());
    if (!document.isObject() || document.object().value(QStringLiteral("version")).toInt() != 1) {
        state.resumed = state.complete;
        return state;
    }
    const auto object = document.object();
    state.resumed = true;
    state.processingMode = object.value(QStringLiteral("processingMode")).toString(QStringLiteral("local"));
    if (!QStringList{QStringLiteral("local"), QStringLiteral("cloud"), QStringLiteral("hybrid")}
             .contains(state.processingMode)) state.processingMode = QStringLiteral("local");
    const auto index = ids.indexOf(object.value(QStringLiteral("step")).toString());
    state.step = static_cast<OnboardingStep>(index < 0 ? 0 : index);
    for (const auto& value : object.value(QStringLiteral("completedSteps")).toArray())
        if (value.isString() && ids.contains(value.toString()) &&
            !state.completedSteps.contains(value.toString()))
            state.completedSteps.append(value.toString());
    state.providerId = object.value(QStringLiteral("providerId")).toString();
    state.modelId = object.value(QStringLiteral("modelId")).toString();
    return state;
}

void OnboardingService::save(const OnboardingSnapshot& state) {
    QJsonArray completed;
    for (const auto& step : state.completedSteps) completed.append(step);
    const QJsonObject object{{QStringLiteral("version"), 1},
                             {QStringLiteral("step"), stepId(state.step)},
                             {QStringLiteral("processingMode"), state.processingMode},
                             {QStringLiteral("completedSteps"), completed},
                             {QStringLiteral("providerId"), state.providerId},
                             {QStringLiteral("modelId"), state.modelId}};
    settings_.setOnboardingFlowJson(
        QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

OnboardingActionResult OnboardingService::chooseProcessingMode(const QString& mode) {
    auto state = snapshot();
    if (!QStringList{QStringLiteral("local"), QStringLiteral("cloud"), QStringLiteral("hybrid")}
             .contains(mode)) return {false, QStringLiteral("onboarding.invalid-mode"), state};
    state.processingMode = mode;
    save(state);
    return persistedResult(state);
}

OnboardingActionResult OnboardingService::chooseModel(const QString& providerId,
                                                       const QString& modelId) {
    auto state = snapshot();
    if (!models_ || !models_->isKnownProvider(providerId) || modelId.trimmed().isEmpty() ||
        !models_->providerStatus(providerId).modelIds.contains(modelId))
        return {false, QStringLiteral("onboarding.model-unavailable"), state};
    if (state.processingMode == QLatin1String("local") &&
        models_->currentModelMetadata(providerId, modelId).providerKind == ProviderKind::Cloud)
        return {false, QStringLiteral("onboarding.cloud-blocked"), state};
    state.providerId = providerId;
    state.modelId = modelId;
    models_->setSelectedModel({providerId, modelId});
    if (!settings_.storageErrorCode().isEmpty()) {
        return persistedResult(state);
    }
    save(state);
    return persistedResult(state);
}

OnboardingActionResult OnboardingService::advance(bool skip) {
    auto state = snapshot();
    if (state.complete) return {false, QStringLiteral("onboarding.already-complete"), state};
    const auto index = static_cast<int>(state.step);
    if (index >= ids.size() - 1) return finish();
    if (state.step == OnboardingStep::Provider && models_ &&
        state.processingMode == QLatin1String("local")) {
        const auto selection = models_->selectedModel();
        if (!selection.providerId.isEmpty() &&
            models_->currentModelMetadata(selection.providerId, selection.modelId)
                .providerKind == ProviderKind::Cloud)
            return {false, QStringLiteral("onboarding.cloud-blocked"), state};
    }
    if (state.step == OnboardingStep::Model && models_ && !skip) {
        const auto selection = models_->selectedModel();
        if (selection.isValid()) {
            if (state.processingMode == QLatin1String("local") &&
                models_->currentModelMetadata(selection.providerId, selection.modelId)
                    .providerKind == ProviderKind::Cloud)
                return {false, QStringLiteral("onboarding.cloud-blocked"), state};
            state.providerId = selection.providerId;
            state.modelId = selection.modelId;
        }
    }
    if (!skip && !state.completedSteps.contains(ids.at(index)))
        state.completedSteps.append(ids.at(index));
    state.step = static_cast<OnboardingStep>(index + 1);
    save(state);
    return persistedResult(state);
}

OnboardingActionResult OnboardingService::back() {
    auto state = snapshot();
    const auto index = static_cast<int>(state.step);
    if (state.complete || index == 0)
        return {false, QStringLiteral("onboarding.no-previous-step"), state};
    state.step = static_cast<OnboardingStep>(index - 1);
    save(state);
    return persistedResult(state);
}

OnboardingActionResult OnboardingService::reopen() {
    auto state = snapshot();
    state.complete = false;
    state.step = OnboardingStep::Welcome;
    settings_.setOnboardingComplete(false);
    if (!settings_.storageErrorCode().isEmpty()) {
        return persistedResult(state);
    }
    save(state);
    return persistedResult(state);
}

OnboardingActionResult OnboardingService::finish() {
    auto state = snapshot();
    if (state.processingMode == QLatin1String("local")) {
        const WorkspaceService workspaces;
        const auto workspaceId = workspaces.normalizedWorkspaceId(
            settings_.selectedWorkspaceId(), settings_.workspaceCatalogJson());
        const auto raw = settings_.workspaceProfilesJson();
        const auto document = QJsonDocument::fromJson(raw.toUtf8());
        const auto entry = document.isObject()
            ? document.object().value(QStringLiteral("workspaces")).toObject()
                .value(workspaceId).toObject()
            : QJsonObject{};
        auto overrides = entry.value(QStringLiteral("overrides")).toObject();
        overrides.insert(QStringLiteral("privacy"), QStringLiteral("local-only"));
        settings_.setWorkspaceProfilesJson(workspaces.updateProfile(
            raw, workspaceId, entry.value(QStringLiteral("presetId")).toString(), overrides));
        if (!settings_.storageErrorCode().isEmpty()) {
            return persistedResult(state);
        }
    }
    state.step = OnboardingStep::Ready;
    if (!state.completedSteps.contains(stepId(OnboardingStep::Ready)))
        state.completedSteps.append(stepId(OnboardingStep::Ready));
    save(state);
    if (!settings_.storageErrorCode().isEmpty()) {
        return persistedResult(state);
    }
    settings_.setOnboardingComplete(true);
    state.complete = true;
    return persistedResult(state);
}
} // namespace sentinel::core
