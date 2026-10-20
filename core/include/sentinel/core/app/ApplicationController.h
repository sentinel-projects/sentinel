// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "sentinel/core/app/StorageAdmission.h"

#include "sentinel/core/chat/ChatModeService.h"

#include "sentinel/core/agent/AgentActivityLog.h"
#include "sentinel/core/agent/AgentLoopState.h"
#include "sentinel/core/agent/AgentPipelineResult.h"
#include "sentinel/core/agent/AgentRuntimeContext.h"
#include "sentinel/core/agent/AgentTaskRuntime.h"
#include "sentinel/core/agent/IAgentRegistry.h"
#include "sentinel/core/agent/IAgentRuntime.h"
#include "sentinel/core/agent/IAgentRunStore.h"
#include "sentinel/core/agent/IAgentStepPlanner.h"
#include "sentinel/core/app/ContextAssembly.h"
#include "sentinel/core/app/ITaskPlanner.h"
#include "sentinel/core/app/OrchestrationDiagnostics.h"
#include "sentinel/core/app/OrchestrationSnapshot.h"
#include "sentinel/core/app/WorkspaceService.h"
#include "sentinel/core/chat/AudioFileSession.h"
#include "sentinel/core/chat/ChatSession.h"
#include "sentinel/core/chat/ConversationHistoryMetadata.h"
#include "sentinel/core/chat/ConversationSession.h"
#include "sentinel/core/chat/ConversationStateGraph.h"
#include "sentinel/core/chat/IChatHistoryStore.h"
#include "sentinel/core/chat/IConversationStore.h"
#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/interfaces/IMemoryStore.h"
#include "sentinel/core/memory/IMemoryCatalog.h"
#include "sentinel/core/memory/MemoryCandidate.h"
#include "sentinel/core/memory/MemoryRecall.h"
#include "sentinel/core/memory/SemanticRetrieval.h"
#include "sentinel/core/model/IModelRouter.h"
#include "sentinel/core/model/IProviderCatalog.h"
#include "sentinel/core/model/ModelManagement.h"
#include "sentinel/core/model/ModelRegistry.h"
#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/runtime/ExecutionLifecycle.h"
#include "sentinel/core/runtime/IToolExecutor.h"
#include "sentinel/core/runtime/LocalInference.h"
#include "sentinel/core/runtime/LocalRuntime.h"
#include "sentinel/core/runtime/LocalRuntimeSession.h"
#include "sentinel/core/runtime/OllamaRuntime.h"
#include "sentinel/core/runtime/RuntimeCapabilities.h"
#include "sentinel/core/runtime/RuntimeIntegration.h"
#include "sentinel/core/runtime/RuntimePermissions.h"
#include "sentinel/core/runtime/RuntimePipeline.h"
#include "sentinel/core/runtime/RuntimeProvider.h"
#include "sentinel/core/runtime/RuntimeSafety.h"
#include "sentinel/core/security/CredentialStore.h"
#include "sentinel/core/security/IApprovalPolicy.h"
#include "sentinel/core/security/ISandboxPolicy.h"
#include "sentinel/core/security/ProviderCredentials.h"
#include "sentinel/core/voice/PiperTts.h"
#include "sentinel/core/voice/Voice.h"
#include "sentinel/core/voice/WhisperTranscription.h"
#include "sentinel/core/voice/UnifiedAudioService.h"
#include "sentinel/core/voice/SpeechAssetCatalogAdapter.h"

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <atomic>
#include <memory>

class QTimer;
class QThread;

