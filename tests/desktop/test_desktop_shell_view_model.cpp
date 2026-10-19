// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/desktop/DesktopShellViewModel.h"

#include "../support/DeterministicChatFixture.h"
#include "sentinel/core/app/ApplicationControllerBuilder.h"
#include "sentinel/core/chat/InMemoryConversationStore.h"

#include "sentinel/core/agent/NullAgentRuntime.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/ApplicationController.h"
#include "sentinel/core/app/ModeManager.h"
#include "sentinel/core/chat/IChatHistoryStore.h"
#include "sentinel/core/chat/LocalEchoProvider.h"
#include "sentinel/core/memory/InMemorySettingsStore.h"
#include "sentinel/core/memory/InMemoryStore.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/runtime/OllamaRuntime.h"

#include <QDir>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QMetaProperty>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using sentinel::core::ApplicationController;
using sentinel::core::AppSettings;
using sentinel::core::IChatHistoryStore;
using sentinel::core::InMemorySettingsStore;
using sentinel::core::InMemoryStore;
using sentinel::core::LocalEchoProvider;
using sentinel::core::ModeManager;
using sentinel::core::OllamaConfig;
using sentinel::core::OllamaHealthCheckResult;
using sentinel::core::OllamaModelSummary;
using sentinel::desktop::ChatMessageListModel;
using sentinel::desktop::DesktopShellViewModel;

class DesktopShellViewModelTest final : public QObject {
    Q_OBJECT

private slots:
    void localizesVoiceFailures();
    void exposesInitialShellState();
    void exposesAgentStatusWithoutRuntime();
    void exposesAgentToolMetadata();
    void exposesModelRoutingMetadata();
    void exposesTaskPlanMetadata();
    void exposesAgentRegistryMetadata();
    void exposesProviderCatalogMetadata();
    void exposesMemoryCatalogMetadata();
    void exposesOrchestrationSnapshotMetadata();
    void exposesOrchestrationReadinessDiagnostics();
    void exposesAgentTaskRuntimeMetadata();
    void exposesLocalRuntimeMetadata();
    void exposesRuntimeProviderRegistryMetadata();
    void exposesOllamaRuntimeBoundaryMetadata();
    void exposesModelManagementReadinessMetadata();
    void exposesLocalAiEcosystemFoundationMetadata();
    void exposesVoiceReadinessMetadata();
    void exposesVoiceConfigurationMetadata();
    void exposesFrontendRuntimeStateContract();
    void forwardsBlockedLocalInferenceRequest();
    void exposesConversationSessionMetadata();
    void exposesConversationStateMetadata();
    void exposesConversationRuntimeMetadata();
    void updatesAndPersistsRoutingModeMetadata();
    void exposesOnlyQmlSafeAgentVisibilityProperties();
    void exposesChatHistoryStatus();
    void exposesConversationStoreReadinessMetadata();
    void forwardsConversationBrowserActions();
    void exposesPersistentPinAndDuplicateConversationActions();
    void exposesConversationDeleteReadinessMetadata();
    void exposesConversationHistorySummaryMetadata();
    void exposesConversationBrowserMetadata();
    void exposesMultiConversationReadinessMetadata();
    void exposesConversationSearchAndExportMetadata();
    void exposesMaintenanceStatuses();
    void exposesMemoryCandidateMetadata();
    void exposesLocalMemoryRecallMetadata();
    void exposesContextAssemblyMetadata();
    void exposesConversationWindowMetadata();
    void exposesConversationSummaryMetadata();
    void exposesConversationCompressionMetadata();
    void exposesManualConversationSummaryGenerationMetadata();
    void exposesRetrievalPlanningMetadata();
    void exposesSemanticVectorReadinessMetadata();
    void exposesSemanticProviderPlanningMetadata();
    void exposesSemanticCandidateOrchestrationMetadata();
    void exposesSemanticArbitrationAndRuntimePlanningMetadata();
    void exposesIsolatedEmbeddingRuntimeMetadata();
    void exposesVectorPersistenceMetadata();
    void exposesSemanticSupplementAssemblyMetadata();
    void exposesSemanticPromptAuthorityMetadata();
    void exposesStartupLoadedMessages();
    void forwardsChatActions();
    void ignoresBlankChatActions();
    void legacyGlobalMemoryClearIsBlocked();
    void clearsChatActions();
    void reportsRuntimeOnlyChatMaintenanceWhenStoreUnavailable();
    void forwardsModeChanges();
    void forwardsMemoryWrites();
    void forwardsSettingsChanges();
    void restoresPersistedLocalProviderEndpointsOnStartup();
    void exposesLanguageSettings();
    void exposesCompanionReadinessMetadata();
    void exposesWorkspaceReadinessMetadata();
    void exposesSkillProfileMetadata();
    void exposesPermissionPolicyMetadata();
    void exposesProductExcellenceWorkflow();
    void onboardingWriteFailureRemainsVisible();
    void languageSettingDoesNotChangeRuntimePresentationFlags();
    void keepsSettingsSeparateFromClearActions();
    void tracksNavigationState();
    void ignoresRepeatedAndUnknownNavigationChanges();
};

class ViewModelFixture {
public:
    AppSettings settings{std::make_unique<InMemorySettingsStore>(),
                         sentinel::core::inMemoryTestCredentialStore()};
    sentinel::test::DeterministicModelServiceFixture models{
        sentinel::test::DeterministicChatReply::Final, &settings};
    std::unique_ptr<ApplicationController> controller{makeController(models)};
    ModeManager modeManager;
    DesktopShellViewModel viewModel{*controller, modeManager, initializedSettings(settings)};

    bool sendMessage(const QString& message) {
        if (!viewModel.sendMessage(message)) return false;
        QElapsedTimer timer;
        timer.start();
        while (viewModel.chatGenerationActive() && timer.elapsed() < 3000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        return !viewModel.chatGenerationActive();
    }

private:
    static AppSettings& initializedSettings(AppSettings& settings) {
        settings.setSelectedRuntimeProvider(QStringLiteral("ollama"));
        settings.setSelectedLocalModel(QStringLiteral("sentinel-test-model"));
        return settings;
    }

    static std::unique_ptr<ApplicationController> makeController(
        sentinel::test::DeterministicModelServiceFixture& models) {
        sentinel::core::ApplicationControllerBuilder builder;
        builder.withMemoryStore(std::make_unique<InMemoryStore>())
            .withConversationStore(std::make_unique<sentinel::core::InMemoryConversationStore>())
            .withModelService(models.takeModelService());
        return builder.build();
    }
};

class StaticChatHistoryStore final : public IChatHistoryStore {
public:
    explicit StaticChatHistoryStore(QList<sentinel::core::ChatMessage> messages = {},
                                    bool available = true)
        : messages_(std::move(messages)), available_(available) {}

    QList<sentinel::core::ChatMessage> loadMessages() const override {
        return available_ ? messages_ : QList<sentinel::core::ChatMessage>{};
    }

    void appendMessage(const sentinel::core::ChatMessage& message) override {
        if (available_) {
            messages_.append(message);
        }
    }

    void clear() override {
        if (available_) {
            messages_.clear();
        }
    }

    bool isAvailable() const override {
        return available_;
    }

private:
    QList<sentinel::core::ChatMessage> messages_;
    bool available_ = true;
};

class FakeOllamaRuntimeClient final : public sentinel::core::IOllamaRuntimeClient {
public:
    explicit FakeOllamaRuntimeClient(QList<OllamaModelSummary> models)
        : models_(std::move(models)) {}

    OllamaConfig config() const override {
        return {};
    }

    OllamaHealthCheckResult healthCheck() const override {
        OllamaHealthCheckResult result;
        result.connectionStatus = sentinel::core::OllamaConnectionStatus::Connected;
        result.healthStatus = sentinel::core::OllamaHealthStatus::Healthy;
        result.summary = QStringLiteral("Fake Ollama health metadata.");
        return result;
    }

    QList<OllamaModelSummary> installedModels() const override {
        return models_;
    }

private:
    QList<OllamaModelSummary> models_;
};

class FixedPlanRuntime final : public sentinel::core::IAgentRuntime {
public:
    FixedPlanRuntime(QList<sentinel::core::ToolDescriptor> tools,
                     sentinel::core::ToolInvocationPlan plan)
        : tools_(std::move(tools)), plan_(std::move(plan)) {}

    QString name() const override {
        return QStringLiteral("FixedPlanRuntime");
    }

    sentinel::core::AgentStatus status() const override {
        return sentinel::core::AgentStatus::Ready;
    }

    QList<sentinel::core::AgentCapabilityDescriptor> capabilities() const override {
        return {};
    }

    QList<sentinel::core::ToolDescriptor> availableTools() const override {
        return tools_;
    }

    sentinel::core::ToolInvocationPlan
    plan(const sentinel::core::AgentRequest& request) const override {
        Q_UNUSED(request);
        return plan_;
    }

    sentinel::core::AgentResponse execute(const sentinel::core::AgentRequest& request) override {
        return {
            true,
            QStringLiteral("Fixed local agent placeholder processed: %1")
                .arg(request.prompt.trimmed()),
            sentinel::core::AgentStatus::Ready,
        };
    }

private:
    QList<sentinel::core::ToolDescriptor> tools_;
    sentinel::core::ToolInvocationPlan plan_;
};

void DesktopShellViewModelTest::restoresPersistedLocalProviderEndpointsOnStartup() {
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("settings.json"));
    {
        AppSettings settings(std::make_unique<sentinel::core::JsonSettingsStore>(path),
                             sentinel::core::inMemoryTestCredentialStore());
        settings.setLmStudioEndpoint(QStringLiteral("http://127.0.0.1:18101"));
        settings.setLlamaCppEndpoint(QStringLiteral("http://127.0.0.1:18102"));
    }
    AppSettings restored(std::make_unique<sentinel::core::JsonSettingsStore>(path),
                         sentinel::core::inMemoryTestCredentialStore());
    sentinel::test::DeterministicModelServiceFixture models{
        sentinel::test::DeterministicChatReply::Final, &restored};
    sentinel::core::ApplicationControllerBuilder builder;
    auto controller =
        builder.withMemoryStore(std::make_unique<InMemoryStore>())
            .withConversationStore(std::make_unique<sentinel::core::InMemoryConversationStore>())
            .withModelService(models.takeModelService())
            .build();
    ModeManager modes;
    DesktopShellViewModel viewModel(*controller, modes, restored);
    QCOMPARE(controller->lmStudioEndpoint(), restored.lmStudioEndpoint());
    QCOMPARE(controller->llamaCppEndpoint(), restored.llamaCppEndpoint());
}

void DesktopShellViewModelTest::exposesInitialShellState() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.providerName(), QStringLiteral("deterministic-chat"));
    QCOMPARE(fixture.viewModel.providerStatus(), QStringLiteral("Available"));
    QCOMPARE(fixture.viewModel.memoryStatus(), QStringLiteral("Available"));
    QCOMPARE(fixture.viewModel.chatHistoryStatus(), QStringLiteral("Runtime Only"));
    QCOMPARE(fixture.viewModel.currentModeName(), QStringLiteral("Chat"));
    QCOMPARE(fixture.viewModel.availableModes(),
             QStringList({QStringLiteral("Chat"), QStringLiteral("Agent")}));
    QCOMPARE(fixture.viewModel.themeName(), QStringLiteral("Liquid Glass Light"));
    QCOMPARE(fixture.viewModel.configurationProfile(), QStringLiteral("Desktop Alpha"));
    QVERIFY(fixture.viewModel.availableLanguages().contains(fixture.viewModel.appLanguage()));
    QCOMPARE(fixture.viewModel.availableLanguages(),
             QStringList({QStringLiteral("en"), QStringLiteral("tr")}));
    QVERIFY(!fixture.viewModel.developerModeEnabled());
    QCOMPARE(fixture.viewModel.updateCheckPolicy(), QStringLiteral("Ask Before Checking"));
    QCOMPARE(fixture.viewModel.notificationPolicy(), QStringLiteral("Important Only"));
    QVERIFY(!fixture.viewModel.onboardingComplete());
    QCOMPARE(fixture.viewModel.onboardingUseCase(), QStringLiteral("General Assistant"));
    QCOMPARE(fixture.viewModel.onboardingAiProvider(), QStringLiteral("Ollama"));
    QVERIFY(fixture.viewModel.recoveryDraftText().isEmpty());
    QVERIFY(!fixture.viewModel.reducedMotionEnabled());
    QVERIFY(!fixture.viewModel.highContrastEnabled());
    QCOMPARE(fixture.viewModel.uiDensity(), QStringLiteral("Comfortable"));
    QVERIFY(fixture.viewModel.activityTimelineSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Memory entries -")));
    QVERIFY(fixture.viewModel.notificationCenterSummaries().isEmpty());
    fixture.viewModel.addNotification(QStringLiteral("Brain"),
                                      QStringLiteral("Memory snapshot saved"),
                                      QStringLiteral("Snapshot persisted locally."));
    QVERIFY(fixture.viewModel.notificationCenterSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Memory snapshot saved")));
    QVERIFY(fixture.viewModel.notificationCategories().contains(QStringLiteral("Security")));
    QVERIFY(fixture.viewModel.aboutSentinelSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Version:")));
    QVERIFY(fixture.viewModel.diagnosticsCenterSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Active provider:")));
    QVERIFY(fixture.viewModel.exportPreviewSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Markdown")));
    QCOMPARE(fixture.viewModel.currentPage(), QStringLiteral("Dashboard"));
    QCOMPARE(fixture.viewModel.availablePages(),
             QStringList({QStringLiteral("Memory"), QStringLiteral("Dashboard"),
                          QStringLiteral("Agents")}));
}

void DesktopShellViewModelTest::exposesAgentStatusWithoutRuntime() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.agentStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(fixture.viewModel.lastAgentResponse(), QStringLiteral("No agent request yet."));
    QCOMPARE(fixture.viewModel.latestToolPlanStatus(), QStringLiteral("Not Requested"));
    QCOMPARE(fixture.viewModel.latestToolPlanSummary(), QStringLiteral("No tool plan yet."));
    QCOMPARE(fixture.viewModel.latestApprovalStatus(), QStringLiteral("Not Requested"));
    QCOMPARE(fixture.viewModel.latestApprovalSummary(),
             QStringLiteral("No approval decision yet."));
    QCOMPARE(fixture.viewModel.latestSandboxStatus(), QStringLiteral("Not Evaluated"));
    QCOMPARE(fixture.viewModel.latestSandboxSummary(),
             QStringLiteral("No sandbox evaluation yet."));
    QCOMPARE(fixture.viewModel.latestToolExecutionStatus(), QStringLiteral("Not Requested"));
    QCOMPARE(fixture.viewModel.latestToolExecutionSummary(),
             QStringLiteral("No tool execution boundary result yet."));
    QCOMPARE(fixture.viewModel.latestAgentPipelineStatus(), QStringLiteral("Not Requested"));
    QCOMPARE(fixture.viewModel.latestAgentPipelineSummary(),
             QStringLiteral("No agent pipeline result yet."));
    QCOMPARE(fixture.viewModel.runtimeSessionId(), QStringLiteral("runtime-session-1"));
    QCOMPARE(fixture.viewModel.runtimeContextStatus(), QStringLiteral("Empty"));
    QCOMPARE(fixture.viewModel.runtimeContextSummary(), QStringLiteral("No runtime context yet."));
    QVERIFY(fixture.viewModel.runtimeContextActiveToolIds().isEmpty());
    QCOMPARE(fixture.viewModel.agentActivityCount(), 0);
    QCOMPARE(fixture.viewModel.latestAgentActivitySummary(),
             QStringLiteral("No agent activity yet."));
    QCOMPARE(fixture.viewModel.availableToolCount(), 0);
    QVERIFY(fixture.viewModel.availableToolIds().isEmpty());
}

void DesktopShellViewModelTest::exposesModelRoutingMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.currentRoutingMode(), QStringLiteral("Local Only"));
    QCOMPARE(fixture.viewModel.modelRoutingStatus(), QStringLiteral("No Available Model"));
    QCOMPARE(fixture.viewModel.selectedModelProviderSummary(),
             QStringLiteral("No configured model route is available."));
}

void DesktopShellViewModelTest::exposesTaskPlanMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.latestTaskPlanStatus(), QStringLiteral("Blocked"));
    QCOMPARE(fixture.viewModel.plannedTaskStepCount(), 0);
    QCOMPARE(fixture.viewModel.latestTaskPlanSummary(),
             QStringLiteral("No available local metadata capability can satisfy this task."));
    QCOMPARE(fixture.viewModel.currentMemoryAffinitySummary(),
             QStringLiteral("Ambient (Available, Public Metadata, Session)"));
}

void DesktopShellViewModelTest::exposesAgentRegistryMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.registeredAgentCount(), 6);
    QCOMPARE(fixture.viewModel.activeAgentSummaries().size(), 6);
    QVERIFY(fixture.viewModel.activeAgentSummaries().contains(
        QStringLiteral("Atlas (Coordinator, Available, Local)")));
    QVERIFY(fixture.viewModel.activeAgentSummaries().contains(
        QStringLiteral("Nyx (Guardian, Available, Local)")));
    QCOMPARE(fixture.viewModel.currentAgentSummary(),
             QStringLiteral("Atlas (Coordinator, Available, Local)"));
}

void DesktopShellViewModelTest::exposesProviderCatalogMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.providerCatalogCount(), 4);
    QCOMPARE(fixture.viewModel.providerCatalogSummaries().size(), 4);
    QVERIFY(fixture.viewModel.providerCatalogSummaries().contains(
        QStringLiteral("Ollama Local (Local, Not Configured)")));
    QVERIFY(fixture.viewModel.providerCatalogSummaries().contains(
        QStringLiteral("Anthropic Cloud (Cloud, Not Configured)")));
}

void DesktopShellViewModelTest::exposesMemoryCatalogMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.memoryCatalogCount(), 5);
    QCOMPARE(fixture.viewModel.memoryCatalogSummaries().size(), 5);
    QVERIFY(fixture.viewModel.memoryCatalogSummaries().contains(
        QStringLiteral("Reflective (Available, Sensitive, User Controlled)")));
    QVERIFY(fixture.viewModel.memoryCatalogSummaries().contains(
        QStringLiteral("Ambient (Available, Public Metadata, Session)")));
    QCOMPARE(fixture.viewModel.currentMemoryAffinitySummary(),
             QStringLiteral("Ambient (Available, Public Metadata, Session)"));
}

void DesktopShellViewModelTest::exposesOrchestrationSnapshotMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.orchestrationSnapshotStatus(), QStringLiteral("Degraded"));
    QVERIFY(fixture.viewModel.orchestrationSnapshotSummary().contains(
        QStringLiteral("4 provider entries, 6 agents, 5 memory categories")));
    QVERIFY(fixture.viewModel.orchestrationSignals().contains(
        QStringLiteral("Routing: Local Only / No Available Model")));
    QVERIFY(fixture.viewModel.orchestrationSignals().contains(
        QStringLiteral("Agent: Atlas (Coordinator, Available, Local)")));
    QVERIFY(fixture.viewModel.orchestrationSignals().contains(
        QStringLiteral("Memory: Ambient (Available, Public Metadata, Session)")));
}

void DesktopShellViewModelTest::exposesOrchestrationReadinessDiagnostics() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.orchestrationReadinessStatus(), QStringLiteral("Blocked"));
    QCOMPARE(fixture.viewModel.orchestrationReadinessSummary(),
             QStringLiteral("Blocked orchestration readiness: 10 deterministic metadata checks, 10 "
                            "diagnostic entries."));
    QCOMPARE(fixture.viewModel.orchestrationDiagnostics().size(), 10);
    QVERIFY(fixture.viewModel.orchestrationDiagnostics().contains(
        QStringLiteral("Info: Privacy Posture - Local-only routing posture is active.")));
    QVERIFY(fixture.viewModel.orchestrationDiagnostics().contains(
        QStringLiteral("Info: Execution Capability - Execution capability remains disabled.")));
}

void DesktopShellViewModelTest::exposesAgentTaskRuntimeMetadata() {
    ViewModelFixture fixture;
    const auto metaObject = fixture.viewModel.metaObject();

    QCOMPARE(fixture.viewModel.agentTaskRuntimeStatus(), QStringLiteral("Ready"));
    QCOMPARE(fixture.viewModel.agentTaskRuntimeTaskCount(), 6);
    QCOMPARE(fixture.viewModel.agentTaskQueueCount(), 6);
    QCOMPARE(fixture.viewModel.agentTaskQueuePlannedCount(), 6);
    QCOMPARE(fixture.viewModel.agentTaskQueueActiveCount(), 0);
    QCOMPARE(fixture.viewModel.agentTaskQueueBlockedCount(), 0);
    QCOMPARE(fixture.viewModel.agentTaskQueueCompletedCount(), 0);
    QCOMPARE(fixture.viewModel.agentTaskQueueRefusedCount(), 0);
    QVERIFY(fixture.viewModel.agentTaskRuntimeSummary().contains(
        QStringLiteral("executes local metadata tasks deterministically")));
    QVERIFY(fixture.viewModel.latestAgentTaskSummary().contains(
        QStringLiteral("Prepare Export Action")));
    QVERIFY(fixture.viewModel.latestAgentTaskLifecycleSummary().contains(QStringLiteral("queued")));
    QCOMPARE(fixture.viewModel.agentTaskQueueSummaries().size(), 6);
    QCOMPARE(fixture.viewModel.agentTaskTraceSummaries().size(), 3);
    QVERIFY(fixture.viewModel.agentTaskTraceSummaries().last().contains(
        QStringLiteral("Execution Boundary")));
    QCOMPARE(fixture.viewModel.agentPlanningSessionStatus(), QStringLiteral("Ready"));
    QCOMPARE(fixture.viewModel.agentPlanningCandidateCount(), 6);
    QCOMPARE(fixture.viewModel.agentPlanningRefusedCount(), 0);
    QVERIFY(fixture.viewModel.agentPlanningSessionSummary().contains(
        QStringLiteral("execution attempted: no")));
    QCOMPARE(fixture.viewModel.agentPlanningCandidateSummaries().size(), 6);
    QCOMPARE(fixture.viewModel.agentPlanningArbitrationSummaries().size(), 6);
    QCOMPARE(fixture.viewModel.agentPlanningRefusalSummaries().size(), 0);
    QVERIFY(fixture.viewModel.agentPlanningFallbackSummary().contains(
        QStringLiteral("No planning fallback")));
    QCOMPARE(fixture.viewModel.agentCapabilityRegistryStatus(),
             QStringLiteral("Refusing Unsafe Capabilities"));
    QCOMPARE(fixture.viewModel.agentCapabilityCount(), 9);
    QCOMPARE(fixture.viewModel.agentCapabilityEnabledCount(), 6);
    QCOMPARE(fixture.viewModel.agentCapabilityDisabledCount(), 2);
    QCOMPARE(fixture.viewModel.agentCapabilityRestrictedCount(), 3);
    QVERIFY(fixture.viewModel.agentCapabilityRegistrySummary().contains(
        QStringLiteral("execution attempted: no")));
    QCOMPARE(fixture.viewModel.agentCapabilitySummaries().size(), 9);
    QVERIFY(fixture.viewModel.agentCapabilitySummaries().at(7).contains(
        QStringLiteral("Shell Execution")));
    QCOMPARE(fixture.viewModel.agentCapabilityReadinessSummaries().size(), 9);
    QCOMPARE(fixture.viewModel.agentCapabilitySafetySummaries().size(), 9);
    QCOMPARE(fixture.viewModel.toolContractRegistryStatus(),
             QStringLiteral("Refusing Unsafe Contracts"));
    QCOMPARE(fixture.viewModel.toolContractCount(), 10);
    QCOMPARE(fixture.viewModel.toolContractEnabledCount(), 6);
    QCOMPARE(fixture.viewModel.toolContractDisabledCount(), 3);
    QCOMPARE(fixture.viewModel.toolContractRestrictedCount(), 4);
    QVERIFY(fixture.viewModel.toolContractRegistrySummary().contains(
        QStringLiteral("execution attempted: no")));
    QCOMPARE(fixture.viewModel.toolContractSummaries().size(), 10);
    QVERIFY(fixture.viewModel.toolContractSummaries().at(7).contains(
        QStringLiteral("Future Subprocess Execution")));
    QCOMPARE(fixture.viewModel.toolContractPermissionSummaries().size(), 10);
    QCOMPARE(fixture.viewModel.toolContractSandboxSummaries().size(), 10);
    QCOMPARE(fixture.viewModel.toolContractReadinessSummaries().size(), 10);
    QCOMPARE(fixture.viewModel.toolContractSafetySummaries().size(), 10);
    QVERIFY(fixture.viewModel.toolContractPermissionSummaries().at(6).contains(
        QStringLiteral("future filesystem access")));
    QVERIFY(
        fixture.viewModel.toolContractSandboxSummaries().at(7).contains(QStringLiteral("Denied")));
    QVERIFY(metaObject->indexOfProperty("agentTaskRuntimeStatus") >= 0);
    QVERIFY(metaObject->indexOfProperty("agentPlanningSessionStatus") >= 0);
    QVERIFY(metaObject->indexOfProperty("agentCapabilityRegistryStatus") >= 0);
    QVERIFY(metaObject->indexOfProperty("toolContractRegistryStatus") >= 0);
    QCOMPARE(metaObject->indexOfProperty("agentTaskRawPayload"), -1);
    QCOMPARE(metaObject->indexOfProperty("agentCapabilityRawPayload"), -1);
    QCOMPARE(metaObject->indexOfProperty("toolContractRawPayload"), -1);
}

