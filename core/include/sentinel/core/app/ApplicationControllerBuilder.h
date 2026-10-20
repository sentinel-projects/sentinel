// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SENTINEL_CORE_APPLICATIONCONTROLLERBUILDER_H
#define SENTINEL_CORE_APPLICATIONCONTROLLERBUILDER_H

#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/ApplicationController.h"
#include "sentinel/core/platform/StandardPathProvider.h"
#include "sentinel/core/runtime/AlarmStore.h"

#include <memory>

namespace sentinel::core {

class ApplicationControllerBuilder {
public:
    ApplicationControllerBuilder();
    ~ApplicationControllerBuilder();

    ApplicationControllerBuilder& withStandardDefaults(const StandardPathProvider& pathProvider,
                                                       AppSettings& settings);

    ApplicationControllerBuilder& withProvider(std::unique_ptr<IChatProvider> provider);
    ApplicationControllerBuilder& withMemoryStore(std::unique_ptr<IMemoryStore> memoryStore);
    ApplicationControllerBuilder& withChatSession(std::unique_ptr<ChatSession> chatSession);
    ApplicationControllerBuilder&
    withChatHistoryStore(std::unique_ptr<IChatHistoryStore> chatHistoryStore);
    ApplicationControllerBuilder& withAgentRuntime(std::unique_ptr<IAgentRuntime> agentRuntime);
    ApplicationControllerBuilder&
    withAgentStepPlanner(std::unique_ptr<IAgentStepPlanner> agentStepPlanner);
    ApplicationControllerBuilder&
    withApprovalPolicy(std::unique_ptr<IApprovalPolicy> approvalPolicy);
    ApplicationControllerBuilder& withSandboxPolicy(std::unique_ptr<ISandboxPolicy> sandboxPolicy);
    ApplicationControllerBuilder& withToolExecutor(std::unique_ptr<IToolExecutor> toolExecutor);
    ApplicationControllerBuilder& withModelRouter(std::unique_ptr<IModelRouter> modelRouter);
    ApplicationControllerBuilder&
    withProviderCatalog(std::unique_ptr<IProviderCatalog> providerCatalog);
    ApplicationControllerBuilder& withTaskPlanner(std::unique_ptr<ITaskPlanner> taskPlanner);
    ApplicationControllerBuilder& withAgentRegistry(std::unique_ptr<IAgentRegistry> agentRegistry);
    ApplicationControllerBuilder& withMemoryCatalog(std::unique_ptr<IMemoryCatalog> memoryCatalog);
    ApplicationControllerBuilder& withLocalRuntime(std::unique_ptr<ILocalRuntime> localRuntime);
    ApplicationControllerBuilder&
    withLocalRuntimeSessions(std::unique_ptr<ILocalRuntimeSessionManager> localRuntimeSessions);
    ApplicationControllerBuilder&
    withRuntimeCapabilities(std::unique_ptr<IRuntimeCapabilityRegistry> runtimeCapabilities);
    ApplicationControllerBuilder&
    withRuntimePermissionPolicy(std::unique_ptr<IRuntimePermissionPolicy> runtimePermissionPolicy);
    ApplicationControllerBuilder&
    withRuntimeSafetyPolicy(std::unique_ptr<IRuntimeSafetyPolicy> runtimeSafetyPolicy);
    ApplicationControllerBuilder&
    withRuntimePipeline(std::unique_ptr<IRuntimePipeline> runtimePipeline);
    ApplicationControllerBuilder&
    withExecutionLifecycle(std::unique_ptr<IExecutionLifecycle> executionLifecycle);
    ApplicationControllerBuilder&
    withExecutionCoordinator(std::unique_ptr<ExecutionCoordinator> executionCoordinator);
    ApplicationControllerBuilder&
    withLocalRuntimeAdapter(std::unique_ptr<ILocalRuntimeAdapter> localRuntimeAdapter);
    ApplicationControllerBuilder&
    withProviderRuntimeBridge(std::unique_ptr<IProviderRuntimeBridge> providerRuntimeBridge);
    ApplicationControllerBuilder& withRuntimeIntegrationReadiness(
        std::unique_ptr<StaticRuntimeIntegrationReadiness> runtimeIntegrationReadiness);
    ApplicationControllerBuilder&
    withOllamaRuntimeClient(std::unique_ptr<IOllamaRuntimeClient> ollamaRuntimeClient);
    ApplicationControllerBuilder&
    withLocalInferenceClient(std::unique_ptr<ILocalInferenceClient> localInferenceClient);
    ApplicationControllerBuilder& withLocalInferenceStreamClient(
        std::unique_ptr<ILocalInferenceStreamClient> localInferenceStreamClient);
    ApplicationControllerBuilder&
    withModelManagementService(std::unique_ptr<IModelManagementService> modelManagementService);
    ApplicationControllerBuilder&
    withTextToSpeechProvider(std::unique_ptr<ITextToSpeechProvider> textToSpeechProvider);
    ApplicationControllerBuilder&
    withSpeechToTextProvider(std::unique_ptr<ISpeechToTextProvider> speechToTextProvider);
    ApplicationControllerBuilder&
    withVoiceRuntimeCoordinator(std::unique_ptr<IVoiceRuntimeCoordinator> voiceRuntimeCoordinator);
    ApplicationControllerBuilder&
    withVoiceRuntimeEnvironment(std::unique_ptr<IVoiceRuntimeEnvironment> voiceRuntimeEnvironment);
    ApplicationControllerBuilder& withPiperTextToSpeechProvider(
        std::unique_ptr<PiperTextToSpeechProvider> piperTextToSpeechProvider);
    ApplicationControllerBuilder&
    withLocalInferenceWorker(std::unique_ptr<ILocalInferenceWorker> localInferenceWorker);
    ApplicationControllerBuilder&
    withConversationStore(std::unique_ptr<IConversationStore> conversationStore);
    ApplicationControllerBuilder&
    withAgentTaskRuntime(std::unique_ptr<IAgentTaskRuntime> agentTaskRuntime);
    ApplicationControllerBuilder& withModelService(std::unique_ptr<ModelService> modelService);

