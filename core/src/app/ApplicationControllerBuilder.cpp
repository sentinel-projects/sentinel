// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/app/ApplicationControllerBuilder.h"

#include <QFileInfo>

#include "sentinel/core/agent/LlmAgentRuntime.h"
#include "sentinel/core/agent/NullAgentRuntime.h"
#include "sentinel/core/chat/LocalEchoProvider.h"
#include "sentinel/core/chat/SQLiteChatHistoryStore.h"
#include "sentinel/core/chat/SQLiteConversationStore.h"
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include "sentinel/core/runtime/AlarmStore.h"
#include "sentinel/core/runtime/OllamaRuntime.h"
#include "sentinel/core/runtime/RealToolExecutor.h"
#include "sentinel/core/runtime/RuntimePermissions.h"
#include "sentinel/core/security/StaticSandboxPolicy.h"

namespace sentinel::core {

ApplicationControllerBuilder::ApplicationControllerBuilder() = default;
ApplicationControllerBuilder::~ApplicationControllerBuilder() = default;

ApplicationControllerBuilder&
ApplicationControllerBuilder::withStandardDefaults(const StandardPathProvider& pathProvider,
                                                   AppSettings& settings) {
    if (!m_storageAdmission) {
        m_storageAdmission = settings.storageAdmission();
    }
    if (!m_storageAdmission) {
        m_storageAdmission =
            std::make_shared<StorageAdmission>(StorageAdmission::Scope::LiveApplication);
    }
    settings.attachStorageAdmission(m_storageAdmission);
    const auto ollamaConfig = OllamaConfig::fromEndpoint(settings.ollamaEndpoint());

    if (!m_modelService)
        m_modelService = std::make_unique<ModelService>(&settings);
    m_localRuntime = std::make_unique<OllamaLocalRuntime>(ollamaConfig, m_modelService.get());
    m_runtimeCapabilities = std::make_unique<OllamaRuntimeCapabilityRegistry>(ollamaConfig, m_modelService.get());
    m_localRuntimeSessions = std::make_unique<OllamaRuntimeSessionManager>(ollamaConfig, m_modelService.get());
    m_localRuntimeAdapter = std::make_unique<OllamaLocalRuntimeAdapter>(ollamaConfig, m_modelService.get());
    m_providerRuntimeBridge = std::make_unique<OllamaProviderRuntimeBridge>(ollamaConfig);
    m_memoryStore =
        std::make_unique<SQLiteMemoryStore>(pathProvider.memoryDatabasePath(), m_storageAdmission);
    m_chatHistoryStore = std::make_unique<SQLiteChatHistoryStore>(
        pathProvider.chatHistoryDatabasePath(), m_storageAdmission);
    m_alarmStore =
        std::make_shared<AlarmStore>(QFileInfo(pathProvider.memoryDatabasePath()).absolutePath() +
                                     QStringLiteral("/alarms.json"));
    m_agentRuntime = std::make_unique<NullAgentRuntime>(NullAgentRuntime::standardTools());
    m_sandboxPolicy = std::make_unique<StaticSandboxPolicy>(
        QSet<QString>{QStringLiteral("tool.metadata.read")});
    m_toolExecutor = std::make_unique<RealToolExecutor>(m_alarmStore);
    m_runtimePermissionPolicy = std::make_unique<LocalOnlyRuntimePermissionPolicy>();
    m_ollamaRuntimeClient = std::make_unique<OllamaHttpRuntimeClient>(ollamaConfig);
    m_localInferenceClient = std::make_unique<OllamaLocalInferenceClient>(ollamaConfig);
    m_localInferenceStreamClient = std::make_unique<OllamaLocalInferenceStreamClient>(ollamaConfig);
    m_conversationStore = std::make_unique<SQLiteConversationStore>(
        pathProvider.conversationDatabasePath(), m_storageAdmission);

    return *this;
}

ApplicationControllerBuilder&
ApplicationControllerBuilder::withProvider(std::unique_ptr<IChatProvider> provider) {
    m_provider = std::move(provider);
    m_agentStepPlanner.reset();
    return *this;
}

ApplicationControllerBuilder&
ApplicationControllerBuilder::withMemoryStore(std::unique_ptr<IMemoryStore> memoryStore) {
    m_memoryStore = std::move(memoryStore);
    return *this;
}

ApplicationControllerBuilder&
ApplicationControllerBuilder::withChatSession(std::unique_ptr<ChatSession> chatSession) {
    m_chatSession = std::move(chatSession);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withChatHistoryStore(
    std::unique_ptr<IChatHistoryStore> chatHistoryStore) {
    m_chatHistoryStore = std::move(chatHistoryStore);
    return *this;
}

ApplicationControllerBuilder&
ApplicationControllerBuilder::withAgentRuntime(std::unique_ptr<IAgentRuntime> agentRuntime) {
    m_agentRuntime = std::move(agentRuntime);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withAgentStepPlanner(
    std::unique_ptr<IAgentStepPlanner> agentStepPlanner) {
    m_agentStepPlanner = std::move(agentStepPlanner);
    return *this;
}

ApplicationControllerBuilder&
ApplicationControllerBuilder::withApprovalPolicy(std::unique_ptr<IApprovalPolicy> approvalPolicy) {
    m_approvalPolicy = std::move(approvalPolicy);
    return *this;
}

ApplicationControllerBuilder&
ApplicationControllerBuilder::withSandboxPolicy(std::unique_ptr<ISandboxPolicy> sandboxPolicy) {
    m_sandboxPolicy = std::move(sandboxPolicy);
    return *this;
}

ApplicationControllerBuilder&
ApplicationControllerBuilder::withToolExecutor(std::unique_ptr<IToolExecutor> toolExecutor) {
    m_toolExecutor = std::move(toolExecutor);
    return *this;
}

ApplicationControllerBuilder&
ApplicationControllerBuilder::withModelRouter(std::unique_ptr<IModelRouter> modelRouter) {
    m_modelRouter = std::move(modelRouter);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withProviderCatalog(
    std::unique_ptr<IProviderCatalog> providerCatalog) {
    m_providerCatalog = std::move(providerCatalog);
    return *this;
}

ApplicationControllerBuilder&
ApplicationControllerBuilder::withTaskPlanner(std::unique_ptr<ITaskPlanner> taskPlanner) {
    m_taskPlanner = std::move(taskPlanner);
    return *this;
}

ApplicationControllerBuilder&
ApplicationControllerBuilder::withAgentRegistry(std::unique_ptr<IAgentRegistry> agentRegistry) {
    m_agentRegistry = std::move(agentRegistry);
    return *this;
}

ApplicationControllerBuilder&
ApplicationControllerBuilder::withMemoryCatalog(std::unique_ptr<IMemoryCatalog> memoryCatalog) {
    m_memoryCatalog = std::move(memoryCatalog);
    return *this;
}

ApplicationControllerBuilder&
ApplicationControllerBuilder::withLocalRuntime(std::unique_ptr<ILocalRuntime> localRuntime) {
    m_localRuntime = std::move(localRuntime);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withLocalRuntimeSessions(
    std::unique_ptr<ILocalRuntimeSessionManager> localRuntimeSessions) {
    m_localRuntimeSessions = std::move(localRuntimeSessions);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withRuntimeCapabilities(
    std::unique_ptr<IRuntimeCapabilityRegistry> runtimeCapabilities) {
    m_runtimeCapabilities = std::move(runtimeCapabilities);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withRuntimePermissionPolicy(
    std::unique_ptr<IRuntimePermissionPolicy> runtimePermissionPolicy) {
    m_runtimePermissionPolicy = std::move(runtimePermissionPolicy);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withRuntimeSafetyPolicy(
    std::unique_ptr<IRuntimeSafetyPolicy> runtimeSafetyPolicy) {
    m_runtimeSafetyPolicy = std::move(runtimeSafetyPolicy);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withRuntimePipeline(
    std::unique_ptr<IRuntimePipeline> runtimePipeline) {
    m_runtimePipeline = std::move(runtimePipeline);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withExecutionLifecycle(
    std::unique_ptr<IExecutionLifecycle> executionLifecycle) {
    m_executionLifecycle = std::move(executionLifecycle);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withExecutionCoordinator(
    std::unique_ptr<ExecutionCoordinator> executionCoordinator) {
    m_executionCoordinator = std::move(executionCoordinator);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withLocalRuntimeAdapter(
    std::unique_ptr<ILocalRuntimeAdapter> localRuntimeAdapter) {
    m_localRuntimeAdapter = std::move(localRuntimeAdapter);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withProviderRuntimeBridge(
    std::unique_ptr<IProviderRuntimeBridge> providerRuntimeBridge) {
    m_providerRuntimeBridge = std::move(providerRuntimeBridge);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withRuntimeIntegrationReadiness(
    std::unique_ptr<StaticRuntimeIntegrationReadiness> runtimeIntegrationReadiness) {
    m_runtimeIntegrationReadiness = std::move(runtimeIntegrationReadiness);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withOllamaRuntimeClient(
    std::unique_ptr<IOllamaRuntimeClient> ollamaRuntimeClient) {
    m_ollamaRuntimeClient = std::move(ollamaRuntimeClient);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withLocalInferenceClient(
    std::unique_ptr<ILocalInferenceClient> localInferenceClient) {
    m_localInferenceClient = std::move(localInferenceClient);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withLocalInferenceStreamClient(
    std::unique_ptr<ILocalInferenceStreamClient> localInferenceStreamClient) {
    m_localInferenceStreamClient = std::move(localInferenceStreamClient);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withModelManagementService(
    std::unique_ptr<IModelManagementService> modelManagementService) {
    m_modelManagementService = std::move(modelManagementService);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withTextToSpeechProvider(
    std::unique_ptr<ITextToSpeechProvider> textToSpeechProvider) {
    m_textToSpeechProvider = std::move(textToSpeechProvider);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withSpeechToTextProvider(
    std::unique_ptr<ISpeechToTextProvider> speechToTextProvider) {
    m_speechToTextProvider = std::move(speechToTextProvider);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withVoiceRuntimeCoordinator(
    std::unique_ptr<IVoiceRuntimeCoordinator> voiceRuntimeCoordinator) {
    m_voiceRuntimeCoordinator = std::move(voiceRuntimeCoordinator);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withVoiceRuntimeEnvironment(
    std::unique_ptr<IVoiceRuntimeEnvironment> voiceRuntimeEnvironment) {
    m_voiceRuntimeEnvironment = std::move(voiceRuntimeEnvironment);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withPiperTextToSpeechProvider(
    std::unique_ptr<PiperTextToSpeechProvider> piperTextToSpeechProvider) {
    m_piperTextToSpeechProvider = std::move(piperTextToSpeechProvider);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withLocalInferenceWorker(
    std::unique_ptr<ILocalInferenceWorker> localInferenceWorker) {
    m_localInferenceWorker = std::move(localInferenceWorker);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withConversationStore(
    std::unique_ptr<IConversationStore> conversationStore) {
    m_conversationStore = std::move(conversationStore);
    return *this;
}

ApplicationControllerBuilder& ApplicationControllerBuilder::withAgentTaskRuntime(
    std::unique_ptr<IAgentTaskRuntime> agentTaskRuntime) {
    m_agentTaskRuntime = std::move(agentTaskRuntime);
    return *this;
}

ApplicationControllerBuilder&
ApplicationControllerBuilder::withModelService(std::unique_ptr<ModelService> modelService) {
    m_modelService = std::move(modelService);
    if (auto* runtime = dynamic_cast<OllamaLocalRuntime*>(m_localRuntime.get()))
        runtime->setModelService(m_modelService.get());
    if (auto* sessions = dynamic_cast<OllamaRuntimeSessionManager*>(m_localRuntimeSessions.get()))
        sessions->setModelService(m_modelService.get());
    if (auto* capabilities = dynamic_cast<OllamaRuntimeCapabilityRegistry*>(m_runtimeCapabilities.get()))
        capabilities->setModelService(m_modelService.get());
    if (auto* adapter = dynamic_cast<OllamaLocalRuntimeAdapter*>(m_localRuntimeAdapter.get()))
        adapter->setModelService(m_modelService.get());
    return *this;
}

std::unique_ptr<ApplicationController> ApplicationControllerBuilder::build() {
    if (!m_agentStepPlanner && m_agentRuntime) {
        m_agentStepPlanner =
            std::make_unique<LlmAgentRuntime>(NullAgentRuntime::standardTools(), nullptr);
    }
    if (m_toolExecutor && m_alarmStore) {
        if (auto* realExecutor = dynamic_cast<RealToolExecutor*>(m_toolExecutor.get())) {
            realExecutor->setAlarmStore(m_alarmStore);
        }
    }

    auto controller = std::make_unique<ApplicationController>(
        std::move(m_provider), std::move(m_memoryStore), std::move(m_chatSession),
        std::move(m_chatHistoryStore), std::move(m_agentRuntime), std::move(m_approvalPolicy),
        std::move(m_sandboxPolicy), std::move(m_toolExecutor), std::move(m_modelRouter),
        std::move(m_providerCatalog), std::move(m_taskPlanner), std::move(m_agentRegistry),
        std::move(m_memoryCatalog), std::move(m_localRuntime), std::move(m_localRuntimeSessions),
        std::move(m_runtimeCapabilities), std::move(m_runtimePermissionPolicy),
        std::move(m_runtimeSafetyPolicy), std::move(m_runtimePipeline),
        std::move(m_executionLifecycle), std::move(m_executionCoordinator),
        std::move(m_localRuntimeAdapter), std::move(m_providerRuntimeBridge),
        std::move(m_runtimeIntegrationReadiness), std::move(m_ollamaRuntimeClient),
        std::move(m_localInferenceClient), std::move(m_localInferenceStreamClient),
        std::move(m_modelManagementService), std::move(m_textToSpeechProvider),
        std::move(m_speechToTextProvider), std::move(m_voiceRuntimeCoordinator),
        std::move(m_voiceRuntimeEnvironment), std::move(m_piperTextToSpeechProvider),
        std::move(m_localInferenceWorker), std::move(m_conversationStore),
        std::move(m_agentTaskRuntime), std::move(m_agentStepPlanner), std::move(m_modelService),
        nullptr, m_storageAdmission);
    controller->attachStorageAdmission(m_storageAdmission);
    controller->attachAlarmStore(m_alarmStore);
    return controller;
}

} // namespace sentinel::core