void DesktopShellViewModelTest::exposesLocalRuntimeMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.localRuntimeStatus(), QStringLiteral("Metadata Only"));
    QCOMPARE(fixture.viewModel.localRuntimeHealth(), QStringLiteral("Not Executable"));
    QCOMPARE(fixture.viewModel.localRuntimeSummary(),
             QStringLiteral("Null Local Runtime is metadata-only; local inference execution is "
                            "disabled."));
    QCOMPARE(fixture.viewModel.localRuntimeResponseStatus(), QStringLiteral("Refused"));
    QCOMPARE(fixture.viewModel.localRuntimeResponseSummary(),
             QStringLiteral("Local runtime boundary is metadata-only; execution is disabled."));
    QCOMPARE(fixture.viewModel.localRuntimeCapabilities().size(), 3);
    QVERIFY(fixture.viewModel.localRuntimeCapabilities().contains(
        QStringLiteral("Streaming (Disabled): Streaming is intentionally disabled.")));
    QCOMPARE(fixture.viewModel.localRuntimeSessionCount(), 1);
    QCOMPARE(fixture.viewModel.localRuntimeSessionStatus(), QStringLiteral("Reserved"));
    QCOMPARE(fixture.viewModel.localRuntimeSessionHealth(), QStringLiteral("Placeholder Only"));
    QCOMPARE(fixture.viewModel.localRuntimeSessionSummary(),
             QStringLiteral("local-runtime-session-1: Reserved placeholder local runtime "
                            "metadata."));
    QCOMPARE(fixture.viewModel.localRuntimeAllocationSummary(),
             QStringLiteral("Metadata-only local runtime allocation; no model or process is "
                            "started."));
    QCOMPARE(fixture.viewModel.localRuntimeReservationSummary(),
             QStringLiteral("Placeholder reservation is held for metadata visibility only."));
    QCOMPARE(fixture.viewModel.runtimeCapabilityCount(), 13);
    QCOMPARE(fixture.viewModel.enabledRuntimeCapabilitySummaries().size(), 2);
    QVERIFY(fixture.viewModel.enabledRuntimeCapabilitySummaries().contains(
        QStringLiteral("Local-Only Enforcement (Security, Enabled): Negotiation metadata enforces "
                       "a local-only posture.")));
    QCOMPARE(fixture.viewModel.disabledRuntimeCapabilitySummaries().size(), 11);
    QVERIFY(fixture.viewModel.disabledRuntimeCapabilitySummaries().contains(
        QStringLiteral("Streaming (Inference, Disabled): Token streaming is disabled.")));
    QCOMPARE(fixture.viewModel.runtimeNegotiationProfileSummary(),
             QStringLiteral("Metadata-only negotiation profile; no runtime capability is "
                            "activated."));
    QCOMPARE(fixture.viewModel.runtimeNegotiationSummary(),
             QStringLiteral("Metadata-only runtime negotiation: 13 capabilities described, 2 "
                            "enabled as safety metadata."));
    QCOMPARE(fixture.viewModel.localOnlyRuntimeEnforcementSummary(),
             QStringLiteral("Local-only enforcement is active; cloud relay and external runtime "
                            "execution remain unavailable."));
    QCOMPARE(fixture.viewModel.runtimePermissionDecision(), QStringLiteral("Denied"));
    QCOMPARE(fixture.viewModel.runtimePermissionSummary(),
             QStringLiteral("Runtime permission policy is metadata-only and denies execution by "
                            "default."));
    QCOMPARE(fixture.viewModel.runtimeSafetyDecision(), QStringLiteral("Compliant"));
    QCOMPARE(fixture.viewModel.runtimeSafetySummary(),
             QStringLiteral("Runtime safety policy report: local-only and no-execution posture is "
                            "enforced with deterministic metadata rules."));
    QCOMPARE(fixture.viewModel.runtimePipelineStatus(), QStringLiteral("Blocked"));
    QCOMPARE(fixture.viewModel.runtimePipelineSummary(),
             QStringLiteral("Runtime request pipeline blocked execution metadata by permission and "
                            "safety policy."));
    QCOMPARE(fixture.viewModel.runtimePipelineTraceSummaries().size(), 4);
    QVERIFY(fixture.viewModel.runtimePipelineTraceSummaries().contains(
        QStringLiteral("Permission Policy [Denied]: Runtime permission policy is metadata-only and "
                       "denies execution by default.")));
    QCOMPARE(fixture.viewModel.executionLifecycleState(), QStringLiteral("Blocked"));
    QCOMPARE(fixture.viewModel.executionLifecycleStatus(), QStringLiteral("Blocked"));
    QCOMPARE(fixture.viewModel.executionLifecycleSummary(),
             QStringLiteral("Execution lifecycle reached blocked metadata state; no execution is "
                            "permitted."));
    QCOMPARE(fixture.viewModel.executionLifecycleTraceSummaries().size(), 7);
    QVERIFY(fixture.viewModel.executionLifecycleTraceSummaries().contains(
        QStringLiteral("7. Blocked [Blocked]: Execution remains intentionally blocked.")));
    QCOMPARE(fixture.viewModel.executionSessionId(), QStringLiteral("execution-session-1"));
    QCOMPARE(fixture.viewModel.executionSessionStatus(), QStringLiteral("Reserved"));
    QCOMPARE(fixture.viewModel.executionSessionOwnership(),
             QStringLiteral("Application Controller"));
    QCOMPARE(fixture.viewModel.executionCoordinationMode(), QStringLiteral("Metadata Only"));
    QCOMPARE(fixture.viewModel.executionSessionSummary(),
             QStringLiteral("Execution session is reserved for metadata only."));
    QCOMPARE(fixture.viewModel.executionCoordinationSnapshotSummary(),
             QStringLiteral("Execution coordination snapshot is read-only for "
                            "execution-session-1; lifecycle is Blocked and execution is "
                            "blocked."));
    QCOMPARE(fixture.viewModel.localRuntimeAdapterStatus(), QStringLiteral("Placeholder"));
    QCOMPARE(fixture.viewModel.localRuntimeAdapterHealth(), QStringLiteral("Metadata Only"));
    QCOMPARE(fixture.viewModel.localRuntimeAdapterSummary(),
             QStringLiteral("Ollama local runtime adapter contract is metadata-only; no runtime "
                            "connection is configured."));
    QCOMPARE(fixture.viewModel.localRuntimeAdapterCapabilitySummaries().size(), 3);
    QVERIFY(fixture.viewModel.localRuntimeAdapterCapabilitySummaries().contains(
        QStringLiteral("Model Discovery (Unavailable, Not Executable): Model discovery is "
                       "intentionally disabled.")));
    QCOMPARE(fixture.viewModel.providerRuntimeBridgeStatus(), QStringLiteral("Not Connected"));
    QCOMPARE(fixture.viewModel.providerRuntimeBridgeSummary(),
             QStringLiteral("Provider runtime bridge is not connected and cannot execute provider "
                            "requests."));
    QCOMPARE(fixture.viewModel.providerRuntimeBridgeResponseSummary(),
             QStringLiteral("Provider runtime bridge is metadata-only; no provider or local "
                            "runtime request was executed."));
    QCOMPARE(fixture.viewModel.runtimeIntegrationReadinessStatus(), QStringLiteral("Blocked"));
    QCOMPARE(fixture.viewModel.runtimeIntegrationReadinessSummary(),
             QStringLiteral("Runtime integration readiness is blocked: Ollama local "
                            "health/discovery metadata is available, but provider bridge "
                            "execution and inference remain disabled."));
    QCOMPARE(fixture.viewModel.runtimeIntegrationReadinessChecks().size(), 5);
    QVERIFY(fixture.viewModel.runtimeIntegrationReadinessChecks().contains(
        QStringLiteral("Pass: Endpoint Configuration - Safe local Ollama endpoint is configured "
                       "for loopback-only health checks.")));
}

void DesktopShellViewModelTest::exposesRuntimeProviderRegistryMetadata() {
    ViewModelFixture fixture;
    QSignalSpy runtimeProviderSpy(&fixture.viewModel,
                                  &DesktopShellViewModel::runtimeProviderRegistryChanged);

    QCOMPARE(fixture.viewModel.selectedRuntimeProvider(), QStringLiteral("ollama"));
    QCOMPARE(fixture.viewModel.activeRuntimeProviderId(), QStringLiteral("ollama"));
    QCOMPARE(fixture.viewModel.activeRuntimeProviderLabel(), QStringLiteral("Local Ollama"));
    QCOMPARE(fixture.viewModel.activeRuntimeReadinessState(), QStringLiteral("unavailable"));
    QCOMPARE(fixture.viewModel.activeRuntimeLocalOnlySummary(), QStringLiteral("Local Only"));
    QCOMPARE(fixture.viewModel.selectableRuntimeProviderIds(),
             QStringList({QStringLiteral("ollama"), QStringLiteral("lm-studio"),
                          QStringLiteral("llama-cpp-server"), QStringLiteral("cloud-api")}));
    QCOMPARE(fixture.viewModel.runtimeProviderCardSummaries().size(), 5);
    QVERIFY(fixture.viewModel.runtimeProviderCardSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("OpenAI-compatible Local")));
    QVERIFY(fixture.viewModel.runtimeProviderCardSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("LM Studio")));
    QVERIFY(fixture.viewModel.runtimeProviderCardSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("llama.cpp server")));
    QVERIFY(fixture.viewModel.runtimeProviderCapabilitySummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("requiresApiKey: no")));
    QVERIFY(fixture.viewModel.runtimeProviderValidationTraces()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("lm-studio: readiness=unknown")));
    QCOMPARE(fixture.viewModel.providerCredentialRegistryStatus(), QStringLiteral("missing"));
    QVERIFY(fixture.viewModel.providerCredentialRegistrySummary().contains(
        QStringLiteral("API key values are persisted")));
    QCOMPARE(fixture.viewModel.providerCredentialSummaries().size(), 4);
    QVERIFY(fixture.viewModel.providerCredentialReadinessSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("execution disabled")));
    QVERIFY(fixture.viewModel.providerCredentialSafetySummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("plaintextStorage=refused")));
    // The production platform store can legitimately be unavailable on a
    // headless runner. The view must faithfully expose its exact safe state.
    const auto credentialSummary = sentinel::core::platformCredentialStore().summary();
    QCOMPARE(fixture.viewModel.credentialStoreSummary(), credentialSummary.summary);
    QCOMPARE(fixture.viewModel.credentialStoreBackendSummary(), credentialSummary.backendSummary);
    QVERIFY(!credentialSummary.plaintextPersistenceAllowed);
    QVERIFY(
        fixture.viewModel.credentialStoreSafetySummary().contains(QStringLiteral("no plaintext")));
    QCOMPARE(fixture.viewModel.credentialStoreTraceSummaries().size(), 5);
    QVERIFY(
        fixture.viewModel.credentialActionReadiness().contains(QStringLiteral("credential store")));
    QVERIFY(fixture.viewModel.credentialExecutionStatus().contains(
        QStringLiteral("user-provided API key")));
    const auto exposedCredentialText =
        fixture.viewModel.credentialStoreSummary() +
        fixture.viewModel.credentialStoreBackendSummary() +
        fixture.viewModel.credentialStoreSafetySummary() +
        fixture.viewModel.providerCredentialSummaries().join(QStringLiteral("\n")) +
        fixture.viewModel.providerCredentialReadinessSummaries().join(QStringLiteral("\n"));
    QVERIFY(!exposedCredentialText.contains(QStringLiteral("sk-test-secret")));
    QVERIFY(!exposedCredentialText.contains(QStringLiteral("apiKey"), Qt::CaseInsensitive));

    fixture.viewModel.setSelectedRuntimeProvider(QStringLiteral("lm-studio"));

    QCOMPARE(fixture.settings.selectedRuntimeProvider(), QStringLiteral("lm-studio"));
    QCOMPARE(fixture.viewModel.selectedRuntimeProvider(), QStringLiteral("lm-studio"));
    QCOMPARE(fixture.viewModel.activeRuntimeProviderId(), QStringLiteral("lm-studio"));
    QCOMPARE(runtimeProviderSpy.count(), 2);
}

void DesktopShellViewModelTest::exposesCompanionReadinessMetadata() {
    ViewModelFixture fixture;
    QSignalSpy companionSpy(&fixture.viewModel, &DesktopShellViewModel::companionChanged);

    QVERIFY(fixture.viewModel.companionEnabled());
    QVERIFY(!fixture.viewModel.companionAvailable());
    QCOMPARE(fixture.viewModel.companionStatus(), QStringLiteral("Readiness Only"));
    QCOMPARE(fixture.viewModel.companionAvailability(), QStringLiteral("Unavailable"));
    QVERIFY(fixture.viewModel.companionPlatformCapability().contains(
        QStringLiteral("native integration unavailable")));
    QVERIFY(fixture.viewModel.companionPermissionPosture().contains(
        QStringLiteral("foreground-safe shell")));
    QVERIFY(fixture.viewModel.companionSafetyBoundary().contains(
        QStringLiteral("no background daemon")));
    QVERIFY(
        fixture.viewModel.companionQuickCaptureSummary().contains(QStringLiteral("short note")));
    QCOMPARE(fixture.viewModel.companionActionSummaries().size(), 6);
    QCOMPARE(fixture.viewModel.companionPlatformSummaries().size(), 3);
    QCOMPARE(fixture.viewModel.companionTraceSummaries().size(), 6);
    QVERIFY(fixture.viewModel.companionActionSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Quick Note")));

    // Attempting to disable companion is ignored since it is permanently enabled
    fixture.viewModel.setCompanionEnabled(false);

    QVERIFY(fixture.viewModel.companionEnabled());
    QVERIFY(!fixture.viewModel.companionAvailable());
    QCOMPARE(fixture.viewModel.companionStatus(), QStringLiteral("Readiness Only"));
    QCOMPARE(companionSpy.count(), 0);

    fixture.viewModel.setCompanionNativeAvailable(true);

    QVERIFY(fixture.viewModel.companionAvailable());
    QCOMPARE(fixture.viewModel.companionStatus(), QStringLiteral("Active"));
    QCOMPARE(fixture.viewModel.companionAvailability(), QStringLiteral("Native Available"));
    QCOMPARE(companionSpy.count(), 1);

    fixture.viewModel.setCompanionPaused(true);

    QVERIFY(fixture.viewModel.companionPaused());
    QCOMPARE(fixture.viewModel.companionStatus(), QStringLiteral("Paused"));
    QVERIFY(fixture.viewModel.companionActionSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Resume Companion")));
    QCOMPARE(companionSpy.count(), 2);

    QSignalSpy requestSpy(&fixture.viewModel, &DesktopShellViewModel::requestCompanionChat);
    QSignalSpy visibleSpy(&fixture.viewModel, &DesktopShellViewModel::companionChatVisibleChanged);

    QVERIFY(!fixture.viewModel.companionChatVisible());
    fixture.viewModel.toggleCompanionChat();
    QVERIFY(fixture.viewModel.companionChatVisible());
    QCOMPARE(requestSpy.count(), 1);
    QCOMPARE(visibleSpy.count(), 1);

    fixture.viewModel.hideCompanionChat();
    QVERIFY(!fixture.viewModel.companionChatVisible());
    QCOMPARE(visibleSpy.count(), 2);
}

void DesktopShellViewModelTest::exposesOllamaRuntimeBoundaryMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.ollamaEndpoint(), QStringLiteral("http://127.0.0.1:11434"));
    QCOMPARE(fixture.viewModel.ollamaConnectionStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(fixture.viewModel.ollamaHealthStatus(), QStringLiteral("Unavailable"));
    QVERIFY(
        fixture.viewModel.ollamaHealthSummary().contains(QStringLiteral("no local health check")));
    QCOMPARE(fixture.viewModel.ollamaModelCount(), 0);
    QVERIFY(fixture.viewModel.ollamaModelNames().isEmpty());
    QVERIFY(fixture.viewModel.ollamaModelSummaries().isEmpty());
}

// Retired local-discovery selection probe; canonical selection now belongs to ModelService
// and is covered by test_model_registry and test_static_model_router.
#if 0
void DesktopShellViewModelTest::exposesDiscoveredModelSelectionMetadata() {
    ApplicationController controller{
        std::make_unique<LocalEchoProvider>(),
        std::make_unique<InMemoryStore>(),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        std::make_unique<FakeOllamaRuntimeClient>(QList<OllamaModelSummary>{
            {QStringLiteral("llama3.2"), QStringLiteral("2026-05-01T10:00:00Z"), 2147483648},
            {QStringLiteral("mistral"), {}, 1024},
        })};
    ModeManager modeManager;
    AppSettings settings{std::make_unique<InMemorySettingsStore>()};
    DesktopShellViewModel viewModel{controller, modeManager, settings};
    QSignalSpy spy(&viewModel, &DesktopShellViewModel::localModelSelectionChanged);

    QCOMPARE(viewModel.ollamaModelCount(), 2);
    QCOMPARE(viewModel.ollamaModelNames(),
             QStringList({QStringLiteral("llama3.2"), QStringLiteral("mistral")}));
    QVERIFY(viewModel.ollamaModelSummaries().contains(
        QStringLiteral("llama3.2 (2.0 GiB, modified 2026-05-01T10:00:00Z, Local Only)")));
    QCOMPARE(viewModel.selectedLocalModelStatus(), QStringLiteral("Missing"));
    QCOMPARE(viewModel.modelRegistryStatus(), QStringLiteral("ready"));
    QVERIFY(viewModel.modelRegistrySummary().contains(QStringLiteral("2 available")));
    QVERIFY(viewModel.modelRegistryModelSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("llama3.2")));
    QCOMPARE(viewModel.selectedLocalModelMetadataSummary(),
             QStringLiteral("No local model metadata available."));

    viewModel.setSelectedLocalModel(QStringLiteral("mistral"));

    QCOMPARE(settings.selectedLocalModel(), QStringLiteral("mistral"));
    QCOMPARE(viewModel.selectedLocalModel(), QStringLiteral("mistral"));
    QCOMPARE(viewModel.selectedLocalModelStatus(), QStringLiteral("Available"));
    QCOMPARE(viewModel.selectedModelCapabilityLabels(),
             QStringList({QStringLiteral("chat"), QStringLiteral("streaming")}));
    QCOMPARE(viewModel.selectedLocalModelMetadataSummary(),
             QStringLiteral("Selected model: mistral (1.0 KiB, Local Only)"));
    QVERIFY(spy.count() >= 1);

    viewModel.setSelectedLocalModel(QStringLiteral("missing"));

    QCOMPARE(settings.selectedLocalModel(), QStringLiteral("missing"));
    QCOMPARE(viewModel.selectedLocalModelStatus(), QStringLiteral("Invalid"));
    QCOMPARE(viewModel.selectedLocalModelMetadataSummary(),
             QStringLiteral("Invalid selection: missing is not in discovered local model "
                            "metadata."));
}

#endif

void DesktopShellViewModelTest::exposesModelManagementReadinessMetadata() {
    ApplicationController controller{
        std::make_unique<LocalEchoProvider>(),
        std::make_unique<InMemoryStore>(),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        std::make_unique<FakeOllamaRuntimeClient>(QList<OllamaModelSummary>{
            {QStringLiteral("llama3.2"), QStringLiteral("2026-05-01T10:00:00Z"), 2147483648},
        })};
    ModeManager modeManager;
    AppSettings settings{std::make_unique<InMemorySettingsStore>()};
    settings.setSelectedRuntimeProvider(QStringLiteral("ollama"));
    DesktopShellViewModel viewModel{controller, modeManager, settings};

    QCOMPARE(viewModel.modelManagementStatus(), QStringLiteral("Available"));
    QCOMPARE(viewModel.modelManagementSummary(),
             QStringLiteral("Live Ollama model management is available: 1 installed model(s); "
                            "pull and delete use /api/pull and /api/delete."));
    QVERIFY(viewModel.modelManagementActionAvailability().contains(
        QStringLiteral("Pull available via Ollama /api/pull")));
    QCOMPARE(viewModel.modelRecommendationSummaries().size(), 3);
    QCOMPARE(viewModel.modelRequirementSummaries().size(), 3);
    QVERIFY(
        viewModel.modelRecommendationSummaries().first().contains(QStringLiteral("llama3.2:3b")));
    QVERIFY(viewModel.modelRequirementSummaries().first().contains(
        QStringLiteral("approx disk about 3 GB")));
}

void DesktopShellViewModelTest::exposesLocalAiEcosystemFoundationMetadata() {
    AppSettings settings{std::make_unique<InMemorySettingsStore>(),
                         sentinel::core::inMemoryTestCredentialStore()};
    sentinel::core::ApplicationControllerBuilder builder;
    builder.withProvider(std::make_unique<LocalEchoProvider>())
        .withMemoryStore(std::make_unique<InMemoryStore>())
        .withModelService(std::make_unique<sentinel::core::ModelService>(&settings))
        .withOllamaRuntimeClient(
            std::make_unique<FakeOllamaRuntimeClient>(QList<OllamaModelSummary>{
                {QStringLiteral("qwen2.5-coder:7b"), QStringLiteral("2026-05-01T10:00:00Z"),
                 5LL * 1024LL * 1024LL * 1024LL},
            }));
    auto controller = builder.build();
    ModeManager modeManager;
    DesktopShellViewModel viewModel{*controller, modeManager, settings};

    viewModel.setSelectedLocalModel(QStringLiteral("qwen2.5-coder:7b"));
    viewModel.assignModelRole(QStringLiteral("coding"), QStringLiteral("qwen2.5-coder:7b"));

    QVERIFY(viewModel.modelLibraryInstalledSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("qwen2.5-coder:7b")));
    QVERIFY(!viewModel.modelLibraryAvailableSummaries()
                 .join(QStringLiteral("\n"))
                 .contains(QStringLiteral("LM Studio")));
    QVERIFY(viewModel.providerDiscoverySummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("llama.cpp server")));
    QVERIFY(viewModel.modelRoleAssignmentSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Coding Model - qwen2.5-coder:7b")));
    QVERIFY(viewModel.modelAdvisorRecommendationSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("qwen2.5-coder:7b")));
    QVERIFY(viewModel.modelAdvisorAvoidSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("cloud-only")));
    QVERIFY(viewModel.downloadsCenterSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("foreground model manager")));
    QVERIFY(viewModel.benchmarkHubSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("no benchmark result recorded")));
    QVERIFY(viewModel.notificationCenterSummaries().isEmpty());
}