namespace sentinel::core {

class AlarmStore;
class AppSettings;
class ControlledTaskService;
class PermissionService;
class ModelLibraryService;
class ModelOperationService;

class ApplicationController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString providerName READ providerName CONSTANT)
    Q_PROPERTY(QString providerStatus READ providerStatus NOTIFY runtimeProviderRegistryChanged)
    Q_PROPERTY(QString agentStatus READ agentStatus NOTIFY agentStatusChanged)
    Q_PROPERTY(QString lastAgentResponse READ lastAgentResponse NOTIFY agentResponseChanged)
    Q_PROPERTY(QString latestToolPlanStatus READ latestToolPlanStatus NOTIFY toolPlanChanged)
    Q_PROPERTY(QString latestToolPlanSummary READ latestToolPlanSummary NOTIFY toolPlanChanged)
    Q_PROPERTY(QString latestApprovalStatus READ latestApprovalStatus NOTIFY approvalChanged)
    Q_PROPERTY(QString latestApprovalSummary READ latestApprovalSummary NOTIFY approvalChanged)
    Q_PROPERTY(QString latestSandboxStatus READ latestSandboxStatus NOTIFY sandboxChanged)
    Q_PROPERTY(QString latestSandboxSummary READ latestSandboxSummary NOTIFY sandboxChanged)
    Q_PROPERTY(QString latestToolExecutionStatus READ latestToolExecutionStatus NOTIFY
                   toolExecutionChanged)
    Q_PROPERTY(QString latestToolExecutionSummary READ latestToolExecutionSummary NOTIFY
                   toolExecutionChanged)
    Q_PROPERTY(QString latestAgentPipelineStatus READ latestAgentPipelineStatus NOTIFY
                   agentPipelineChanged)
    Q_PROPERTY(QString latestAgentPipelineSummary READ latestAgentPipelineSummary NOTIFY
                   agentPipelineChanged)
    Q_PROPERTY(QString runtimeSessionId READ runtimeSessionId NOTIFY runtimeContextChanged)
    Q_PROPERTY(QString runtimeContextStatus READ runtimeContextStatus NOTIFY runtimeContextChanged)
    Q_PROPERTY(
        QString runtimeContextSummary READ runtimeContextSummary NOTIFY runtimeContextChanged)
    Q_PROPERTY(QStringList runtimeContextActiveToolIds READ runtimeContextActiveToolIds NOTIFY
                   runtimeContextChanged)
    Q_PROPERTY(
        QString conversationSessionId READ conversationSessionId NOTIFY conversationSessionChanged)
    Q_PROPERTY(QString conversationSessionStatus READ conversationSessionStatus NOTIFY
                   conversationSessionChanged)
    Q_PROPERTY(QString interactionMode READ interactionMode NOTIFY conversationSessionChanged)
    Q_PROPERTY(QString attentionState READ attentionState NOTIFY conversationSessionChanged)
    Q_PROPERTY(
        QString contextWindowSummary READ contextWindowSummary NOTIFY conversationSessionChanged)
    Q_PROPERTY(QString conversationState READ conversationState NOTIFY conversationStateChanged)
    Q_PROPERTY(QString conversationTransitionStatus READ conversationTransitionStatus NOTIFY
                   conversationStateChanged)
    Q_PROPERTY(QString conversationTransitionSummary READ conversationTransitionSummary NOTIFY
                   conversationStateChanged)
    Q_PROPERTY(QString conversationRuntimeSummary READ conversationRuntimeSummary NOTIFY
                   conversationRuntimeChanged)
    Q_PROPERTY(QStringList conversationRuntimeSummaryLines READ conversationRuntimeSummaryLines
                   NOTIFY conversationRuntimeChanged)
    Q_PROPERTY(QString conversationRuntimeRequestId READ conversationRuntimeRequestId NOTIFY
                   conversationRuntimeChanged)
    Q_PROPERTY(QString conversationRuntimeActiveModel READ conversationRuntimeActiveModel NOTIFY
                   conversationRuntimeChanged)
    Q_PROPERTY(QString conversationRuntimeActiveRoute READ conversationRuntimeActiveRoute NOTIFY
                   conversationRuntimeChanged)
    Q_PROPERTY(bool conversationRuntimeStreaming READ conversationRuntimeStreaming NOTIFY
                   conversationRuntimeChanged)
    Q_PROPERTY(QString conversationRuntimeLastSuccessSummary READ
                   conversationRuntimeLastSuccessSummary NOTIFY conversationRuntimeChanged)
    Q_PROPERTY(QString conversationRuntimeLastErrorSummary READ conversationRuntimeLastErrorSummary
                   NOTIFY conversationRuntimeChanged)
    Q_PROPERTY(QString conversationRuntimeLastLatencySummary READ
                   conversationRuntimeLastLatencySummary NOTIFY conversationRuntimeChanged)
    Q_PROPERTY(int agentActivityCount READ agentActivityCount NOTIFY agentActivityChanged)
    Q_PROPERTY(QString latestAgentActivitySummary READ latestAgentActivitySummary NOTIFY
                   agentActivityChanged)
    Q_PROPERTY(QString currentRoutingMode READ currentRoutingMode WRITE setRoutingModeByName NOTIFY
                   modelRoutingChanged)
    Q_PROPERTY(QString modelRoutingStatus READ modelRoutingStatus NOTIFY modelRoutingChanged)
    Q_PROPERTY(QString selectedModelProviderSummary READ selectedModelProviderSummary NOTIFY
                   modelRoutingChanged)
    Q_PROPERTY(QString latestTaskPlanStatus READ latestTaskPlanStatus NOTIFY taskPlanChanged)
    Q_PROPERTY(QString latestTaskPlanSummary READ latestTaskPlanSummary NOTIFY taskPlanChanged)
    Q_PROPERTY(int plannedTaskStepCount READ plannedTaskStepCount NOTIFY taskPlanChanged)
    Q_PROPERTY(int registeredAgentCount READ registeredAgentCount CONSTANT)
    Q_PROPERTY(QStringList activeAgentSummaries READ activeAgentSummaries CONSTANT)
    Q_PROPERTY(QString currentAgentSummary READ currentAgentSummary NOTIFY taskPlanChanged)
    Q_PROPERTY(QString currentMemoryAffinitySummary READ currentMemoryAffinitySummary NOTIFY
                   taskPlanChanged)
    Q_PROPERTY(int providerCatalogCount READ providerCatalogCount CONSTANT)
    Q_PROPERTY(QStringList providerCatalogSummaries READ providerCatalogSummaries CONSTANT)
    Q_PROPERTY(int memoryCatalogCount READ memoryCatalogCount CONSTANT)
    Q_PROPERTY(QStringList memoryCatalogSummaries READ memoryCatalogSummaries CONSTANT)
    Q_PROPERTY(QString orchestrationSnapshotStatus READ orchestrationSnapshotStatus NOTIFY
                   orchestrationSnapshotChanged)
    Q_PROPERTY(QString orchestrationSnapshotSummary READ orchestrationSnapshotSummary NOTIFY
                   orchestrationSnapshotChanged)
    Q_PROPERTY(QStringList orchestrationSignals READ orchestrationSignals NOTIFY
                   orchestrationSnapshotChanged)
    Q_PROPERTY(QString orchestrationReadinessStatus READ orchestrationReadinessStatus NOTIFY
                   orchestrationSnapshotChanged)
    Q_PROPERTY(QString orchestrationReadinessSummary READ orchestrationReadinessSummary NOTIFY
                   orchestrationSnapshotChanged)
    Q_PROPERTY(QStringList orchestrationDiagnostics READ orchestrationDiagnostics NOTIFY
                   orchestrationSnapshotChanged)
    Q_PROPERTY(QString agentTaskRuntimeStatus READ agentTaskRuntimeStatus CONSTANT)
    Q_PROPERTY(QString agentTaskRuntimeSummary READ agentTaskRuntimeSummary CONSTANT)
    Q_PROPERTY(int agentTaskRuntimeTaskCount READ agentTaskRuntimeTaskCount CONSTANT)
    Q_PROPERTY(int agentTaskQueueCount READ agentTaskQueueCount CONSTANT)
    Q_PROPERTY(int agentTaskQueueActiveCount READ agentTaskQueueActiveCount CONSTANT)
    Q_PROPERTY(int agentTaskQueuePlannedCount READ agentTaskQueuePlannedCount CONSTANT)
    Q_PROPERTY(int agentTaskQueueBlockedCount READ agentTaskQueueBlockedCount CONSTANT)
    Q_PROPERTY(int agentTaskQueueCompletedCount READ agentTaskQueueCompletedCount CONSTANT)
    Q_PROPERTY(int agentTaskQueueRefusedCount READ agentTaskQueueRefusedCount CONSTANT)
    Q_PROPERTY(QString latestAgentTaskSummary READ latestAgentTaskSummary CONSTANT)
    Q_PROPERTY(
        QString latestAgentTaskLifecycleSummary READ latestAgentTaskLifecycleSummary CONSTANT)
    Q_PROPERTY(QStringList agentTaskQueueSummaries READ agentTaskQueueSummaries CONSTANT)
    Q_PROPERTY(QStringList agentTaskTraceSummaries READ agentTaskTraceSummaries CONSTANT)
    Q_PROPERTY(QString agentPlanningSessionStatus READ agentPlanningSessionStatus CONSTANT)
    Q_PROPERTY(QString agentPlanningSessionSummary READ agentPlanningSessionSummary CONSTANT)
    Q_PROPERTY(int agentPlanningCandidateCount READ agentPlanningCandidateCount CONSTANT)
    Q_PROPERTY(int agentPlanningRefusedCount READ agentPlanningRefusedCount CONSTANT)
    Q_PROPERTY(
        QStringList agentPlanningCandidateSummaries READ agentPlanningCandidateSummaries CONSTANT)
    Q_PROPERTY(QStringList agentPlanningArbitrationSummaries READ agentPlanningArbitrationSummaries
                   CONSTANT)
    Q_PROPERTY(
        QStringList agentPlanningRefusalSummaries READ agentPlanningRefusalSummaries CONSTANT)
    Q_PROPERTY(QString agentPlanningFallbackSummary READ agentPlanningFallbackSummary CONSTANT)
    Q_PROPERTY(QString agentCapabilityRegistryStatus READ agentCapabilityRegistryStatus CONSTANT)
    Q_PROPERTY(QString agentCapabilityRegistrySummary READ agentCapabilityRegistrySummary CONSTANT)
    Q_PROPERTY(int agentCapabilityCount READ agentCapabilityCount CONSTANT)
    Q_PROPERTY(int agentCapabilityEnabledCount READ agentCapabilityEnabledCount CONSTANT)
    Q_PROPERTY(int agentCapabilityDisabledCount READ agentCapabilityDisabledCount CONSTANT)
    Q_PROPERTY(int agentCapabilityRestrictedCount READ agentCapabilityRestrictedCount CONSTANT)
    Q_PROPERTY(QStringList agentCapabilitySummaries READ agentCapabilitySummaries CONSTANT)
    Q_PROPERTY(QStringList agentCapabilityReadinessSummaries READ agentCapabilityReadinessSummaries
                   CONSTANT)
    Q_PROPERTY(
        QStringList agentCapabilitySafetySummaries READ agentCapabilitySafetySummaries CONSTANT)
    Q_PROPERTY(QString toolContractRegistryStatus READ toolContractRegistryStatus CONSTANT)
    Q_PROPERTY(QString toolContractRegistrySummary READ toolContractRegistrySummary CONSTANT)
    Q_PROPERTY(int toolContractCount READ toolContractCount CONSTANT)
    Q_PROPERTY(int toolContractEnabledCount READ toolContractEnabledCount CONSTANT)
    Q_PROPERTY(int toolContractDisabledCount READ toolContractDisabledCount CONSTANT)
    Q_PROPERTY(int toolContractRestrictedCount READ toolContractRestrictedCount CONSTANT)
    Q_PROPERTY(QStringList toolContractSummaries READ toolContractSummaries CONSTANT)
    Q_PROPERTY(
        QStringList toolContractPermissionSummaries READ toolContractPermissionSummaries CONSTANT)
    Q_PROPERTY(QStringList toolContractSandboxSummaries READ toolContractSandboxSummaries CONSTANT)
    Q_PROPERTY(
        QStringList toolContractReadinessSummaries READ toolContractReadinessSummaries CONSTANT)
    Q_PROPERTY(QStringList toolContractSafetySummaries READ toolContractSafetySummaries CONSTANT)
    Q_PROPERTY(QString localRuntimeStatus READ localRuntimeStatus CONSTANT)
    Q_PROPERTY(QString localRuntimeHealth READ localRuntimeHealth CONSTANT)
    Q_PROPERTY(QString localRuntimeSummary READ localRuntimeSummary CONSTANT)
    Q_PROPERTY(QStringList localRuntimeCapabilities READ localRuntimeCapabilities CONSTANT)
    Q_PROPERTY(QString localRuntimeResponseStatus READ localRuntimeResponseStatus CONSTANT)
    Q_PROPERTY(QString localRuntimeResponseSummary READ localRuntimeResponseSummary CONSTANT)
    Q_PROPERTY(int localRuntimeSessionCount READ localRuntimeSessionCount CONSTANT)
    Q_PROPERTY(QString localRuntimeSessionStatus READ localRuntimeSessionStatus CONSTANT)
    Q_PROPERTY(QString localRuntimeSessionHealth READ localRuntimeSessionHealth CONSTANT)
    Q_PROPERTY(QString localRuntimeSessionSummary READ localRuntimeSessionSummary CONSTANT)
    Q_PROPERTY(QString localRuntimeAllocationSummary READ localRuntimeAllocationSummary CONSTANT)
    Q_PROPERTY(QString localRuntimeReservationSummary READ localRuntimeReservationSummary CONSTANT)
    Q_PROPERTY(QStringList localRuntimeSessionSummaries READ localRuntimeSessionSummaries CONSTANT)
    Q_PROPERTY(int runtimeCapabilityCount READ runtimeCapabilityCount CONSTANT)
    Q_PROPERTY(QStringList enabledRuntimeCapabilitySummaries READ enabledRuntimeCapabilitySummaries
                   CONSTANT)
    Q_PROPERTY(QStringList disabledRuntimeCapabilitySummaries READ
                   disabledRuntimeCapabilitySummaries CONSTANT)
    Q_PROPERTY(
        QString runtimeNegotiationProfileSummary READ runtimeNegotiationProfileSummary CONSTANT)
    Q_PROPERTY(QString runtimeNegotiationSummary READ runtimeNegotiationSummary CONSTANT)
    Q_PROPERTY(
        QString localOnlyRuntimeEnforcementSummary READ localOnlyRuntimeEnforcementSummary CONSTANT)
    Q_PROPERTY(QString runtimePermissionDecision READ runtimePermissionDecision CONSTANT)
    Q_PROPERTY(QString runtimePermissionSummary READ runtimePermissionSummary CONSTANT)
    Q_PROPERTY(QString runtimeSafetyDecision READ runtimeSafetyDecision CONSTANT)
    Q_PROPERTY(QString runtimeSafetySummary READ runtimeSafetySummary CONSTANT)
    Q_PROPERTY(QString runtimePipelineStatus READ runtimePipelineStatus CONSTANT)
    Q_PROPERTY(QString runtimePipelineSummary READ runtimePipelineSummary CONSTANT)
    Q_PROPERTY(
        QStringList runtimePipelineTraceSummaries READ runtimePipelineTraceSummaries CONSTANT)
    Q_PROPERTY(QString executionLifecycleState READ executionLifecycleState CONSTANT)
    Q_PROPERTY(QString executionLifecycleStatus READ executionLifecycleStatus CONSTANT)
    Q_PROPERTY(QString executionLifecycleSummary READ executionLifecycleSummary CONSTANT)
    Q_PROPERTY(
        QStringList executionLifecycleTraceSummaries READ executionLifecycleTraceSummaries CONSTANT)
    Q_PROPERTY(QString executionSessionId READ executionSessionId CONSTANT)
    Q_PROPERTY(QString executionSessionStatus READ executionSessionStatus CONSTANT)
    Q_PROPERTY(QString executionSessionOwnership READ executionSessionOwnership CONSTANT)
    Q_PROPERTY(QString executionCoordinationMode READ executionCoordinationMode CONSTANT)
    Q_PROPERTY(QString executionSessionSummary READ executionSessionSummary CONSTANT)
    Q_PROPERTY(QString executionCoordinationSnapshotSummary READ
                   executionCoordinationSnapshotSummary CONSTANT)
    Q_PROPERTY(QString localRuntimeAdapterStatus READ localRuntimeAdapterStatus CONSTANT)
    Q_PROPERTY(QString localRuntimeAdapterHealth READ localRuntimeAdapterHealth CONSTANT)
    Q_PROPERTY(QString localRuntimeAdapterSummary READ localRuntimeAdapterSummary CONSTANT)
    Q_PROPERTY(QStringList localRuntimeAdapterCapabilitySummaries READ
                   localRuntimeAdapterCapabilitySummaries CONSTANT)
    Q_PROPERTY(QString providerRuntimeBridgeStatus READ providerRuntimeBridgeStatus CONSTANT)
    Q_PROPERTY(QString providerRuntimeBridgeSummary READ providerRuntimeBridgeSummary CONSTANT)
    Q_PROPERTY(QString providerRuntimeBridgeResponseSummary READ
                   providerRuntimeBridgeResponseSummary CONSTANT)
    Q_PROPERTY(
        QString runtimeIntegrationReadinessStatus READ runtimeIntegrationReadinessStatus CONSTANT)
    Q_PROPERTY(
        QString runtimeIntegrationReadinessSummary READ runtimeIntegrationReadinessSummary CONSTANT)
    Q_PROPERTY(QStringList runtimeIntegrationReadinessChecks READ runtimeIntegrationReadinessChecks
                   CONSTANT)
    Q_PROPERTY(QString selectedRuntimeProvider READ selectedRuntimeProvider WRITE
                   setSelectedRuntimeProvider NOTIFY runtimeProviderRegistryChanged)
    Q_PROPERTY(QString activeRuntimeProviderId READ activeRuntimeProviderId NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QString activeRuntimeProviderLabel READ activeRuntimeProviderLabel NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QString activeRuntimeModelLabel READ activeRuntimeModelLabel NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QString activeRuntimeReadinessState READ activeRuntimeReadinessState NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QString activeRuntimeReadinessSummary READ activeRuntimeReadinessSummary NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QString activeRuntimeLocalOnlySummary READ activeRuntimeLocalOnlySummary NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QStringList selectableRuntimeProviderIds READ selectableRuntimeProviderIds NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QStringList selectableRuntimeProviderLabels READ selectableRuntimeProviderLabels
                   NOTIFY runtimeProviderRegistryChanged)
    Q_PROPERTY(QStringList runtimeProviderCardSummaries READ runtimeProviderCardSummaries NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QStringList runtimeProviderCapabilitySummaries READ
                   runtimeProviderCapabilitySummaries NOTIFY runtimeProviderRegistryChanged)
    Q_PROPERTY(QStringList runtimeProviderValidationTraces READ runtimeProviderValidationTraces
                   NOTIFY runtimeProviderRegistryChanged)
    Q_PROPERTY(QStringList installedRuntimeProviderSummaries READ installedRuntimeProviderSummaries
                   NOTIFY runtimeProviderRegistryChanged)
    Q_PROPERTY(QStringList configuredRuntimeProviderSummaries READ
                   configuredRuntimeProviderSummaries NOTIFY runtimeProviderRegistryChanged)
    Q_PROPERTY(QStringList availableLocalRuntimeSummaries READ availableLocalRuntimeSummaries NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QString providerCredentialRegistryStatus READ providerCredentialRegistryStatus NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QString providerCredentialRegistrySummary READ providerCredentialRegistrySummary
                   NOTIFY runtimeProviderRegistryChanged)
    Q_PROPERTY(QStringList providerCredentialSummaries READ providerCredentialSummaries NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QStringList providerCredentialReadinessSummaries READ
                   providerCredentialReadinessSummaries NOTIFY runtimeProviderRegistryChanged)
    Q_PROPERTY(QStringList providerCredentialSafetySummaries READ providerCredentialSafetySummaries
                   NOTIFY runtimeProviderRegistryChanged)
    Q_PROPERTY(QString credentialStoreSummary READ credentialStoreSummary NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QString credentialStoreBackendSummary READ credentialStoreBackendSummary NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QString credentialStoreSafetySummary READ credentialStoreSafetySummary NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QStringList credentialStoreTraceSummaries READ credentialStoreTraceSummaries NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QString credentialActionReadiness READ credentialActionReadiness NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QString credentialExecutionStatus READ credentialExecutionStatus NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QString ollamaEndpoint READ ollamaEndpoint NOTIFY ollamaStatusChanged)
    Q_PROPERTY(
        QString ollamaConnectionStatus READ ollamaConnectionStatus NOTIFY ollamaStatusChanged)
    Q_PROPERTY(QString ollamaHealthStatus READ ollamaHealthStatus NOTIFY ollamaStatusChanged)
    Q_PROPERTY(QString ollamaHealthSummary READ ollamaHealthSummary NOTIFY ollamaStatusChanged)
    Q_PROPERTY(QString ollamaDiscoveryStatus READ ollamaDiscoveryStatus NOTIFY ollamaStatusChanged)
    Q_PROPERTY(int ollamaModelCount READ ollamaModelCount NOTIFY ollamaStatusChanged)
    Q_PROPERTY(QStringList ollamaModelNames READ ollamaModelNames NOTIFY ollamaStatusChanged)
    Q_PROPERTY(
        QStringList ollamaModelSummaries READ ollamaModelSummaries NOTIFY ollamaStatusChanged)
    Q_PROPERTY(QString selectedLocalModel READ selectedLocalModel WRITE setSelectedLocalModel NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QString selectedLocalModelStatus READ selectedLocalModelStatus NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QString selectedLocalModelSummary READ selectedLocalModelSummary NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QString selectedLocalModelMetadataSummary READ selectedLocalModelMetadataSummary
                   NOTIFY localModelSelectionChanged)
    Q_PROPERTY(QString activeLocalRuntimeBadge READ activeLocalRuntimeBadge NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(
        QString modelRegistryStatus READ modelRegistryStatus NOTIFY localModelSelectionChanged)
    Q_PROPERTY(
        QString modelRegistrySummary READ modelRegistrySummary NOTIFY localModelSelectionChanged)
    Q_PROPERTY(QStringList modelRegistryModelSummaries READ modelRegistryModelSummaries NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QStringList modelLibraryInstalledSummaries READ modelLibraryInstalledSummaries NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QStringList modelLibraryAvailableSummaries READ modelLibraryAvailableSummaries NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QStringList modelLibraryRecommendedSummaries READ modelLibraryRecommendedSummaries
                   NOTIFY localModelSelectionChanged)
    Q_PROPERTY(QStringList modelLibraryDetailSummaries READ modelLibraryDetailSummaries NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QStringList providerDiscoverySummaries READ providerDiscoverySummaries NOTIFY
                   runtimeProviderRegistryChanged)
    Q_PROPERTY(QStringList modelAdvisorRecommendationSummaries READ
                   modelAdvisorRecommendationSummaries NOTIFY localModelSelectionChanged)
    Q_PROPERTY(QStringList modelAdvisorAvoidSummaries READ modelAdvisorAvoidSummaries CONSTANT)
    Q_PROPERTY(QStringList downloadsCenterSummaries READ downloadsCenterSummaries NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QStringList benchmarkHubSummaries READ benchmarkHubSummaries NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QStringList selectedModelCapabilityLabels READ selectedModelCapabilityLabels NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(
        QString modelManagementStatus READ modelManagementStatus NOTIFY localModelSelectionChanged)
    Q_PROPERTY(QString modelManagementSummary READ modelManagementSummary NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(
        QString modelManagementActionAvailability READ modelManagementActionAvailability CONSTANT)
    Q_PROPERTY(QStringList modelRecommendationSummaries READ modelRecommendationSummaries CONSTANT)
    Q_PROPERTY(QStringList modelRequirementSummaries READ modelRequirementSummaries CONSTANT)
    Q_PROPERTY(QString voiceRuntimeMode READ voiceRuntimeMode CONSTANT)
    Q_PROPERTY(bool voiceEnabled READ voiceEnabled CONSTANT)
    Q_PROPERTY(QString voiceReadinessStatus READ voiceReadinessStatus CONSTANT)
    Q_PROPERTY(QString voiceReadinessSummary READ voiceReadinessSummary CONSTANT)
    Q_PROPERTY(QStringList voiceReadinessChecks READ voiceReadinessChecks CONSTANT)
    Q_PROPERTY(QStringList voiceCapabilitySummaries READ voiceCapabilitySummaries CONSTANT)
    Q_PROPERTY(QString textToSpeechStatus READ textToSpeechStatus CONSTANT)
    Q_PROPERTY(QString textToSpeechSummary READ textToSpeechSummary CONSTANT)
    Q_PROPERTY(QString speechToTextStatus READ speechToTextStatus CONSTANT)
    Q_PROPERTY(QString speechToTextSummary READ speechToTextSummary CONSTANT)
    Q_PROPERTY(QString voiceSessionId READ voiceSessionId CONSTANT)
    Q_PROPERTY(QString voiceSessionStatus READ voiceSessionStatus CONSTANT)
    Q_PROPERTY(QString voiceSessionSummary READ voiceSessionSummary CONSTANT)
    Q_PROPERTY(QString voicePipelineStatus READ voicePipelineStatus CONSTANT)
    Q_PROPERTY(QString voicePipelineSummary READ voicePipelineSummary CONSTANT)
    Q_PROPERTY(QStringList voicePipelineTraceSummaries READ voicePipelineTraceSummaries CONSTANT)
    Q_PROPERTY(QString voicePipelineSessionStatus READ voicePipelineSessionStatus NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString voicePipelineSessionSummary READ voicePipelineSessionSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QStringList voicePipelineSessionStageReadinessSummaries READ
                   voicePipelineSessionStageReadinessSummaries NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QStringList voicePipelineSessionTraceSummaries READ
                   voicePipelineSessionTraceSummaries NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString voicePipelineSessionFallbackSummary READ voicePipelineSessionFallbackSummary
                   NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString voicePipelineSessionSafetySummary READ voicePipelineSessionSafetySummary
                   NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QStringList voicePipelineSessionSafetyChecks READ voicePipelineSessionSafetyChecks
                   NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(int voicePipelineSessionReadyStageCount READ voicePipelineSessionReadyStageCount
                   NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(int voicePipelineSessionBlockedStageCount READ voicePipelineSessionBlockedStageCount
                   NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(
        QString audioFileSessionStatus READ audioFileSessionStatus NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString audioFileSessionSummary READ audioFileSessionSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString audioFileSessionReadinessSummary READ audioFileSessionReadinessSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QStringList audioFileValidationSummaries READ audioFileValidationSummaries NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QStringList audioFileSupportedExtensionSummaries READ
                   audioFileSupportedExtensionSummaries CONSTANT)
    Q_PROPERTY(QString audioFileSessionFallbackSummary READ audioFileSessionFallbackSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString audioFileSessionSafetySummary READ audioFileSessionSafetySummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QStringList audioFileSessionSafetyChecks READ audioFileSessionSafetyChecks NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QStringList audioFileSessionRefusalSummaries READ audioFileSessionRefusalSummaries
                   NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QStringList audioFileTraceSummaries READ audioFileTraceSummaries NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString voiceRuntimeStatus READ voiceRuntimeStatus CONSTANT)
    Q_PROPERTY(QString voiceRuntimeSummary READ voiceRuntimeSummary CONSTANT)
    Q_PROPERTY(QStringList voiceRuntimeCheckSummaries READ voiceRuntimeCheckSummaries CONSTANT)
    Q_PROPERTY(bool voiceRuntimeAvailable READ voiceRuntimeAvailable CONSTANT)
    Q_PROPERTY(bool voiceTextToSpeechAvailable READ voiceTextToSpeechAvailable CONSTANT)
    Q_PROPERTY(bool voiceSpeechToTextAvailable READ voiceSpeechToTextAvailable CONSTANT)
    Q_PROPERTY(bool voiceMicrophoneEnabled READ voiceMicrophoneEnabled CONSTANT)
    Q_PROPERTY(bool voicePlaybackEnabled READ voicePlaybackEnabled CONSTANT)
    Q_PROPERTY(bool voiceLocalOnlyPolicy READ voiceLocalOnlyPolicy CONSTANT)
    Q_PROPERTY(bool voiceProcessExecutionEnabled READ voiceProcessExecutionEnabled CONSTANT)
    Q_PROPERTY(QString voiceRuntimeEnvironmentStatus READ voiceRuntimeEnvironmentStatus NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString voiceRuntimeEnvironmentSummary READ voiceRuntimeEnvironmentSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(
        QStringList voiceBinarySummaries READ voiceBinarySummaries NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(
        QStringList voiceModelSummaries READ voiceModelSummaries NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(
        QStringList voiceRuntimePermissionSummaries READ voiceRuntimePermissionSummaries CONSTANT)
    Q_PROPERTY(QString voiceRuntimeSafetyStatus READ voiceRuntimeSafetyStatus CONSTANT)
    Q_PROPERTY(QString voiceRuntimeSafetySummary READ voiceRuntimeSafetySummary CONSTANT)
    Q_PROPERTY(QStringList voiceRuntimeSafetyChecks READ voiceRuntimeSafetyChecks CONSTANT)
    Q_PROPERTY(bool voiceRuntimeExecutionAllowed READ voiceRuntimeExecutionAllowed CONSTANT)
    Q_PROPERTY(QString piperTtsStatus READ piperTtsStatus NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString piperTtsSummary READ piperTtsSummary NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QStringList piperTtsReadinessChecks READ piperTtsReadinessChecks NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(bool piperTtsReady READ piperTtsReady NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString piperTtsFileOutputStatus READ piperTtsFileOutputStatus NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString piperTtsFileOutputSummary READ piperTtsFileOutputSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(
        QString piperSynthesisStatus READ piperSynthesisStatus NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString piperSynthesisReadinessSummary READ piperSynthesisReadinessSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString piperSynthesisLastSummary READ piperSynthesisLastSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString piperSynthesisFallbackSummary READ piperSynthesisFallbackSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString piperSynthesisSafetySummary READ piperSynthesisSafetySummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QStringList piperSynthesisTraceSummaries READ piperSynthesisTraceSummaries NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString piperBinaryPath READ piperBinaryPath WRITE setPiperBinaryPath NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString piperModelPath READ piperModelPath WRITE setPiperModelPath NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString whisperBinaryPath READ whisperBinaryPath WRITE setWhisperBinaryPath NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString whisperModelPath READ whisperModelPath WRITE setWhisperModelPath NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QStringList voiceConfigurationSummaries READ voiceConfigurationSummaries NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString voiceConfigurationReadinessSummary READ voiceConfigurationReadinessSummary
                   NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QStringList voiceConfigurationStatusBadges READ voiceConfigurationStatusBadges NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QStringList voiceConfigurationHintSummaries READ voiceConfigurationHintSummaries
                   NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QStringList voiceConfigurationValidationSummaries READ
                   voiceConfigurationValidationSummaries NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString piperFileOutputReadinessStatus READ piperFileOutputReadinessStatus NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString piperFileOutputReadinessSummary READ piperFileOutputReadinessSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(bool piperFileOutputExecutionEnabled READ piperFileOutputExecutionEnabled WRITE
                   setPiperFileOutputExecutionEnabled NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString piperFileOutputExecutionStatus READ piperFileOutputExecutionStatus NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString piperFileOutputExecutionSummary READ piperFileOutputExecutionSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString piperFileOutputAudioPathSummary READ piperFileOutputAudioPathSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString whisperPreparationReadinessStatus READ whisperPreparationReadinessStatus
                   NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString whisperPreparationReadinessSummary READ whisperPreparationReadinessSummary
                   NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString whisperTranscriptionStatus READ whisperTranscriptionStatus NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString whisperTranscriptionReadinessSummary READ
                   whisperTranscriptionReadinessSummary NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString whisperTranscriptionLastSummary READ whisperTranscriptionLastSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString whisperTranscriptionFallbackSummary READ whisperTranscriptionFallbackSummary
                   NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString whisperTranscriptionSafetySummary READ whisperTranscriptionSafetySummary
                   NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QStringList whisperTranscriptionTraceSummaries READ
                   whisperTranscriptionTraceSummaries NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(bool localChatInferenceEnabled READ localChatInferenceEnabled WRITE
                   setLocalChatInferenceEnabled NOTIFY localChatInferenceRoutingChanged)
    Q_PROPERTY(QString localChatInferenceStatus READ localChatInferenceStatus NOTIFY
                   localChatInferenceRoutingChanged)
    Q_PROPERTY(QString localChatInferenceSummary READ localChatInferenceSummary NOTIFY
                   localChatInferenceRoutingChanged)
    Q_PROPERTY(bool localChatSendAvailable READ localChatSendAvailable NOTIFY
                   localChatInferenceRoutingChanged)
    Q_PROPERTY(QString localChatSendAvailabilitySummary READ localChatSendAvailabilitySummary NOTIFY
                   localChatInferenceRoutingChanged)
    Q_PROPERTY(QString chatSendLifecycleState READ chatSendLifecycleState NOTIFY
                   localChatInferenceRoutingChanged)
    Q_PROPERTY(QString chatSendLifecycleSummary READ chatSendLifecycleSummary NOTIFY
                   localChatInferenceRoutingChanged)
    Q_PROPERTY(bool chatGenerationActive READ chatGenerationActive NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString activeChatProviderId READ activeChatProviderId NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString activeChatModelId READ activeChatModelId NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString chatErrorCategory READ chatErrorCategory NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString chatCatalogState READ chatCatalogState NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString chatProviderKind READ chatProviderKind NOTIFY chatMessagesChanged)
    Q_PROPERTY(bool chatAttachmentInputAvailable READ chatAttachmentInputAvailable NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(QVariantMap chatInputCapabilities READ chatInputCapabilities NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(bool promptContextInjectionEnabled READ promptContextInjectionEnabled WRITE
                   setPromptContextInjectionEnabled NOTIFY promptContextInjectionChanged)
    Q_PROPERTY(QString promptContextInjectionStatus READ promptContextInjectionStatus NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QString promptContextInjectionSummary READ promptContextInjectionSummary NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(int promptContextInjectedBlockCount READ promptContextInjectedBlockCount NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QString promptContextSourceSummary READ promptContextSourceSummary NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QString promptContextSizeSummary READ promptContextSizeSummary NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QString promptContextUsedSummary READ promptContextUsedSummary NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(int promptContextUsedMemoryCount READ promptContextUsedMemoryCount NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QString contextBudgetUsageSummary READ contextBudgetUsageSummary NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(int contextIncludedCandidateCount READ contextIncludedCandidateCount NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(int contextExcludedCandidateCount READ contextExcludedCandidateCount NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QStringList contextAssemblyTraceSummaries READ contextAssemblyTraceSummaries NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QStringList promptContextBlockSummaries READ promptContextBlockSummaries NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(
        QString conversationWindowStatus READ conversationWindowStatus NOTIFY chatMessagesChanged)
    Q_PROPERTY(
        QString conversationWindowSummary READ conversationWindowSummary NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString conversationWindowBudgetSummary READ conversationWindowBudgetSummary NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(int conversationWindowBudgetCharacters READ conversationWindowBudgetCharacters NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(int conversationWindowIncludedMessageCount READ
                   conversationWindowIncludedMessageCount NOTIFY chatMessagesChanged)
    Q_PROPERTY(int conversationWindowTruncatedMessageCount READ
                   conversationWindowTruncatedMessageCount NOTIFY chatMessagesChanged)
    Q_PROPERTY(int conversationWindowOmittedMessageCount READ conversationWindowOmittedMessageCount
                   NOTIFY chatMessagesChanged)
    Q_PROPERTY(
        QString conversationSummaryStatus READ conversationSummaryStatus NOTIFY chatMessagesChanged)
    Q_PROPERTY(
        QString conversationSummaryText READ conversationSummaryText NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString conversationSummaryBudgetSummary READ conversationSummaryBudgetSummary NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(int conversationSummaryBudgetCharacters READ conversationSummaryBudgetCharacters
                   NOTIFY chatMessagesChanged)
    Q_PROPERTY(int conversationSummaryBlockCount READ conversationSummaryBlockCount NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(int conversationSummaryMessageCount READ conversationSummaryMessageCount NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(int conversationSummaryOmittedMessageCount READ
                   conversationSummaryOmittedMessageCount NOTIFY chatMessagesChanged)
    Q_PROPERTY(int conversationSummaryTruncatedBlockCount READ
                   conversationSummaryTruncatedBlockCount NOTIFY chatMessagesChanged)
    Q_PROPERTY(QStringList conversationSummaryBlockSummaries READ conversationSummaryBlockSummaries
                   NOTIFY chatMessagesChanged)
    Q_PROPERTY(
        QString retrievalPlanningStatus READ retrievalPlanningStatus NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString retrievalPlanningSummary READ retrievalPlanningSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString retrievalPlanningReadiness READ retrievalPlanningReadiness NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString retrievalPlanningBudgetSummary READ retrievalPlanningBudgetSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString retrievalPlanningSourceSummary READ retrievalPlanningSourceSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int retrievalPlanningSelectedSourceCount READ retrievalPlanningSelectedSourceCount
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int retrievalPlanningExcludedSourceCount READ retrievalPlanningExcludedSourceCount
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int retrievalPlanningSelectedCandidateCount READ
                   retrievalPlanningSelectedCandidateCount NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int retrievalPlanningExcludedCandidateCount READ
                   retrievalPlanningExcludedCandidateCount NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int retrievalPlanningTruncatedCandidateCount READ
                   retrievalPlanningTruncatedCandidateCount NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QStringList retrievalPlanningSourceSummaries READ retrievalPlanningSourceSummaries
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString memoryRelevanceSummaryText READ memoryRelevanceSummaryText NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString memoryRelevanceBudgetSummary READ memoryRelevanceBudgetSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int memoryRelevanceIncludedCount READ memoryRelevanceIncludedCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int memoryRelevanceExcludedCount READ memoryRelevanceExcludedCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QStringList memoryRelevanceTraceSummaries READ memoryRelevanceTraceSummaries NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QStringList memoryRelevanceExclusionSummaries READ memoryRelevanceExclusionSummaries
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString conversationSalienceSummaryText READ conversationSalienceSummaryText NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString conversationSalienceBudgetSummary READ conversationSalienceBudgetSummary
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString conversationSalienceAllocationSummary READ
                   conversationSalienceAllocationSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int conversationSalienceIncludedCount READ conversationSalienceIncludedCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int conversationSalienceExcludedCount READ conversationSalienceExcludedCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int conversationSalienceTruncatedCount READ conversationSalienceTruncatedCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QStringList conversationSalienceTraceSummaries READ
                   conversationSalienceTraceSummaries NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QStringList conversationSalienceExclusionSummaries READ
                   conversationSalienceExclusionSummaries NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString conversationCompressionStatus READ conversationCompressionStatus NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString conversationCompressionReadinessSummary READ
                   conversationCompressionReadinessSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString conversationCompressionPressureSummary READ
                   conversationCompressionPressureSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int conversationCompressionCandidateCount READ conversationCompressionCandidateCount
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int conversationCompressionSelectedCandidateCount READ
                   conversationCompressionSelectedCandidateCount NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString conversationCompressionFallbackReason READ
                   conversationCompressionFallbackReason NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString conversationCompressionTraceSummary READ conversationCompressionTraceSummary
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString conversationCompressionBudgetSummary READ
                   conversationCompressionBudgetSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QStringList conversationCompressionCandidateSummaries READ
                   conversationCompressionCandidateSummaries NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QStringList conversationCompressionTraceSummaries READ
                   conversationCompressionTraceSummaries NOTIFY contextAssemblyChanged)
    Q_PROPERTY(bool conversationSummaryAvailable READ conversationSummaryAvailable NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString conversationSummaryGenerationStatus READ conversationSummaryGenerationStatus
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString conversationSummaryReadinessSummary READ conversationSummaryReadinessSummary
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString conversationSummaryBlockedReason READ conversationSummaryBlockedReason NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString conversationSummaryEstimatedCompressionGain READ
                   conversationSummaryEstimatedCompressionGain NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString conversationSummaryPreviewSummary READ conversationSummaryPreviewSummary
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString conversationSummaryPersistenceSummary READ
                   conversationSummaryPersistenceSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString conversationSummaryInjectionSummary READ conversationSummaryInjectionSummary
                   NOTIFY promptContextInjectionChanged)
    Q_PROPERTY(
        QString summaryContinuityStatus READ summaryContinuityStatus NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString summaryContinuityFreshnessSummary READ summaryContinuityFreshnessSummary
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString summaryContinuityCoverageSummary READ summaryContinuityCoverageSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString summaryContinuityContributionSummary READ
                   summaryContinuityContributionSummary NOTIFY promptContextInjectionChanged)
    Q_PROPERTY(QString summaryContinuityFallbackSummary READ summaryContinuityFallbackSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString summaryContinuityOrderingSummary READ summaryContinuityOrderingSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString summaryContinuityBudgetTrace READ summaryContinuityBudgetTrace NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(bool contextExplainabilityEnabled READ contextExplainabilityEnabled NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QString contextReasoningSummary READ contextReasoningSummary NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QString contextReasoningBudgetSummary READ contextReasoningBudgetSummary NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QString contextReasoningOrderingSummary READ contextReasoningOrderingSummary NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QString contextReasoningFallbackSummary READ contextReasoningFallbackSummary NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QStringList contextReasoningContributionSummaries READ
                   contextReasoningContributionSummaries NOTIFY promptContextInjectionChanged)
    Q_PROPERTY(QStringList contextReasoningInclusionHints READ contextReasoningInclusionHints NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QStringList contextReasoningExclusionHints READ contextReasoningExclusionHints NOTIFY
                   promptContextInjectionChanged)
    Q_PROPERTY(QStringList contextReasoningDeveloperTraces READ contextReasoningDeveloperTraces
                   NOTIFY promptContextInjectionChanged)
    Q_PROPERTY(QStringList conversationSummaryCandidateSegments READ
                   conversationSummaryCandidateSegments NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QStringList conversationSummaryGenerationTraceSummaries READ
                   conversationSummaryGenerationTraceSummaries NOTIFY contextAssemblyChanged)
    Q_PROPERTY(bool semanticRetrievalEnabled READ semanticRetrievalEnabled CONSTANT)
    Q_PROPERTY(QString semanticRetrievalStatus READ semanticRetrievalStatus CONSTANT)
    Q_PROPERTY(QString semanticRetrievalSummary READ semanticRetrievalSummary CONSTANT)
    Q_PROPERTY(QString semanticReadiness READ semanticReadiness CONSTANT)
    Q_PROPERTY(QString embeddingProviderReadiness READ embeddingProviderReadiness CONSTANT)
    Q_PROPERTY(QString embeddingProviderSummary READ embeddingProviderSummary CONSTANT)
    Q_PROPERTY(QString vectorIndexReadiness READ vectorIndexReadiness CONSTANT)
    Q_PROPERTY(QString vectorIndexSummary READ vectorIndexSummary CONSTANT)
    Q_PROPERTY(int vectorIndexedItemCount READ vectorIndexedItemCount CONSTANT)
    Q_PROPERTY(QString semanticProviderMode READ semanticProviderMode CONSTANT)
    Q_PROPERTY(QString selectedSemanticProviderName READ selectedSemanticProviderName CONSTANT)
    Q_PROPERTY(QString semanticProviderReadiness READ semanticProviderReadiness CONSTANT)
    Q_PROPERTY(QString semanticProviderHealth READ semanticProviderHealth CONSTANT)
    Q_PROPERTY(QString semanticProviderStatusSummary READ semanticProviderStatusSummary CONSTANT)
    Q_PROPERTY(QString semanticActivationReadiness READ semanticActivationReadiness CONSTANT)
    Q_PROPERTY(QString semanticActivationSummary READ semanticActivationSummary CONSTANT)
    Q_PROPERTY(QStringList semanticProviderCapabilitySummaries READ
                   semanticProviderCapabilitySummaries CONSTANT)
    Q_PROPERTY(
        QStringList semanticActivationRequiredSteps READ semanticActivationRequiredSteps CONSTANT)
    Q_PROPERTY(
        QStringList semanticRetrievalReadinessChecks READ semanticRetrievalReadinessChecks CONSTANT)
    Q_PROPERTY(
        QString semanticCandidateStatus READ semanticCandidateStatus NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticCandidateSummary READ semanticCandidateSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticCandidateBudgetSummary READ semanticCandidateBudgetSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticCandidateArbitrationSummary READ semanticCandidateArbitrationSummary
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int semanticCandidateCount READ semanticCandidateCount NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int semanticCandidateSelectedCount READ semanticCandidateSelectedCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int semanticCandidateExcludedCount READ semanticCandidateExcludedCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int semanticCandidateTruncatedCount READ semanticCandidateTruncatedCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QStringList semanticCandidateParticipationSummaries READ
                   semanticCandidateParticipationSummaries NOTIFY contextAssemblyChanged)
    Q_PROPERTY(
        QString hybridRetrievalStatus READ hybridRetrievalStatus NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString hybridRetrievalReadiness READ hybridRetrievalReadiness NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(
        QString hybridRetrievalSummary READ hybridRetrievalSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QStringList hybridRetrievalReadinessChecks READ hybridRetrievalReadinessChecks NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticArbitrationStatus READ semanticArbitrationStatus NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticArbitrationReadiness READ semanticArbitrationReadiness NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticArbitrationSummary READ semanticArbitrationSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticArbitrationBudgetSummary READ semanticArbitrationBudgetSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QStringList semanticArbitrationSelectionSummaries READ
                   semanticArbitrationSelectionSummaries NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QStringList semanticArbitrationChecks READ semanticArbitrationChecks NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString embeddingRuntimeReadiness READ embeddingRuntimeReadiness NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(
        QString embeddingRuntimeSummary READ embeddingRuntimeSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString embeddingRuntimeBudgetSummary READ embeddingRuntimeBudgetSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QStringList embeddingRuntimeRequirementSummaries READ
                   embeddingRuntimeRequirementSummaries NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QStringList embeddingRuntimeConstraintSummaries READ
                   embeddingRuntimeConstraintSummaries NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticSearchStatus READ semanticSearchStatus NOTIFY contextAssemblyChanged)
    Q_PROPERTY(
        QString semanticSearchReadiness READ semanticSearchReadiness NOTIFY contextAssemblyChanged)
    Q_PROPERTY(
        QString semanticSearchSummary READ semanticSearchSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticSearchBudgetSummary READ semanticSearchBudgetSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticSearchRuntimeState READ semanticSearchRuntimeState NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int semanticSearchCandidateCount READ semanticSearchCandidateCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticSearchArbitrationSummary READ semanticSearchArbitrationSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QStringList semanticSearchCandidateSummaries READ semanticSearchCandidateSummaries
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(
        QStringList semanticSearchChecks READ semanticSearchChecks NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString hybridBridgeStatus READ hybridBridgeStatus NOTIFY contextAssemblyChanged)
    Q_PROPERTY(
        QString hybridBridgeReadiness READ hybridBridgeReadiness NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString hybridBridgeSummary READ hybridBridgeSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString hybridBridgeBudgetSummary READ hybridBridgeBudgetSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString hybridBridgeSourceSummary READ hybridBridgeSourceSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString hybridBridgeArbitrationSummary READ hybridBridgeArbitrationSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString hybridBridgeFallbackSummary READ hybridBridgeFallbackSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int hybridBridgeCandidateCount READ hybridBridgeCandidateCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int hybridBridgeSemanticFillCount READ hybridBridgeSemanticFillCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QStringList hybridBridgeCandidateSummaries READ hybridBridgeCandidateSummaries NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QStringList hybridBridgeChecks READ hybridBridgeChecks NOTIFY contextAssemblyChanged)
    Q_PROPERTY(bool localInferenceStreamingEnabled READ localInferenceStreamingEnabled WRITE
                   setLocalInferenceStreamingEnabled NOTIFY localInferenceChanged)
    Q_PROPERTY(int localInferenceTimeoutMs READ localInferenceTimeoutMs WRITE
                   setLocalInferenceTimeoutMs NOTIFY localInferenceChanged)
    Q_PROPERTY(double localInferenceTemperature READ localInferenceTemperature WRITE
                   setLocalInferenceTemperature NOTIFY localInferenceChanged)
    Q_PROPERTY(double localInferenceTopP READ localInferenceTopP WRITE setLocalInferenceTopP NOTIFY
                   localInferenceChanged)
    Q_PROPERTY(int localInferenceMaxTokens READ localInferenceMaxTokens WRITE
                   setLocalInferenceMaxTokens NOTIFY localInferenceChanged)
    Q_PROPERTY(bool localInferenceBusy READ localInferenceBusy NOTIFY localInferenceChanged)
    Q_PROPERTY(QString localInferenceRuntimeState READ localInferenceRuntimeState NOTIFY
                   localInferenceChanged)
    Q_PROPERTY(QString localInferenceStatus READ localInferenceStatus NOTIFY localInferenceChanged)
    Q_PROPERTY(
        QString localInferenceSummary READ localInferenceSummary NOTIFY localInferenceChanged)
    Q_PROPERTY(QString localInferenceLastResponseSummary READ localInferenceLastResponseSummary
                   NOTIFY localInferenceChanged)
    Q_PROPERTY(QString localInferenceLatencySummary READ localInferenceLatencySummary NOTIFY
                   localInferenceChanged)
    Q_PROPERTY(QStringList localInferenceTraceSummaries READ localInferenceTraceSummaries NOTIFY
                   localInferenceChanged)
    Q_PROPERTY(bool localInferenceStreamingAvailable READ localInferenceStreamingAvailable NOTIFY
                   localInferenceChanged)
    Q_PROPERTY(QString localInferenceStreamStatus READ localInferenceStreamStatus NOTIFY
                   localInferenceChanged)
    Q_PROPERTY(QString localInferenceStreamSummary READ localInferenceStreamSummary NOTIFY
                   localInferenceChanged)
    Q_PROPERTY(QString localInferenceStreamingText READ localInferenceStreamingText NOTIFY
                   localInferenceChanged)
    Q_PROPERTY(int availableToolCount READ availableToolCount CONSTANT)
    Q_PROPERTY(QStringList availableToolIds READ availableToolIds CONSTANT)
    Q_PROPERTY(QStringList chatMessages READ chatMessages NOTIFY chatMessagesChanged)
    Q_PROPERTY(QVariantList chatMessageRecords READ chatMessageRecords NOTIFY chatMessagesChanged)
    Q_PROPERTY(
        QString conversationStoreStatus READ conversationStoreStatus NOTIFY chatMessagesChanged)
    Q_PROPERTY(int conversationStoreConversationCount READ conversationStoreConversationCount NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(
        QString activeConversationSummary READ activeConversationSummary NOTIFY chatMessagesChanged)
    Q_PROPERTY(QStringList conversationStoreSummaries READ conversationStoreSummaries NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(QString activeConversationId READ activeConversationId NOTIFY chatMessagesChanged)
    Q_PROPERTY(
        bool activeConversationArchived READ activeConversationArchived NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString activeConversationStateSummary READ activeConversationStateSummary NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(QStringList conversationIds READ conversationIds NOTIFY chatMessagesChanged)
    Q_PROPERTY(QStringList conversationTitles READ conversationTitles NOTIFY chatMessagesChanged)
    Q_PROPERTY(QStringList conversationActiveSummaries READ conversationActiveSummaries NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(QStringList conversationLastUpdatedSummaries READ conversationLastUpdatedSummaries
                   NOTIFY chatMessagesChanged)
    Q_PROPERTY(QStringList conversationMessageCountSummaries READ conversationMessageCountSummaries
                   NOTIFY chatMessagesChanged)
    Q_PROPERTY(QStringList conversationArchivedSummaries READ conversationArchivedSummaries NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(QStringList conversationPinnedSummaries READ conversationPinnedSummaries NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(int activeConversationCount READ activeConversationCount NOTIFY chatMessagesChanged)
    Q_PROPERTY(
        int archivedConversationCount READ archivedConversationCount NOTIFY chatMessagesChanged)
    Q_PROPERTY(int userCreatedConversationCount READ userCreatedConversationCount NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(bool conversationBrowserEmptyStateVisible READ conversationBrowserEmptyStateVisible
                   NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString conversationBrowserEmptyStateSummary READ
                   conversationBrowserEmptyStateSummary NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString conversationHistorySummaryText READ conversationHistorySummaryText NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(QStringList conversationHistorySummaryLines READ conversationHistorySummaryLines
                   NOTIFY chatMessagesChanged)
    Q_PROPERTY(int conversationHistoryMessageCount READ conversationHistoryMessageCount NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(QString conversationPersistenceStatus READ conversationPersistenceStatus NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(QString conversationLastSavedStatus READ conversationLastSavedStatus NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(QString conversationLastRestoredStatus READ conversationLastRestoredStatus NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(
        QString conversationBrowserStatus READ conversationBrowserStatus NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString conversationBrowserSummaryText READ conversationBrowserSummaryText NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(
        int conversationListEntryCount READ conversationListEntryCount NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString conversationListCurrentTitle READ conversationListCurrentTitle NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(int conversationListCurrentMessageCount READ conversationListCurrentMessageCount
                   NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString conversationListCurrentPersistenceStatus READ
                   conversationListCurrentPersistenceStatus NOTIFY chatMessagesChanged)
    Q_PROPERTY(QString conversationListCurrentLastUpdatedSummary READ
                   conversationListCurrentLastUpdatedSummary NOTIFY chatMessagesChanged)
    Q_PROPERTY(
        QString conversationListCurrentSearchAvailabilitySummary READ
            conversationListCurrentSearchAvailabilitySummary NOTIFY conversationSearchChanged)
    Q_PROPERTY(
        QString conversationListCurrentExportAvailabilitySummary READ
            conversationListCurrentExportAvailabilitySummary NOTIFY conversationExportChanged)
    Q_PROPERTY(QString conversationListCurrentSummary READ conversationListCurrentSummary NOTIFY
                   chatMessagesChanged)
    Q_PROPERTY(QString conversationCurrentStorageMode READ conversationCurrentStorageMode CONSTANT)
    Q_PROPERTY(QString conversationFutureStorageMode READ conversationFutureStorageMode CONSTANT)
    Q_PROPERTY(QString conversationMigrationReadiness READ conversationMigrationReadiness CONSTANT)
    Q_PROPERTY(
        QString conversationMigrationStatusSummary READ conversationMigrationStatusSummary CONSTANT)
    Q_PROPERTY(
        QString conversationSchemaStatusSummary READ conversationSchemaStatusSummary CONSTANT)
    Q_PROPERTY(QString conversationSearchQueryText READ conversationSearchQueryText NOTIFY
                   conversationSearchChanged)
    Q_PROPERTY(QString conversationSearchStatus READ conversationSearchStatus NOTIFY
                   conversationSearchChanged)
    Q_PROPERTY(QString conversationSearchSummaryText READ conversationSearchSummaryText NOTIFY
                   conversationSearchChanged)
    Q_PROPERTY(int conversationSearchResultCount READ conversationSearchResultCount NOTIFY
                   conversationSearchChanged)
    Q_PROPERTY(QStringList conversationSearchResultSummaries READ conversationSearchResultSummaries
                   NOTIFY conversationSearchChanged)
    Q_PROPERTY(bool conversationExportAvailable READ conversationExportAvailable CONSTANT)
    Q_PROPERTY(
        QString conversationExportReadinessStatus READ conversationExportReadinessStatus CONSTANT)
    Q_PROPERTY(
        QString conversationExportReadinessSummary READ conversationExportReadinessSummary CONSTANT)
    Q_PROPERTY(QStringList conversationExportReadinessChecks READ conversationExportReadinessChecks
                   CONSTANT)
    Q_PROPERTY(QString conversationExportLastResultSummary READ conversationExportLastResultSummary
                   NOTIFY conversationExportChanged)
    Q_PROPERTY(QString conversationExportLastStatus READ conversationExportLastStatus NOTIFY
                   conversationExportChanged)
    Q_PROPERTY(QString conversationExportLastFileName READ conversationExportLastFileName NOTIFY
                   conversationExportChanged)
    Q_PROPERTY(int conversationExportLastMessageCount READ conversationExportLastMessageCount NOTIFY
                   conversationExportChanged)
    Q_PROPERTY(QString conversationExportLastTimestamp READ conversationExportLastTimestamp NOTIFY
                   conversationExportChanged)
    Q_PROPERTY(QString conversationDuplicateLastStatus READ conversationDuplicateLastStatus NOTIFY
                   conversationDuplicateChanged)
    Q_PROPERTY(QString conversationDuplicateLastResultSummary READ
                   conversationDuplicateLastResultSummary NOTIFY conversationDuplicateChanged)
    Q_PROPERTY(bool conversationDeleteAvailable READ conversationDeleteAvailable NOTIFY
                   conversationDeleteChanged)
    Q_PROPERTY(QString conversationDeletePolicyStatus READ conversationDeletePolicyStatus NOTIFY
                   conversationDeleteChanged)
    Q_PROPERTY(QString conversationDeletePolicySummary READ conversationDeletePolicySummary NOTIFY
                   conversationDeleteChanged)
    Q_PROPERTY(QStringList conversationDeletePolicyRequirements READ
                   conversationDeletePolicyRequirements NOTIFY conversationDeleteChanged)
    Q_PROPERTY(QString conversationDeleteReadinessStatus READ conversationDeleteReadinessStatus
                   NOTIFY conversationDeleteChanged)
    Q_PROPERTY(QString conversationDeleteReadinessSummary READ conversationDeleteReadinessSummary
                   NOTIFY conversationDeleteChanged)
    Q_PROPERTY(QStringList conversationDeleteReadinessChecks READ conversationDeleteReadinessChecks
                   NOTIFY conversationDeleteChanged)
    Q_PROPERTY(QString conversationDeleteLastStatus READ conversationDeleteLastStatus NOTIFY
                   conversationDeleteChanged)
    Q_PROPERTY(QString conversationDeleteLastResultSummary READ conversationDeleteLastResultSummary
                   NOTIFY conversationDeleteChanged)
    Q_PROPERTY(int memoryCandidateCount READ memoryCandidateCount NOTIFY memoryCandidatesChanged)
    Q_PROPERTY(int pendingMemoryCandidateCount READ pendingMemoryCandidateCount NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(int approvedMemoryCandidateCount READ approvedMemoryCandidateCount NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(int rejectedMemoryCandidateCount READ rejectedMemoryCandidateCount NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(int archivedMemoryCandidateCount READ archivedMemoryCandidateCount NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(int committedMemoryCandidateCount READ committedMemoryCandidateCount NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(
        QStringList memoryCandidateIds READ memoryCandidateIds NOTIFY memoryCandidatesChanged)
    Q_PROPERTY(QStringList memoryCandidateReviewStates READ memoryCandidateReviewStates NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(QStringList memoryCandidateCommitStatuses READ memoryCandidateCommitStatuses NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(QStringList memoryCandidateSummaries READ memoryCandidateSummaries NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(QStringList pendingMemoryCandidateSummaries READ pendingMemoryCandidateSummaries
                   NOTIFY memoryCandidatesChanged)
    Q_PROPERTY(QStringList approvedMemoryCandidateSummaries READ approvedMemoryCandidateSummaries
                   NOTIFY memoryCandidatesChanged)
    Q_PROPERTY(QStringList rejectedMemoryCandidateSummaries READ rejectedMemoryCandidateSummaries
                   NOTIFY memoryCandidatesChanged)
    Q_PROPERTY(QStringList archivedMemoryCandidateSummaries READ archivedMemoryCandidateSummaries
                   NOTIFY memoryCandidatesChanged)
    Q_PROPERTY(QString lastMemoryCandidateReviewStatus READ lastMemoryCandidateReviewStatus NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(QString lastMemoryCandidateReviewSummary READ lastMemoryCandidateReviewSummary NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(QString memoryCommitReadinessStatus READ memoryCommitReadinessStatus NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(QString memoryCommitReadinessSummary READ memoryCommitReadinessSummary NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(QStringList memoryCommitReadinessChecks READ memoryCommitReadinessChecks NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(int memoryCommitPlanCount READ memoryCommitPlanCount NOTIFY memoryCandidatesChanged)
    Q_PROPERTY(QString memoryCommitTargetSummary READ memoryCommitTargetSummary NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(QStringList memoryCommitCandidateSummaries READ memoryCommitCandidateSummaries NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(
        QString lastMemoryCommitStatus READ lastMemoryCommitStatus NOTIFY memoryCandidatesChanged)
    Q_PROPERTY(QString lastMemoryCommitResultSummary READ lastMemoryCommitResultSummary NOTIFY
                   memoryCandidatesChanged)
    Q_PROPERTY(QString memoryRecallPolicyStatus READ memoryRecallPolicyStatus CONSTANT)
    Q_PROPERTY(QString memoryRecallPolicySummary READ memoryRecallPolicySummary CONSTANT)
    Q_PROPERTY(QString memoryRecallQueryText READ memoryRecallQueryText NOTIFY memoryRecallChanged)
    Q_PROPERTY(QString memoryRecallStatus READ memoryRecallStatus NOTIFY memoryRecallChanged)
    Q_PROPERTY(
        QString memoryRecallSummaryText READ memoryRecallSummaryText NOTIFY memoryRecallChanged)
    Q_PROPERTY(int memoryRecallResultCount READ memoryRecallResultCount NOTIFY memoryRecallChanged)
    Q_PROPERTY(QStringList memoryRecallResultSummaries READ memoryRecallResultSummaries NOTIFY
                   memoryRecallChanged)
    Q_PROPERTY(QString contextAssemblyPolicyStatus READ contextAssemblyPolicyStatus CONSTANT)
    Q_PROPERTY(QString contextAssemblyPolicySummary READ contextAssemblyPolicySummary CONSTANT)
    Q_PROPERTY(
        QString contextAssemblyStatus READ contextAssemblyStatus NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString contextAssemblySummaryText READ contextAssemblySummaryText NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int contextAssemblySourceCount READ contextAssemblySourceCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int contextAssemblyAvailableSourceCount READ contextAssemblyAvailableSourceCount
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int contextAssemblyCandidateBlockCount READ contextAssemblyCandidateBlockCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int contextAssemblyEstimatedSize READ contextAssemblyEstimatedSize NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString conversationContextAvailability READ conversationContextAvailability NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString committedMemoryContextAvailability READ committedMemoryContextAvailability
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString runtimeMetadataContextAvailability READ runtimeMetadataContextAvailability
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString orchestrationContextAvailability READ orchestrationContextAvailability NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QStringList contextAssemblySourceSummaries READ contextAssemblySourceSummaries NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QStringList contextAssemblyReadinessChecks READ contextAssemblyReadinessChecks NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int memoryEntryCount READ memoryEntryCount NOTIFY memoryEntriesChanged)
    Q_PROPERTY(QStringList memoryEntries READ memoryEntries NOTIFY memoryEntriesChanged)
    Q_PROPERTY(QString memoryMaintenanceStatus READ memoryMaintenanceStatus NOTIFY
                   maintenanceStatusChanged)
    Q_PROPERTY(
        QString chatMaintenanceStatus READ chatMaintenanceStatus NOTIFY maintenanceStatusChanged)

public:
    IAgentRuntime* agentRuntime() const {
        return agentRuntime_.get();
    }
    ChatModeService* chatModeService() const {
        return chatMode_.get();
    }
    QString activeAgentSessionId() const {
        return activeAgentSessionId_;
    }
    bool respondToAgentApproval(bool allow) {
        if (!agentAwaitingApproval())
            return false;
        resumeAgentLoopWithApproval(allow, false);
        return true;
    }
    const IAgentRunStore* agentRunStore() const {
        return agentRunStore_.get();
    }
    IAgentRunStore* mutableAgentRunStore() const { return agentRunStore_.get(); }
    IConversationStore* conversationStore() const { return conversationStore_.get(); }
    IChatHistoryStore* chatHistoryStore() const { return chatHistoryStore_.get(); }
    IMemoryStore* memoryStore() const { return memoryStore_.get(); }
    QVariantList persistentPermissionGrants() const;
    bool revokePersistentPermission(const QString& id);
    bool clearPersistentPermissions();
    ApplicationController(
        std::unique_ptr<IChatProvider> provider, std::unique_ptr<IMemoryStore> memoryStore,
        std::unique_ptr<ChatSession> chatSession = nullptr,
        std::unique_ptr<IChatHistoryStore> chatHistoryStore = nullptr,
        std::unique_ptr<IAgentRuntime> agentRuntime = nullptr,
        std::unique_ptr<IApprovalPolicy> approvalPolicy = nullptr,
        std::unique_ptr<ISandboxPolicy> sandboxPolicy = nullptr,
        std::unique_ptr<IToolExecutor> toolExecutor = nullptr,
        std::unique_ptr<IModelRouter> modelRouter = nullptr,
        std::unique_ptr<IProviderCatalog> providerCatalog = nullptr,
        std::unique_ptr<ITaskPlanner> taskPlanner = nullptr,
        std::unique_ptr<IAgentRegistry> agentRegistry = nullptr,
        std::unique_ptr<IMemoryCatalog> memoryCatalog = nullptr,
        std::unique_ptr<ILocalRuntime> localRuntime = nullptr,
        std::unique_ptr<ILocalRuntimeSessionManager> localRuntimeSessions = nullptr,
        std::unique_ptr<IRuntimeCapabilityRegistry> runtimeCapabilities = nullptr,
        std::unique_ptr<IRuntimePermissionPolicy> runtimePermissionPolicy = nullptr,
        std::unique_ptr<IRuntimeSafetyPolicy> runtimeSafetyPolicy = nullptr,
        std::unique_ptr<IRuntimePipeline> runtimePipeline = nullptr,
        std::unique_ptr<IExecutionLifecycle> executionLifecycle = nullptr,
        std::unique_ptr<ExecutionCoordinator> executionCoordinator = nullptr,
        std::unique_ptr<ILocalRuntimeAdapter> localRuntimeAdapter = nullptr,
        std::unique_ptr<IProviderRuntimeBridge> providerRuntimeBridge = nullptr,
        std::unique_ptr<StaticRuntimeIntegrationReadiness> runtimeIntegrationReadiness = nullptr,
        std::unique_ptr<IOllamaRuntimeClient> ollamaRuntimeClient = nullptr,
        std::unique_ptr<ILocalInferenceClient> localInferenceClient = nullptr,
        std::unique_ptr<ILocalInferenceStreamClient> localInferenceStreamClient = nullptr,
        std::unique_ptr<IModelManagementService> modelManagementService = nullptr,
        std::unique_ptr<ITextToSpeechProvider> textToSpeechProvider = nullptr,
        std::unique_ptr<ISpeechToTextProvider> speechToTextProvider = nullptr,
        std::unique_ptr<IVoiceRuntimeCoordinator> voiceRuntimeCoordinator = nullptr,
        std::unique_ptr<IVoiceRuntimeEnvironment> voiceRuntimeEnvironment = nullptr,
        std::unique_ptr<PiperTextToSpeechProvider> piperTextToSpeechProvider = nullptr,
        std::unique_ptr<ILocalInferenceWorker> localInferenceWorker = nullptr,
        std::unique_ptr<IConversationStore> conversationStore = nullptr,
        std::unique_ptr<IAgentTaskRuntime> agentTaskRuntime = nullptr,
        std::unique_ptr<IAgentStepPlanner> agentStepPlanner = nullptr,
        std::unique_ptr<ModelService> modelService = nullptr, QObject* parent = nullptr);
    ~ApplicationController() override;

    ModelLibraryService* modelLibrary() const { return modelLibrary_.get(); }
    ModelOperationService* modelOperations() const { return modelOperations_.get(); }
    VoiceSessionService* audioSession() const { return audioSession_; }
    void configureSpeechTts(const QString& engine, const QString& kokoroModelPath,
                            const QString& kokoroVoice);

    QString providerName() const;
    QString providerStatus() const;
    QString providerCatalogStatus(const QString& providerId, const QString& modelId) const;
    QString providerCatalogDetail(const QString& providerId, const QString& modelId) const;
    QString agentStatus() const;
    QString lastAgentResponse() const;
    QString latestToolPlanStatus() const;
    QString latestToolPlanSummary() const;
    QString latestApprovalStatus() const;
    QString latestApprovalSummary() const;
    QString latestSandboxStatus() const;
    QString latestSandboxSummary() const;
    QString latestToolExecutionStatus() const;
    QString latestToolExecutionSummary() const;
    QString latestAgentPipelineStatus() const;
    QString latestAgentPipelineSummary() const;
    QString runtimeSessionId() const;
    QString runtimeContextStatus() const;
    QString runtimeContextSummary() const;
    QStringList runtimeContextActiveToolIds() const;
    const ConversationSession& currentConversationSession() const;
    QString conversationSessionId() const;
    QString conversationSessionStatus() const;
    QString interactionMode() const;
    QString attentionState() const;
    QString contextWindowSummary() const;
    QString conversationState() const;
    QString conversationTransitionStatus() const;
    QString conversationTransitionSummary() const;
    QString conversationRuntimeSummary() const;
    QStringList conversationRuntimeSummaryLines() const;
    QString conversationRuntimeRequestId() const;
    QString conversationRuntimeActiveModel() const;
    QString conversationRuntimeActiveRoute() const;
    bool conversationRuntimeStreaming() const;
    QString conversationRuntimeLastSuccessSummary() const;
    QString conversationRuntimeLastErrorSummary() const;
    QString conversationRuntimeLastLatencySummary() const;
    int agentActivityCount() const;
    QString latestAgentActivitySummary() const;
    QString currentRoutingMode() const;
    void setRoutingModeByName(const QString& routingModeName);
    QString modelRoutingStatus() const;
    QString selectedModelProviderSummary() const;
    QString latestTaskPlanStatus() const;
    QString latestTaskPlanSummary() const;
    int plannedTaskStepCount() const;
    int registeredAgentCount() const;
    QStringList activeAgentSummaries() const;
    QString currentAgentSummary() const;
    QString currentMemoryAffinitySummary() const;
    int providerCatalogCount() const;
    QStringList providerCatalogSummaries() const;
    int memoryCatalogCount() const;
    QStringList memoryCatalogSummaries() const;
    OrchestrationSnapshot currentOrchestrationSnapshot() const;
    QString orchestrationSnapshotStatus() const;
    QString orchestrationSnapshotSummary() const;
    QStringList orchestrationSignals() const;
    OrchestrationReadinessReport currentOrchestrationReadinessReport() const;
    QString orchestrationReadinessStatus() const;
    QString orchestrationReadinessSummary() const;
    QStringList orchestrationDiagnostics() const;
    QString agentTaskRuntimeStatus() const;
    QString agentTaskRuntimeSummary() const;
    int agentTaskRuntimeTaskCount() const;
    int agentTaskQueueCount() const;
    int agentTaskQueueActiveCount() const;
    int agentTaskQueuePlannedCount() const;
    int agentTaskQueueBlockedCount() const;
    int agentTaskQueueCompletedCount() const;
    int agentTaskQueueRefusedCount() const;
    QString latestAgentTaskSummary() const;
    QString latestAgentTaskLifecycleSummary() const;
    QStringList agentTaskQueueSummaries() const;
    QStringList agentTaskTraceSummaries() const;
    QString agentPlanningSessionStatus() const;
    QString agentPlanningSessionSummary() const;
    int agentPlanningCandidateCount() const;
    int agentPlanningRefusedCount() const;
    QStringList agentPlanningCandidateSummaries() const;
    QStringList agentPlanningArbitrationSummaries() const;
    QStringList agentPlanningRefusalSummaries() const;
    QString agentPlanningFallbackSummary() const;
    QString agentCapabilityRegistryStatus() const;
    QString agentCapabilityRegistrySummary() const;
    int agentCapabilityCount() const;
    int agentCapabilityEnabledCount() const;
    int agentCapabilityDisabledCount() const;
    int agentCapabilityRestrictedCount() const;
    QStringList agentCapabilitySummaries() const;
    QStringList agentCapabilityReadinessSummaries() const;
    QStringList agentCapabilitySafetySummaries() const;
    QString toolContractRegistryStatus() const;
    QString toolContractRegistrySummary() const;
    int toolContractCount() const;
    int toolContractEnabledCount() const;
    int toolContractDisabledCount() const;
    int toolContractRestrictedCount() const;
    QStringList toolContractSummaries() const;
    QStringList toolContractPermissionSummaries() const;
    QStringList toolContractSandboxSummaries() const;
    QStringList toolContractReadinessSummaries() const;
    QStringList toolContractSafetySummaries() const;
    QString localRuntimeStatus() const;
    QString localRuntimeHealth() const;
    QString localRuntimeSummary() const;
    QStringList localRuntimeCapabilities() const;
    QString localRuntimeResponseStatus() const;
    QString localRuntimeResponseSummary() const;
    int localRuntimeSessionCount() const;
    QString localRuntimeSessionStatus() const;
    QString localRuntimeSessionHealth() const;
    QString localRuntimeSessionSummary() const;
    QString localRuntimeAllocationSummary() const;
    QString localRuntimeReservationSummary() const;
    QStringList localRuntimeSessionSummaries() const;
    int runtimeCapabilityCount() const;
    QStringList enabledRuntimeCapabilitySummaries() const;
    QStringList disabledRuntimeCapabilitySummaries() const;
    QString runtimeNegotiationProfileSummary() const;
    QString runtimeNegotiationSummary() const;
    QString localOnlyRuntimeEnforcementSummary() const;
    QString runtimePermissionDecision() const;
    QString runtimePermissionSummary() const;
    QString runtimeSafetyDecision() const;
    QString runtimeSafetySummary() const;
    QString runtimePipelineStatus() const;
    QString runtimePipelineSummary() const;
    QStringList runtimePipelineTraceSummaries() const;
    QString executionLifecycleState() const;
    QString executionLifecycleStatus() const;
    QString executionLifecycleSummary() const;
    QStringList executionLifecycleTraceSummaries() const;
    QString executionSessionId() const;
    QString executionSessionStatus() const;
    QString executionSessionOwnership() const;
    QString executionCoordinationMode() const;
    QString executionSessionSummary() const;
    QString executionCoordinationSnapshotSummary() const;
    QString localRuntimeAdapterStatus() const;
    QString localRuntimeAdapterHealth() const;
    QString localRuntimeAdapterSummary() const;
    QStringList localRuntimeAdapterCapabilitySummaries() const;
    QString providerRuntimeBridgeStatus() const;
    QString providerRuntimeBridgeSummary() const;
    QString providerRuntimeBridgeResponseSummary() const;
    QString runtimeIntegrationReadinessStatus() const;
    QString runtimeIntegrationReadinessSummary() const;
    QStringList runtimeIntegrationReadinessChecks() const;
    QString selectedRuntimeProvider() const;
    void setSelectedRuntimeProvider(const QString& providerId);
    QString activeRuntimeProviderId() const;
    QString activeRuntimeProviderLabel() const;
    QString activeRuntimeModelLabel() const;
    QString activeRuntimeReadinessState() const;
    QString activeRuntimeReadinessSummary() const;
    QString activeRuntimeLocalOnlySummary() const;
    QStringList selectableRuntimeProviderIds() const;
    QStringList selectableRuntimeProviderLabels() const;
    QStringList runtimeProviderCardSummaries() const;
    QStringList runtimeProviderCapabilitySummaries() const;
    QStringList runtimeProviderValidationTraces() const;
    QStringList installedRuntimeProviderSummaries() const;
    QStringList configuredRuntimeProviderSummaries() const;
    QStringList availableLocalRuntimeSummaries() const;
    QString providerCredentialRegistryStatus() const;
    QString providerCredentialRegistrySummary() const;
    QStringList providerCredentialSummaries() const;
    QStringList providerCredentialReadinessSummaries() const;
    QStringList providerCredentialSafetySummaries() const;
    QString credentialStoreSummary() const;
    QString credentialStoreBackendSummary() const;
    QString credentialStoreSafetySummary() const;
    QStringList credentialStoreTraceSummaries() const;
    QString credentialActionReadiness() const;
    QString credentialExecutionStatus() const;
    QString ollamaEndpoint() const;
    void setOllamaEndpoint(const QString& endpoint);
    QString lmStudioEndpoint() const;
    void setLmStudioEndpoint(const QString& endpoint);
    QString llamaCppEndpoint() const;
    void setLlamaCppEndpoint(const QString& endpoint);
    void configureWebSearch(const QString& provider, const QString& apiKey, int maxResults);
    void configureMcpServers(const QString& serversJson);
    // Triggers an immediate model discovery pass (used when credentials or
    // cloud provider selection change).
    void refreshModelDiscovery();
    QString cloudModelDiscoveryError() const;
    void setSemanticProvider(const QString& provider, const QString& embeddingModel);
    QString ollamaConnectionStatus() const;
    QString ollamaHealthStatus() const;
    QString ollamaHealthSummary() const;
    QString ollamaDiscoveryStatus() const;
    int ollamaModelCount() const;
    QStringList ollamaModelNames() const;
    QStringList installedOllamaModelNames() const;
    QStringList loadedLMStudioModelNames() const;
    QStringList ollamaModelSummaries() const;
    QList<OllamaModelSummary> currentOllamaModels() const;
    OllamaModelDiscoveryResult ollamaDiscovery() const;
    QString selectedLocalModel() const;
    void setSelectedLocalModel(const QString& model);
    QString selectedLocalModelStatus() const;
    QString selectedLocalModelSummary() const;
    QString selectedLocalModelMetadataSummary() const;
    QString activeLocalRuntimeBadge() const;
    QString modelRegistryStatus() const;
    QString modelRegistrySummary() const;
    QStringList modelRegistryModelSummaries() const;
    QStringList modelLibraryInstalledSummaries() const;
    QStringList modelLibraryAvailableSummaries() const;
    QStringList modelLibraryRecommendedSummaries() const;
    QStringList modelLibraryDetailSummaries() const;
    QStringList providerDiscoverySummaries() const;
    QStringList modelAdvisorRecommendationSummaries() const;
    QStringList modelAdvisorAvoidSummaries() const;
    QStringList downloadsCenterSummaries() const;
    QStringList benchmarkHubSummaries() const;
    QStringList selectedModelCapabilityLabels() const;
    QVariantList modelCapabilitySettings() const;
    QString modelCapabilityCompatibilitySummary() const;
    bool setModelCapabilityOverride(const QString& capabilityId, const QString& value);
    bool setModelCapabilityNumberOverride(const QString& capabilityId, int value);
    void resetModelCapabilityOverrides();
    QString modelManagementStatus() const;
    QString modelManagementSummary() const;
    QString modelManagementActionAvailability() const;
    QStringList modelRecommendationSummaries() const;
    QStringList modelRequirementSummaries() const;
    QString voiceRuntimeMode() const;
    bool voiceEnabled() const;
    VoiceReadinessReport currentVoiceReadinessReport() const;
    QString voiceReadinessStatus() const;
    QString voiceReadinessSummary() const;
    QStringList voiceReadinessChecks() const;
    QStringList voiceCapabilitySummaries() const;
    QString textToSpeechStatus() const;
    QString textToSpeechSummary() const;
    QString speechToTextStatus() const;
    QString speechToTextSummary() const;
    VoiceSession currentVoiceSession() const;
    VoicePipelineResult currentVoicePipelineResult() const;
    VoicePipelineSessionResult currentVoicePipelineSessionResult() const;
    AudioFileSessionResult currentAudioFileSessionResult() const;
    VoiceRuntimeSummary currentVoiceRuntimeSummary() const;
    QString voiceSessionId() const;
    QString voiceSessionStatus() const;
    QString voiceSessionSummary() const;
    QString voicePipelineStatus() const;
    QString voicePipelineSummary() const;
    QStringList voicePipelineTraceSummaries() const;
    QString voicePipelineSessionStatus() const;
    QString voicePipelineSessionSummary() const;
    QStringList voicePipelineSessionStageReadinessSummaries() const;
    QStringList voicePipelineSessionTraceSummaries() const;
    QString voicePipelineSessionFallbackSummary() const;
    QString voicePipelineSessionSafetySummary() const;
    QStringList voicePipelineSessionSafetyChecks() const;
    int voicePipelineSessionReadyStageCount() const;
    int voicePipelineSessionBlockedStageCount() const;
    QString audioFileSessionStatus() const;
    QString audioFileSessionSummary() const;
    QString audioFileSessionReadinessSummary() const;
    QStringList audioFileValidationSummaries() const;
    QStringList audioFileSupportedExtensionSummaries() const;
    QString audioFileSessionFallbackSummary() const;
    QString audioFileSessionSafetySummary() const;
    QStringList audioFileSessionSafetyChecks() const;
    QStringList audioFileSessionRefusalSummaries() const;
    QStringList audioFileTraceSummaries() const;
    QString voiceRuntimeStatus() const;
    QString voiceRuntimeSummary() const;
    QStringList voiceRuntimeCheckSummaries() const;
    bool voiceRuntimeAvailable() const;
    bool voiceTextToSpeechAvailable() const;
    bool voiceSpeechToTextAvailable() const;
    bool voiceMicrophoneEnabled() const;
    bool voicePlaybackEnabled() const;
    bool voiceLocalOnlyPolicy() const;
    bool voiceProcessExecutionEnabled() const;
    QString voiceRuntimeEnvironmentStatus() const;
    QString voiceRuntimeEnvironmentSummary() const;
    QStringList voiceBinarySummaries() const;
    QStringList voiceModelSummaries() const;
    QStringList voiceRuntimePermissionSummaries() const;
    QString voiceRuntimeSafetyStatus() const;
    QString voiceRuntimeSafetySummary() const;
    QStringList voiceRuntimeSafetyChecks() const;
    bool voiceRuntimeExecutionAllowed() const;
    QString piperTtsStatus() const;
    QString piperTtsSummary() const;
    QStringList piperTtsReadinessChecks() const;
    bool piperTtsReady() const;
    QString piperTtsFileOutputStatus() const;
    QString piperTtsFileOutputSummary() const;
    QString piperSynthesisStatus() const;
    QString piperSynthesisReadinessSummary() const;
    QString piperSynthesisLastSummary() const;
    QString piperSynthesisFallbackSummary() const;
    QString piperSynthesisSafetySummary() const;
    QStringList piperSynthesisTraceSummaries() const;
    QString piperBinaryPath() const;
    void setPiperBinaryPath(const QString& path);
    QString piperModelPath() const;
    void setPiperModelPath(const QString& path);
    QString whisperBinaryPath() const;
    void setWhisperBinaryPath(const QString& path);
    QString whisperModelPath() const;
    void setWhisperModelPath(const QString& path);
    QStringList voiceConfigurationSummaries() const;
    QString voiceConfigurationReadinessSummary() const;
    QStringList voiceConfigurationStatusBadges() const;
    QStringList voiceConfigurationHintSummaries() const;
    QStringList voiceConfigurationValidationSummaries() const;
    QString piperFileOutputReadinessStatus() const;
    QString piperFileOutputReadinessSummary() const;
    bool piperFileOutputExecutionEnabled() const;
    void setPiperFileOutputExecutionEnabled(bool enabled);
    bool whisperTranscriptionExecutionEnabled() const;
    void setWhisperTranscriptionExecutionEnabled(bool enabled);
    QString piperFileOutputExecutionStatus() const;
    QString piperFileOutputExecutionSummary() const;
    QString piperFileOutputAudioPathSummary() const;
    QString whisperPreparationReadinessStatus() const;
    QString whisperPreparationReadinessSummary() const;
    QString voiceRuntimeReadinessSummary() const;
    QString voiceRuntimeHealth() const;
    int voiceRuntimeConfiguredCount() const;
    int voiceRuntimeMissingCount() const;
    int voiceRuntimeRefusedCount() const;
    QString voiceRuntimePermissionFoundationSummary() const;
    QString voiceRuntimeSandboxSummary() const;
    QString voiceRuntimeSafetyReportSummary() const;
    QStringList voiceRuntimeReadinessChecks() const;
    QString whisperRuntimeStatus() const;
    QString whisperRuntimeReadinessSummary() const;
    QString whisperRuntimePathSummary() const;
    QString whisperTranscriptionStatus() const;
    QString whisperTranscriptionReadinessSummary() const;
    QString whisperTranscriptionLastSummary() const;
    QString whisperTranscriptionFallbackSummary() const;
    QString whisperTranscriptionSafetySummary() const;
    QStringList whisperTranscriptionTraceSummaries() const;
    QString piperRuntimeStatus() const;
    QString piperRuntimeReadinessSummary() const;
    QString piperRuntimePathSummary() const;
    bool localChatInferenceEnabled() const;
    void setLocalChatInferenceEnabled(bool enabled);
    QString localChatInferenceStatus() const;
    QString localChatInferenceSummary() const;
    bool localChatSendAvailable() const;
    QString localChatSendAvailabilitySummary() const;
    QString chatSendLifecycleState() const;
    QString chatSendLifecycleSummary() const;
    bool chatGenerationActive() const;
    QString activeChatProviderId() const;
    QString activeChatModelId() const;
    QString chatErrorCategory() const;
    QString chatCatalogState() const;
    QString chatProviderKind() const;
    bool chatAttachmentInputAvailable() const;
    QVariantMap chatInputCapabilities() const;
    bool promptContextInjectionEnabled() const;
    void setPromptContextInjectionEnabled(bool enabled);
    PromptContextInjectionResult latestPromptContextInjectionResult() const;
    bool semanticPromptInclusionEnabled() const;
    void setSemanticPromptInclusionEnabled(bool enabled);
    SemanticPromptInclusionPolicy semanticPromptInclusionPolicy() const;
    SemanticPromptInclusionResult latestSemanticPromptInclusionResult() const;
    QString promptContextInjectionStatus() const;
    QString promptContextInjectionSummary() const;
    int promptContextInjectedBlockCount() const;
    QString promptContextSourceSummary() const;
    QString promptContextSizeSummary() const;
    QString promptContextUsedSummary() const;
    int promptContextUsedMemoryCount() const;
    QString contextBudgetUsageSummary() const;
    int contextIncludedCandidateCount() const;
    int contextExcludedCandidateCount() const;
    QStringList contextAssemblyTraceSummaries() const;
    QStringList promptContextBlockSummaries() const;
    ConversationWindowPolicy conversationWindowPolicy() const;
    ConversationWindowResult conversationWindowResult() const;
    QString conversationWindowStatus() const;
    QString conversationWindowSummary() const;
    QString conversationWindowBudgetSummary() const;
    int conversationWindowBudgetCharacters() const;
    int conversationWindowIncludedMessageCount() const;
    int conversationWindowTruncatedMessageCount() const;
    int conversationWindowOmittedMessageCount() const;
    ConversationSummaryPolicy conversationSummaryPolicy() const;
    ConversationSummaryResult conversationSummaryResult() const;
    QString conversationSummaryStatus() const;
    QString conversationSummaryText() const;
    QString conversationSummaryBudgetSummary() const;
    int conversationSummaryBudgetCharacters() const;
    int conversationSummaryBlockCount() const;
    int conversationSummaryMessageCount() const;
    int conversationSummaryOmittedMessageCount() const;
    int conversationSummaryTruncatedBlockCount() const;
    QStringList conversationSummaryBlockSummaries() const;
    RetrievalPlanningPolicy retrievalPlanningPolicy() const;
    RetrievalPlanningResult retrievalPlanningResult() const;
    QString retrievalPlanningStatus() const;
    QString retrievalPlanningSummary() const;
    QString retrievalPlanningReadiness() const;
    QString retrievalPlanningBudgetSummary() const;
    QString retrievalPlanningSourceSummary() const;
    int retrievalPlanningSelectedSourceCount() const;
    int retrievalPlanningExcludedSourceCount() const;
    int retrievalPlanningSelectedCandidateCount() const;
    int retrievalPlanningExcludedCandidateCount() const;
    int retrievalPlanningTruncatedCandidateCount() const;
    QStringList retrievalPlanningSourceSummaries() const;
    QString memoryRelevanceSummaryText() const;
    QString memoryRelevanceBudgetSummary() const;
    int memoryRelevanceIncludedCount() const;
    int memoryRelevanceExcludedCount() const;
    QStringList memoryRelevanceTraceSummaries() const;
    QStringList memoryRelevanceExclusionSummaries() const;
    QString conversationSalienceSummaryText() const;
    QString conversationSalienceBudgetSummary() const;
    QString conversationSalienceAllocationSummary() const;
    int conversationSalienceIncludedCount() const;
    int conversationSalienceExcludedCount() const;
    int conversationSalienceTruncatedCount() const;
    QStringList conversationSalienceTraceSummaries() const;
    QStringList conversationSalienceExclusionSummaries() const;
    ConversationCompressionSummary conversationCompressionSummary() const;
    QString conversationCompressionStatus() const;
    QString conversationCompressionReadinessSummary() const;
    QString conversationCompressionPressureSummary() const;
    int conversationCompressionCandidateCount() const;
    int conversationCompressionSelectedCandidateCount() const;
    QString conversationCompressionFallbackReason() const;
    QString conversationCompressionTraceSummary() const;
    QString conversationCompressionBudgetSummary() const;
    QStringList conversationCompressionCandidateSummaries() const;
    QStringList conversationCompressionTraceSummaries() const;
    ConversationSummaryResult conversationSummaryGenerationResult() const;
    bool conversationSummaryAvailable() const;
    QString conversationSummaryGenerationStatus() const;
    QString conversationSummaryReadinessSummary() const;
    QString conversationSummaryBlockedReason() const;
    QString conversationSummaryEstimatedCompressionGain() const;
    QString conversationSummaryPreviewSummary() const;
    QString conversationSummaryPersistenceSummary() const;
    QString conversationSummaryInjectionSummary() const;
    QString summaryContinuityStatus() const;
    QString summaryContinuityFreshnessSummary() const;
    QString summaryContinuityCoverageSummary() const;
    QString summaryContinuityContributionSummary() const;
    QString summaryContinuityFallbackSummary() const;
    QString summaryContinuityOrderingSummary() const;
    QString summaryContinuityBudgetTrace() const;
    ContextDecisionSummary contextDecisionSummary() const;
    bool contextExplainabilityEnabled() const;
    QString contextReasoningSummary() const;
    QString contextReasoningBudgetSummary() const;
    QString contextReasoningOrderingSummary() const;
    QString contextReasoningFallbackSummary() const;
    QStringList contextReasoningContributionSummaries() const;
    QStringList contextReasoningInclusionHints() const;
    QStringList contextReasoningExclusionHints() const;
    QStringList contextReasoningDeveloperTraces() const;
    QStringList conversationSummaryCandidateSegments() const;
    QStringList conversationSummaryGenerationTraceSummaries() const;
    SemanticRetrievalPolicy semanticRetrievalPolicy() const;
    bool semanticRetrievalEnabled() const;
    QString semanticRetrievalStatus() const;
    QString semanticRetrievalSummary() const;
    QString semanticReadiness() const;
    QString embeddingProviderReadiness() const;
    QString embeddingProviderSummary() const;
    QString vectorIndexReadiness() const;
    QString vectorIndexSummary() const;
    int vectorIndexedItemCount() const;
    SemanticProviderPolicy semanticProviderPolicy() const;
    SemanticProviderSelection semanticProviderSelection() const;
    SemanticActivationReadiness semanticActivationReadinessResult() const;
    QString semanticProviderMode() const;
    QString selectedSemanticProviderName() const;
    QString semanticProviderReadiness() const;
    QString semanticProviderHealth() const;
    QString semanticProviderStatusSummary() const;
    QString semanticActivationReadiness() const;
    QString semanticActivationSummary() const;
    QStringList semanticProviderCapabilitySummaries() const;
    QStringList semanticActivationRequiredSteps() const;
    QStringList semanticRetrievalReadinessChecks() const;
    SemanticCandidatePolicy semanticCandidatePolicy() const;
    SemanticCandidateArbitration semanticCandidateArbitration() const;
    QString semanticCandidateStatus() const;
    QString semanticCandidateSummary() const;
    QString semanticCandidateBudgetSummary() const;
    QString semanticCandidateArbitrationSummary() const;
    int semanticCandidateCount() const;
    int semanticCandidateSelectedCount() const;
    int semanticCandidateExcludedCount() const;
    int semanticCandidateTruncatedCount() const;
    QStringList semanticCandidateParticipationSummaries() const;
    HybridRetrievalPolicy hybridRetrievalPolicy() const;
    HybridRetrievalReadiness hybridRetrievalReadinessResult() const;
    QString hybridRetrievalStatus() const;
    QString hybridRetrievalReadiness() const;
    QString hybridRetrievalSummary() const;
    QStringList hybridRetrievalReadinessChecks() const;
    SemanticArbitrationPolicy semanticArbitrationPolicy() const;
    SemanticArbitrationResult semanticArbitrationResult() const;
    QString semanticArbitrationStatus() const;
    QString semanticArbitrationReadiness() const;
    QString semanticArbitrationSummary() const;
    QString semanticArbitrationBudgetSummary() const;
    QStringList semanticArbitrationSelectionSummaries() const;
    QStringList semanticArbitrationChecks() const;
    EmbeddingRuntimePlan embeddingRuntimePlanResult() const;
    QString embeddingRuntimeReadiness() const;
    QString embeddingRuntimeSummary() const;
    QString embeddingRuntimeBudgetSummary() const;
    QStringList embeddingRuntimeRequirementSummaries() const;
    QStringList embeddingRuntimeConstraintSummaries() const;
    EmbeddingGenerationResult isolatedEmbeddingRuntimeResult() const;
    QString isolatedEmbeddingRuntimeStatus() const;
    QString isolatedEmbeddingRuntimeHealth() const;
    QString isolatedEmbeddingRuntimeReadiness() const;
    QString isolatedEmbeddingRuntimeSummary() const;
    QString isolatedEmbeddingRuntimeBoundedState() const;
    QStringList isolatedEmbeddingRuntimeChecks() const;
    VectorPersistencePolicy vectorPersistencePolicy() const;
    VectorIndexSnapshotSummary vectorIndexSnapshotSummary() const;
    QString vectorPersistenceStatus() const;
    QString vectorPersistenceHealth() const;
    QString vectorPersistenceReadiness() const;
    QString vectorPersistenceSummary() const;
    QString vectorPersistenceBoundedState() const;
    int vectorPersistenceIndexedItemCount() const;
    QStringList vectorPersistenceChecks() const;
    SemanticSearchPolicy semanticSearchPolicy() const;
    SemanticSearchResult semanticSearchResult() const;
    QString semanticSearchStatus() const;
    QString semanticSearchReadiness() const;
    QString semanticSearchSummary() const;
    QString semanticSearchBudgetSummary() const;
    QString semanticSearchRuntimeState() const;
    int semanticSearchCandidateCount() const;
    QString semanticSearchArbitrationSummary() const;
    QStringList semanticSearchCandidateSummaries() const;
    QStringList semanticSearchChecks() const;
    HybridRetrievalBridgePolicy hybridRetrievalBridgePolicy() const;
    HybridRetrievalBridgeResult hybridRetrievalBridgeResult() const;
    SemanticAcceptancePolicy semanticAcceptancePolicy() const;
    SemanticAcceptanceResult semanticAcceptanceResult() const;
    SemanticSupplementAssemblyPolicy semanticSupplementAssemblyPolicy() const;
    SemanticSupplementAssemblyResult semanticSupplementAssemblyResult() const;
    SemanticPromptAuthorityPolicy semanticPromptAuthorityPolicy() const;
    SemanticPromptAuthorityResult semanticPromptAuthorityResult() const;
    QString semanticAcceptanceStatus() const;
    QString semanticAcceptanceReadiness() const;
    QString semanticAcceptanceSummary() const;
    QString semanticAcceptanceBudgetSummary() const;
    QString semanticAcceptanceSourceSummary() const;
    QString semanticAcceptanceArbitrationSummary() const;
    QString semanticAcceptanceFallbackSummary() const;
    int semanticAcceptanceAcceptedCount() const;
    int semanticAcceptanceBudgetCharacters() const;
    QStringList semanticAcceptanceCandidateSummaries() const;
    QStringList semanticAcceptanceChecks() const;
    QString semanticSupplementAssemblyStatus() const;
    QString semanticSupplementAssemblyReadiness() const;
    QString semanticSupplementAssemblySummary() const;
    QString semanticSupplementAssemblyBudgetSummary() const;
    QString semanticSupplementAssemblySafetySummary() const;
    int semanticSupplementAssemblyBlockCount() const;
    int semanticSupplementAssemblyBudgetCharacters() const;
    QStringList semanticSupplementAssemblyChecks() const;
    QString semanticPromptAuthorityStatus() const;
    QString semanticPromptAuthorityDecisionSummary() const;
    QString semanticPromptAuthoritySafetySummary() const;
    QString semanticPromptAuthorityReadinessSummary() const;
    QString semanticPromptAuthorityFallbackSummary() const;
    QString semanticPromptAuthorityAuditSummary() const;
    int semanticPromptAuthorityWouldIncludeBlockCount() const;
    QStringList semanticPromptAuthorityChecks() const;
    QString semanticPromptInclusionStatus() const;
    QString semanticPromptInclusionSummary() const;
    int semanticPromptInclusionIncludedCount() const;
    QString semanticPromptInclusionBudgetSummary() const;
    QString semanticPromptInclusionFallbackSummary() const;
    QString semanticPromptInclusionAuditSummary() const;
    bool semanticPromptInclusionDeterministicAuthorityPreserved() const;
    QStringList semanticPromptInclusionChecks() const;
    QString hybridBridgeStatus() const;
    QString hybridBridgeReadiness() const;
    QString hybridBridgeSummary() const;
    QString hybridBridgeBudgetSummary() const;
    QString hybridBridgeSourceSummary() const;
    QString hybridBridgeArbitrationSummary() const;
    QString hybridBridgeFallbackSummary() const;
    int hybridBridgeCandidateCount() const;
    int hybridBridgeSemanticFillCount() const;
    QStringList hybridBridgeCandidateSummaries() const;
    QStringList hybridBridgeChecks() const;
    bool localInferenceStreamingEnabled() const;
    void setLocalInferenceStreamingEnabled(bool enabled);
    int localInferenceTimeoutMs() const;
    void setLocalInferenceTimeoutMs(int timeoutMs);
    double localInferenceTemperature() const;
    void setLocalInferenceTemperature(double temperature);
    double localInferenceTopP() const;
    void setLocalInferenceTopP(double topP);
    int localInferenceMaxTokens() const;
    void setLocalInferenceMaxTokens(int maxTokens);
    bool localInferenceBusy() const;
    QString localInferenceRuntimeState() const;
    QString localInferenceStatus() const;
    QString localInferenceSummary() const;
    QString localInferenceLastResponseSummary() const;
    QString localInferenceLatencySummary() const;
    QStringList localInferenceTraceSummaries() const;
    bool localInferenceStreamingAvailable() const;
    QString localInferenceStreamStatus() const;
    QString localInferenceStreamSummary() const;
    QString localInferenceStreamingText() const;
    int availableToolCount() const;
    QStringList availableToolIds() const;
    QString memoryStatus() const;
    QString chatHistoryStatus() const;
    QString conversationStoreStatus() const;
    int conversationStoreConversationCount() const;
    QString activeConversationSummary() const;
    QStringList conversationStoreSummaries() const;
    QString activeConversationId() const;
    bool activeConversationArchived() const;
    QString activeConversationStateSummary() const;
    QStringList conversationIds() const;
    QStringList conversationTitles() const;
    QStringList conversationActiveSummaries() const;
    QStringList conversationLastUpdatedSummaries() const;
    QStringList conversationMessageCountSummaries() const;
    QStringList conversationArchivedSummaries() const;
    QStringList conversationPinnedSummaries() const;
    int activeConversationCount() const;
    int archivedConversationCount() const;
    int userCreatedConversationCount() const;
    bool conversationBrowserEmptyStateVisible() const;
    QString conversationBrowserEmptyStateSummary() const;
    ConversationHistorySummary conversationHistorySummary() const;
    QString conversationHistorySummaryText() const;
    QStringList conversationHistorySummaryLines() const;
    int conversationHistoryMessageCount() const;
    QString conversationPersistenceStatus() const;
    QString conversationLastSavedStatus() const;
    QString conversationLastRestoredStatus() const;
    ConversationId currentConversationId() const;
    ConversationDescriptor currentConversationDescriptor() const;
    ConversationDisplayTitle currentConversationDisplayTitle() const;
    ConversationListEntry currentConversationListEntry() const;
    ConversationListSummary conversationListSummary() const;
    QString conversationBrowserStatus() const;
    QString conversationBrowserSummaryText() const;
    int conversationListEntryCount() const;
    QString conversationListCurrentTitle() const;
    int conversationListCurrentMessageCount() const;
    QString conversationListCurrentPersistenceStatus() const;
    QString conversationListCurrentLastUpdatedSummary() const;
    QString conversationListCurrentSearchAvailabilitySummary() const;
    QString conversationListCurrentExportAvailabilitySummary() const;
    QString conversationListCurrentSummary() const;
    ConversationSchemaPlan conversationSchemaPlan() const;
    QString conversationCurrentStorageMode() const;
    QString conversationFutureStorageMode() const;
    QString conversationMigrationReadiness() const;
    QString conversationMigrationStatusSummary() const;
    QString conversationSchemaStatusSummary() const;
    ConversationSearchSummary conversationSearchSummary() const;
    QString conversationSearchQueryText() const;
    QString conversationSearchStatus() const;
    QString conversationSearchSummaryText() const;
    int conversationSearchResultCount() const;
    QStringList conversationSearchResultSummaries() const;
    ConversationExportReadiness conversationExportReadiness() const;
    ConversationExportResult latestConversationExportResult() const;
    ConversationDuplicateResult latestConversationDuplicateResult() const;
    bool conversationExportAvailable() const;
    QString conversationExportReadinessStatus() const;
    QString conversationExportReadinessSummary() const;
    QStringList conversationExportReadinessChecks() const;
    QString conversationExportLastResultSummary() const;
    QString conversationExportLastStatus() const;
    QString conversationExportLastFileName() const;
    int conversationExportLastMessageCount() const;
    QString conversationExportLastTimestamp() const;
    QString conversationDuplicateLastStatus() const;
    QString conversationDuplicateLastResultSummary() const;
    ConversationDeletePolicy conversationDeletePolicy() const;
    ConversationDeleteReadiness conversationDeleteReadiness() const;
    ConversationDeleteResult latestConversationDeleteResult() const;
    bool conversationDeleteAvailable() const;
    QString conversationDeletePolicyStatus() const;
    QString conversationDeletePolicySummary() const;
    QStringList conversationDeletePolicyRequirements() const;
    QString conversationDeleteReadinessStatus() const;
    QString conversationDeleteReadinessSummary() const;
    QStringList conversationDeleteReadinessChecks() const;
    QString conversationDeleteLastStatus() const;
    QString conversationDeleteLastResultSummary() const;
    int memoryCandidateCount() const;
    int pendingMemoryCandidateCount() const;
    int approvedMemoryCandidateCount() const;
    int rejectedMemoryCandidateCount() const;
    int archivedMemoryCandidateCount() const;
    int committedMemoryCandidateCount() const;
    QStringList memoryCandidateIds() const;
    QStringList memoryCandidateReviewStates() const;
    QStringList memoryCandidateCommitStatuses() const;
    QStringList memoryCandidateSummaries() const;
    QStringList pendingMemoryCandidateSummaries() const;
    QStringList approvedMemoryCandidateSummaries() const;
    QStringList rejectedMemoryCandidateSummaries() const;
    QStringList archivedMemoryCandidateSummaries() const;
    MemoryCandidateReviewResult latestMemoryCandidateReviewResult() const;
    QString lastMemoryCandidateReviewStatus() const;
    QString lastMemoryCandidateReviewSummary() const;
    MemoryCommitPolicy memoryCommitPolicy() const;
    MemoryCommitReadiness memoryCommitReadiness() const;
    MemoryCommitResult latestMemoryCommitResult() const;
    QString memoryCommitReadinessStatus() const;
    QString memoryCommitReadinessSummary() const;
    QStringList memoryCommitReadinessChecks() const;
    int memoryCommitPlanCount() const;
    QString memoryCommitTargetSummary() const;
    QStringList memoryCommitCandidateSummaries() const;
    QString lastMemoryCommitStatus() const;
    QString lastMemoryCommitResultSummary() const;
    MemoryRecallPolicy memoryRecallPolicy() const;
    MemoryRecallSummary latestMemoryRecallSummary() const;
    ContextAssemblyPolicy contextAssemblyPolicy() const;
    ContextAssemblySummary contextAssemblySummary() const;
    QString memoryRecallPolicyStatus() const;
    QString memoryRecallPolicySummary() const;
    QString memoryRecallQueryText() const;
    QString memoryRecallStatus() const;
    QString memoryRecallSummaryText() const;
    int memoryRecallResultCount() const;
    QStringList memoryRecallResultSummaries() const;
    QString contextAssemblyPolicyStatus() const;
    QString contextAssemblyPolicySummary() const;
    QString contextAssemblyStatus() const;
    QString contextAssemblySummaryText() const;
    int contextAssemblySourceCount() const;
    int contextAssemblyAvailableSourceCount() const;
    int contextAssemblyCandidateBlockCount() const;
    int contextAssemblyEstimatedSize() const;
    QString conversationContextAvailability() const;
    QString committedMemoryContextAvailability() const;
    QString runtimeMetadataContextAvailability() const;
    QString orchestrationContextAvailability() const;
    QStringList contextAssemblySourceSummaries() const;
    QStringList contextAssemblyReadinessChecks() const;
    int memoryEntryCount() const;
    QString memoryMaintenanceStatus() const;
    QString chatMaintenanceStatus() const;
    const QList<ChatMessage>& chatHistory() const;
    QStringList chatMessages() const;
    QStringList memoryEntries() const;
    void setConversationExportDirectory(const QString& directoryPath);

    bool sendMessageWithAttachments(const QString& message, const QList<ChatAttachment>& attachments);
    Q_INVOKABLE bool sendMessage(const QString& message);
    Q_INVOKABLE bool stopChatGeneration();
    bool tryShutdownRuntime();
    Q_INVOKABLE bool regenerateChatResponse(int userMessageId);
    Q_INVOKABLE bool retryChatResponse(int assistantMessageId);
    Q_INVOKABLE QString editAndResendChatMessage(int userMessageId, const QString& text);
    QVariantList chatMessageRecords() const;
    Q_INVOKABLE bool runLocalInference(const QString& prompt, const QString& model);
    Q_INVOKABLE bool requestConversationSummaryGeneration();
    Q_INVOKABLE bool cancelLocalInference();
    Q_INVOKABLE bool generatePiperTtsFile(const QString& text);
    Q_INVOKABLE bool searchConversation(const QString& query);
    Q_INVOKABLE QVariantList searchChats(const QString& query) const;
    Q_INVOKABLE void clearConversationSearch();
    Q_INVOKABLE bool exportTranscript(const QString& format);
    Q_INVOKABLE bool requestConversationExport(const QString& format);
    Q_INVOKABLE QString createConversation(const QString& title);
    Q_INVOKABLE bool switchConversation(const QString& conversationId);
    Q_INVOKABLE bool renameConversation(const QString& conversationId, const QString& title);
    Q_INVOKABLE bool pinConversation(const QString& conversationId);
    Q_INVOKABLE bool unpinConversation(const QString& conversationId);
    Q_INVOKABLE QString duplicateConversation(const QString& conversationId);
    Q_INVOKABLE bool archiveConversation(const QString& conversationId);
    Q_INVOKABLE bool unarchiveConversation(const QString& conversationId);
    Q_INVOKABLE bool requestPermanentDeleteConversation(const QString& conversationId);
    Q_INVOKABLE QString createMemoryCandidateFromConversationText(const QString& text);
    Q_INVOKABLE bool approveMemoryCandidate(const QString& candidateId);
    Q_INVOKABLE bool rejectMemoryCandidate(const QString& candidateId);
    Q_INVOKABLE bool resetMemoryCandidate(const QString& candidateId);
    Q_INVOKABLE bool archiveMemoryCandidate(const QString& candidateId);
    Q_INVOKABLE bool requestMemoryCandidateCommit(const QString& candidateId);
    Q_INVOKABLE bool recallLocalMemory(const QString& query);
    Q_INVOKABLE void clearLocalMemoryRecall();
    bool runAgentRequestWithAttachments(const QString& request, const QList<ChatAttachment>& attachments);
    Q_INVOKABLE bool runAgentRequest(const QString& request);
    Q_INVOKABLE bool cancelAgentRun();
    Q_INVOKABLE bool agentLoopActive() const;
    bool agentAwaitingApproval() const;
    // C++ composition only; never exposed as model or IPC maintenance authority.
    void attachStorageAdmission(std::shared_ptr<StorageAdmission> admission) {
        storageAdmission_ = std::move(admission);
    }
    std::shared_ptr<StorageAdmission> storageAdmission() const {
        return storageAdmission_;
    }
    void attachAlarmStore(std::shared_ptr<AlarmStore> alarmStore);
    Q_INVOKABLE bool agentAutonomousMode() const;
    Q_INVOKABLE void setAgentAutonomousMode(bool enabled);
    void attachControlledTaskSettings(AppSettings& settings);
    ModelSelection currentWorkspaceModelSelection() const;
    bool currentWorkspaceRequiresLocal() const;
    QString currentWorkspaceName() const;
    WorkspaceProfileSnapshot currentWorkspaceProfile() const;
    ModelService* modelService() const {
        return modelService_.get();
    }
    ExtensionService* extensionService() const;
    const PermissionService* permissionService() const;
    void refreshWorkspaceExtensions();
    void setToolPermissionPolicyState(const QString& state);
    ControlledTaskService* controlledTasks() const {
        return controlledTaskService_.get();
    }
    Q_INVOKABLE bool clearMemory();
    Q_INVOKABLE bool clearChat();
    Q_INVOKABLE QVariantMap manageMemory(const QVariantMap& request);
    Q_INVOKABLE void remember(const QString& key, const QString& value);
    Q_INVOKABLE void refreshOllamaStatus();

signals:
    void modelLibraryChanged();
    void ollamaStatusChanged();
    void chatMessagesChanged();
    void memoryEntriesChanged();
    void maintenanceStatusChanged();
    void agentStatusChanged();
    void agentResponseChanged();
    void toolPlanChanged();
    void approvalChanged();
    void sandboxChanged();
    void toolExecutionChanged();
    void agentPipelineChanged();
    void runtimeContextChanged();
    void conversationSessionChanged();
    void conversationStateChanged();
    void conversationRuntimeChanged();
    void conversationSearchChanged();
    void conversationExportChanged();
    void conversationDuplicateChanged();
    void conversationDeleteChanged();
    void memoryCandidatesChanged();
    void memoryRecallChanged();
    void contextAssemblyChanged();
    void agentActivityChanged();
    void agentLoopStateChanged();
    void modelRoutingChanged();
    void taskPlanChanged();
    void orchestrationSnapshotChanged();
    void runtimeProviderRegistryChanged();
    void localModelSelectionChanged();
    void modelCapabilitiesChanged();
    void localChatInferenceRoutingChanged();
    void localInferenceChanged();
    void voiceConfigurationChanged();
    void promptContextInjectionChanged();

private:
    bool startAgentLoopRun(const QString& goal, const QList<ChatAttachment>& attachments = {});
    void resumeAgentLoopWithApproval(bool approved, bool alwaysAllow = false);
    AgentLoopState currentAgentSessionState() const;
    void onAgentStepRecord(const AgentStepRecord& record);
    void onAgentEvent(const AgentEvent& event);
    void onAgentLoopStatus(const QString& status);
    void onAgentLoopFinished(const AgentLoopState& state);
    void appendAgentLoopChatMessage(const QString& text, bool persist = true);
    QString agentApprovalRequestText(const AgentLoopState& state) const;
    void checkDueAlarms();
    void resetCompletedConversationState();
    void transitionConversationState(ConversationState nextState, const QString& reason);
    void refreshLatestTaskPlan();
    void refreshConversationSession();
    void setMemoryMaintenanceStatus(const QString& status);
    void setChatMaintenanceStatus(const QString& status);
    RuntimePermissionRequest runtimePermissionRequest() const;
    RuntimePermissionDecision currentRuntimePermissionDecision() const;
    RuntimeSafetyReport currentRuntimeSafetyReport() const;
    RuntimePipelineResult currentRuntimePipelineResult() const;
    ExecutionRequest executionRequest() const;
    ExecutionLifecycleResult currentExecutionLifecycleResult() const;
    ExecutionCoordinationSnapshot currentExecutionCoordinationSnapshot() const;
    ProviderRuntimeBridgeRequest providerRuntimeBridgeRequest() const;
    ProviderRuntimeBridgeResponse currentProviderRuntimeBridgeResponse() const;
    RuntimeIntegrationReport currentRuntimeIntegrationReport() const;
    RuntimeProviderRegistry currentRuntimeProviderRegistry() const;
    CredentialStore currentCredentialStore() const;
    ProviderCredentialRegistry currentProviderCredentialRegistry() const;
    OllamaHealthCheckResult currentOllamaHealthCheck() const;
    QString effectiveLocalModel(const QString& requestedModel) const;
    bool discoveredModelNamesContain(const QString& model,
                                     const QList<OllamaModelSummary>& models) const;
    bool localInferenceEndpointAllowed() const;
    bool runLocalInferenceStream(const QString& prompt, const QString& model);
    ConversationWindowResult conversationWindowForPrompt(const QString& prompt) const;
    ConversationSummaryResult conversationSummaryForPrompt(const QString& prompt) const;
    int currentConversationMessageCountForSummary() const;
    QString persistedSummaryExclusionReason() const;
    bool persistedSummaryReadyForContinuity(QString* reason = nullptr) const;
    MemoryRelevanceSummary memoryRelevanceSummaryForPrompt(const QString& prompt) const;
    QList<ConversationSalienceCandidate>
    conversationSalienceCandidatesForPrompt(const QString& prompt) const;
    ConversationSalienceSummary conversationSalienceSummaryForPrompt(const QString& prompt) const;
    ConversationCompressionSummary
    conversationCompressionSummaryForPrompt(const QString& prompt) const;
    ConversationSummaryResult
    planConversationSummaryGenerationForActiveConversation(bool explicitUserAction) const;
    bool persistConversationSummaryMetadata(const ConversationSummaryResult& result);
    bool startConversationSummaryInference(const ConversationSummaryResult& plannedResult);
    void finishConversationSummaryInference(const QString& requestId,
                                            const LocalInferenceResponse& response);
    QString buildConversationSummaryPrompt(const ConversationSummaryResult& plannedResult) const;
    QString sanitizeGeneratedConversationSummary(const QString& text) const;
    ConversationSummaryResult blockedConversationSummaryResult(const QString& reason,
                                                               const QString& fallback) const;
    QList<RetrievalCandidate> retrievalCandidatesForPrompt(const QString& prompt) const;
    RetrievalPlanningResult retrievalPlanningForPrompt(const QString& prompt) const;
    QList<SemanticCandidate> semanticCandidatesForPrompt(const QString& prompt) const;
    SemanticCandidateArbitration semanticCandidateArbitrationForPrompt(const QString& prompt) const;
    QList<PromptContextBlock> promptContextBlocks(const QString& prompt) const;
    PromptContextInjectionResult preparePromptContextInjection(const QString& prompt);
    void finishLocalInferenceRequest(const QString& requestId,
                                     const LocalInferenceResponse& response);
    void updateLocalInferenceStreamRequest(const QString& requestId,
                                           const LocalInferenceStreamChunk& chunk);
    void finishLocalInferenceStreamRequest(const QString& requestId,
                                           const LocalInferenceStreamResult& result);
    void finalizeLocalChatInference(bool succeeded);
    void initializeActiveConversation();
    bool ensureActiveConversation();
    void loadActiveConversationTranscript();
    bool persistActiveConversationMessage(const ChatMessage& message);
    ConversationMessageRecord
    conversationMessageRecordFromChatMessage(const ChatMessage& message) const;
    ChatMessage
    chatMessageFromConversationMessageRecord(const ConversationMessageRecord& message) const;
    QList<ConversationRecord> conversationRecords() const;
    ConversationRecord activeConversationRecord() const;
    void resetConversationRuntimeState();
    void refreshConversationHistorySummary();
    void resetConversationSearchSummary();
    void setConversationRuntimeRequest(const QString& requestId, const QString& model,
                                       const QString& route, bool streaming);
    void setConversationRuntimeResult(bool succeeded, const QString& summary,
                                      qint64 latencyMs = -1);
    void setChatSendLifecycle(const QString& state, const QString& summary);
    bool bindAgentPlannerToResolvedModel(const ModelBindingResolution& resolution);
    LocalInferenceResponse blockedLocalInferenceResponse(const LocalInferenceRequest& request,
                                                         LocalInferenceError error,
                                                         const QString& summary) const;
    QList<MemoryCandidate> memoryCandidates() const;
    int memoryCandidateCountForState(MemoryReviewState state) const;
    QStringList memoryCandidateSummariesForState(MemoryReviewState state) const;
    const MemoryCandidate* findMemoryCandidate(const QList<MemoryCandidate>& candidates,
                                               const QString& candidateId) const;
    MemoryCommitReadiness memoryCommitReadinessForCandidateId(const QString& candidateId) const;
    bool memoryKeyExists(const QString& key) const;
    MemoryAccessContext currentMemoryAccessContext() const;
    MemoryEntries currentMemoryEntries() const;
    void refreshMemoryRecallForCurrentEntries();
    ContextAssemblySource contextAssemblySource(ContextAssemblySourceKind kind) const;
    ModelRegistry currentModelRegistry() const;
    MemoryCandidate memoryCandidateFromConversationText(const QString& text) const;
    bool reviewMemoryCandidate(const QString& candidateId, MemoryCandidateReviewAction action);
    WhisperRuntimeDescriptor currentWhisperRuntimeDescriptor() const;
    PiperRuntimeDescriptor currentPiperRuntimeDescriptor() const;
    VoiceRuntimeReadinessReport currentVoiceRuntimeReadinessReport() const;
    PiperSynthesisConfig currentPiperSynthesisConfig() const;
    PiperSynthesisRequest currentPiperSynthesisRequest() const;
    PiperSynthesisReadiness currentPiperSynthesisReadiness() const;
    WhisperTranscriptionConfig currentWhisperTranscriptionConfig() const;
    WhisperTranscriptionRequest currentWhisperTranscriptionRequest() const;
    WhisperTranscriptionReadiness currentWhisperTranscriptionReadiness() const;

    std::unique_ptr<ModelService> modelService_;
    AppSettings* workspaceSettings_ = nullptr;
    std::unique_ptr<ModelLibraryService> modelLibrary_;
    std::unique_ptr<SpeechAssetCatalogAdapter> speechAssetCatalog_;
    std::unique_ptr<ModelOperationService> modelOperations_;
    std::unique_ptr<IAgentRunStore> agentRunStore_;
    std::unique_ptr<IAgentRuntime> agentRuntime_;
    std::unique_ptr<ControlledTaskService> controlledTaskService_;
    std::unique_ptr<IAgentStepPlanner> agentStepPlanner_;
    std::unique_ptr<IApprovalPolicy> approvalPolicy_;
    std::unique_ptr<ISandboxPolicy> sandboxPolicy_;
    std::unique_ptr<IToolExecutor> toolExecutor_;
    std::unique_ptr<IProviderCatalog> providerCatalog_;
    std::unique_ptr<IModelRouter> modelRouter_;
    std::unique_ptr<ITaskPlanner> taskPlanner_;
    std::unique_ptr<IAgentRegistry> agentRegistry_;
    std::unique_ptr<IMemoryCatalog> memoryCatalog_;
    std::unique_ptr<ILocalRuntime> localRuntime_;
    std::unique_ptr<ILocalRuntimeSessionManager> localRuntimeSessions_;
    std::unique_ptr<IRuntimeCapabilityRegistry> runtimeCapabilities_;
    std::unique_ptr<IRuntimePermissionPolicy> runtimePermissionPolicy_;
    std::unique_ptr<IRuntimeSafetyPolicy> runtimeSafetyPolicy_;
    std::unique_ptr<IRuntimePipeline> runtimePipeline_;
    std::unique_ptr<IExecutionLifecycle> executionLifecycle_;
    std::unique_ptr<IAgentTaskRuntime> agentTaskRuntime_;
    std::unique_ptr<ExecutionCoordinator> executionCoordinator_;
    std::unique_ptr<ILocalRuntimeAdapter> localRuntimeAdapter_;
    std::unique_ptr<IProviderRuntimeBridge> providerRuntimeBridge_;
    std::unique_ptr<StaticRuntimeIntegrationReadiness> runtimeIntegrationReadiness_;
    std::unique_ptr<IOllamaRuntimeClient> ollamaRuntimeClient_;
    std::unique_ptr<ILocalInferenceWorker> localInferenceWorker_;
    mutable std::unique_ptr<ILocalInferenceWorker> lmStudioInferenceWorker_;
    bool localInferenceClientIsRealOllama_ = false;
    bool localInferenceStreamClientIsRealOllama_ = false;

    bool isLMStudioProvider() const {
        const auto providerId = selectedRuntimeProvider();
        return providerId == QStringLiteral("lm-studio") ||
               providerId == QStringLiteral("llama-cpp-server") ||
               providerId == QStringLiteral("openai-compatible-local") ||
               providerId == QStringLiteral("cloud-api") ||
               providerId == QStringLiteral("openai") || providerId == QStringLiteral("claude") ||
               providerId == QStringLiteral("gemini") || providerId == QStringLiteral("deepseek") ||
               providerId == QStringLiteral("groq") || providerId == QStringLiteral("mistral");
    }
    bool isLocalChatProvider() const {
        const auto providerId = selectedRuntimeProvider();
        return providerId == QStringLiteral("ollama") ||
               providerId == QStringLiteral("lm-studio") ||
               providerId == QStringLiteral("llama-cpp-server") ||
               providerId == QStringLiteral("openai-compatible-local") ||
               providerId == QStringLiteral("cloud-api") ||
               providerId == QStringLiteral("openai") || providerId == QStringLiteral("claude") ||
               providerId == QStringLiteral("gemini") || providerId == QStringLiteral("deepseek") ||
               providerId == QStringLiteral("groq") || providerId == QStringLiteral("mistral");
    }
    bool hasActiveLocalInferenceRuntime() const;
    LMStudioConfig currentCloudOrLMStudioConfig() const;
    LMStudioConfig currentCloudOrLMStudioConfig(const ModelBinding& binding) const;
    ILocalInferenceWorker* activeLocalInferenceWorker() const;
    void updatePiperTtsProviderConfig();
    void updateWhisperSttProviderConfig();
    void refreshSpeechTtsRuntime();
    std::unique_ptr<IModelManagementService> modelManagementService_;
    std::unique_ptr<ITextToSpeechProvider> textToSpeechProvider_;
    std::unique_ptr<ISpeechToTextProvider> speechToTextProvider_;
    std::unique_ptr<IVoiceRuntimeCoordinator> voiceRuntimeCoordinator_;
    std::unique_ptr<IVoiceRuntimeEnvironment> voiceRuntimeEnvironment_;
    std::unique_ptr<PiperTextToSpeechProvider> piperTextToSpeechProvider_;
    VoiceSessionService* audioSession_{nullptr}; // QObject child; no raw recording persistence
    QString selectedSpeechTtsEngine_{QStringLiteral("Piper")};
    QString kokoroSpeechModelPath_;
    QString kokoroSpeechVoice_;
    std::shared_ptr<StorageAdmission> storageAdmission_;
    std::unique_ptr<IMemoryStore> memoryStore_;
    std::unique_ptr<class MemoryArchiveJobs> memoryArchiveJobs_;
    std::unique_ptr<IMemoryCandidateStore> memoryCandidateStore_;
    MemoryCandidateReviewResult latestMemoryCandidateReviewResult_;
    MemoryCommitPolicy memoryCommitPolicy_;
    MemoryCommitResult latestMemoryCommitResult_;
    MemoryRecallPolicy memoryRecallPolicy_;
    MemoryRecallSummary latestMemoryRecallSummary_;
    ContextAssemblyPolicy contextAssemblyPolicy_;
    ConversationWindowPolicy conversationWindowPolicy_;
    ConversationSummaryPolicy conversationSummaryPolicy_;
    RetrievalPlanningPolicy retrievalPlanningPolicy_;
    ConversationSaliencePolicy conversationSaliencePolicy_;
    ConversationCompressionPolicy conversationCompressionPolicy_;
    ConversationSummaryResult latestConversationSummaryGenerationResult_;
    ConversationSummaryMetadataRecord latestConversationSummaryMetadata_;
    SemanticRetrievalPolicy semanticRetrievalPolicy_;
    VectorPersistencePolicy vectorPersistencePolicy_;
    SemanticSearchPolicy semanticSearchPolicy_;
    HybridRetrievalBridgePolicy hybridRetrievalBridgePolicy_;
    SemanticAcceptancePolicy semanticAcceptancePolicy_;
    SemanticSupplementAssemblyPolicy semanticSupplementAssemblyPolicy_;
    SemanticPromptAuthorityPolicy semanticPromptAuthorityPolicy_;
    SemanticPromptInclusionPolicy semanticPromptInclusionPolicy_;
    SemanticProviderPolicy semanticProviderPolicy_;
    SemanticProviderMode selectedSemanticProviderMode_ = SemanticProviderMode::Disabled;
    QString semanticEmbeddingModel_ = QStringLiteral("nomic-embed-text");
    EmbeddingProviderStatus semanticEmbeddingStatus_ = EmbeddingProviderStatus::NotConfigured;
    QString semanticEmbeddingSummary_ =
        QStringLiteral("Semantic embedding provider is not configured.");
    SemanticCandidatePolicy semanticCandidatePolicy_;
    HybridRetrievalPolicy hybridRetrievalPolicy_;
    SemanticArbitrationPolicy semanticArbitrationPolicy_;
    PromptContextInjectionPolicy promptContextInjectionPolicy_;
    PromptContextInjectionResult latestPromptContextInjectionResult_;
    SemanticPromptInclusionResult latestSemanticPromptInclusionResult_;
    std::unique_ptr<ChatSession> chatSession_;
    std::unique_ptr<IChatHistoryStore> chatHistoryStore_;
    std::unique_ptr<IConversationStore> conversationStore_;
    std::unique_ptr<ChatModeService> chatMode_;
    QString memoryMaintenanceStatus_ = QStringLiteral("Ready");
    QString chatMaintenanceStatus_ = QStringLiteral("Ready");
    QString lastAgentResponse_ = QStringLiteral("No agent request yet.");
    AgentPipelineResult latestAgentPipelineResult_;
    TaskPlan latestTaskPlan_;
    RuntimeSession runtimeSession_;
    ConversationSessionStore conversationSession_;
    StaticConversationStateGraph conversationStateGraph_;
    AgentActivityLog agentActivityLog_;
    bool localChatInferenceEnabled_ = false;
    bool agentAutonomousMode_ = false;
    QString activeAgentSessionId_;
    QString agentEventSubscriptionId_;
    std::shared_ptr<AlarmStore> alarmStore_;
    bool promptContextInjectionEnabled_ = false;
    bool localInferenceStreamingEnabled_ = true;
    int localInferenceTimeoutMs_ = 0;
    double localInferenceTemperature_ = 0.7;
    double localInferenceTopP_ = 0.9;
    int localInferenceMaxTokens_ = 2048;
    bool localInferenceBusy_ = false;
    bool activeLocalInferenceIsChatRequest_ = false;
    bool activeLocalInferenceIsSummaryRequest_ = false;
    quint64 localInferenceRequestSequence_ = 0;
    QString activeLocalInferenceRequestId_;
    QString activeLocalInferenceConversationId_;
    QString chatSendLifecycleState_ = QStringLiteral("idle");
    QString chatSendLifecycleSummary_ = QStringLiteral("Ready when local chat requirements pass.");
    QString conversationRuntimeRequestId_ = QStringLiteral("None");
    QString conversationRuntimeActiveModel_ = QStringLiteral("None");
    QString conversationRuntimeActiveRoute_ = QStringLiteral("Provider");
    bool conversationRuntimeStreaming_ = false;
    QString conversationRuntimeLastSuccessSummary_ = QStringLiteral("No successful response yet.");
    QString conversationRuntimeLastErrorSummary_ = QStringLiteral("No error or refusal yet.");
    QString conversationRuntimeLastLatencySummary_ = QStringLiteral("No latency recorded.");
    ConversationHistorySummary conversationHistorySummary_;
    ConversationClearResult latestConversationClearResult_;
    ConversationSearchSummary latestConversationSearchSummary_;
    ConversationExportReadiness conversationExportReadiness_;
    ConversationExportResult latestConversationExportResult_;
    ConversationDuplicateResult latestConversationDuplicateResult_;
    ConversationDeletePolicy conversationDeletePolicy_;
    ConversationDeleteResult latestConversationDeleteResult_;
    QString conversationExportDirectory_;
    QString activeConversationId_;
    LocalInferenceResponse latestLocalInferenceResponse_;
    LocalInferenceStreamResult latestLocalInferenceStreamResult_;
    bool piperFileOutputExecutionEnabled_ = false;
    bool whisperTranscriptionExecutionEnabled_ = false;
    PiperTtsResult latestPiperTtsResult_;
    PiperSynthesisResult latestPiperSynthesisResult_;
    WhisperTranscriptionResult latestWhisperTranscriptionResult_;
    QString piperBinaryPath_;
    QString piperModelPath_;
    QString whisperBinaryPath_;
    QString whisperModelPath_;

    QTimer* ollamaPollTimer_ = nullptr;
    QThread* ollamaCheckThread_ = nullptr;
    bool pendingModelRefresh_ = false;
    std::shared_ptr<std::atomic_bool> discoveryCancellation_;
    quint64 discoveryGeneration_ = 0;
    quint64 activeLocalInferenceHealthSequence_ = 0;
    QString activeLocalInferenceProviderId_;
    mutable OllamaHealthCheckResult cachedOllamaHealthCheck_;
    mutable QString lmStudioEndpoint_;
    mutable QString llamaCppEndpoint_;
    mutable bool ollamaCacheInitialized_ = false;

    void pollOllama();
    void initializeOllamaCache() const;
};

} // namespace sentinel::core