    std::unique_ptr<ApplicationController> build();

private:
    std::unique_ptr<IChatProvider> m_provider;
    std::unique_ptr<ModelService> m_modelService;
    std::unique_ptr<IMemoryStore> m_memoryStore;
    std::unique_ptr<ChatSession> m_chatSession;
    std::unique_ptr<IChatHistoryStore> m_chatHistoryStore;
    std::unique_ptr<IAgentRuntime> m_agentRuntime;
    std::unique_ptr<IAgentStepPlanner> m_agentStepPlanner;
    std::unique_ptr<IApprovalPolicy> m_approvalPolicy;
    std::unique_ptr<ISandboxPolicy> m_sandboxPolicy;
    std::unique_ptr<IToolExecutor> m_toolExecutor;
    std::unique_ptr<IModelRouter> m_modelRouter;
    std::unique_ptr<IProviderCatalog> m_providerCatalog;
    std::unique_ptr<ITaskPlanner> m_taskPlanner;
    std::unique_ptr<IAgentRegistry> m_agentRegistry;
    std::unique_ptr<IMemoryCatalog> m_memoryCatalog;
    std::unique_ptr<ILocalRuntime> m_localRuntime;
    std::unique_ptr<ILocalRuntimeSessionManager> m_localRuntimeSessions;
    std::unique_ptr<IRuntimeCapabilityRegistry> m_runtimeCapabilities;
    std::unique_ptr<IRuntimePermissionPolicy> m_runtimePermissionPolicy;
    std::unique_ptr<IRuntimeSafetyPolicy> m_runtimeSafetyPolicy;
    std::unique_ptr<IRuntimePipeline> m_runtimePipeline;
    std::unique_ptr<IExecutionLifecycle> m_executionLifecycle;
    std::unique_ptr<ExecutionCoordinator> m_executionCoordinator;
    std::unique_ptr<ILocalRuntimeAdapter> m_localRuntimeAdapter;
    std::unique_ptr<IProviderRuntimeBridge> m_providerRuntimeBridge;
    std::unique_ptr<StaticRuntimeIntegrationReadiness> m_runtimeIntegrationReadiness;
    std::unique_ptr<IOllamaRuntimeClient> m_ollamaRuntimeClient;
    std::unique_ptr<ILocalInferenceClient> m_localInferenceClient;
    std::unique_ptr<ILocalInferenceStreamClient> m_localInferenceStreamClient;
    std::unique_ptr<IModelManagementService> m_modelManagementService;
    std::unique_ptr<ITextToSpeechProvider> m_textToSpeechProvider;
    std::unique_ptr<ISpeechToTextProvider> m_speechToTextProvider;
    std::unique_ptr<IVoiceRuntimeCoordinator> m_voiceRuntimeCoordinator;
    std::unique_ptr<IVoiceRuntimeEnvironment> m_voiceRuntimeEnvironment;
    std::unique_ptr<PiperTextToSpeechProvider> m_piperTextToSpeechProvider;
    std::unique_ptr<ILocalInferenceWorker> m_localInferenceWorker;
    std::unique_ptr<IConversationStore> m_conversationStore;
    std::unique_ptr<IAgentTaskRuntime> m_agentTaskRuntime;
    std::shared_ptr<StorageAdmission> m_storageAdmission;
    std::shared_ptr<AlarmStore> m_alarmStore;
};

} // namespace sentinel::core

#endif // SENTINEL_CORE_APPLICATIONCONTROLLERBUILDER_H