void DesktopShellViewModelTest::exposesVoiceReadinessMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.voiceRuntimeMode(), QStringLiteral("Disabled"));
    QVERIFY(!fixture.viewModel.voiceEnabled());
    QCOMPARE(fixture.viewModel.voiceReadinessStatus(), QStringLiteral("Disabled"));
    QVERIFY(fixture.viewModel.voiceReadinessSummary().contains(QStringLiteral("metadata-only")));
    QCOMPARE(fixture.viewModel.textToSpeechStatus(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.speechToTextStatus(), QStringLiteral("Disabled"));
    QVERIFY(
        fixture.viewModel.textToSpeechSummary().contains(QStringLiteral("disabled by default")));
    QVERIFY(
        fixture.viewModel.speechToTextSummary().contains(QStringLiteral("disabled by default")));
    QCOMPARE(fixture.viewModel.voiceCapabilitySummaries().size(), 2);
    QVERIFY(fixture.viewModel.voiceCapabilitySummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Speech To Text")));
    QVERIFY(fixture.viewModel.voiceReadinessChecks()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("No microphone access")));
    QVERIFY(fixture.viewModel.voiceReadinessChecks()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("No Piper")));
    QCOMPARE(fixture.viewModel.voiceSessionId(), QStringLiteral("voice-session-1"));
    QCOMPARE(fixture.viewModel.voiceSessionStatus(), QStringLiteral("completed"));
    QVERIFY(fixture.viewModel.voiceSessionSummary().contains(QStringLiteral("metadata-only")));
    QCOMPARE(fixture.viewModel.voicePipelineStatus(), QStringLiteral("completed"));
    QVERIFY(fixture.viewModel.voicePipelineSummary().contains(QStringLiteral("metadata-only")));
    QCOMPARE(fixture.viewModel.voicePipelineTraceSummaries().size(), 7);
    QVERIFY(fixture.viewModel.voicePipelineTraceSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("synthesis-placeholder")));
    QCOMPARE(fixture.viewModel.voicePipelineSessionStatus(), QStringLiteral("fallback"));
    QVERIFY(fixture.viewModel.voicePipelineSessionSummary().contains(
        QStringLiteral("execution attempted: no")));
    QCOMPARE(fixture.viewModel.voicePipelineSessionStageReadinessSummaries().size(), 5);
    QVERIFY(fixture.viewModel.voicePipelineSessionStageReadinessSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("transcription-readiness")));
    QVERIFY(fixture.viewModel.voicePipelineSessionTraceSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Fallback metadata selected")));
    QVERIFY(fixture.viewModel.voicePipelineSessionFallbackSummary().contains(
        QStringLiteral("no playback")));
    QVERIFY(fixture.viewModel.voicePipelineSessionSafetySummary().contains(
        QStringLiteral("no-execution guarantees")));
    QCOMPARE(fixture.viewModel.voicePipelineSessionSafetyChecks().size(), 8);
    QCOMPARE(fixture.viewModel.voicePipelineSessionReadyStageCount(), 2);
    QCOMPARE(fixture.viewModel.voicePipelineSessionBlockedStageCount(), 2);
    QCOMPARE(fixture.viewModel.audioFileSessionStatus(), QStringLiteral("disabled"));
    QVERIFY(fixture.viewModel.audioFileSessionSummary().contains(
        QStringLiteral("execution attempted: no")));
    QVERIFY(fixture.viewModel.audioFileSessionReadinessSummary().contains(
        QStringLiteral("supported extensions wav/mp3/flac/ogg")));
    QVERIFY(fixture.viewModel.audioFileValidationSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("disabled-by-policy")));
    QCOMPARE(fixture.viewModel.audioFileSupportedExtensionSummaries().size(), 4);
    QVERIFY(fixture.viewModel.audioFileSessionFallbackSummary().contains(
        QStringLiteral("no file loading")));
    QVERIFY(fixture.viewModel.audioFileSessionSafetySummary().contains(
        QStringLiteral("no-execution guarantees")));
    QCOMPARE(fixture.viewModel.audioFileSessionSafetyChecks().size(), 8);
    QVERIFY(fixture.viewModel.audioFileSessionRefusalSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("disabled-by-policy")));
    QVERIFY(fixture.viewModel.audioFileTraceSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("no file")));
    QCOMPARE(fixture.viewModel.voiceRuntimeStatus(), QStringLiteral("Unavailable"));
    QVERIFY(fixture.viewModel.voiceRuntimeSummary().contains(QStringLiteral("playback disabled")));
    QCOMPARE(fixture.viewModel.voiceRuntimeCheckSummaries().size(), 7);
    QVERIFY(!fixture.viewModel.voiceRuntimeAvailable());
    QVERIFY(!fixture.viewModel.voiceTextToSpeechAvailable());
    QVERIFY(!fixture.viewModel.voiceSpeechToTextAvailable());
    QVERIFY(!fixture.viewModel.voiceMicrophoneEnabled());
    QVERIFY(!fixture.viewModel.voicePlaybackEnabled());
    QVERIFY(fixture.viewModel.voiceLocalOnlyPolicy());
    QVERIFY(!fixture.viewModel.voiceProcessExecutionEnabled());
    QCOMPARE(fixture.viewModel.voiceRuntimeEnvironmentStatus(), QStringLiteral("Blocked"));
    QVERIFY(fixture.viewModel.voiceRuntimeEnvironmentSummary().contains(
        QStringLiteral("metadata-only")));
    QCOMPARE(fixture.viewModel.voiceBinarySummaries().size(), 2);
    QVERIFY(fixture.viewModel.voiceBinarySummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Whisper Binary: Missing")));
    QCOMPARE(fixture.viewModel.voiceModelSummaries().size(), 2);
    QVERIFY(fixture.viewModel.voiceModelSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Piper Voice Model: Missing")));
    QCOMPARE(fixture.viewModel.voiceRuntimePermissionSummaries().size(), 4);
    QVERIFY(fixture.viewModel.voiceRuntimePermissionSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Microphone: Denied")));
    QCOMPARE(fixture.viewModel.voiceRuntimeSafetyStatus(), QStringLiteral("Blocked"));
    QVERIFY(
        fixture.viewModel.voiceRuntimeSafetySummary().contains(QStringLiteral("blocks execution")));
    QCOMPARE(fixture.viewModel.voiceRuntimeSafetyChecks().size(), 7);
    QVERIFY(!fixture.viewModel.voiceRuntimeExecutionAllowed());
    QCOMPARE(fixture.viewModel.piperTtsStatus(), QStringLiteral("Disabled"));
    QVERIFY(fixture.viewModel.piperTtsSummary().contains(QStringLiteral("disabled by default")));
    QCOMPARE(fixture.viewModel.piperTtsReadinessChecks().size(), 9);
    QVERIFY(fixture.viewModel.piperTtsReadinessChecks()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Piper binary")));
    QVERIFY(!fixture.viewModel.piperTtsReady());
    QCOMPARE(fixture.viewModel.piperTtsFileOutputStatus(), QStringLiteral("Disabled"));
    QVERIFY(fixture.viewModel.piperTtsFileOutputSummary().contains(
        QStringLiteral("No playback or microphone access")));
    QCOMPARE(fixture.viewModel.piperSynthesisStatus(), QStringLiteral("Disabled"));
    QVERIFY(fixture.viewModel.piperSynthesisReadinessSummary().contains(
        QStringLiteral("execution attempted: no")));
    QVERIFY(fixture.viewModel.piperSynthesisLastSummary().contains(
        QStringLiteral("No Piper synthesis request")));
    QVERIFY(fixture.viewModel.piperSynthesisFallbackSummary().contains(QStringLiteral("no audio")));
    QVERIFY(fixture.viewModel.piperSynthesisSafetySummary().contains(
        QStringLiteral("execution attempted: no")));
    QVERIFY(fixture.viewModel.piperSynthesisTraceSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("No subprocess execution")));
    QCOMPARE(fixture.viewModel.whisperTranscriptionStatus(), QStringLiteral("Disabled"));
    QVERIFY(fixture.viewModel.whisperTranscriptionReadinessSummary().contains(
        QStringLiteral("execution attempted: no")));
    QVERIFY(fixture.viewModel.whisperTranscriptionLastSummary().contains(
        QStringLiteral("No Whisper transcription request")));
    QVERIFY(fixture.viewModel.whisperTranscriptionFallbackSummary().contains(
        QStringLiteral("no transcript")));
    QVERIFY(fixture.viewModel.whisperTranscriptionSafetySummary().contains(
        QStringLiteral("execution attempted: no")));
    QVERIFY(fixture.viewModel.whisperTranscriptionTraceSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("No microphone capture")));
}

void DesktopShellViewModelTest::exposesVoiceConfigurationMetadata() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
#ifdef Q_OS_WIN
    const auto piperBinaryPath = dir.filePath(QStringLiteral("piper.exe"));
#else
    const auto piperBinaryPath = dir.filePath(QStringLiteral("piper"));
#endif
    const auto piperModelPath = dir.filePath(QStringLiteral("voice.onnx"));
    const auto whisperBinaryPath = dir.filePath(QStringLiteral("whisper"));
    const auto whisperModelPath = dir.filePath(QStringLiteral("whisper-models"));

    QFile piperBinary{piperBinaryPath};
    QVERIFY(piperBinary.open(QIODevice::WriteOnly));
    piperBinary.close();
    QVERIFY(QFile::setPermissions(piperBinaryPath, QFile::ReadOwner | QFile::ExeOwner));

    QFile piperModel{piperModelPath};
    QVERIFY(piperModel.open(QIODevice::WriteOnly));
    piperModel.close();

    QFile whisperBinary{whisperBinaryPath};
    QVERIFY(whisperBinary.open(QIODevice::WriteOnly));
    whisperBinary.close();
    QVERIFY(QFile::setPermissions(whisperBinaryPath, QFile::ReadOwner));
    QVERIFY(QDir{}.mkpath(whisperModelPath));

    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::voiceConfigurationChanged);

    fixture.viewModel.applyVoiceConfigurationPaths(piperBinaryPath, piperModelPath,
                                                   whisperBinaryPath, whisperModelPath);

    QCOMPARE(spy.count(), 4);
    QCOMPARE(fixture.settings.piperBinaryPath(), piperBinaryPath);
    QCOMPARE(fixture.settings.piperModelPath(), piperModelPath);
    QCOMPARE(fixture.settings.whisperBinaryPath(), whisperBinaryPath);
    QCOMPARE(fixture.settings.whisperModelPath(), whisperModelPath);
    QVERIFY(fixture.viewModel.voiceConfigurationReadinessSummary().contains(
        QStringLiteral("Piper file-output TTS: Ready")));
    QVERIFY(fixture.viewModel.voiceConfigurationSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Piper binary metadata only: path exists, readable, "
                                         "executable")));
    QVERIFY(fixture.viewModel.voiceConfigurationSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Whisper binary metadata only: path exists, readable, "
                                         "non-executable")));
    QVERIFY(fixture.viewModel.voiceConfigurationStatusBadges().contains(
        QStringLiteral("Piper file-output TTS: Ready")));

    const auto autoDetectMsg = fixture.viewModel.autoDetectVoicePaths();
    QVERIFY(!autoDetectMsg.isEmpty());
    // Re-apply the mock non-executable path so subsequent isolation checks are deterministic
    fixture.viewModel.setWhisperBinaryPath(whisperBinaryPath);
    QVERIFY(fixture.viewModel.voiceConfigurationStatusBadges().contains(
        QStringLiteral("Piper binary: Ready")));
    QVERIFY(fixture.viewModel.voiceConfigurationStatusBadges().contains(
        QStringLiteral("Whisper binary: Blocked (path is not executable)")));
    QVERIFY(fixture.viewModel.voiceConfigurationValidationSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Piper binary: Ready - exists, readable, file, "
                                         "executable")));
    QVERIFY(fixture.viewModel.voiceConfigurationHintSummaries().contains(
        QStringLiteral("Piper binary hint: configured path is executable; no "
                       "suggestion needed.")));
    QVERIFY(fixture.viewModel.voiceConfigurationHintSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("settings are not changed automatically")) ||
            fixture.viewModel.voiceConfigurationHintSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("no executable found in known Homebrew/local locations")));
    QCOMPARE(fixture.viewModel.piperTtsStatus(), QStringLiteral("Ready Metadata"));
    QCOMPARE(fixture.viewModel.piperFileOutputReadinessStatus(), QStringLiteral("Ready"));
    QVERIFY(fixture.viewModel.piperFileOutputReadinessSummary().contains(
        QStringLiteral("Ready for a later controlled file-output TTS phase")));
    QVERIFY(!fixture.viewModel.piperFileOutputExecutionEnabled());
    QCOMPARE(fixture.viewModel.piperFileOutputExecutionStatus(), QStringLiteral("Ready Metadata"));
    QVERIFY(
        fixture.viewModel.piperFileOutputExecutionSummary().contains(QStringLiteral("disabled")));

    fixture.viewModel.setPiperFileOutputExecutionEnabled(true);

    QVERIFY(fixture.settings.piperFileOutputExecutionEnabled());
    QVERIFY(fixture.viewModel.piperFileOutputExecutionEnabled());
    QCOMPARE(fixture.viewModel.piperFileOutputExecutionStatus(), QStringLiteral("Configured"));
    QVERIFY(
        fixture.viewModel.piperFileOutputExecutionSummary().contains(QStringLiteral("enabled")));
    QCOMPARE(fixture.viewModel.piperFileOutputAudioPathSummary(),
             QStringLiteral("No generated Piper audio file."));
    QCOMPARE(fixture.viewModel.whisperPreparationReadinessStatus(), QStringLiteral("Blocked"));
    QVERIFY(fixture.viewModel.whisperPreparationReadinessSummary().contains(
        QStringLiteral("Whisper binary path is not executable")));
    QCOMPARE(fixture.viewModel.whisperTranscriptionStatus(), QStringLiteral("Missing Binary"));
    QVERIFY(fixture.viewModel.whisperTranscriptionReadinessSummary().contains(
        QStringLiteral("2 configured, 1 missing")));
    QCOMPARE(fixture.viewModel.piperSynthesisStatus(), QStringLiteral("Ready Metadata"));
    QVERIFY(fixture.viewModel.piperSynthesisReadinessSummary().contains(
        QStringLiteral("2 configured, 0 missing")));
    QVERIFY(fixture.viewModel.piperTtsSummary().contains(QStringLiteral("metadata")));
    QVERIFY(fixture.viewModel.piperTtsReady());
}

// Retired local-inference façade probe; readiness and execution contracts are owned by
// test_local_inference and the model-routing suites.
#if 0
void DesktopShellViewModelTest::exposesLocalInferenceBoundaryMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.localInferenceStatus(), QStringLiteral("Not Requested"));
    QVERIFY(fixture.viewModel.localInferenceSummary().contains(QStringLiteral("loopback-only")));
    QCOMPARE(fixture.viewModel.localInferenceLastResponseSummary(),
             QStringLiteral("No local inference request yet."));
    QCOMPARE(fixture.viewModel.selectedLocalModelSummary(),
             QStringLiteral("Ollama runtime client is unavailable."));
    QCOMPARE(fixture.viewModel.selectedLocalModelStatus(), QStringLiteral("Discovery Failed"));
    QCOMPARE(fixture.viewModel.selectedLocalModelMetadataSummary(),
             QStringLiteral("Selected model: sentinel-test-model (Local Only, discovery metadata unavailable)."));
    QCOMPARE(fixture.viewModel.activeLocalRuntimeBadge(),
             QStringLiteral("Ollama Local / sentinel-test-model"));
    QVERIFY(fixture.viewModel.localChatInferenceEnabled());
    QCOMPARE(fixture.viewModel.localChatInferenceStatus(), QStringLiteral("Discovery Failed"));
    QCOMPARE(fixture.viewModel.localChatInferenceSummary(),
             QStringLiteral("Ollama runtime client is unavailable."));
    QVERIFY(!fixture.viewModel.localChatSendAvailable());
    QCOMPARE(fixture.viewModel.localChatSendAvailabilitySummary(),
             QStringLiteral("Select an installed Ollama model in Settings before sending."));
    QCOMPARE(fixture.viewModel.localInferenceRuntimeState(), QStringLiteral("Unavailable"));
    QCOMPARE(fixture.viewModel.localInferenceLatencySummary(),
             QStringLiteral("No local inference latency recorded."));
    QVERIFY(fixture.viewModel.localInferenceStreamingAvailable());
    QVERIFY(fixture.viewModel.localInferenceStreamingEnabled());
    QCOMPARE(fixture.viewModel.localInferenceStreamStatus(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.localInferenceStreamSummary(),
             QStringLiteral("Ollama local streaming boundary is configured for loopback-only "
                            "/api/generate."));
    QVERIFY(fixture.viewModel.localInferenceStreamingText().isEmpty());
    QVERIFY(fixture.viewModel.localInferenceTraceSummaries().isEmpty());
}

#endif

void DesktopShellViewModelTest::exposesFrontendRuntimeStateContract() {
    ViewModelFixture fixture;
    const auto metaObject = fixture.viewModel.metaObject();

    const auto propertyNames = QStringList{
        QStringLiteral("activeRuntimeProviderLabel"),
        QStringLiteral("activeRuntimeReadinessState"),
        QStringLiteral("conversationRuntimeStreaming"),
        QStringLiteral("localInferenceBusy"),
        QStringLiteral("localInferenceStreamStatus"),
        QStringLiteral("agentLoopActive"),
        QStringLiteral("agentAwaitingApproval"),
        QStringLiteral("latestToolExecutionStatus"),
        QStringLiteral("latestToolExecutionSummary"),
        QStringLiteral("globalErrorVisible"),
        QStringLiteral("globalErrorMessage"),
    };

    for (const auto& name : propertyNames)
        QVERIFY2(metaObject->indexOfProperty(name.toLatin1().constData()) >= 0,
                 qPrintable(QStringLiteral("Missing QML runtime property: %1").arg(name)));

    QVERIFY(!fixture.viewModel.activeRuntimeProviderLabel().isEmpty());
    QVERIFY(!fixture.viewModel.activeRuntimeReadinessState().isEmpty());
    QVERIFY(!fixture.viewModel.localInferenceStreamStatus().isEmpty());
    QVERIFY(!fixture.viewModel.latestToolExecutionStatus().isEmpty());
    QVERIFY(!fixture.viewModel.latestToolExecutionSummary().isEmpty());
    QVERIFY(!fixture.viewModel.agentLoopActive());
    QVERIFY(!fixture.viewModel.agentAwaitingApproval());
    QVERIFY(!fixture.viewModel.globalErrorVisible());
}

void DesktopShellViewModelTest::forwardsBlockedLocalInferenceRequest() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::localInferenceChanged);

    const auto ran =
        fixture.viewModel.runLocalInference(QStringLiteral("hello"), QStringLiteral("llama3.2"));

    QVERIFY(!ran);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(fixture.viewModel.localInferenceStatus(), QStringLiteral("Blocked"));
    QCOMPARE(fixture.viewModel.localInferenceRuntimeState(), QStringLiteral("Failed"));
    QCOMPARE(fixture.viewModel.localInferenceSummary(),
             QStringLiteral("Local inference blocked by runtime permission policy."));
    QVERIFY(fixture.viewModel.localInferenceTraceSummaries().contains(
        QStringLiteral("2. Permission Policy [Denied]: Runtime permission policy is "
                       "metadata-only and denies execution by default.")));
}

void DesktopShellViewModelTest::exposesConversationSessionMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.conversationSessionId(), QStringLiteral("conversation-session-1"));
    QCOMPARE(fixture.viewModel.conversationSessionStatus(), QStringLiteral("Active"));
    QCOMPARE(fixture.viewModel.interactionMode(), QStringLiteral("Companion"));
    QCOMPARE(fixture.viewModel.attentionState(), QStringLiteral("Observing"));
    QCOMPARE(fixture.viewModel.contextWindowSummary(),
             QStringLiteral("Workspace context window: Local Only route, Atlas (Coordinator, "
                            "Available, Local), Ambient (Available, Public Metadata, Session)."));
}

void DesktopShellViewModelTest::exposesConversationStateMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.conversationState(), QStringLiteral("Idle"));
    QCOMPARE(fixture.viewModel.conversationTransitionStatus(), QStringLiteral("Not Requested"));
    QCOMPARE(fixture.viewModel.conversationTransitionSummary(),
             QStringLiteral("No conversation transition yet."));
}

void DesktopShellViewModelTest::exposesConversationRuntimeMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.conversationRuntimeRequestId(), QStringLiteral("None"));
    QCOMPARE(fixture.viewModel.conversationRuntimeActiveModel(), QStringLiteral("None"));
    QCOMPARE(fixture.viewModel.conversationRuntimeActiveRoute(), QStringLiteral("Provider"));
    QVERIFY(!fixture.viewModel.conversationRuntimeStreaming());
    QCOMPARE(fixture.viewModel.conversationRuntimeLastSuccessSummary(),
             QStringLiteral("No successful response yet."));
    QCOMPARE(fixture.viewModel.conversationRuntimeLastErrorSummary(),
             QStringLiteral("No error or refusal yet."));
    QCOMPARE(fixture.viewModel.conversationRuntimeLastLatencySummary(),
             QStringLiteral("No latency recorded."));
    QVERIFY(fixture.viewModel.conversationRuntimeSummary().contains(QStringLiteral("Idle")));
    QVERIFY(fixture.viewModel.conversationRuntimeSummaryLines().contains(
        QStringLiteral("State: Idle")));
}

void DesktopShellViewModelTest::updatesAndPersistsRoutingModeMetadata() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::modelRoutingChanged);
    QSignalSpy taskPlanSpy(&fixture.viewModel, &DesktopShellViewModel::taskPlanChanged);
    QSignalSpy conversationSpy(&fixture.viewModel,
                               &DesktopShellViewModel::conversationSessionChanged);
    QSignalSpy snapshotSpy(&fixture.viewModel,
                           &DesktopShellViewModel::orchestrationSnapshotChanged);

    fixture.viewModel.setRoutingModeByName(QStringLiteral("Quality"));

    QCOMPARE(fixture.viewModel.currentRoutingMode(), QStringLiteral("Quality"));
    QCOMPARE(fixture.viewModel.modelRoutingStatus(), QStringLiteral("No Available Model"));
    QCOMPARE(fixture.viewModel.selectedModelProviderSummary(),
             QStringLiteral("No configured model route is available."));
    QCOMPARE(fixture.settings.routingModeName(), QStringLiteral("Quality"));
    QVERIFY(spy.count() >= 1);
    QCOMPARE(fixture.viewModel.latestTaskPlanStatus(), QStringLiteral("Blocked"));
    QVERIFY(taskPlanSpy.count() >= 1);
    QCOMPARE(fixture.viewModel.orchestrationSnapshotStatus(), QStringLiteral("Degraded"));
    QVERIFY(fixture.viewModel.orchestrationSignals().contains(
        QStringLiteral("Routing: Quality / No Available Model")));
    QVERIFY(fixture.viewModel.contextWindowSummary().contains(QStringLiteral("Quality route")));
    QVERIFY(conversationSpy.count() >= 1);
    QVERIFY(snapshotSpy.count() >= 1);

    fixture.viewModel.setRoutingModeByName(QStringLiteral("unknown"));
    QCOMPARE(fixture.viewModel.currentRoutingMode(), QStringLiteral("Local Only"));
    QCOMPARE(fixture.settings.routingModeName(), QStringLiteral("Local Only"));
}

void DesktopShellViewModelTest::exposesAgentToolMetadata() {
    ApplicationController controller{std::make_unique<LocalEchoProvider>(),
                                     std::make_unique<InMemoryStore>(), nullptr, nullptr,
                                     std::make_unique<sentinel::core::NullAgentRuntime>()};
    ModeManager modeManager;
    AppSettings settings{std::make_unique<InMemorySettingsStore>()};
    DesktopShellViewModel viewModel{controller, modeManager, settings};

    QCOMPARE(viewModel.availableToolCount(), 1);
    QCOMPARE(viewModel.availableToolIds(), QStringList{QStringLiteral("local-plan-summary")});
}

// Retired legacy pipeline metadata probe. Current execution semantics are covered by
// test_agent_loop, test_llm_agent_runtime, and test_tool_execution_gateway.
#if 0
void DesktopShellViewModelTest::exposesLatestToolExecutionStatus() {
    ApplicationController controller{std::make_unique<LocalEchoProvider>(),
                                     std::make_unique<InMemoryStore>(), nullptr, nullptr,
                                     std::make_unique<sentinel::core::NullAgentRuntime>()};
    ModeManager modeManager;
    AppSettings settings{std::make_unique<InMemorySettingsStore>()};
    DesktopShellViewModel viewModel{controller, modeManager, settings};
    QSignalSpy executionSpy(&viewModel, &DesktopShellViewModel::toolExecutionChanged);

    QCOMPARE(viewModel.latestToolExecutionStatus(), QStringLiteral("Not Requested"));
    QCOMPARE(viewModel.latestToolExecutionSummary(),
             QStringLiteral("No tool execution boundary result yet."));
    QCOMPARE(viewModel.latestAgentPipelineStatus(), QStringLiteral("Not Requested"));
    QCOMPARE(viewModel.latestAgentPipelineSummary(),
             QStringLiteral("No agent pipeline result yet."));

    QVERIFY(viewModel.runAgentRequest(QStringLiteral("draft local plan")));
    QCOMPARE(viewModel.latestToolExecutionStatus(), QStringLiteral("Succeeded"));
    QCOMPARE(viewModel.latestToolExecutionSummary(),
             QStringLiteral("Executed: Local Plan Summary"));
    QCOMPARE(viewModel.latestAgentPipelineStatus(), QStringLiteral("Succeeded"));
    QCOMPARE(viewModel.latestAgentPipelineSummary(),
             QStringLiteral("Executed: Local Plan Summary"));
    QCOMPARE(executionSpy.count(), 1);
}

#endif

// Retired legacy pipeline metadata probe; session-context ownership is test_agent_loop.
#if 0
void DesktopShellViewModelTest::exposesRuntimeContextStatus() {
    ApplicationController controller{std::make_unique<LocalEchoProvider>(),
                                     std::make_unique<InMemoryStore>(), nullptr, nullptr,
                                     std::make_unique<sentinel::core::NullAgentRuntime>()};
    ModeManager modeManager;
    AppSettings settings{std::make_unique<InMemorySettingsStore>()};
    DesktopShellViewModel viewModel{controller, modeManager, settings};
    QSignalSpy runtimeContextSpy(&viewModel, &DesktopShellViewModel::runtimeContextChanged);

    QCOMPARE(viewModel.runtimeSessionId(), QStringLiteral("runtime-session-1"));
    QCOMPARE(viewModel.runtimeContextStatus(), QStringLiteral("Empty"));
    QCOMPARE(viewModel.runtimeContextSummary(), QStringLiteral("No runtime context yet."));
    QVERIFY(viewModel.runtimeContextActiveToolIds().isEmpty());

    QVERIFY(viewModel.runAgentRequest(QStringLiteral("draft local plan")));

    QCOMPARE(viewModel.runtimeSessionId(), QStringLiteral("runtime-session-1"));
    QCOMPARE(viewModel.runtimeContextStatus(), QStringLiteral("Active"));
    QCOMPARE(viewModel.runtimeContextSummary(),
             QStringLiteral("Runtime context captured pipeline result: Succeeded"));
    QCOMPARE(viewModel.runtimeContextActiveToolIds(),
             QStringList{QStringLiteral("local-plan-summary")});
    QCOMPARE(runtimeContextSpy.count(), 1);
}

#endif

// Retired legacy pipeline metadata probe; AgentRuntime events own this contract.
#if 0
void DesktopShellViewModelTest::exposesAgentActivityStatus() {
    ApplicationController controller{std::make_unique<LocalEchoProvider>(),
                                     std::make_unique<InMemoryStore>(), nullptr, nullptr,
                                     std::make_unique<sentinel::core::NullAgentRuntime>()};
    ModeManager modeManager;
    AppSettings settings{std::make_unique<InMemorySettingsStore>()};
    DesktopShellViewModel viewModel{controller, modeManager, settings};
    QSignalSpy activitySpy(&viewModel, &DesktopShellViewModel::agentActivityChanged);

    QCOMPARE(viewModel.agentActivityCount(), 0);
    QCOMPARE(viewModel.latestAgentActivitySummary(), QStringLiteral("No agent activity yet."));

    QVERIFY(viewModel.runAgentRequest(QStringLiteral("draft local plan")));

    QCOMPARE(viewModel.agentActivityCount(), 6);
    QCOMPARE(viewModel.latestAgentActivitySummary(),
             QStringLiteral("Agent pipeline finished: Succeeded"));
    QCOMPARE(activitySpy.count(), 1);
}

#endif

// Retired metadata-only planner probe; blocked execution is covered by test_agent_loop.
#if 0
void DesktopShellViewModelTest::updatesVisibleAgentValuesForBlockedPipeline() {
    const sentinel::core::ToolDescriptor tool{
        QStringLiteral("blocked-tool"),
        QStringLiteral("Blocked Tool"),
        QStringLiteral("Metadata-only blocked capability."),
        sentinel::core::ToolRiskLevel::Low,
        sentinel::core::ToolExecutionMode::MetadataOnly,
        {},
    };
    const sentinel::core::ToolInvocationPlan plan{
        sentinel::core::ToolInvocationPlanStatus::Planned,
        QStringLiteral("Metadata-only tool plan prepared."),
        {
            sentinel::core::PlannedToolInvocation{
                tool.id,
                tool.name,
                QStringLiteral("Plan metadata."),
                QStringLiteral("Metadata-only rationale."),
                tool.riskLevel,
                tool.executionMode,
                {},
                {
                    sentinel::core::CapabilityDescriptor{
                        QStringLiteral("tool.blocked.capability"),
                        QStringLiteral("Blocked metadata capability."),
                    },
                },
            },
        },
    };
    ApplicationController controller{
        std::make_unique<LocalEchoProvider>(), std::make_unique<InMemoryStore>(), nullptr, nullptr,
        std::make_unique<FixedPlanRuntime>(QList<sentinel::core::ToolDescriptor>{tool}, plan)};
    ModeManager modeManager;
    AppSettings settings{std::make_unique<InMemorySettingsStore>()};
    DesktopShellViewModel viewModel{controller, modeManager, settings};
    QSignalSpy pipelineSpy(&viewModel, &DesktopShellViewModel::agentPipelineChanged);
    QSignalSpy runtimeContextSpy(&viewModel, &DesktopShellViewModel::runtimeContextChanged);
    QSignalSpy activitySpy(&viewModel, &DesktopShellViewModel::agentActivityChanged);

    QVERIFY(viewModel.runAgentRequest(QStringLiteral("draft blocked plan")));

    QCOMPARE(viewModel.latestAgentPipelineStatus(), QStringLiteral("Blocked"));
    QCOMPARE(viewModel.latestAgentPipelineSummary(),
             QStringLiteral("Execution boundary blocked by sandbox capability metadata."));
    QCOMPARE(viewModel.runtimeContextStatus(), QStringLiteral("Active"));
    QCOMPARE(viewModel.runtimeContextSummary(),
             QStringLiteral("Runtime context captured pipeline result: Blocked"));
    QCOMPARE(viewModel.runtimeContextActiveToolIds(), QStringList{QStringLiteral("blocked-tool")});
    QCOMPARE(viewModel.agentActivityCount(), 6);
    QCOMPARE(viewModel.latestAgentActivitySummary(),
             QStringLiteral("Agent pipeline finished: Blocked"));
    QCOMPARE(pipelineSpy.count(), 1);
    QCOMPARE(runtimeContextSpy.count(), 1);
    QCOMPARE(activitySpy.count(), 1);
}

#endif

void DesktopShellViewModelTest::exposesOnlyQmlSafeAgentVisibilityProperties() {
    ApplicationController controller{std::make_unique<LocalEchoProvider>(),
                                     std::make_unique<InMemoryStore>(), nullptr, nullptr,
                                     std::make_unique<sentinel::core::NullAgentRuntime>()};
    ModeManager modeManager;
    AppSettings settings{std::make_unique<InMemorySettingsStore>()};
    DesktopShellViewModel viewModel{controller, modeManager, settings};
    const auto* metaObject = viewModel.metaObject();

    const QHash<QString, QByteArray> expectedTypes{
        {QStringLiteral("latestAgentPipelineStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("latestAgentPipelineSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimeSessionId"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimeContextStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimeContextSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimeContextActiveToolIds"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("conversationSessionId"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSessionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("interactionMode"), QByteArrayLiteral("QString")},
        {QStringLiteral("attentionState"), QByteArrayLiteral("QString")},
        {QStringLiteral("contextWindowSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationState"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationTransitionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationTransitionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationRuntimeSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationRuntimeSummaryLines"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("conversationRuntimeRequestId"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationRuntimeActiveModel"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationRuntimeActiveRoute"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationRuntimeStreaming"), QByteArrayLiteral("bool")},
        {QStringLiteral("conversationRuntimeLastSuccessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationRuntimeLastErrorSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationRuntimeLastLatencySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("agentActivityCount"), QByteArrayLiteral("int")},
        {QStringLiteral("latestAgentActivitySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("latestTaskPlanStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("latestTaskPlanSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("plannedTaskStepCount"), QByteArrayLiteral("int")},
        {QStringLiteral("registeredAgentCount"), QByteArrayLiteral("int")},
        {QStringLiteral("activeAgentSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("currentAgentSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("currentMemoryAffinitySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("providerCatalogCount"), QByteArrayLiteral("int")},
        {QStringLiteral("providerCatalogSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("memoryCatalogCount"), QByteArrayLiteral("int")},
        {QStringLiteral("memoryCatalogSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("orchestrationSnapshotStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("orchestrationSnapshotSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("orchestrationSignals"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("orchestrationReadinessStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("orchestrationReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("orchestrationDiagnostics"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("agentTaskRuntimeStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("agentTaskRuntimeSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("agentTaskRuntimeTaskCount"), QByteArrayLiteral("int")},
        {QStringLiteral("agentTaskQueueCount"), QByteArrayLiteral("int")},
        {QStringLiteral("agentTaskQueueActiveCount"), QByteArrayLiteral("int")},
        {QStringLiteral("agentTaskQueuePlannedCount"), QByteArrayLiteral("int")},
        {QStringLiteral("agentTaskQueueBlockedCount"), QByteArrayLiteral("int")},
        {QStringLiteral("agentTaskQueueCompletedCount"), QByteArrayLiteral("int")},
        {QStringLiteral("agentTaskQueueRefusedCount"), QByteArrayLiteral("int")},
        {QStringLiteral("latestAgentTaskSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("latestAgentTaskLifecycleSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("agentTaskQueueSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("agentTaskTraceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("agentPlanningSessionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("agentPlanningSessionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("agentPlanningCandidateCount"), QByteArrayLiteral("int")},
        {QStringLiteral("agentPlanningRefusedCount"), QByteArrayLiteral("int")},
        {QStringLiteral("agentPlanningCandidateSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("agentPlanningArbitrationSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("agentPlanningRefusalSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("agentPlanningFallbackSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("agentCapabilityRegistryStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("agentCapabilityRegistrySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("agentCapabilityCount"), QByteArrayLiteral("int")},
        {QStringLiteral("agentCapabilityEnabledCount"), QByteArrayLiteral("int")},
        {QStringLiteral("agentCapabilityDisabledCount"), QByteArrayLiteral("int")},
        {QStringLiteral("agentCapabilityRestrictedCount"), QByteArrayLiteral("int")},
        {QStringLiteral("agentCapabilitySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("agentCapabilityReadinessSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("agentCapabilitySafetySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("toolContractRegistryStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("toolContractRegistrySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("toolContractCount"), QByteArrayLiteral("int")},
        {QStringLiteral("toolContractEnabledCount"), QByteArrayLiteral("int")},
        {QStringLiteral("toolContractDisabledCount"), QByteArrayLiteral("int")},
        {QStringLiteral("toolContractRestrictedCount"), QByteArrayLiteral("int")},
        {QStringLiteral("toolContractSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("toolContractPermissionSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("toolContractSandboxSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("toolContractReadinessSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("toolContractSafetySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("localRuntimeStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeHealth"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeCapabilities"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("localRuntimeResponseStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeResponseSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeSessionCount"), QByteArrayLiteral("int")},
        {QStringLiteral("localRuntimeSessionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeSessionHealth"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeSessionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeAllocationSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeReservationSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeSessionSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("runtimeCapabilityCount"), QByteArrayLiteral("int")},
        {QStringLiteral("enabledRuntimeCapabilitySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("disabledRuntimeCapabilitySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("runtimeNegotiationProfileSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimeNegotiationSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localOnlyRuntimeEnforcementSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimePermissionDecision"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimePermissionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimeSafetyDecision"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimeSafetySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimePipelineStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimePipelineSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimePipelineTraceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("executionLifecycleState"), QByteArrayLiteral("QString")},
        {QStringLiteral("executionLifecycleStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("executionLifecycleSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("executionLifecycleTraceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("executionSessionId"), QByteArrayLiteral("QString")},
        {QStringLiteral("executionSessionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("executionSessionOwnership"), QByteArrayLiteral("QString")},
        {QStringLiteral("executionCoordinationMode"), QByteArrayLiteral("QString")},
        {QStringLiteral("executionSessionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("executionCoordinationSnapshotSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeAdapterStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeAdapterHealth"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeAdapterSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localRuntimeAdapterCapabilitySummaries"),
         QByteArrayLiteral("QStringList")},
        {QStringLiteral("providerRuntimeBridgeStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("providerRuntimeBridgeSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("providerRuntimeBridgeResponseSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimeIntegrationReadinessStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimeIntegrationReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("runtimeIntegrationReadinessChecks"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("selectedRuntimeProvider"), QByteArrayLiteral("QString")},
        {QStringLiteral("activeRuntimeProviderId"), QByteArrayLiteral("QString")},
        {QStringLiteral("activeRuntimeProviderLabel"), QByteArrayLiteral("QString")},
        {QStringLiteral("activeRuntimeModelLabel"), QByteArrayLiteral("QString")},
        {QStringLiteral("activeRuntimeReadinessState"), QByteArrayLiteral("QString")},
        {QStringLiteral("activeRuntimeReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("activeRuntimeLocalOnlySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("selectableRuntimeProviderIds"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("selectableRuntimeProviderLabels"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("runtimeProviderCardSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("runtimeProviderCapabilitySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("runtimeProviderValidationTraces"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("installedRuntimeProviderSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("configuredRuntimeProviderSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("availableLocalRuntimeSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("providerCredentialRegistryStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("providerCredentialRegistrySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("providerCredentialSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("providerCredentialReadinessSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("providerCredentialSafetySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("credentialStoreSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("credentialStoreBackendSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("credentialStoreSafetySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("credentialStoreTraceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("credentialActionReadiness"), QByteArrayLiteral("QString")},
        {QStringLiteral("credentialExecutionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("ollamaEndpoint"), QByteArrayLiteral("QString")},
        {QStringLiteral("ollamaConnectionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("ollamaHealthStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("ollamaHealthSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("ollamaModelCount"), QByteArrayLiteral("int")},
        {QStringLiteral("ollamaModelNames"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("ollamaModelSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("selectedLocalModelStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("selectedLocalModelSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("selectedLocalModelMetadataSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("activeLocalRuntimeBadge"), QByteArrayLiteral("QString")},
        {QStringLiteral("modelRegistryStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("modelRegistrySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("modelRegistryModelSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("modelLibraryInstalledSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("modelLibraryAvailableSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("modelLibraryRecommendedSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("modelLibraryDetailSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("providerDiscoverySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("modelRoleIds"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("modelRoleAssignmentSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("modelAdvisorRecommendationSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("modelAdvisorAvoidSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("downloadsCenterSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("benchmarkHubSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("selectedModelCapabilityLabels"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("modelManagementStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("modelManagementSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("modelManagementActionAvailability"), QByteArrayLiteral("QString")},
        {QStringLiteral("modelRecommendationSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("modelRequirementSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("companionEnabled"), QByteArrayLiteral("bool")},
        {QStringLiteral("companionAvailable"), QByteArrayLiteral("bool")},
        {QStringLiteral("companionPaused"), QByteArrayLiteral("bool")},
        {QStringLiteral("companionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("companionAvailability"), QByteArrayLiteral("QString")},
        {QStringLiteral("companionPlatformCapability"), QByteArrayLiteral("QString")},
        {QStringLiteral("companionPermissionPosture"), QByteArrayLiteral("QString")},
        {QStringLiteral("companionSafetyBoundary"), QByteArrayLiteral("QString")},
        {QStringLiteral("companionQuickCaptureSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("companionActionSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("companionPlatformSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("companionTraceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("developerModeEnabled"), QByteArrayLiteral("bool")},
        {QStringLiteral("updateCheckPolicy"), QByteArrayLiteral("QString")},
        {QStringLiteral("notificationPolicy"), QByteArrayLiteral("QString")},
        {QStringLiteral("onboardingComplete"), QByteArrayLiteral("bool")},
        {QStringLiteral("onboardingUseCase"), QByteArrayLiteral("QString")},
        {QStringLiteral("onboardingAiProvider"), QByteArrayLiteral("QString")},
        {QStringLiteral("recoveryDraftText"), QByteArrayLiteral("QString")},
        {QStringLiteral("reducedMotionEnabled"), QByteArrayLiteral("bool")},
        {QStringLiteral("highContrastEnabled"), QByteArrayLiteral("bool")},
        {QStringLiteral("uiDensity"), QByteArrayLiteral("QString")},
        {QStringLiteral("activityTimelineSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("notificationCenterSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("notificationCategories"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("notificationLifecycleSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("notificationFilteredSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("notificationSearchQuery"), QByteArrayLiteral("QString")},
        {QStringLiteral("notificationCategoryFilter"), QByteArrayLiteral("QString")},
        {QStringLiteral("updateWorkflowState"), QByteArrayLiteral("QString")},
        {QStringLiteral("releaseNotesSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("aboutSentinelSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("accessibilitySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("diagnosticsCenterSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("exportPreviewSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("brainInsightSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("recoveryReliabilitySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("productPolishSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("selectedSkillProfile"), QByteArrayLiteral("QString")},
        {QStringLiteral("selectedSkillProfileName"), QByteArrayLiteral("QString")},
        {QStringLiteral("selectedSkillProfileSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("selectedSkillProfileDescription"), QByteArrayLiteral("QString")},
        {QStringLiteral("selectedSkillProfileReadiness"), QByteArrayLiteral("QString")},
        {QStringLiteral("selectedSkillProfilePolicyPosture"), QByteArrayLiteral("QString")},
        {QStringLiteral("skillProfileIds"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("skillProfileNames"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("skillProfileSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("skillProfileCapabilitySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("skillProfileReadinessChecks"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("skillProfileDeveloperDiagnostics"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("selectedWorkspaceId"), QByteArrayLiteral("QString")},
        {QStringLiteral("selectedWorkspaceName"), QByteArrayLiteral("QString")},
        {QStringLiteral("selectedWorkspaceAccessState"), QByteArrayLiteral("QString")},
        {QStringLiteral("workspacePermissionPosture"), QByteArrayLiteral("QString")},
        {QStringLiteral("selectedWorkspaceRootSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("workspaceReadinessStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("workspaceReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("workspacePermissionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("workspaceIds"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("workspaceNames"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("workspaceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("workspacePermissionPostures"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("workspaceActionPlaceholders"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("workspaceReadinessChecks"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("workspaceBoundaryDiagnostics"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("workspaceTemplateNames"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("workspaceLastActionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("workspaceLastActionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("attachmentSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("attachmentStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("attachmentPreviewSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("fileChatActionSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("localKnowledgeBaseEnabled"), QByteArrayLiteral("bool")},
        {QStringLiteral("localKnowledgeBaseStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("knowledgeBaseDocumentSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("recentRetrievalSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("retrievalExplainabilitySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("brainWorkspaceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("exportCenterSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("privacyCenterSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("defaultPermissionPolicyState"), QByteArrayLiteral("QString")},
        {QStringLiteral("permissionPolicyStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("permissionPolicySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("permissionPolicyStateLabels"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("permissionPolicyDomainIds"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("permissionPolicyDomainNames"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("permissionPolicyDomainSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("permissionPolicyDeveloperDiagnostics"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("toolGatewayStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("toolGatewaySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("toolGatewayPermissionPosture"), QByteArrayLiteral("QString")},
        {QStringLiteral("toolGatewayToolCount"), QByteArrayLiteral("int")},
        {QStringLiteral("toolGatewayMetadataSafeCount"), QByteArrayLiteral("int")},
        {QStringLiteral("toolGatewayUnavailableCount"), QByteArrayLiteral("int")},
        {QStringLiteral("toolGatewayRefusedCount"), QByteArrayLiteral("int")},
        {QStringLiteral("toolGatewayToolSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("toolGatewayDeveloperDiagnostics"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voiceRuntimeMode"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceEnabled"), QByteArrayLiteral("bool")},
        {QStringLiteral("voiceReadinessStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceReadinessChecks"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voiceCapabilitySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("textToSpeechStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("textToSpeechSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("speechToTextStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("speechToTextSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceSessionId"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceSessionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceSessionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voicePipelineStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("voicePipelineSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voicePipelineTraceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voicePipelineSessionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("voicePipelineSessionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voicePipelineSessionStageReadinessSummaries"),
         QByteArrayLiteral("QStringList")},
        {QStringLiteral("voicePipelineSessionTraceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voicePipelineSessionFallbackSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voicePipelineSessionSafetySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voicePipelineSessionSafetyChecks"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voicePipelineSessionReadyStageCount"), QByteArrayLiteral("int")},
        {QStringLiteral("voicePipelineSessionBlockedStageCount"), QByteArrayLiteral("int")},
        {QStringLiteral("audioFileSessionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("audioFileSessionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("audioFileSessionReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("audioFileValidationSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("audioFileSupportedExtensionSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("audioFileSessionFallbackSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("audioFileSessionSafetySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("audioFileSessionSafetyChecks"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("audioFileSessionRefusalSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("audioFileTraceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voiceRuntimeStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceRuntimeSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceRuntimeCheckSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voiceRuntimeAvailable"), QByteArrayLiteral("bool")},
        {QStringLiteral("voiceTextToSpeechAvailable"), QByteArrayLiteral("bool")},
        {QStringLiteral("voiceSpeechToTextAvailable"), QByteArrayLiteral("bool")},
        {QStringLiteral("voiceMicrophoneEnabled"), QByteArrayLiteral("bool")},
        {QStringLiteral("voicePlaybackEnabled"), QByteArrayLiteral("bool")},
        {QStringLiteral("voiceLocalOnlyPolicy"), QByteArrayLiteral("bool")},
        {QStringLiteral("voiceProcessExecutionEnabled"), QByteArrayLiteral("bool")},
        {QStringLiteral("voiceRuntimeEnvironmentStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceRuntimeEnvironmentSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceBinarySummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voiceModelSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voiceRuntimePermissionSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voiceRuntimeSafetyStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceRuntimeSafetySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceRuntimeSafetyChecks"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voiceRuntimeExecutionAllowed"), QByteArrayLiteral("bool")},
        {QStringLiteral("piperTtsStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperTtsSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperTtsReadinessChecks"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("piperTtsReady"), QByteArrayLiteral("bool")},
        {QStringLiteral("piperTtsFileOutputStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperTtsFileOutputSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperSynthesisStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperSynthesisReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperSynthesisLastSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperSynthesisFallbackSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperSynthesisSafetySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperSynthesisTraceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voiceConfigurationSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voiceConfigurationReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceConfigurationStatusBadges"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voiceConfigurationHintSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("voiceConfigurationValidationSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("piperFileOutputReadinessStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperFileOutputReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperFileOutputExecutionEnabled"), QByteArrayLiteral("bool")},
        {QStringLiteral("piperFileOutputExecutionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperFileOutputExecutionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperFileOutputAudioPathSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("whisperPreparationReadinessStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("whisperPreparationReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceRuntimeReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceRuntimeHealth"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceRuntimeConfiguredCount"), QByteArrayLiteral("int")},
        {QStringLiteral("voiceRuntimeMissingCount"), QByteArrayLiteral("int")},
        {QStringLiteral("voiceRuntimeRefusedCount"), QByteArrayLiteral("int")},
        {QStringLiteral("voiceRuntimePermissionFoundationSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceRuntimeSandboxSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceRuntimeSafetyReportSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("voiceRuntimeReadinessChecks"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("whisperRuntimeStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("whisperRuntimeReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("whisperRuntimePathSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("whisperTranscriptionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("whisperTranscriptionReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("whisperTranscriptionLastSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("whisperTranscriptionFallbackSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("whisperTranscriptionSafetySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("whisperTranscriptionTraceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("piperRuntimeStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperRuntimeReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("piperRuntimePathSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localChatInferenceStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("localChatInferenceSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localChatSendAvailable"), QByteArrayLiteral("bool")},
        {QStringLiteral("localChatSendAvailabilitySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("chatSendLifecycleState"), QByteArrayLiteral("QString")},
        {QStringLiteral("chatSendLifecycleSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("promptContextInjectionEnabled"), QByteArrayLiteral("bool")},
        {QStringLiteral("promptContextInjectionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("promptContextInjectionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("promptContextInjectedBlockCount"), QByteArrayLiteral("int")},
        {QStringLiteral("promptContextSourceSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("promptContextSizeSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("promptContextUsedSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("contextBudgetUsageSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("contextIncludedCandidateCount"), QByteArrayLiteral("int")},
        {QStringLiteral("contextExcludedCandidateCount"), QByteArrayLiteral("int")},
        {QStringLiteral("contextAssemblyTraceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("promptContextBlockSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("conversationSummaryStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSummaryText"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSummaryBudgetSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSummaryBudgetCharacters"), QByteArrayLiteral("int")},
        {QStringLiteral("conversationSummaryBlockCount"), QByteArrayLiteral("int")},
        {QStringLiteral("conversationSummaryMessageCount"), QByteArrayLiteral("int")},
        {QStringLiteral("conversationSummaryOmittedMessageCount"), QByteArrayLiteral("int")},
        {QStringLiteral("conversationSummaryTruncatedBlockCount"), QByteArrayLiteral("int")},
        {QStringLiteral("conversationSummaryBlockSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("conversationCompressionStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationCompressionReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationCompressionPressureSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationCompressionCandidateCount"), QByteArrayLiteral("int")},
        {QStringLiteral("conversationCompressionSelectedCandidateCount"), QByteArrayLiteral("int")},
        {QStringLiteral("conversationCompressionFallbackReason"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationCompressionTraceSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationCompressionBudgetSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationCompressionCandidateSummaries"),
         QByteArrayLiteral("QStringList")},
        {QStringLiteral("conversationCompressionTraceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("conversationSummaryAvailable"), QByteArrayLiteral("bool")},
        {QStringLiteral("conversationSummaryGenerationStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSummaryReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSummaryBlockedReason"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSummaryEstimatedCompressionGain"),
         QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSummaryPreviewSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSummaryPersistenceSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSummaryInjectionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("summaryContinuityStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("summaryContinuityFreshnessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("summaryContinuityCoverageSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("summaryContinuityContributionSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("summaryContinuityFallbackSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("summaryContinuityOrderingSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("summaryContinuityBudgetTrace"), QByteArrayLiteral("QString")},
        {QStringLiteral("contextExplainabilityEnabled"), QByteArrayLiteral("bool")},
        {QStringLiteral("contextExplainabilityVisible"), QByteArrayLiteral("bool")},
        {QStringLiteral("contextReasoningSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("contextReasoningBudgetSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("contextReasoningOrderingSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("contextReasoningFallbackSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("contextReasoningContributionSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("contextReasoningInclusionHints"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("contextReasoningExclusionHints"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("contextReasoningDeveloperTraces"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("conversationSummaryCandidateSegments"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("conversationSummaryGenerationTraceSummaries"),
         QByteArrayLiteral("QStringList")},
        {QStringLiteral("localInferenceBusy"), QByteArrayLiteral("bool")},
        {QStringLiteral("localInferenceRuntimeState"), QByteArrayLiteral("QString")},
        {QStringLiteral("localInferenceStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("localInferenceSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localInferenceLastResponseSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localInferenceLatencySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localInferenceTraceSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("localInferenceStreamingAvailable"), QByteArrayLiteral("bool")},
        {QStringLiteral("localInferenceStreamStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("localInferenceStreamSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("localInferenceStreamingText"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSearchQueryText"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSearchStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSearchSummaryText"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSearchResultCount"), QByteArrayLiteral("int")},
        {QStringLiteral("conversationSearchResultSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("conversationBrowserStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationBrowserSummaryText"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationListEntryCount"), QByteArrayLiteral("int")},
        {QStringLiteral("conversationListCurrentTitle"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationListCurrentMessageCount"), QByteArrayLiteral("int")},
        {QStringLiteral("conversationListCurrentPersistenceStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationListCurrentLastUpdatedSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationListCurrentSearchAvailabilitySummary"),
         QByteArrayLiteral("QString")},
        {QStringLiteral("conversationListCurrentExportAvailabilitySummary"),
         QByteArrayLiteral("QString")},
        {QStringLiteral("conversationListCurrentSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationCurrentStorageMode"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationFutureStorageMode"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationMigrationReadiness"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationMigrationStatusSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationSchemaStatusSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationExportAvailable"), QByteArrayLiteral("bool")},
        {QStringLiteral("conversationExportReadinessStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationExportReadinessSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationExportReadinessChecks"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("conversationExportLastResultSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationExportLastStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationExportLastFileName"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationExportLastMessageCount"), QByteArrayLiteral("int")},
        {QStringLiteral("conversationExportLastTimestamp"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationPinnedSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("conversationDuplicateLastStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("conversationDuplicateLastResultSummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("memoryRecallPolicyStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("memoryRecallPolicySummary"), QByteArrayLiteral("QString")},
        {QStringLiteral("memoryRecallQueryText"), QByteArrayLiteral("QString")},
        {QStringLiteral("memoryRecallStatus"), QByteArrayLiteral("QString")},
        {QStringLiteral("memoryRecallSummaryText"), QByteArrayLiteral("QString")},
        {QStringLiteral("memoryRecallResultCount"), QByteArrayLiteral("int")},
        {QStringLiteral("memoryRecallResultSummaries"), QByteArrayLiteral("QStringList")},
        {QStringLiteral("memoryEntryCount"), QByteArrayLiteral("int")},
    };

    const QSet<QString> writableProperties{
        QStringLiteral("contextExplainabilityVisible"),
        QStringLiteral("companionEnabled"),
        QStringLiteral("currentModeName"),
        QStringLiteral("developerModeEnabled"),
        QStringLiteral("notificationPolicy"),
        QStringLiteral("onboardingComplete"),
        QStringLiteral("onboardingAiProvider"),
        QStringLiteral("onboardingUseCase"),
        QStringLiteral("reducedMotionEnabled"),
        QStringLiteral("highContrastEnabled"),
        QStringLiteral("uiDensity"),
        QStringLiteral("notificationSearchQuery"),
        QStringLiteral("notificationCategoryFilter"),
        QStringLiteral("piperFileOutputExecutionEnabled"),
        QStringLiteral("promptContextInjectionEnabled"),
        QStringLiteral("recoveryDraftText"),
        QStringLiteral("defaultPermissionPolicyState"),
        QStringLiteral("selectedSkillProfile"),
        QStringLiteral("selectedWorkspaceId"),
        QStringLiteral("localKnowledgeBaseEnabled"),
        QStringLiteral("selectedRuntimeProvider"),
        QStringLiteral("updateCheckPolicy"),
        QStringLiteral("ollamaEndpoint"),
    };

    for (auto it = expectedTypes.cbegin(); it != expectedTypes.cend(); ++it) {
        const auto propertyIndex = metaObject->indexOfProperty(it.key().toUtf8().constData());
        QVERIFY2(propertyIndex >= 0,
                 qPrintable(QStringLiteral("Missing property %1").arg(it.key())));
        const auto property = metaObject->property(propertyIndex);
        QCOMPARE(QByteArray(property.typeName()), it.value());
        QVERIFY2(property.isWritable() == writableProperties.contains(it.key()),
                 qPrintable(QStringLiteral("Property writable mismatch: %1").arg(it.key())));
    }

    const QStringList forbiddenProperties{
        QStringLiteral("latestAgentPipelineResult"),
        QStringLiteral("runtimeContext"),
        QStringLiteral("conversationSession"),
        QStringLiteral("conversationContextWindow"),
        QStringLiteral("conversationStateGraph"),
        QStringLiteral("conversationRuntimeStateRecord"),
        QStringLiteral("conversationSearchSummary"),
        QStringLiteral("conversationSearchResults"),
        QStringLiteral("conversationExportReadiness"),
        QStringLiteral("conversationExportResult"),
        QStringLiteral("conversationTransitionResult"),
        QStringLiteral("conversationTransitions"),
        QStringLiteral("agentActivityLog"),
        QStringLiteral("agentActivityEntries"),
        QStringLiteral("providerCatalog"),
        QStringLiteral("providerCatalogEntries"),
        QStringLiteral("memoryCatalog"),
        QStringLiteral("memoryCatalogEntries"),
        QStringLiteral("memoryShards"),
        QStringLiteral("memoryRecallSummary"),
        QStringLiteral("memoryRecallResults"),
        QStringLiteral("memoryRecallQuery"),
        QStringLiteral("memoryRecallPolicy"),
        QStringLiteral("orchestrationSnapshot"),
        QStringLiteral("skillProfileRegistry"),
        QStringLiteral("skillProfileEntries"),
        QStringLiteral("permissionPolicyRegistry"),
        QStringLiteral("permissionPolicyEntries"),
        QStringLiteral("permissionPolicyRawPayload"),
        QStringLiteral("toolExecutionGateway"),
        QStringLiteral("toolGatewayRegistry"),
        QStringLiteral("toolGatewayEntries"),
        QStringLiteral("toolGatewayRawPayload"),
        QStringLiteral("toolExecutionHandle"),
        QStringLiteral("workspaceStateSummary"),
        QStringLiteral("orchestrationReadinessReport"),
        QStringLiteral("orchestrationReadinessChecks"),
        QStringLiteral("orchestrationDiagnosticEntries"),
        QStringLiteral("localRuntime"),
        QStringLiteral("localRuntimeDescriptor"),
        QStringLiteral("localRuntimeRequest"),
        QStringLiteral("localRuntimeResponse"),
        QStringLiteral("localRuntimeSession"),
        QStringLiteral("localRuntimeSessions"),
        QStringLiteral("localRuntimeSessionManager"),
        QStringLiteral("localRuntimeAllocation"),
        QStringLiteral("localRuntimeReservation"),
        QStringLiteral("runtimeCapabilityRegistry"),
        QStringLiteral("runtimeCapabilities"),
        QStringLiteral("runtimeNegotiationProfile"),
        QStringLiteral("runtimeNegotiationResult"),
        QStringLiteral("runtimePermissionPolicy"),
        QStringLiteral("runtimePermissionRequest"),
        QStringLiteral("runtimePermissionDecisionRecord"),
        QStringLiteral("runtimeSafetyPolicy"),
        QStringLiteral("runtimeSafetyReport"),
        QStringLiteral("runtimePipeline"),
        QStringLiteral("runtimePipelineRequest"),
        QStringLiteral("runtimePipelineResult"),
        QStringLiteral("executionLifecycle"),
        QStringLiteral("executionLifecycleResult"),
        QStringLiteral("executionLifecycleTraces"),
        QStringLiteral("executionCoordinator"),
        QStringLiteral("executionCoordinationSnapshot"),
        QStringLiteral("executionSession"),
        QStringLiteral("executionRequest"),
        QStringLiteral("localRuntimeAdapter"),
        QStringLiteral("localRuntimeAdapterDescriptor"),
        QStringLiteral("providerRuntimeBridge"),
        QStringLiteral("providerRuntimeBridgeRequest"),
        QStringLiteral("providerRuntimeBridgeResponse"),
        QStringLiteral("runtimeIntegrationReadiness"),
        QStringLiteral("runtimeIntegrationReport"),
        QStringLiteral("runtimeIntegrationChecks"),
        QStringLiteral("localInferenceClient"),
        QStringLiteral("localInferenceRequest"),
        QStringLiteral("localInferenceResponse"),
        QStringLiteral("localInferenceStreamClient"),
        QStringLiteral("localInferenceStreamResult"),
        QStringLiteral("localInferenceStreamChunk"),
        QStringLiteral("localInferenceTrace"),
        QStringLiteral("ollamaRuntimeClient"),
        QStringLiteral("ollamaConfig"),
        QStringLiteral("ollamaEndpointRecord"),
        QStringLiteral("ollamaHealthCheck"),
        QStringLiteral("ollamaModels"),
        QStringLiteral("modelManagementService"),
        QStringLiteral("modelManagementRequest"),
        QStringLiteral("modelManagementResult"),
        QStringLiteral("textToSpeechProvider"),
        QStringLiteral("speechToTextProvider"),
        QStringLiteral("voiceProviderDescriptor"),
        QStringLiteral("voiceRequest"),
        QStringLiteral("voiceResponse"),
        QStringLiteral("voiceReadinessReport"),
        QStringLiteral("voiceRuntimeCoordinator"),
        QStringLiteral("voiceRuntimeSummaryRecord"),
        QStringLiteral("voiceSession"),
        QStringLiteral("voicePipelineResult"),
        QStringLiteral("voicePipelineTrace"),
        QStringLiteral("voicePipelineSession"),
        QStringLiteral("voicePipelineSessionResult"),
        QStringLiteral("voicePipelineSessionReadiness"),
        QStringLiteral("voicePipelineSessionTrace"),
        QStringLiteral("voicePipelineSessionSafetyReport"),
        QStringLiteral("audioFileSession"),
        QStringLiteral("audioFileSessionResult"),
        QStringLiteral("audioFileDescriptor"),
        QStringLiteral("audioFileValidation"),
        QStringLiteral("audioFileTrace"),
        QStringLiteral("audioFileSessionSafetyReport"),
        QStringLiteral("agentRegistry"),
        QStringLiteral("agentDescriptors"),
        QStringLiteral("taskPlanner"),
        QStringLiteral("latestTaskPlan"),
        QStringLiteral("controller"),
    };
    for (const auto& propertyName : forbiddenProperties) {
        QCOMPARE(metaObject->indexOfProperty(propertyName.toUtf8().constData()), -1);
    }
}

void DesktopShellViewModelTest::exposesChatHistoryStatus() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.chatHistoryStatus(), QStringLiteral("Runtime Only"));
}

void DesktopShellViewModelTest::exposesConversationStoreReadinessMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.conversationStoreStatus(), QStringLiteral("Ready"));
    QCOMPARE(fixture.viewModel.conversationStoreConversationCount(), 1);
    QVERIFY(fixture.viewModel.activeConversationSummary().contains(
        QStringLiteral("Current Transcript")));
    QCOMPARE(fixture.viewModel.conversationStoreSummaries().size(), 1);
    QCOMPARE(fixture.viewModel.conversationIds().size(), 1);
    QCOMPARE(fixture.viewModel.conversationTitles().first(), QStringLiteral("Current Transcript"));
    QCOMPARE(fixture.viewModel.conversationArchivedSummaries().first(), QStringLiteral("Active"));
    QVERIFY(!fixture.viewModel.activeConversationArchived());

    QVERIFY(fixture.sendMessage(QStringLiteral("store exposure")));

    QCOMPARE(fixture.viewModel.conversationStoreConversationCount(), 1);
    QVERIFY(fixture.viewModel.activeConversationSummary().contains(QStringLiteral("3 messages")));
    QCOMPARE(fixture.viewModel.conversationMessageCountSummaries().first(),
             QStringLiteral("3 messages"));
}

void DesktopShellViewModelTest::forwardsConversationBrowserActions() {
    ViewModelFixture fixture;

    const auto firstId = fixture.viewModel.activeConversationId();
    const auto secondId = fixture.viewModel.createConversation(QStringLiteral("Second"));
    QVERIFY(!secondId.isEmpty());
    QCOMPARE(fixture.viewModel.activeConversationId(), secondId);
    QVERIFY(fixture.viewModel.conversationTitles().contains(QStringLiteral("Second")));

    QVERIFY(fixture.viewModel.renameConversation(secondId, QStringLiteral("Renamed")));
    QVERIFY(fixture.viewModel.conversationTitles().contains(QStringLiteral("Renamed")));
    QVERIFY(fixture.viewModel.archiveConversation(secondId));
    QVERIFY(fixture.viewModel.conversationArchivedSummaries().contains(QStringLiteral("Archived")));
    QVERIFY(fixture.viewModel.unarchiveConversation(secondId));
    QVERIFY(fixture.viewModel.switchConversation(firstId));
    QCOMPARE(fixture.viewModel.activeConversationId(), firstId);
}

void DesktopShellViewModelTest::exposesPersistentPinAndDuplicateConversationActions() {
    ViewModelFixture fixture;

    const auto sourceId = fixture.viewModel.activeConversationId();
    QVERIFY(fixture.sendMessage(QStringLiteral("view model duplicate token")));

    QVERIFY(fixture.viewModel.pinConversation(sourceId));
    QCOMPARE(fixture.viewModel.conversationPinnedSummaries().first(), QStringLiteral("Pinned"));

    QVERIFY(fixture.viewModel.unpinConversation(sourceId));
    QCOMPARE(fixture.viewModel.conversationPinnedSummaries().first(), QStringLiteral("Unpinned"));

    const auto duplicateId = fixture.viewModel.duplicateConversation(sourceId);
    QVERIFY(!duplicateId.isEmpty());
    QCOMPARE(fixture.viewModel.conversationDuplicateLastStatus(), QStringLiteral("Succeeded"));
    QVERIFY(fixture.viewModel.conversationDuplicateLastResultSummary().contains(
        QStringLiteral("Copy")));
    QVERIFY(fixture.viewModel.conversationTitles().join(QStringLiteral(" ")).contains(
        QStringLiteral("Copy")));
    QVERIFY(fixture.viewModel.switchConversation(duplicateId));
    QCOMPARE(fixture.viewModel.conversationHistoryMessageCount(), 2);
}

void DesktopShellViewModelTest::exposesConversationDeleteReadinessMetadata() {
    ViewModelFixture fixture;
    QSignalSpy deleteSpy(&fixture.viewModel, &DesktopShellViewModel::conversationDeleteChanged);

    QVERIFY(fixture.viewModel.conversationDeleteAvailable());
    QCOMPARE(fixture.viewModel.conversationDeletePolicyStatus(), QStringLiteral("Enabled"));
    QVERIFY(fixture.viewModel.conversationDeletePolicySummary().contains(
        QStringLiteral("Permanent delete enabled")));
    QVERIFY(fixture.viewModel.conversationDeletePolicyRequirements().contains(
        QStringLiteral("User must confirm deletion in the UI")));
    QCOMPARE(fixture.viewModel.conversationDeleteReadinessStatus(), QStringLiteral("Ready"));
    QVERIFY(fixture.viewModel.conversationDeleteReadinessSummary().contains(
        QStringLiteral("Permanent delete is available")));
    QVERIFY(fixture.viewModel.conversationDeleteReadinessChecks().contains(
        QStringLiteral("Permanent delete: Enabled")));
    QVERIFY(fixture.viewModel.activeConversationStateSummary().contains(
        QStringLiteral("sending is available")));
    QCOMPARE(fixture.viewModel.activeConversationCount(), 1);
    QCOMPARE(fixture.viewModel.archivedConversationCount(), 0);
    QVERIFY(fixture.viewModel.conversationBrowserEmptyStateVisible());
    QVERIFY(fixture.viewModel.conversationBrowserEmptyStateSummary().contains(
        QStringLiteral("No user-created conversations")));

    const auto activeId = fixture.viewModel.activeConversationId();
    // Create a second conversation so there is somewhere to switch after deletion
    fixture.viewModel.createConversation(QStringLiteral("Second"));
    QVERIFY(fixture.viewModel.switchConversation(activeId));

    // Delete should now succeed
    QVERIFY(fixture.viewModel.requestPermanentDeleteConversation(activeId));

    QVERIFY(deleteSpy.size() >= 1);
    QCOMPARE(fixture.viewModel.conversationDeleteLastStatus(), QStringLiteral("Deleted"));
    QVERIFY(fixture.viewModel.conversationDeleteLastResultSummary().contains(
        QStringLiteral("deleted successfully")));
    QVERIFY(fixture.viewModel.activeConversationId() != activeId);
    QCOMPARE(fixture.viewModel.conversationStoreConversationCount(), 1);
}

void DesktopShellViewModelTest::exposesConversationHistorySummaryMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.conversationPersistenceStatus(), QStringLiteral("Runtime Only"));
    QCOMPARE(fixture.viewModel.conversationHistoryMessageCount(), 0);
    QVERIFY(fixture.viewModel.conversationHistorySummaryText().contains(QStringLiteral("1 message")));
    QVERIFY(fixture.viewModel.conversationLastSavedStatus().contains(
        QStringLiteral("Runtime-only transcript")));

    QVERIFY(fixture.sendMessage(QStringLiteral("status")));

    QCOMPARE(fixture.viewModel.conversationHistoryMessageCount(), 2);
    QVERIFY(fixture.viewModel.conversationHistorySummaryText().contains(QStringLiteral("3 messages")));
    QCOMPARE(fixture.viewModel.conversationLastSavedStatus(),
             QStringLiteral("Runtime-only transcript; persistence unavailable."));
}

void DesktopShellViewModelTest::exposesConversationBrowserMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.conversationBrowserStatus(), QStringLiteral("Empty Transcript"));
    QCOMPARE(fixture.viewModel.conversationListEntryCount(), 1);
    QCOMPARE(fixture.viewModel.conversationListCurrentTitle(),
             QStringLiteral("Current Transcript"));
    QCOMPARE(fixture.viewModel.conversationListCurrentMessageCount(), 1);
    QCOMPARE(fixture.viewModel.conversationListCurrentPersistenceStatus(),
             QStringLiteral("Runtime Only"));
    QVERIFY(fixture.viewModel.conversationListCurrentSearchAvailabilitySummary().contains(
        QStringLiteral("Search ready")));
    QVERIFY(fixture.viewModel.conversationListCurrentExportAvailabilitySummary().contains(
        QStringLiteral("Available")));

    QVERIFY(fixture.sendMessage(QStringLiteral("browser token")));

    QCOMPARE(fixture.viewModel.conversationBrowserStatus(), QStringLiteral("Ready"));
    QCOMPARE(fixture.viewModel.conversationListCurrentMessageCount(), 3);
}

void DesktopShellViewModelTest::exposesMultiConversationReadinessMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.conversationCurrentStorageMode(),
             QStringLiteral("Single Transcript"));
    QCOMPARE(fixture.viewModel.conversationFutureStorageMode(),
             QStringLiteral("Multi Conversation"));
    QCOMPARE(fixture.viewModel.conversationMigrationReadiness(), QStringLiteral("Not Started"));
    QCOMPARE(fixture.viewModel.conversationMigrationStatusSummary(),
             QStringLiteral("Not Started / Planned"));
    QVERIFY(fixture.viewModel.conversationSchemaStatusSummary().contains(
        QStringLiteral("No conversation schema migration applied")));
    QCOMPARE(fixture.viewModel.conversationListEntryCount(), 1);
}

void DesktopShellViewModelTest::exposesConversationSearchAndExportMetadata() {
    ViewModelFixture fixture;
    QSignalSpy searchSpy(&fixture.viewModel, &DesktopShellViewModel::conversationSearchChanged);
    QSignalSpy exportSpy(&fixture.viewModel, &DesktopShellViewModel::conversationExportChanged);

    QCOMPARE(fixture.viewModel.conversationSearchStatus(), QStringLiteral("Empty Query"));
    QCOMPARE(fixture.viewModel.conversationSearchResultCount(), 0);
    QVERIFY(fixture.viewModel.conversationSearchSummaryText().contains(
        QStringLiteral("No transcript search query")));
    QVERIFY(fixture.viewModel.conversationExportAvailable());
    QCOMPARE(fixture.viewModel.conversationExportReadinessStatus(), QStringLiteral("Ready"));
    QVERIFY(fixture.viewModel.conversationExportReadinessChecks().contains(
        QStringLiteral("Output: App-controlled export directory")));
    QCOMPARE(fixture.viewModel.conversationExportLastStatus(), QStringLiteral("Not Run"));

    QVERIFY(fixture.sendMessage(QStringLiteral("search token")));
    QVERIFY(fixture.viewModel.searchConversation(QStringLiteral("token")));

    QCOMPARE(fixture.viewModel.conversationSearchQueryText(), QStringLiteral("token"));
    QCOMPARE(fixture.viewModel.conversationSearchStatus(), QStringLiteral("Completed"));
    QCOMPARE(fixture.viewModel.conversationSearchResultCount(), 1);
    QVERIFY(fixture.viewModel.conversationSearchResultSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("user #2")));
    QVERIFY(fixture.viewModel.conversationListCurrentSearchAvailabilitySummary().contains(
        QStringLiteral("Completed")));
    QVERIFY(searchSpy.count() >= 1);

    QTemporaryDir exportDir;
    QVERIFY(exportDir.isValid());
    fixture.controller->setConversationExportDirectory(exportDir.path());
    QVERIFY(fixture.viewModel.exportTranscript(QStringLiteral("json")));
    QCOMPARE(fixture.viewModel.conversationExportLastStatus(), QStringLiteral("Succeeded"));
    QVERIFY(fixture.viewModel.conversationExportLastFileName().endsWith(QStringLiteral(".json")));
    QCOMPARE(fixture.viewModel.conversationExportLastMessageCount(), 3);
    QVERIFY(fixture.viewModel.conversationListCurrentExportAvailabilitySummary().contains(
        QStringLiteral("Last export: Succeeded")));
    QCOMPARE(exportSpy.count(), 1);

    fixture.viewModel.clearConversationSearch();
    QCOMPARE(fixture.viewModel.conversationSearchStatus(), QStringLiteral("Empty Query"));
    QCOMPARE(fixture.viewModel.conversationSearchResultCount(), 0);
}

void DesktopShellViewModelTest::exposesMaintenanceStatuses() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.memoryMaintenanceStatus(), QStringLiteral("Ready"));
    QCOMPARE(fixture.viewModel.chatMaintenanceStatus(), QStringLiteral("Ready"));
}

void DesktopShellViewModelTest::exposesMemoryCandidateMetadata() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::memoryCandidatesChanged);

    const auto id = fixture.viewModel.createMemoryCandidateFromConversationText(
        QStringLiteral("Sentinel memory candidates require user review."));

    QCOMPARE(id, QStringLiteral("memory-candidate-1"));
    QCOMPARE(fixture.viewModel.memoryCandidateCount(), 1);
    QCOMPARE(fixture.viewModel.pendingMemoryCandidateCount(), 1);
    QCOMPARE(fixture.viewModel.approvedMemoryCandidateCount(), 0);
    QCOMPARE(fixture.viewModel.archivedMemoryCandidateCount(), 0);
    QCOMPARE(fixture.viewModel.committedMemoryCandidateCount(), 0);
    QCOMPARE(fixture.viewModel.memoryCandidateIds(), QStringList{id});
    QCOMPARE(fixture.viewModel.memoryCandidateReviewStates(),
             QStringList{QStringLiteral("Pending Review")});
    QCOMPARE(fixture.viewModel.memoryCandidateCommitStatuses(),
             QStringList{QStringLiteral("Not Committed")});
    QVERIFY(fixture.viewModel.memoryCandidateSummaries().first().contains(
        QStringLiteral("Pending Review")));
    QVERIFY(fixture.viewModel.approveMemoryCandidate(id));
    QCOMPARE(fixture.viewModel.pendingMemoryCandidateCount(), 0);
    QCOMPARE(fixture.viewModel.approvedMemoryCandidateCount(), 1);
    QCOMPARE(fixture.viewModel.lastMemoryCandidateReviewStatus(), QStringLiteral("Accepted"));
    QVERIFY(
        fixture.viewModel.lastMemoryCandidateReviewSummary().contains(QStringLiteral("Approve")));
    QCOMPARE(fixture.viewModel.memoryCommitPlanCount(), 1);
    QCOMPARE(fixture.viewModel.memoryCommitReadinessStatus(), QStringLiteral("Ready"));
    QVERIFY(fixture.viewModel.memoryCommitReadinessSummary().contains(
        QStringLiteral("explicit user action")));
    QVERIFY(
        fixture.viewModel.memoryCommitTargetSummary().contains(QStringLiteral("Key-value Memory")));
    QVERIFY(fixture.viewModel.memoryCommitCandidateSummaries().first().contains(id));
    QVERIFY(fixture.viewModel.requestMemoryCandidateCommit(id));
    QCOMPARE(fixture.viewModel.lastMemoryCommitStatus(), QStringLiteral("Committed"));
    QVERIFY(fixture.viewModel.lastMemoryCommitResultSummary().contains(
        QStringLiteral("memory.semantic.conversation-memory-candidate.memory-candidate-1")));
    QCOMPARE(fixture.viewModel.committedMemoryCandidateCount(), 1);
    QCOMPARE(fixture.viewModel.memoryCandidateCommitStatuses(),
             QStringList{QStringLiteral("Committed")});
    QVERIFY(
        fixture.viewModel.memoryCandidateSummaries().first().contains(QStringLiteral("Committed")));
    QCOMPARE(spy.count(), 3);
}

void DesktopShellViewModelTest::exposesLocalMemoryRecallMetadata() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::memoryRecallChanged);

    fixture.viewModel.remember(QStringLiteral("preference.answerStyle"),
                               QStringLiteral("Use concise local summaries."));
    QVERIFY(fixture.viewModel.recallLocalMemory(QStringLiteral("concise")));

    QCOMPARE(fixture.viewModel.memoryRecallPolicyStatus(), QStringLiteral("Local Literal Recall"));
    QCOMPARE(fixture.viewModel.memoryRecallStatus(), QStringLiteral("Completed"));
    QCOMPARE(fixture.viewModel.memoryRecallResultCount(), 1);
    QCOMPARE(fixture.viewModel.memoryEntryCount(), 1);
    QVERIFY(fixture.viewModel.memoryRecallSummaryText().contains(QStringLiteral("Found 1")));
    QVERIFY(fixture.viewModel.memoryRecallResultSummaries().first().contains(
        QStringLiteral("preference.answerStyle")));
    QVERIFY(spy.count() >= 1);

    fixture.viewModel.clearLocalMemoryRecall();
    QCOMPARE(fixture.viewModel.memoryRecallStatus(), QStringLiteral("Not Searched"));
    QCOMPARE(fixture.viewModel.memoryRecallResultCount(), 0);
}

void DesktopShellViewModelTest::exposesContextAssemblyMetadata() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::contextAssemblyChanged);

    fixture.viewModel.remember(QStringLiteral("preference.answerStyle"),
                               QStringLiteral("Use concise local summaries."));

    QCOMPARE(fixture.viewModel.contextAssemblyPolicyStatus(), QStringLiteral("Planning Only"));
    QCOMPARE(fixture.viewModel.contextAssemblyStatus(), QStringLiteral("Ready"));
    QVERIFY(fixture.viewModel.contextAssemblySummaryText().contains(QStringLiteral("available")));
    QVERIFY(fixture.viewModel.contextAssemblySourceCount() >= 4);
    QVERIFY(fixture.viewModel.contextAssemblyAvailableSourceCount() >= 3);
    QVERIFY(fixture.viewModel.contextAssemblyCandidateBlockCount() >= 3);
    QVERIFY(fixture.viewModel.contextAssemblyEstimatedSize() > 0);
    QCOMPARE(fixture.viewModel.conversationContextAvailability(), QStringLiteral("Available"));
    QCOMPARE(fixture.viewModel.committedMemoryContextAvailability(), QStringLiteral("Available"));
    QCOMPARE(fixture.viewModel.runtimeMetadataContextAvailability(), QStringLiteral("Available"));
    QCOMPARE(fixture.viewModel.orchestrationContextAvailability(), QStringLiteral("Available"));
    QVERIFY(fixture.viewModel.contextAssemblySourceSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Committed Memory Context")));
    QVERIFY(fixture.viewModel.contextAssemblyReadinessChecks().contains(
        QStringLiteral("Prompt assembly: disabled")));
    QCOMPARE(fixture.viewModel.promptContextInjectionStatus(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.promptContextInjectedBlockCount(), 0);
    QVERIFY(fixture.sendMessage(QStringLiteral("concise local summaries")));
    QVERIFY(fixture.viewModel.memoryRelevanceSummaryText().contains(
        QStringLiteral("Memory relevance")));
    QVERIFY(fixture.viewModel.memoryRelevanceIncludedCount() >= 1);
    QVERIFY(fixture.viewModel.conversationSalienceSummaryText().contains(
        QStringLiteral("Conversation salience")));
    QVERIFY(fixture.viewModel.conversationSalienceIncludedCount() >= 1);
    QVERIFY(fixture.viewModel.conversationSalienceBudgetSummary().contains(
        QStringLiteral("salience context characters")));
    QVERIFY(fixture.viewModel.conversationSalienceAllocationSummary().contains(
        QStringLiteral("Active conversation")));
    QCOMPARE(fixture.viewModel.promptContextUsedMemoryCount(), 0);
    QVERIFY(!fixture.viewModel.memoryRelevanceTraceSummaries().isEmpty());
    QVERIFY(!fixture.viewModel.conversationSalienceTraceSummaries().isEmpty());
    QVERIFY(spy.count() >= 1);
}

void DesktopShellViewModelTest::exposesConversationWindowMetadata() {
    ViewModelFixture fixture;

    for (int i = 0; i < 12; ++i) {
        QVERIFY(fixture.sendMessage(
            QStringLiteral("window marker %1 %2").arg(i).arg(QString(120, QLatin1Char('w')))));
    }

    QCOMPARE(fixture.viewModel.conversationWindowStatus(), QStringLiteral("Truncated"));
    QCOMPARE(fixture.viewModel.conversationWindowBudgetCharacters(), 1200);
    QVERIFY(fixture.viewModel.conversationWindowSummary().contains(QStringLiteral("includes")));
    QVERIFY(fixture.viewModel.conversationWindowBudgetSummary().contains(QStringLiteral("1200")));
    QVERIFY(fixture.viewModel.conversationWindowIncludedMessageCount() > 0);
    QVERIFY(fixture.viewModel.conversationWindowOmittedMessageCount() > 0);
    QVERIFY(fixture.viewModel.conversationWindowTruncatedMessageCount() >= 0);
}

void DesktopShellViewModelTest::exposesConversationSummaryMetadata() {
    ViewModelFixture fixture;

    for (int i = 0; i < 12; ++i) {
        QVERIFY(fixture.sendMessage(
            QStringLiteral("summary marker %1 %2").arg(i).arg(QString(120, QLatin1Char('s')))));
    }

    QCOMPARE(fixture.viewModel.conversationSummaryBudgetCharacters(), 700);
    QVERIFY(fixture.viewModel.conversationSummaryStatus() == QStringLiteral("Ready") ||
            fixture.viewModel.conversationSummaryStatus() == QStringLiteral("Truncated"));
    QVERIFY(fixture.viewModel.conversationSummaryText().contains(QStringLiteral("older")));
    QVERIFY(fixture.viewModel.conversationSummaryBudgetSummary().contains(QStringLiteral("700")));
    QVERIFY(fixture.viewModel.conversationSummaryBlockCount() > 0);
    QVERIFY(fixture.viewModel.conversationSummaryMessageCount() > 0);
    QVERIFY(fixture.viewModel.conversationSummaryOmittedMessageCount() >= 0);
    QVERIFY(fixture.viewModel.conversationSummaryTruncatedBlockCount() >= 0);
    QVERIFY(!fixture.viewModel.conversationSummaryBlockSummaries().isEmpty());
}

void DesktopShellViewModelTest::exposesConversationCompressionMetadata() {
    ViewModelFixture fixture;

    QCOMPARE(fixture.viewModel.conversationCompressionStatus(), QStringLiteral("Not Needed"));
    QCOMPARE(fixture.viewModel.conversationCompressionCandidateCount(), 0);

    for (int i = 0; i < 18; ++i) {
        QVERIFY(fixture.sendMessage(QStringLiteral("remember my compression marker %1 %2")
                                                  .arg(i)
                                                  .arg(QString(220, QLatin1Char('m')))));
    }

    QVERIFY(fixture.viewModel.conversationCompressionStatus() == QStringLiteral("Planned") ||
            fixture.viewModel.conversationCompressionStatus() == QStringLiteral("Needed") ||
            fixture.viewModel.conversationCompressionStatus() == QStringLiteral("Approaching"));
    QVERIFY(fixture.viewModel.conversationCompressionReadinessSummary().contains(
        QStringLiteral("pressure")));
    QVERIFY(fixture.viewModel.conversationCompressionPressureSummary().contains(
        QStringLiteral("messages")));
    QVERIFY(fixture.viewModel.conversationCompressionCandidateCount() > 0);
    QVERIFY(fixture.viewModel.conversationCompressionSelectedCandidateCount() > 0);
    QVERIFY(fixture.viewModel.conversationCompressionBudgetSummary().contains(
        QStringLiteral("compression candidate")));
    QVERIFY(!fixture.viewModel.conversationCompressionCandidateSummaries().isEmpty());
    QVERIFY(!fixture.viewModel.conversationCompressionTraceSummaries().isEmpty());
}

void DesktopShellViewModelTest::exposesManualConversationSummaryGenerationMetadata() {
    ViewModelFixture fixture;

    for (int i = 0; i < 18; ++i) {
        QVERIFY(
            fixture.sendMessage(QStringLiteral("remember summary view model marker %1 %2")
                                              .arg(i)
                                              .arg(QString(180, QLatin1Char('v')))));
    }

    QVERIFY(fixture.viewModel.requestConversationSummaryGeneration());
    QCOMPARE(fixture.viewModel.conversationSummaryAvailable(), true);
    QCOMPARE(fixture.viewModel.conversationSummaryGenerationStatus(), QStringLiteral("Planned"));
    QVERIFY(!fixture.viewModel.conversationSummaryReadinessSummary().trimmed().isEmpty());
    QVERIFY(!fixture.viewModel.conversationSummaryBlockedReason().trimmed().isEmpty());
    QVERIFY(fixture.viewModel.conversationSummaryEstimatedCompressionGain().contains(
        QStringLiteral("estimated gain")));
    QVERIFY(fixture.viewModel.conversationSummaryPreviewSummary().contains(
        QStringLiteral("Manual summary preview")));
    QVERIFY(!fixture.viewModel.summaryContinuityStatus().trimmed().isEmpty());
    QVERIFY(fixture.viewModel.summaryContinuityOrderingSummary().contains(
        QStringLiteral("active conversation recency")));
    QVERIFY(fixture.viewModel.summaryContinuityBudgetTrace().contains(
        QStringLiteral("Continuity budget")));
    QVERIFY(fixture.viewModel.contextExplainabilityEnabled());
    QVERIFY(fixture.viewModel.contextExplainabilityVisible());
    QVERIFY(
        fixture.viewModel.contextReasoningSummary().contains(QStringLiteral("Context reasoning")));
    QVERIFY(fixture.viewModel.contextReasoningBudgetSummary().contains(QStringLiteral("chars")));
    QVERIFY(fixture.viewModel.contextReasoningOrderingSummary().contains(
        QStringLiteral("recent transcript")));
    QVERIFY(!fixture.viewModel.contextReasoningFallbackSummary().trimmed().isEmpty());
    QVERIFY(!fixture.viewModel.contextReasoningDeveloperTraces().isEmpty());
    QVERIFY(!fixture.viewModel.conversationSummaryCandidateSegments().isEmpty());
    QVERIFY(!fixture.viewModel.conversationSummaryGenerationTraceSummaries().isEmpty());
}

void DesktopShellViewModelTest::exposesRetrievalPlanningMetadata() {
    ViewModelFixture fixture;

    for (int i = 0; i < 12; ++i) {
        QVERIFY(fixture.sendMessage(
            QStringLiteral("retrieval marker %1 %2").arg(i).arg(QString(120, QLatin1Char('r')))));
    }
    fixture.viewModel.remember(QStringLiteral("retrieval.preference"),
                               QStringLiteral("local metadata only"));

    QVERIFY(fixture.viewModel.retrievalPlanningStatus() == QStringLiteral("Ready") ||
            fixture.viewModel.retrievalPlanningStatus() == QStringLiteral("Truncated"));
    QCOMPARE(fixture.viewModel.retrievalPlanningReadiness(), QStringLiteral("Ready"));
    QVERIFY(fixture.viewModel.retrievalPlanningSelectedSourceCount() >= 4);
    QVERIFY(fixture.viewModel.retrievalPlanningSelectedCandidateCount() >= 4);
    QVERIFY(fixture.viewModel.retrievalPlanningExcludedCandidateCount() >= 0);
    QVERIFY(fixture.viewModel.retrievalPlanningTruncatedCandidateCount() >= 0);
    QVERIFY(fixture.viewModel.retrievalPlanningBudgetSummary().contains(QStringLiteral("3200")));
    QVERIFY(fixture.viewModel.retrievalPlanningSourceSummary().contains(
        QStringLiteral("Conversation Context")));
    QVERIFY(!fixture.viewModel.retrievalPlanningSourceSummaries().isEmpty());
}

void DesktopShellViewModelTest::exposesSemanticVectorReadinessMetadata() {
    ViewModelFixture fixture;
    const auto metaObject = fixture.viewModel.metaObject();

    QCOMPARE(fixture.viewModel.semanticRetrievalEnabled(), false);
    QCOMPARE(fixture.viewModel.semanticRetrievalStatus(), QStringLiteral("Disabled"));
    QVERIFY(fixture.viewModel.semanticRetrievalSummary().contains(
        QStringLiteral("Semantic retrieval is disabled")));
    QVERIFY(fixture.viewModel.semanticReadiness().contains(
        QStringLiteral("Semantic-ready metadata only")));
    QCOMPARE(fixture.viewModel.embeddingProviderReadiness(), QStringLiteral("Not Configured"));
    QCOMPARE(fixture.viewModel.vectorIndexReadiness(), QStringLiteral("Not Configured"));
    QCOMPARE(fixture.viewModel.vectorIndexedItemCount(), 0);
    QVERIFY(fixture.viewModel.embeddingProviderSummary().contains(
        QStringLiteral("No embedding provider is configured")));
    QVERIFY(fixture.viewModel.vectorIndexSummary().contains(QStringLiteral("Indexed items: 0")));
    QVERIFY(fixture.viewModel.semanticRetrievalReadinessChecks().contains(
        QStringLiteral("Raw vectors exposed to QML: no")));
    QVERIFY(metaObject->indexOfProperty("semanticRetrievalSummary") >= 0);
    QVERIFY(metaObject->indexOfProperty("embeddingProviderSummary") >= 0);
    QVERIFY(metaObject->indexOfProperty("vectorIndexedItemCount") >= 0);
    QCOMPARE(metaObject->indexOfProperty("embeddingVector"), -1);
}

void DesktopShellViewModelTest::exposesSemanticProviderPlanningMetadata() {
    ViewModelFixture fixture;
    const auto metaObject = fixture.viewModel.metaObject();

    QCOMPARE(fixture.viewModel.semanticProviderMode(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.selectedSemanticProviderName(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.semanticProviderReadiness(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.semanticProviderHealth(), QStringLiteral("Not Checked"));
    QCOMPARE(fixture.viewModel.semanticActivationReadiness(), QStringLiteral("Refused"));
    QVERIFY(fixture.viewModel.semanticProviderStatusSummary().contains(
        QStringLiteral("Selected semantic provider: Disabled")));
    QVERIFY(fixture.viewModel.semanticActivationSummary().contains(
        QStringLiteral("Semantic activation refused")));
    QVERIFY(fixture.viewModel.semanticProviderCapabilitySummaries().contains(
        QStringLiteral("Vector writes blocked")));
    QVERIFY(fixture.viewModel.semanticActivationRequiredSteps()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("deterministic retrieval")));
    QVERIFY(metaObject->indexOfProperty("selectedSemanticProviderName") >= 0);
    QVERIFY(metaObject->indexOfProperty("semanticActivationSummary") >= 0);
    QCOMPARE(metaObject->indexOfProperty("semanticProviderConfigPath"), -1);
}

void DesktopShellViewModelTest::exposesSemanticCandidateOrchestrationMetadata() {
    ViewModelFixture fixture;
    const auto metaObject = fixture.viewModel.metaObject();

    for (int i = 0; i < 8; ++i) {
        QVERIFY(fixture.sendMessage(QStringLiteral("view semantic candidate %1 %2")
                                                  .arg(i)
                                                  .arg(QString(90, QLatin1Char('v')))));
    }
    fixture.viewModel.remember(QStringLiteral("view.candidate"),
                               QStringLiteral("qml safe metadata"));

    QVERIFY(fixture.viewModel.semanticCandidateStatus() == QStringLiteral("Ready") ||
            fixture.viewModel.semanticCandidateStatus() == QStringLiteral("Truncated"));
    QVERIFY(fixture.viewModel.semanticCandidateCount() >=
            fixture.viewModel.semanticCandidateSelectedCount());
    QVERIFY(fixture.viewModel.semanticCandidateSelectedCount() >= 4);
    QVERIFY(fixture.viewModel.semanticCandidateExcludedCount() >= 1);
    QVERIFY(fixture.viewModel.semanticCandidateBudgetSummary().contains(QStringLiteral("3200")));
    QVERIFY(fixture.viewModel.semanticCandidateArbitrationSummary().contains(
        QStringLiteral("Deterministic source order")));
    QVERIFY(fixture.viewModel.semanticCandidateParticipationSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Committed Memory")));
    QCOMPARE(fixture.viewModel.hybridRetrievalStatus(), QStringLiteral("Deterministic Only"));
    QVERIFY(fixture.viewModel.hybridRetrievalReadiness().contains(
        QStringLiteral("semantic retrieval is disabled")));
    QVERIFY(fixture.viewModel.hybridRetrievalReadinessChecks().contains(
        QStringLiteral("Prompt mutation from semantic candidates: disabled")));
    QVERIFY(metaObject->indexOfProperty("semanticCandidateSummary") >= 0);
    QVERIFY(metaObject->indexOfProperty("hybridRetrievalSummary") >= 0);
    QCOMPARE(metaObject->indexOfProperty("semanticCandidateContent"), -1);
}

void DesktopShellViewModelTest::exposesSemanticArbitrationAndRuntimePlanningMetadata() {
    ViewModelFixture fixture;
    const auto metaObject = fixture.viewModel.metaObject();

    for (int i = 0; i < 6; ++i) {
        QVERIFY(fixture.sendMessage(QStringLiteral("view semantic arbitration %1 %2")
                                                  .arg(i)
                                                  .arg(QString(90, QLatin1Char('s')))));
    }
    fixture.viewModel.remember(QStringLiteral("view.arbitration"),
                               QStringLiteral("runtime planning metadata"));

    QCOMPARE(fixture.viewModel.semanticArbitrationStatus(), QStringLiteral("Simulated"));
    QVERIFY(fixture.viewModel.semanticArbitrationReadiness().contains(
        QStringLiteral("cannot change prompts")));
    QVERIFY(fixture.viewModel.semanticArbitrationSummary().contains(
        QStringLiteral("Deterministic retrieval remains final authority")));
    QVERIFY(fixture.viewModel.semanticArbitrationBudgetSummary().contains(
        QStringLiteral("0 semantic candidates selected")));
    QVERIFY(!fixture.viewModel.semanticArbitrationSelectionSummaries().isEmpty());
    QVERIFY(fixture.viewModel.semanticArbitrationChecks().contains(
        QStringLiteral("Provider/model inference: disabled")));
    QCOMPARE(fixture.viewModel.embeddingRuntimeReadiness(), QStringLiteral("Blocked"));
    QVERIFY(fixture.viewModel.embeddingRuntimeBudgetSummary().contains(
        QStringLiteral("planned embedding jobs")));
    QVERIFY(fixture.viewModel.embeddingRuntimeRequirementSummaries().contains(
        QStringLiteral("Explicit local embedding provider gate")));
    QVERIFY(fixture.viewModel.embeddingRuntimeConstraintSummaries().contains(
        QStringLiteral("No filesystem indexing while disabled")));
    QVERIFY(metaObject->indexOfProperty("semanticArbitrationSummary") >= 0);
    QVERIFY(metaObject->indexOfProperty("embeddingRuntimeSummary") >= 0);
    QCOMPARE(metaObject->indexOfProperty("semanticCandidateScorePayload"), -1);
    QCOMPARE(metaObject->indexOfProperty("embeddingRuntimePath"), -1);
}

void DesktopShellViewModelTest::exposesIsolatedEmbeddingRuntimeMetadata() {
    ViewModelFixture fixture;
    const auto metaObject = fixture.viewModel.metaObject();

    QCOMPARE(fixture.viewModel.isolatedEmbeddingRuntimeStatus(), QStringLiteral("Refused"));
    QCOMPARE(fixture.viewModel.isolatedEmbeddingRuntimeHealth(), QStringLiteral("Blocked"));
    QCOMPARE(fixture.viewModel.isolatedEmbeddingRuntimeReadiness(), QStringLiteral("Refused"));
    QVERIFY(fixture.viewModel.isolatedEmbeddingRuntimeSummary().contains(
        QStringLiteral("readiness metadata only")));
    QVERIFY(fixture.viewModel.isolatedEmbeddingRuntimeBoundedState().contains(
        QStringLiteral("no vectors persisted")));
    QVERIFY(fixture.viewModel.isolatedEmbeddingRuntimeChecks().contains(
        QStringLiteral("Filesystem indexing: disabled")));
    QVERIFY(fixture.viewModel.isolatedEmbeddingRuntimeChecks().contains(
        QStringLiteral("Background indexing jobs: disabled")));
    QVERIFY(metaObject->indexOfProperty("isolatedEmbeddingRuntimeStatus") >= 0);
    QVERIFY(metaObject->indexOfProperty("isolatedEmbeddingRuntimeBoundedState") >= 0);
    QCOMPARE(metaObject->indexOfProperty("isolatedEmbeddingVectorPayload"), -1);
    QCOMPARE(metaObject->indexOfProperty("isolatedEmbeddingDebugPayload"), -1);
}

void DesktopShellViewModelTest::exposesVectorPersistenceMetadata() {
    ViewModelFixture fixture;
    const auto metaObject = fixture.viewModel.metaObject();

    QCOMPARE(fixture.viewModel.vectorPersistenceStatus(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.vectorPersistenceHealth(), QStringLiteral("Blocked"));
    QCOMPARE(fixture.viewModel.vectorPersistenceReadiness(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.vectorPersistenceIndexedItemCount(), 0);
    QVERIFY(fixture.viewModel.vectorPersistenceSummary().contains(
        QStringLiteral("disabled by default")));
    QVERIFY(
        fixture.viewModel.vectorPersistenceBoundedState().contains(QStringLiteral("local-only")));
    QVERIFY(fixture.viewModel.vectorPersistenceChecks().contains(
        QStringLiteral("Automatic indexing: disabled")));
    QVERIFY(fixture.viewModel.vectorPersistenceChecks().contains(
        QStringLiteral("Cloud/API/vector services: blocked")));
    QVERIFY(metaObject->indexOfProperty("vectorPersistenceStatus") >= 0);
    QVERIFY(metaObject->indexOfProperty("vectorPersistenceIndexedItemCount") >= 0);
    QCOMPARE(metaObject->indexOfProperty("vectorPersistencePath"), -1);
    QCOMPARE(metaObject->indexOfProperty("vectorPersistenceRawVectors"), -1);
    QCOMPARE(metaObject->indexOfProperty("vectorPersistenceDebugPayload"), -1);

    QCOMPARE(fixture.viewModel.semanticSearchStatus(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.semanticSearchCandidateCount(), 0);
    QVERIFY(fixture.viewModel.semanticSearchSummary().contains(QStringLiteral("disabled")));
    QVERIFY(fixture.viewModel.semanticSearchRuntimeState().contains(QStringLiteral("local-only")));
    QVERIFY(fixture.viewModel.semanticSearchArbitrationSummary().contains(
        QStringLiteral("deterministic candidates remain final authority")));
    QVERIFY(fixture.viewModel.semanticSearchChecks().contains(
        QStringLiteral("Filesystem indexing: disabled")));
    QVERIFY(metaObject->indexOfProperty("semanticSearchStatus") >= 0);
    QVERIFY(metaObject->indexOfProperty("semanticSearchCandidateCount") >= 0);
    QCOMPARE(metaObject->indexOfProperty("semanticSearchRawVectors"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticSearchDebugPayload"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticSearchPromptPayload"), -1);

    QCOMPARE(fixture.viewModel.hybridBridgeStatus(), QStringLiteral("Deterministic Only"));
    QCOMPARE(fixture.viewModel.hybridBridgeSemanticFillCount(), 0);
    QVERIFY(fixture.viewModel.hybridBridgeReadiness().contains(
        QStringLiteral("deterministic fallback")));
    QVERIFY(fixture.viewModel.hybridBridgeBudgetSummary().contains(
        QStringLiteral("bridge candidates")));
    QVERIFY(
        fixture.viewModel.hybridBridgeFallbackSummary().contains(QStringLiteral("deterministic")));
    QVERIFY(fixture.viewModel.hybridBridgeChecks().contains(
        QStringLiteral("Prompt content injection: disabled")));
    QVERIFY(metaObject->indexOfProperty("hybridBridgeStatus") >= 0);
    QVERIFY(metaObject->indexOfProperty("hybridBridgeCandidateCount") >= 0);
    QCOMPARE(metaObject->indexOfProperty("hybridBridgeRawVectors"), -1);
    QCOMPARE(metaObject->indexOfProperty("hybridBridgePromptPayload"), -1);
    QCOMPARE(metaObject->indexOfProperty("hybridBridgeFilesystemPath"), -1);

    QCOMPARE(fixture.viewModel.semanticAcceptanceStatus(), QStringLiteral("Deterministic Only"));
    QCOMPARE(fixture.viewModel.semanticAcceptanceAcceptedCount(), 0);
    QVERIFY(fixture.viewModel.semanticAcceptanceBudgetSummary().contains(
        QStringLiteral("semantic supplements")));
    QVERIFY(fixture.viewModel.semanticAcceptanceFallbackSummary().contains(
        QStringLiteral("deterministic")));
    QVERIFY(fixture.viewModel.semanticAcceptanceChecks().contains(
        QStringLiteral("Deterministic candidate replacement: no")));
    QVERIFY(metaObject->indexOfProperty("semanticAcceptanceStatus") >= 0);
    QVERIFY(metaObject->indexOfProperty("semanticAcceptanceAcceptedCount") >= 0);
    QCOMPARE(metaObject->indexOfProperty("semanticAcceptanceRawVectors"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticAcceptancePromptPayload"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticAcceptanceFilesystemPath"), -1);
}

void DesktopShellViewModelTest::exposesSemanticSupplementAssemblyMetadata() {
    ViewModelFixture fixture;
    const auto metaObject = fixture.viewModel.metaObject();

    QCOMPARE(fixture.viewModel.semanticSupplementAssemblyStatus(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.semanticSupplementAssemblyBlockCount(), 0);
    QVERIFY(fixture.viewModel.semanticSupplementAssemblyReadiness().contains(
        QStringLiteral("disabled")));
    QVERIFY(fixture.viewModel.semanticSupplementAssemblyBudgetSummary().contains(
        QStringLiteral("0 of")));
    QVERIFY(fixture.viewModel.semanticSupplementAssemblySafetySummary().contains(
        QStringLiteral("non-authoritative")));
    QVERIFY(fixture.viewModel.semanticSupplementAssemblyChecks().contains(
        QStringLiteral("Live prompt inclusion: blocked")));
    QVERIFY(fixture.viewModel.semanticSupplementAssemblyChecks().contains(
        QStringLiteral("Provider handles exposed: no")));
    QVERIFY(metaObject->indexOfProperty("semanticSupplementAssemblyStatus") >= 0);
    QVERIFY(metaObject->indexOfProperty("semanticSupplementAssemblyBlockCount") >= 0);
    QCOMPARE(metaObject->indexOfProperty("semanticSupplementAssemblyPromptBlock"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticSupplementAssemblyRawVectors"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticSupplementAssemblyRawScores"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticSupplementAssemblyProviderHandle"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticSupplementAssemblyFilesystemPath"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticSupplementAssemblyDebugDump"), -1);
}

void DesktopShellViewModelTest::exposesSemanticPromptAuthorityMetadata() {
    ViewModelFixture fixture;
    const auto metaObject = fixture.viewModel.metaObject();

    QCOMPARE(fixture.viewModel.semanticPromptAuthorityStatus(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.semanticPromptAuthorityWouldIncludeBlockCount(), 0);
    QVERIFY(fixture.viewModel.semanticPromptAuthorityDecisionSummary().contains(
        QStringLiteral("Denied")));
    QVERIFY(fixture.viewModel.semanticPromptAuthoritySafetySummary().contains(
        QStringLiteral("deterministic-only fallback")));
    QVERIFY(fixture.viewModel.semanticPromptAuthorityReadinessSummary().contains(
        QStringLiteral("Disabled")));
    QVERIFY(fixture.viewModel.semanticPromptAuthorityFallbackSummary().contains(
        QStringLiteral("deterministic prompt assembly remains unchanged")));
    QVERIFY(fixture.viewModel.semanticPromptAuthorityAuditSummary().contains(
        QStringLiteral("disabled")));
    QVERIFY(fixture.viewModel.semanticPromptAuthorityChecks().contains(
        QStringLiteral("Semantic authority escalation: blocked")));
    QVERIFY(fixture.viewModel.semanticPromptAuthorityChecks().contains(
        QStringLiteral("Raw prompt payloads exposed: no")));
    QCOMPARE(fixture.viewModel.semanticPromptInclusionEnabled(), false);
    QCOMPARE(fixture.viewModel.semanticPromptInclusionStatus(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.semanticPromptInclusionIncludedCount(), 0);
    QVERIFY(
        fixture.viewModel.semanticPromptInclusionSummary().contains(QStringLiteral("disabled")));
    QVERIFY(fixture.viewModel.semanticPromptInclusionBudgetSummary().contains(
        QStringLiteral("0 semantic supplement characters")));
    QVERIFY(fixture.viewModel.semanticPromptInclusionFallbackSummary().contains(
        QStringLiteral("deterministic-only")));
    QVERIFY(fixture.viewModel.semanticPromptInclusionAuditSummary().contains(
        QStringLiteral("disabled")));
    QVERIFY(fixture.viewModel.semanticPromptInclusionDeterministicAuthorityPreserved());
    QVERIFY(fixture.viewModel.semanticPromptInclusionChecks().contains(
        QStringLiteral("Raw prompt payloads exposed: no")));
    QVERIFY(metaObject->indexOfProperty("semanticPromptAuthorityStatus") >= 0);
    QVERIFY(metaObject->indexOfProperty("semanticPromptAuthorityDecisionSummary") >= 0);
    QVERIFY(metaObject->indexOfProperty("semanticPromptInclusionStatus") >= 0);
    QVERIFY(metaObject->indexOfProperty("semanticPromptInclusionBudgetSummary") >= 0);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptAuthorityRawPrompt"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptAuthoritySupplementBlock"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptAuthorityRawVectors"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptAuthorityRawScores"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptAuthorityProviderHandle"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptAuthorityFilesystemPath"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptAuthorityDebugDump"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptInclusionRawPrompt"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptInclusionSupplementBlock"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptInclusionRawVectors"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptInclusionRawScores"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptInclusionProviderHandle"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptInclusionFilesystemPath"), -1);
    QCOMPARE(metaObject->indexOfProperty("semanticPromptInclusionDebugDump"), -1);
}

void DesktopShellViewModelTest::exposesStartupLoadedMessages() {
    QList<sentinel::core::ChatMessage> persisted{
        {7, sentinel::core::ChatRole::System, QStringLiteral("previous system"),
         QDateTime::fromString(QStringLiteral("2026-05-15T12:00:00.000Z"), Qt::ISODateWithMs),
         sentinel::core::ChatMessageStatus::Received},
        {8, sentinel::core::ChatRole::User, QStringLiteral("previous user"),
         QDateTime::fromString(QStringLiteral("2026-05-15T12:01:00.000Z"), Qt::ISODateWithMs),
         sentinel::core::ChatMessageStatus::Sent},
    };
    sentinel::test::DeterministicModelServiceFixture models;
    sentinel::core::ApplicationControllerBuilder builder;
    builder.withMemoryStore(std::make_unique<InMemoryStore>())
        .withChatHistoryStore(std::make_unique<StaticChatHistoryStore>(persisted))
        .withModelService(models.takeModelService());
    auto controller = builder.build();
    ModeManager modeManager;
    AppSettings settings{std::make_unique<InMemorySettingsStore>()};
    settings.setSelectedRuntimeProvider(QStringLiteral("ollama"));
    settings.setSelectedLocalModel(QStringLiteral("sentinel-test-model"));
    DesktopShellViewModel viewModel{*controller, modeManager, settings};

    QCOMPARE(viewModel.chatMessages()->rowCount(), 2);
    const auto firstIndex = viewModel.chatMessages()->index(0, 0);
    const auto secondIndex = viewModel.chatMessages()->index(1, 0);
    QCOMPARE(viewModel.chatMessages()->data(firstIndex, ChatMessageListModel::ContentRole),
             QStringLiteral("previous system"));
    QCOMPARE(viewModel.chatMessages()->data(secondIndex, ChatMessageListModel::ContentRole),
             QStringLiteral("previous user"));
    QCOMPARE(viewModel.chatMessages()->data(secondIndex, ChatMessageListModel::StatusRole),
             QStringLiteral("sent"));
}

void DesktopShellViewModelTest::forwardsChatActions() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::chatMessagesChanged);
    QSignalSpy stateSpy(&fixture.viewModel, &DesktopShellViewModel::conversationStateChanged);

    const auto sent = fixture.sendMessage(QStringLiteral("status"));

    QVERIFY(sent);
    QCOMPARE(fixture.viewModel.chatMessages()->rowCount(), 3);
    const auto lastIndex = fixture.viewModel.chatMessages()->index(2, 0);
    QCOMPARE(
        fixture.viewModel.chatMessages()->data(lastIndex, ChatMessageListModel::ContentRole),
        QStringLiteral("SENTINEL_TEST_RESPONSE"));
    QCOMPARE(fixture.viewModel.chatMessages()->data(lastIndex, ChatMessageListModel::StatusRole),
             QStringLiteral("completed"));
    QCOMPARE(fixture.viewModel.conversationState(), QStringLiteral("Idle"));
    QCOMPARE(fixture.viewModel.conversationTransitionStatus(), QStringLiteral("Not Requested"));
    QVERIFY(spy.count() >= 1);
    QCOMPARE(stateSpy.count(), 0);
}

// Retired pre-session Agent pipeline probe; end-to-end session execution is tested at its owner.
#if 0
void DesktopShellViewModelTest::forwardsDeterministicAgentRequest() {
    ApplicationController controller{std::make_unique<LocalEchoProvider>(),
                                     std::make_unique<InMemoryStore>(), nullptr, nullptr,
                                     std::make_unique<sentinel::core::NullAgentRuntime>()};
    ModeManager modeManager;
    AppSettings settings{std::make_unique<InMemorySettingsStore>()};
    DesktopShellViewModel viewModel{controller, modeManager, settings};
    QSignalSpy statusSpy(&viewModel, &DesktopShellViewModel::agentStatusChanged);
    QSignalSpy responseSpy(&viewModel, &DesktopShellViewModel::agentResponseChanged);
    QSignalSpy planSpy(&viewModel, &DesktopShellViewModel::toolPlanChanged);
    QSignalSpy approvalSpy(&viewModel, &DesktopShellViewModel::approvalChanged);
    QSignalSpy sandboxSpy(&viewModel, &DesktopShellViewModel::sandboxChanged);
    QSignalSpy toolExecutionSpy(&viewModel, &DesktopShellViewModel::toolExecutionChanged);
    QSignalSpy pipelineSpy(&viewModel, &DesktopShellViewModel::agentPipelineChanged);
    QSignalSpy runtimeContextSpy(&viewModel, &DesktopShellViewModel::runtimeContextChanged);
    QSignalSpy activitySpy(&viewModel, &DesktopShellViewModel::agentActivityChanged);
    QSignalSpy conversationStateSpy(&viewModel, &DesktopShellViewModel::conversationStateChanged);

    const auto ran = viewModel.runAgentRequest(QStringLiteral("draft local action"));

    QVERIFY(ran);
    QCOMPARE(viewModel.agentStatus(), QStringLiteral("Ready"));
    QCOMPARE(viewModel.lastAgentResponse(), QStringLiteral("Executed: Local Plan Summary"));
    QCOMPARE(viewModel.latestToolPlanStatus(), QStringLiteral("Planned"));
    QCOMPARE(viewModel.latestToolPlanSummary(),
             QStringLiteral("Tool plan prepared: Local Plan Summary"));
    QCOMPARE(viewModel.latestApprovalStatus(), QStringLiteral("Not Required"));
    QCOMPARE(viewModel.latestApprovalSummary(),
             QStringLiteral("Planned tool invocations do not require approval."));
    QCOMPARE(viewModel.latestSandboxStatus(), QStringLiteral("Allowed"));
    QCOMPARE(viewModel.latestSandboxSummary(),
             QStringLiteral("Planned tool capabilities are allowed by sandbox metadata policy."));
    QCOMPARE(viewModel.latestToolExecutionStatus(), QStringLiteral("Succeeded"));
    QCOMPARE(viewModel.latestToolExecutionSummary(),
             QStringLiteral("Executed: Local Plan Summary"));
    QCOMPARE(viewModel.latestAgentPipelineStatus(), QStringLiteral("Succeeded"));
    QCOMPARE(viewModel.latestAgentPipelineSummary(),
             QStringLiteral("Executed: Local Plan Summary"));
    QCOMPARE(viewModel.runtimeContextStatus(), QStringLiteral("Active"));
    QCOMPARE(viewModel.runtimeContextSummary(),
             QStringLiteral("Runtime context captured pipeline result: Succeeded"));
    QCOMPARE(viewModel.runtimeContextActiveToolIds(),
             QStringList{QStringLiteral("local-plan-summary")});
    QCOMPARE(viewModel.agentActivityCount(), 6);
    QCOMPARE(viewModel.latestAgentActivitySummary(),
             QStringLiteral("Agent pipeline finished: Succeeded"));
    QCOMPARE(viewModel.conversationState(), QStringLiteral("Completed"));
    QCOMPARE(viewModel.conversationTransitionStatus(), QStringLiteral("Accepted"));
    QCOMPARE(viewModel.conversationTransitionSummary(),
             QStringLiteral("Accepted conversation transition: Responding -> Completed: agent "
                            "response metadata completed"));
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(responseSpy.count(), 1);
    QCOMPARE(planSpy.count(), 1);
    QCOMPARE(approvalSpy.count(), 1);
    QCOMPARE(sandboxSpy.count(), 1);
    QCOMPARE(toolExecutionSpy.count(), 1);
    QCOMPARE(pipelineSpy.count(), 1);
    QCOMPARE(runtimeContextSpy.count(), 1);
    QCOMPARE(activitySpy.count(), 1);
    QVERIFY(conversationStateSpy.count() >= 6);
}

#endif

void DesktopShellViewModelTest::ignoresBlankChatActions() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::chatMessagesChanged);

    const auto sent = fixture.sendMessage(QStringLiteral("   "));

    QVERIFY(!sent);
    QCOMPARE(fixture.viewModel.chatMessages()->rowCount(), 1);
    QCOMPARE(fixture.viewModel.chatSendLifecycleState(), QStringLiteral("refused"));
    QCOMPARE(fixture.viewModel.chatSendLifecycleSummary(),
             QStringLiteral("Chat is not ready for this message."));
    QCOMPARE(spy.count(), 0);
}

void DesktopShellViewModelTest::clearsChatActions() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::chatMessagesChanged);

    fixture.sendMessage(QStringLiteral("status"));
    fixture.viewModel.clearChat();

    QCOMPARE(fixture.viewModel.chatMessages()->rowCount(), 1);
    QVERIFY(spy.count() >= 2);
}

void DesktopShellViewModelTest::legacyGlobalMemoryClearIsBlocked() {
    ViewModelFixture fixture;
    QSignalSpy memorySpy(&fixture.viewModel, &DesktopShellViewModel::memoryEntriesChanged);
    QSignalSpy maintenanceSpy(&fixture.viewModel, &DesktopShellViewModel::maintenanceStatusChanged);
    fixture.viewModel.remember(QStringLiteral("mode"), QStringLiteral("Companion"));

    const auto cleared = fixture.viewModel.clearMemory();

    QVERIFY(!cleared);
    QCOMPARE(fixture.viewModel.memoryEntries().size(), 1);
    QVERIFY(fixture.viewModel.memoryMaintenanceStatus().startsWith(QStringLiteral("Blocked:")));
    QCOMPARE(memorySpy.count(), 1);
    QCOMPARE(maintenanceSpy.count(), 1);
}

void DesktopShellViewModelTest::reportsRuntimeOnlyChatMaintenanceWhenStoreUnavailable() {
    sentinel::test::DeterministicModelServiceFixture models;
    sentinel::core::ApplicationControllerBuilder builder;
    builder.withMemoryStore(std::make_unique<InMemoryStore>())
        .withChatHistoryStore(
            std::make_unique<StaticChatHistoryStore>(QList<sentinel::core::ChatMessage>{}, false))
        .withModelService(models.takeModelService());
    auto controller = builder.build();
    ModeManager modeManager;
    AppSettings settings{std::make_unique<InMemorySettingsStore>()};
    settings.setSelectedRuntimeProvider(QStringLiteral("ollama"));
    settings.setSelectedLocalModel(QStringLiteral("sentinel-test-model"));
    DesktopShellViewModel viewModel{*controller, modeManager, settings};
    QSignalSpy maintenanceSpy(&viewModel, &DesktopShellViewModel::maintenanceStatusChanged);

    viewModel.sendMessage(QStringLiteral("status"));
    const auto cleared = viewModel.clearChat();

    QVERIFY(!cleared);
    QCOMPARE(viewModel.chatMaintenanceStatus(), QStringLiteral("Ready"));
    QCOMPARE(viewModel.chatMessages()->rowCount(), 3);
    QCOMPARE(maintenanceSpy.count(), 0);
}

void DesktopShellViewModelTest::forwardsModeChanges() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::currentModeChanged);

    fixture.viewModel.setModeByName(QStringLiteral("Agent"));

    QCOMPARE(fixture.viewModel.currentModeName(), QStringLiteral("Agent"));
    QCOMPARE(spy.count(), 1);
}

void DesktopShellViewModelTest::forwardsMemoryWrites() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::memoryEntriesChanged);

    fixture.viewModel.remember(QStringLiteral("mode"), QStringLiteral("Companion"));

    QCOMPARE(fixture.viewModel.memoryEntries(), QStringList{QStringLiteral("mode: Companion")});
    QCOMPARE(spy.count(), 1);
}

void DesktopShellViewModelTest::forwardsSettingsChanges() {
    ViewModelFixture fixture;
    QSignalSpy themeSpy(&fixture.viewModel, &DesktopShellViewModel::themeNameChanged);
    QSignalSpy profileSpy(&fixture.viewModel, &DesktopShellViewModel::configurationProfileChanged);
    QSignalSpy modelSpy(&fixture.viewModel, &DesktopShellViewModel::localModelSelectionChanged);
    QSignalSpy runtimeProviderSpy(&fixture.viewModel,
                                  &DesktopShellViewModel::runtimeProviderRegistryChanged);
    QSignalSpy chatRoutingSpy(&fixture.viewModel,
                              &DesktopShellViewModel::localChatInferenceRoutingChanged);
    QSignalSpy inferenceSpy(&fixture.viewModel, &DesktopShellViewModel::localInferenceChanged);
    QSignalSpy contextInjectionSpy(&fixture.viewModel,
                                   &DesktopShellViewModel::promptContextInjectionChanged);
    QSignalSpy developerModeSpy(&fixture.viewModel, &DesktopShellViewModel::developerModeChanged);
    QSignalSpy skillProfileSpy(&fixture.viewModel, &DesktopShellViewModel::skillProfileChanged);
    QSignalSpy permissionPolicySpy(&fixture.viewModel,
                                   &DesktopShellViewModel::permissionPolicyChanged);
    QSignalSpy contextVisibilitySpy(&fixture.viewModel,
                                    &DesktopShellViewModel::contextExplainabilityVisibleChanged);

    fixture.viewModel.setThemeName(QStringLiteral("Liquid Glass Dark"));
    fixture.viewModel.setConfigurationProfile(QStringLiteral("Phase 2 Shell"));
    fixture.viewModel.setSelectedLocalModel(QStringLiteral(" local-model "));
    fixture.viewModel.setSelectedRuntimeProvider(QStringLiteral("openai-compatible"));
    fixture.viewModel.setLocalChatInferenceEnabled(true);
    fixture.viewModel.setLocalInferenceStreamingEnabled(true);
    fixture.viewModel.setLocalInferenceTimeoutMs(45000);
    fixture.viewModel.setPromptContextInjectionEnabled(true);
    fixture.viewModel.setContextExplainabilityVisible(false);
    fixture.viewModel.setDeveloperModeEnabled(true);
    fixture.viewModel.setSelectedSkillProfile(QStringLiteral("researcher"));
    fixture.viewModel.setDefaultPermissionPolicyState(QStringLiteral("trusted"));

    QCOMPARE(fixture.viewModel.themeName(), QStringLiteral("Liquid Glass Dark"));
    QCOMPARE(fixture.viewModel.configurationProfile(), QStringLiteral("Phase 2 Shell"));
    QCOMPARE(fixture.viewModel.selectedLocalModel(), QString());
    QCOMPARE(fixture.settings.selectedLocalModel(), QStringLiteral("local-model"));
    QCOMPARE(fixture.viewModel.selectedRuntimeProvider(), QStringLiteral("openai-compatible"));
    QCOMPARE(fixture.settings.selectedRuntimeProvider(), QStringLiteral("openai-compatible"));
    QCOMPARE(fixture.viewModel.activeRuntimeProviderId(), QStringLiteral("ollama"));
    QVERIFY(fixture.viewModel.localChatInferenceEnabled());
    QVERIFY(fixture.settings.localChatInferenceEnabled());
    QVERIFY(fixture.viewModel.localInferenceStreamingEnabled());
    QVERIFY(fixture.settings.localInferenceStreamingEnabled());
    QCOMPARE(fixture.viewModel.localInferenceTimeoutMs(), 45000);
    QCOMPARE(fixture.settings.localInferenceTimeoutMs(), 45000);
    QVERIFY(fixture.viewModel.promptContextInjectionEnabled());
    QVERIFY(fixture.settings.promptContextInjectionEnabled());
    QVERIFY(!fixture.viewModel.contextExplainabilityEnabled());
    QVERIFY(!fixture.viewModel.contextExplainabilityVisible());
    QVERIFY(!fixture.settings.contextExplainabilityVisible());
    QVERIFY(fixture.controller->contextExplainabilityEnabled());
    QVERIFY(
        fixture.viewModel.contextReasoningSummary().contains(QStringLiteral("Context reasoning")));
    QVERIFY(fixture.viewModel.developerModeEnabled());
    QVERIFY(fixture.settings.developerModeEnabled());
    QCOMPARE(fixture.viewModel.selectedSkillProfile(), QStringLiteral("researcher"));
    QCOMPARE(fixture.viewModel.selectedSkillProfileName(), QStringLiteral("Researcher"));
    QCOMPARE(fixture.settings.selectedSkillProfile(), QStringLiteral("researcher"));
    QCOMPARE(fixture.viewModel.defaultPermissionPolicyState(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.settings.defaultPermissionPolicyState(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.promptContextInjectionStatus(), QStringLiteral("Empty"));
    QCOMPARE(fixture.viewModel.localChatInferenceStatus(), QStringLiteral("Provider Disabled"));
    QVERIFY(!fixture.viewModel.localChatSendAvailable());
    QCOMPARE(themeSpy.count(), 1);
    QCOMPARE(profileSpy.count(), 1);
    QVERIFY(modelSpy.count() >= 1);
    QVERIFY(runtimeProviderSpy.count() >= 1);
    QVERIFY(chatRoutingSpy.count() >= 3);
    QCOMPARE(inferenceSpy.count(), 1);
    QCOMPARE(contextInjectionSpy.count(), 1);
    QCOMPARE(developerModeSpy.count(), 1);
    QCOMPARE(skillProfileSpy.count(), 1);
    QCOMPARE(permissionPolicySpy.count(), 0);
    QCOMPARE(contextVisibilitySpy.count(), 1);
}

void DesktopShellViewModelTest::onboardingWriteFailureRemainsVisible() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile blocker(dir.filePath(QStringLiteral("blocked")));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.write("fixture");
    blocker.close();
    AppSettings settings{std::make_unique<sentinel::core::JsonSettingsStore>(
                             blocker.fileName() + QStringLiteral("/settings.json")),
                         sentinel::core::inMemoryTestCredentialStore()};
    sentinel::core::ApplicationControllerBuilder builder;
    builder.withMemoryStore(std::make_unique<InMemoryStore>())
        .withModelService(std::make_unique<sentinel::core::ModelService>(&settings));
    auto controller = builder.build();
    ModeManager modeManager;
    DesktopShellViewModel viewModel{*controller, modeManager, settings};
    viewModel.setOnboardingComplete(true);
    QVERIFY(!viewModel.onboardingComplete());
    QVERIFY(!viewModel.onboardingErrorText().isEmpty());
}

void DesktopShellViewModelTest::exposesLanguageSettings() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::appLanguageChanged);

    fixture.viewModel.setAppLanguage(QStringLiteral("tr"));

    QCOMPARE(fixture.viewModel.appLanguage(), QStringLiteral("tr"));
    QCOMPARE(fixture.viewModel.languageDisplayName(QStringLiteral("en")),
             QStringLiteral("English"));
    QCOMPARE(fixture.viewModel.languageDisplayName(QStringLiteral("tr")), QStringLiteral("Türkçe"));
    QCOMPARE(spy.count(), 1);
}

void DesktopShellViewModelTest::exposesWorkspaceReadinessMetadata() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::workspaceChanged);

    QCOMPARE(fixture.viewModel.selectedWorkspaceId(), QStringLiteral("personal"));
    QCOMPARE(fixture.viewModel.selectedWorkspaceName(), QStringLiteral("Personal"));
    QCOMPARE(fixture.viewModel.selectedWorkspaceAccessState(), QStringLiteral("Active"));
    QCOMPARE(fixture.viewModel.workspacePermissionPosture(), QStringLiteral("Workspace Only"));
    QVERIFY(
        fixture.viewModel.workspacePermissionPostures().contains(QStringLiteral("Workspace Only")));
    QVERIFY(fixture.viewModel.workspaceTemplateNames().contains(QStringLiteral("Research")));
    QVERIFY(fixture.viewModel.workspaceReadinessSummary().contains(
        QStringLiteral("isolated by workspace")));
    QVERIFY(fixture.viewModel.workspaceReadinessChecks().contains(
        QStringLiteral("Filesystem scanning: disabled")));
    QVERIFY(fixture.viewModel.workspaceBoundaryDiagnostics().contains(
        QStringLiteral("Local RAG: disabled by default; manual indexing only")));
    QVERIFY(fixture.viewModel.workspaceActionPlaceholders().contains(
        QStringLiteral("Create Workspace: available")));
    QVERIFY(fixture.viewModel.workspaceIds().contains(QStringLiteral("personal")));
    QVERIFY(fixture.viewModel.workspaceIds().contains(QStringLiteral("coding")));
    QCOMPARE(fixture.viewModel.localKnowledgeBaseStatus(), QStringLiteral("Disabled"));
    QVERIFY(fixture.viewModel.privacyCenterSummaries().contains(
        QStringLiteral("Indexing: Manual Only")));
    QVERIFY(fixture.viewModel.brainWorkspaceSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Knowledge Base Summary")));

    fixture.viewModel.setSelectedWorkspaceId(QStringLiteral("unknown"));
    QCOMPARE(fixture.viewModel.selectedWorkspaceId(), QStringLiteral("personal"));
    QCOMPARE(spy.count(), 0);

    const auto createdId =
        fixture.viewModel.createWorkspace(QStringLiteral("Case Notes"), QStringLiteral("Research"));
    QVERIFY(!createdId.isEmpty());
    QCOMPARE(fixture.viewModel.selectedWorkspaceId(), createdId);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(fixture.viewModel.setWorkspaceRoot(createdId, root.path()));
    QCOMPARE(fixture.viewModel.selectedWorkspaceRootPath(),
             QFileInfo(root.path()).canonicalFilePath());
    QVERIFY(fixture.viewModel.clearWorkspaceRoot());
    QVERIFY(fixture.viewModel.selectedWorkspaceRootPath().isEmpty());
    QVERIFY(fixture.viewModel.openWorkspaceFolder(root.path()));
    QCOMPARE(fixture.viewModel.selectedWorkspaceRootPath(),
             QFileInfo(root.path()).canonicalFilePath());
    QVERIFY(fixture.viewModel.renameWorkspace(createdId, QStringLiteral("Case Notes 2")));
    QVERIFY(fixture.viewModel.archiveWorkspace(createdId));
    QVERIFY(!fixture.viewModel.duplicateWorkspace(QStringLiteral("personal")).isEmpty());
    QVERIFY(fixture.viewModel.workspaceLastActionSummary().contains(QStringLiteral("Duplicated")));
}

void DesktopShellViewModelTest::exposesSkillProfileMetadata() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::skillProfileChanged);

    QCOMPARE(fixture.viewModel.selectedSkillProfile(), QStringLiteral("personal-assistant"));
    QCOMPARE(fixture.viewModel.selectedSkillProfileName(), QStringLiteral("Personal Assistant"));
    QCOMPARE(fixture.viewModel.selectedSkillProfileReadiness(), QStringLiteral("Metadata only"));
    QVERIFY(fixture.viewModel.selectedSkillProfileSummary().contains(
        QStringLiteral("Everyday planning")));
    QVERIFY(fixture.viewModel.selectedSkillProfileDescription().contains(
        QStringLiteral("keeping calendars")));
    QVERIFY(fixture.viewModel.selectedSkillProfilePolicyPosture().contains(
        QStringLiteral("autonomous actions")));
    QCOMPARE(fixture.viewModel.skillProfileIds(),
             QStringList({QStringLiteral("developer"), QStringLiteral("student"),
                          QStringLiteral("researcher"), QStringLiteral("personal-assistant"),
                          QStringLiteral("custom")}));
    QVERIFY(fixture.viewModel.skillProfileNames().contains(QStringLiteral("Personal Assistant")));
    QVERIFY(fixture.viewModel.skillProfileSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Custom")));
    QVERIFY(fixture.viewModel.skillProfileCapabilitySummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Prompt mutation: disabled")));
    QVERIFY(fixture.viewModel.skillProfileReadinessChecks().contains(
        QStringLiteral("Hidden system prompt changes: disabled")));
    QVERIFY(fixture.viewModel.skillProfileDeveloperDiagnostics().contains(
        QStringLiteral("Runtime authority: unchanged")));

    fixture.viewModel.setSelectedSkillProfile(QStringLiteral("student"));
    QCOMPARE(fixture.viewModel.selectedSkillProfile(), QStringLiteral("student"));
    QCOMPARE(fixture.viewModel.selectedSkillProfileName(), QStringLiteral("Student"));
    QCOMPARE(fixture.settings.selectedSkillProfile(), QStringLiteral("student"));
    QCOMPARE(spy.count(), 1);

    fixture.viewModel.setSelectedSkillProfile(QStringLiteral("unknown"));
    QCOMPARE(fixture.viewModel.selectedSkillProfile(), QStringLiteral("developer"));
    QCOMPARE(spy.count(), 2);
}

void DesktopShellViewModelTest::exposesPermissionPolicyMetadata() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::permissionPolicyChanged);

    QCOMPARE(fixture.viewModel.defaultPermissionPolicyState(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.permissionPolicyStatus(), QStringLiteral("Operational"));
    QVERIFY(!fixture.viewModel.permissionPolicySummary().isEmpty());
    QCOMPARE(fixture.viewModel.permissionPolicyStateLabels(),
             QStringList({QStringLiteral("Disabled"), QStringLiteral("Ask Every Time"),
                          QStringLiteral("Trusted"), QStringLiteral("Enabled")}));
    QCOMPARE(fixture.viewModel.permissionPolicyDomainIds().size(), 10);
    QVERIFY(fixture.viewModel.permissionPolicyDomainNames().contains(
        QStringLiteral("Workspace Access")));
    QCOMPARE(fixture.viewModel.permissionPolicyDomainSummaries().size(), 10);
    QVERIFY(fixture.viewModel.permissionPolicyDomainSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Tool Execution / Disabled")));
    QVERIFY(!fixture.viewModel.permissionPolicyDeveloperDiagnostics().isEmpty());

    fixture.viewModel.setDefaultPermissionPolicyState(QStringLiteral("ask-every-time"));
    QCOMPARE(fixture.viewModel.defaultPermissionPolicyState(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.settings.defaultPermissionPolicyState(), QStringLiteral("Disabled"));
    QCOMPARE(spy.count(), 0);

    fixture.viewModel.setDefaultPermissionPolicyState(QStringLiteral("unknown"));
    QCOMPARE(fixture.viewModel.defaultPermissionPolicyState(), QStringLiteral("Disabled"));
    QCOMPARE(spy.count(), 0);
}

// Retired registry-service facade probe; gateway behavior is owned by
// test_tool_execution_gateway and test_real_tool_executor_tools.
#if 0
void DesktopShellViewModelTest::exposesToolGatewayMetadata() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::permissionPolicyChanged);

    QCOMPARE(fixture.viewModel.toolGatewayStatus(), QStringLiteral("Operational"));
    QCOMPARE(fixture.viewModel.toolGatewayPermissionPosture(), QStringLiteral("Disabled"));
    QCOMPARE(fixture.viewModel.toolGatewayToolCount(), 35);
    QCOMPARE(fixture.viewModel.toolGatewayMetadataSafeCount(), 32);
    QCOMPARE(fixture.viewModel.toolGatewayUnavailableCount(), 0);
    QCOMPARE(fixture.viewModel.toolGatewayRefusedCount(), 3);
    QVERIFY(fixture.viewModel.toolGatewaySummary().contains(QStringLiteral("fully operational")));
    QVERIFY(fixture.viewModel.toolGatewayToolSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Run Command / Available / Disabled")));
    QVERIFY(fixture.viewModel.toolGatewayDeveloperDiagnostics()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Gateway execution grant: allowed")));

    fixture.viewModel.setDefaultPermissionPolicyState(QStringLiteral("Trusted"));
    QCOMPARE(fixture.viewModel.toolGatewayPermissionPosture(), QStringLiteral("Trusted"));
    QVERIFY(fixture.viewModel.toolGatewayToolSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Run Command / Available / Trusted")));
    QCOMPARE(spy.count(), 1);
}

#endif

// Retired AgentRuntimeService facade probe; current contracts are covered by
// test_agent_runtime_service and test_agent_loop.
#if 0
void DesktopShellViewModelTest::exposesAgentRuntimeMetadata() {
    ViewModelFixture fixture;
    QSignalSpy runtimeSpy(&fixture.viewModel, &DesktopShellViewModel::agentRuntimeChanged);

    QCOMPARE(fixture.viewModel.agentRuntimeStatus(),
             QStringLiteral("Ready for approved execution"));
    QCOMPARE(fixture.viewModel.agentRuntimeApprovalPosture(),
             QStringLiteral("Approval and sandbox gates enforced"));
    QCOMPARE(fixture.viewModel.agentRuntimeAgentCount(), 5);
    QCOMPARE(fixture.viewModel.agentRuntimeReadyAgentCount(), 5);
    QCOMPARE(fixture.viewModel.agentRuntimeRefusedAgentCount(), 0);
    QVERIFY(fixture.viewModel.agentRuntimeSummary().contains(
        QStringLiteral("executes tools only after approval")));
    QVERIFY(fixture.viewModel.agentRuntimeAgentSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Voice Assistant")));
    QVERIFY(!fixture.viewModel.agentRuntimeReadinessSummaries().isEmpty());
    QVERIFY(fixture.viewModel.agentRuntimeDeveloperDiagnostics()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral(
                    "Runtime execution grant: approval and sandbox policy required")));

    QCOMPARE(fixture.viewModel.agentPlanId(), QStringLiteral("agent-plan-general-assistant"));
    QCOMPARE(fixture.viewModel.agentPlanEstimatedRisk(), QStringLiteral("Low"));
    QCOMPARE(fixture.viewModel.agentPlanApprovalState(),
             QStringLiteral("Approval required before execution"));
    QVERIFY(fixture.viewModel.agentPlanGoalSummary().contains(QStringLiteral("safe next steps")));
    QVERIFY(fixture.viewModel.agentPlanSteps()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("identify the tools required")));
    QVERIFY(fixture.viewModel.agentPlanRequiredTools().isEmpty());
    QVERIFY(fixture.viewModel.agentPlanRequiredPermissions()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("agent-execution / Disabled")));
    QVERIFY(fixture.viewModel.agentPlanRefusalReason().contains(
        QStringLiteral("available through the approval")));
    QVERIFY(fixture.viewModel.agentPlanDiagnostics()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Execution grant: approval and sandbox policy required")));

    fixture.viewModel.setDefaultPermissionPolicyState(QStringLiteral("Trusted"));
    QCOMPARE(fixture.viewModel.agentRuntimeReadinessSummaries()
                 .join(QStringLiteral("\n"))
                 .contains(QStringLiteral("Ready for approved execution / Trusted")),
             true);
    QCOMPARE(runtimeSpy.count(), 1);

    fixture.viewModel.setSelectedSkillProfile(QStringLiteral("researcher"));
    QCOMPARE(runtimeSpy.count(), 2);
}

#endif

void DesktopShellViewModelTest::languageSettingDoesNotChangeRuntimePresentationFlags() {
    ViewModelFixture fixture;

    fixture.viewModel.setLocalChatInferenceEnabled(true);
    fixture.viewModel.setPromptContextInjectionEnabled(true);
    fixture.viewModel.setContextExplainabilityVisible(false);
    fixture.viewModel.setDeveloperModeEnabled(true);

    fixture.viewModel.setAppLanguage(QStringLiteral("tr"));

    QVERIFY(fixture.viewModel.localChatInferenceEnabled());
    QVERIFY(fixture.viewModel.promptContextInjectionEnabled());
    QVERIFY(!fixture.viewModel.contextExplainabilityVisible());
    QVERIFY(fixture.viewModel.developerModeEnabled());
    QCOMPARE(fixture.viewModel.localRuntimeStatus(), QStringLiteral("Metadata Only"));
    QCOMPARE(fixture.viewModel.providerName(), QStringLiteral("deterministic-chat"));
}

// Retired controlled-task facade probe; the current session workflow is owned by
// test_controlled_agent_tasks and test_agent_task_runtime.
#if 0
void DesktopShellViewModelTest::exposesControlledAgentTaskWorkflow() {
    ViewModelFixture fixture;

    const auto taskId =
        fixture.viewModel.planControlledAgentTask(QStringLiteral("Prepare a summary of documents"));
    QVERIFY(!taskId.isEmpty());
    QVERIFY(fixture.viewModel.controlledTaskActiveSummary().contains(
        QStringLiteral("Pending Approval")));
    QCOMPARE(fixture.viewModel.controlledTaskPlanSteps().size(), 3);
    QVERIFY(fixture.viewModel.controlledTaskNotificationCategories().contains(
        QStringLiteral("Approval Needed")));

    QVERIFY(fixture.viewModel.approveControlledAgentTask(taskId, QStringLiteral("Approve Once")));
    QVERIFY(fixture.viewModel.startControlledAgentTask(taskId));
    QVERIFY(fixture.viewModel.controlledTaskCurrentStep().contains(QStringLiteral("Running")));

    QVERIFY(fixture.viewModel.executeControlledAgentStep(taskId));
    QVERIFY(fixture.viewModel.controlledTaskProgressSummary().contains(QStringLiteral("1 of 3")));
    QVERIFY(fixture.viewModel.retryControlledAgentStep(taskId));
    QVERIFY(fixture.viewModel.controlledTaskExplainabilitySummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Step executed by:")));

    QVERIFY(fixture.viewModel.setControlledToolPermission(QStringLiteral("Files"),
                                                          QStringLiteral("Allow For Workspace")));
    QVERIFY(fixture.viewModel.controlledTaskPermissionSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Files: Allow For Workspace")));
    QVERIFY(fixture.settings.controlledAgentTasksJson().contains(taskId));
}

#endif

void DesktopShellViewModelTest::exposesProductExcellenceWorkflow() {
    ViewModelFixture fixture;
    QSignalSpy nativeSpy(&fixture.viewModel, &DesktopShellViewModel::nativeExperienceChanged);

    fixture.viewModel.setOnboardingAiProvider(QStringLiteral("llama.cpp server"));
    fixture.viewModel.setReducedMotionEnabled(true);
    fixture.viewModel.setReducedTransparencyEnabled(true);
    fixture.viewModel.setHighContrastEnabled(true);
    fixture.viewModel.setUiDensity(QStringLiteral("Compact"));

    QCOMPARE(fixture.viewModel.onboardingAiProvider(), QStringLiteral("llama.cpp server"));
    QVERIFY(fixture.viewModel.reducedMotionEnabled());
    QVERIFY(fixture.viewModel.highContrastEnabled());
    QVERIFY(fixture.viewModel.reducedTransparencyEnabled());
    const QVariantMap important{{"category", "Security"}, {"priority", "High"}};
    const QVariantMap ordinary{{"category", "Agent"}, {"priority", "Normal"}};
    fixture.viewModel.setNotificationPolicy(QStringLiteral("Important Only"));
    QVERIFY(fixture.viewModel.shouldShowNotification(important));
    QVERIFY(!fixture.viewModel.shouldShowNotification(ordinary));
    fixture.viewModel.setDndEnabled(true);
    QVERIFY(fixture.viewModel.dndEnabled());
    QVERIFY(!fixture.viewModel.shouldShowNotification(important));
    QVERIFY(QJsonDocument::fromJson(fixture.settings.notificationCenterJson().toUtf8()).object().value("doNotDisturb").toBool());
    fixture.viewModel.setDndEnabled(false);
    fixture.viewModel.setChannelMuted(QStringLiteral("Security"), true);
    QVERIFY(!fixture.viewModel.shouldShowNotification(important));
    fixture.viewModel.setChannelMuted(QStringLiteral("Security"), false);
    QVERIFY(fixture.viewModel.shouldShowNotification(important));
    fixture.viewModel.setNotificationPolicy(QStringLiteral("Custom"));
    fixture.viewModel.setNotifyAgentResponses(false);
    QVERIFY(!fixture.viewModel.shouldShowNotification(ordinary));
    fixture.viewModel.setNotifyAgentResponses(true);
    QVERIFY(fixture.viewModel.shouldShowNotification(ordinary));
    fixture.viewModel.setNotificationPolicy(QStringLiteral("All"));
    QCOMPARE(fixture.viewModel.uiDensity(), QStringLiteral("Compact"));
    QVERIFY(fixture.viewModel.accessibilitySummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Reduced motion: Enabled")));

    fixture.viewModel.addNotificationWithPriority(
        QStringLiteral("Security"), QStringLiteral("Test security notice"),
        QStringLiteral("A real local-only notification."), QStringLiteral("High"));
    QVERIFY(fixture.viewModel.notificationFilteredSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Test security notice")));
    fixture.viewModel.setNotificationSearchQuery(QStringLiteral("Test security notice"));
    fixture.viewModel.setNotificationCategoryFilter(QStringLiteral("Security"));
    QVERIFY(fixture.viewModel.notificationFilteredSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Test security notice")));
    fixture.viewModel.setDndEnabled(true);
    fixture.viewModel.setChannelMuted(QStringLiteral("Workspace"), true);
    QVERIFY(fixture.viewModel.markAllNotificationsRead());
    QVERIFY(fixture.viewModel.archiveNotification(fixture.viewModel.latestNotificationId()));
    QVERIFY(fixture.viewModel.clearArchivedNotifications());
    QVERIFY(fixture.viewModel.dndEnabled());
    QVERIFY(fixture.viewModel.isChannelMuted(QStringLiteral("Workspace")));
    fixture.viewModel.setDndEnabled(false);

    QVERIFY(fixture.viewModel.notificationLifecycleSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Persistence: local settings JSON")));

    QVERIFY(fixture.viewModel.checkForUpdates());
    QVERIFY(fixture.viewModel.updateWorkflowState().contains(
        QStringLiteral("Checking for updates...")));
    fixture.viewModel.confirmUpdateDownload();
    const auto state = fixture.viewModel.updateWorkflowState();
    QVERIFY(state.contains(QStringLiteral("Redirected to download page")) ||
            state.contains(QStringLiteral("Failed to open browser")));

    QVERIFY(fixture.viewModel.prepareExportPreview(QStringLiteral("Brain entries"),
                                                   QStringLiteral("JSON")));
    QVERIFY(fixture.viewModel.exportPreviewSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Brain entries")));
    QVERIFY(fixture.viewModel.diagnosticsCenterSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Notification statistics:")));
    QVERIFY(fixture.viewModel.brainInsightSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Visualization only")));
    QVERIFY(fixture.viewModel.recoveryReliabilitySummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Crash recovery draft:")));
    QVERIFY(nativeSpy.count() > 0);
}

void DesktopShellViewModelTest::keepsSettingsSeparateFromClearActions() {
    ViewModelFixture fixture;
    fixture.viewModel.setThemeName(QStringLiteral("Liquid Glass Dark"));
    fixture.viewModel.setConfigurationProfile(QStringLiteral("Desktop Stable"));
    fixture.viewModel.remember(QStringLiteral("mode"), QStringLiteral("Companion"));
    fixture.sendMessage(QStringLiteral("status"));

    fixture.viewModel.clearMemory();
    fixture.viewModel.clearChat();

    QCOMPARE(fixture.viewModel.themeName(), QStringLiteral("Liquid Glass Dark"));
    QCOMPARE(fixture.viewModel.configurationProfile(), QStringLiteral("Desktop Stable"));
}

void DesktopShellViewModelTest::tracksNavigationState() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::currentPageChanged);

    fixture.viewModel.setCurrentPage(QStringLiteral("Memory"));

    QCOMPARE(fixture.viewModel.currentPage(), QStringLiteral("Memory"));
    QCOMPARE(spy.count(), 1);

    fixture.viewModel.setCurrentPage(QStringLiteral("Settings"));
    QCOMPARE(fixture.viewModel.currentPage(), QStringLiteral("Settings"));
    QCOMPARE(spy.count(), 2);

    fixture.viewModel.setCurrentPage(QStringLiteral("Agents"));
    QCOMPARE(fixture.viewModel.currentPage(), QStringLiteral("Agents"));
    QCOMPARE(spy.count(), 3);
}

void DesktopShellViewModelTest::ignoresRepeatedAndUnknownNavigationChanges() {
    ViewModelFixture fixture;
    QSignalSpy spy(&fixture.viewModel, &DesktopShellViewModel::currentPageChanged);

    fixture.viewModel.setCurrentPage(QStringLiteral("Dashboard"));
    QCOMPARE(spy.count(), 0);

    fixture.viewModel.setCurrentPage(QStringLiteral("Unknown"));
    QCOMPARE(fixture.viewModel.currentPage(), QStringLiteral("Dashboard"));
    QCOMPARE(spy.count(), 0);
}

void DesktopShellViewModelTest::localizesVoiceFailures() {
    ViewModelFixture fixture;
    auto message = [&](const char* code) { return fixture.viewModel.voiceStatusMessage(QString::fromLatin1(code)); };
    QVERIFY(message("").isEmpty());
    QVERIFY(message("ModelUnavailable: technical detail").contains("speech engine"));
    QVERIFY(message("RuntimeUnavailable").contains("speech engine"));
    QVERIFY(message("MicrophonePermissionDenied").contains("denied"));
    QVERIFY(message("AudioDeviceUnavailable").contains("microphone"));
    QVERIFY(message("TranscriptionFailure").contains("recognition failed"));
    QVERIFY(message("NoSpeechDetected").contains("No speech"));
    QVERIFY(message("Cancelled").contains("cancelled"));
    QCOMPARE(message("Localized system guidance"), QStringLiteral("Localized system guidance"));
    QVERIFY(message("ModelUnavailable") != message("AudioDeviceUnavailable"));
}

QTEST_MAIN(DesktopShellViewModelTest)

#include "test_desktop_shell_view_model.moc"
