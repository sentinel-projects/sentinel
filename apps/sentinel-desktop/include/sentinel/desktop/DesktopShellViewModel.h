// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "sentinel/desktop/SystemDictationService.h"

#include "sentinel/core/agent/AgentRuntimeService.h"
#include "sentinel/core/agent/ControlledAgentTasks.h"
#include "sentinel/core/app/CompanionService.h"
#include "sentinel/core/app/SkillProfileService.h"
#include "sentinel/core/app/WorkspaceService.h"
#include "sentinel/core/memory/LocalRagStore.h"
#include "sentinel/core/memory/SemanticRetrieval.h"
#include "sentinel/core/platform/WinTaskbarIntegration.h"
#include "sentinel/core/runtime/ToolExecution.h"
#include "sentinel/core/runtime/ToolExecutionGateway.h"
#include "sentinel/core/security/PermissionPolicyService.h"
#include "sentinel/desktop/ChatMessageListModel.h"
#include "sentinel/desktop/DesktopControllerBridge.h"
#include "sentinel/desktop/DesktopErrorProjection.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <memory>

class QFile;
class QProcess;
class QSystemTrayIcon;
class QTimer;
class QNetworkAccessManager;
class QNetworkReply;

namespace sentinel::core {
class AppSettings;
class ApplicationController;
class ModeManager;
} // namespace sentinel::core

namespace sentinel::desktop {

class DesktopShellViewModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(
        QString daemonConnectionStatus READ daemonConnectionStatus NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(bool daemonConnected READ daemonConnected NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QVariantMap pendingDaemonApproval READ pendingDaemonApproval NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QVariantList persistentPermissionGrants READ persistentPermissionGrants NOTIFY
                   persistentPermissionGrantsChanged)
    Q_PROPERTY(QString providerName READ providerName NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString providerStatus READ providerStatus NOTIFY providerStatusChanged)
    Q_PROPERTY(QString agentStatus READ agentStatus NOTIFY agentStatusChanged)
    Q_PROPERTY(bool agentLoopActive READ agentLoopActive NOTIFY agentLoopChanged)
    Q_PROPERTY(bool agentAwaitingApproval READ agentAwaitingApproval NOTIFY agentLoopChanged)
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
    Q_PROPERTY(QStringList availableRoutingModes READ availableRoutingModes NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString modelRoutingStatus READ modelRoutingStatus NOTIFY modelRoutingChanged)
    Q_PROPERTY(QString selectedModelProviderSummary READ selectedModelProviderSummary NOTIFY
                   modelRoutingChanged)
    Q_PROPERTY(QString latestTaskPlanStatus READ latestTaskPlanStatus NOTIFY taskPlanChanged)
    Q_PROPERTY(QString latestTaskPlanSummary READ latestTaskPlanSummary NOTIFY taskPlanChanged)
    Q_PROPERTY(int plannedTaskStepCount READ plannedTaskStepCount NOTIFY taskPlanChanged)
    Q_PROPERTY(int registeredAgentCount READ registeredAgentCount NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(
        QStringList activeAgentSummaries READ activeAgentSummaries NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString currentAgentSummary READ currentAgentSummary NOTIFY taskPlanChanged)
    Q_PROPERTY(QString currentMemoryAffinitySummary READ currentMemoryAffinitySummary NOTIFY
                   taskPlanChanged)
    Q_PROPERTY(int providerCatalogCount READ providerCatalogCount NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList providerCatalogSummaries READ providerCatalogSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int memoryCatalogCount READ memoryCatalogCount NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList memoryCatalogSummaries READ memoryCatalogSummaries NOTIFY
                   runtimeProjectionChanged)
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
    Q_PROPERTY(
        QString agentTaskRuntimeStatus READ agentTaskRuntimeStatus NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString agentTaskRuntimeSummary READ agentTaskRuntimeSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int agentTaskRuntimeTaskCount READ agentTaskRuntimeTaskCount NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int agentTaskQueueCount READ agentTaskQueueCount NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(int agentTaskQueueActiveCount READ agentTaskQueueActiveCount NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int agentTaskQueuePlannedCount READ agentTaskQueuePlannedCount NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int agentTaskQueueBlockedCount READ agentTaskQueueBlockedCount NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int agentTaskQueueCompletedCount READ agentTaskQueueCompletedCount NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int agentTaskQueueRefusedCount READ agentTaskQueueRefusedCount NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(
        QString latestAgentTaskSummary READ latestAgentTaskSummary NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString latestAgentTaskLifecycleSummary READ latestAgentTaskLifecycleSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList agentTaskQueueSummaries READ agentTaskQueueSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList agentTaskTraceSummaries READ agentTaskTraceSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString agentPlanningSessionStatus READ agentPlanningSessionStatus NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString agentPlanningSessionSummary READ agentPlanningSessionSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int agentPlanningCandidateCount READ agentPlanningCandidateCount NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int agentPlanningRefusedCount READ agentPlanningRefusedCount NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList agentPlanningCandidateSummaries READ agentPlanningCandidateSummaries
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList agentPlanningArbitrationSummaries READ agentPlanningArbitrationSummaries
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList agentPlanningRefusalSummaries READ agentPlanningRefusalSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString agentPlanningFallbackSummary READ agentPlanningFallbackSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString agentCapabilityRegistryStatus READ agentCapabilityRegistryStatus NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString agentCapabilityRegistrySummary READ agentCapabilityRegistrySummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int agentCapabilityCount READ agentCapabilityCount NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(int agentCapabilityEnabledCount READ agentCapabilityEnabledCount NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int agentCapabilityDisabledCount READ agentCapabilityDisabledCount NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int agentCapabilityRestrictedCount READ agentCapabilityRestrictedCount NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList agentCapabilitySummaries READ agentCapabilitySummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList agentCapabilityReadinessSummaries READ agentCapabilityReadinessSummaries
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList agentCapabilitySafetySummaries READ agentCapabilitySafetySummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString toolContractRegistryStatus READ toolContractRegistryStatus NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString toolContractRegistrySummary READ toolContractRegistrySummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int toolContractCount READ toolContractCount NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(
        int toolContractEnabledCount READ toolContractEnabledCount NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(int toolContractDisabledCount READ toolContractDisabledCount NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(int toolContractRestrictedCount READ toolContractRestrictedCount NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList toolContractSummaries READ toolContractSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList toolContractPermissionSummaries READ toolContractPermissionSummaries
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList toolContractSandboxSummaries READ toolContractSandboxSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList toolContractReadinessSummaries READ toolContractReadinessSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList toolContractSafetySummaries READ toolContractSafetySummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString localRuntimeStatus READ localRuntimeStatus NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString localRuntimeHealth READ localRuntimeHealth NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString localRuntimeSummary READ localRuntimeSummary NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList localRuntimeCapabilities READ localRuntimeCapabilities NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString localRuntimeResponseStatus READ localRuntimeResponseStatus NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString localRuntimeResponseSummary READ localRuntimeResponseSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(
        int localRuntimeSessionCount READ localRuntimeSessionCount NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString localRuntimeSessionStatus READ localRuntimeSessionStatus NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString localRuntimeSessionHealth READ localRuntimeSessionHealth NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString localRuntimeSessionSummary READ localRuntimeSessionSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString localRuntimeAllocationSummary READ localRuntimeAllocationSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString localRuntimeReservationSummary READ localRuntimeReservationSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList localRuntimeSessionSummaries READ localRuntimeSessionSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(
        int runtimeCapabilityCount READ runtimeCapabilityCount NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList enabledRuntimeCapabilitySummaries READ enabledRuntimeCapabilitySummaries
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList disabledRuntimeCapabilitySummaries READ
                   disabledRuntimeCapabilitySummaries NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString runtimeNegotiationProfileSummary READ runtimeNegotiationProfileSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString runtimeNegotiationSummary READ runtimeNegotiationSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString localOnlyRuntimeEnforcementSummary READ localOnlyRuntimeEnforcementSummary
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString runtimePermissionDecision READ runtimePermissionDecision NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString runtimePermissionSummary READ runtimePermissionSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(
        QString runtimeSafetyDecision READ runtimeSafetyDecision NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(
        QString runtimeSafetySummary READ runtimeSafetySummary NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(
        QString runtimePipelineStatus READ runtimePipelineStatus NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(
        QString runtimePipelineSummary READ runtimePipelineSummary NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList runtimePipelineTraceSummaries READ runtimePipelineTraceSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString executionLifecycleState READ executionLifecycleState NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString executionLifecycleStatus READ executionLifecycleStatus NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString executionLifecycleSummary READ executionLifecycleSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList executionLifecycleTraceSummaries READ executionLifecycleTraceSummaries
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString executionSessionId READ executionSessionId NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(
        QString executionSessionStatus READ executionSessionStatus NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString executionSessionOwnership READ executionSessionOwnership NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString executionCoordinationMode READ executionCoordinationMode NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString executionSessionSummary READ executionSessionSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString executionCoordinationSnapshotSummary READ
                   executionCoordinationSnapshotSummary NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString localRuntimeAdapterStatus READ localRuntimeAdapterStatus NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString localRuntimeAdapterHealth READ localRuntimeAdapterHealth NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString localRuntimeAdapterSummary READ localRuntimeAdapterSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList localRuntimeAdapterCapabilitySummaries READ
                   localRuntimeAdapterCapabilitySummaries NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString providerRuntimeBridgeStatus READ providerRuntimeBridgeStatus NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString providerRuntimeBridgeSummary READ providerRuntimeBridgeSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString providerRuntimeBridgeResponseSummary READ
                   providerRuntimeBridgeResponseSummary NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString runtimeIntegrationReadinessStatus READ runtimeIntegrationReadinessStatus
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString runtimeIntegrationReadinessSummary READ runtimeIntegrationReadinessSummary
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList runtimeIntegrationReadinessChecks READ runtimeIntegrationReadinessChecks
                   NOTIFY runtimeProjectionChanged)
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
    Q_PROPERTY(QString ollamaEndpoint READ ollamaEndpoint WRITE setOllamaEndpoint NOTIFY
                   ollamaStatusChanged)
    Q_PROPERTY(QString lmStudioEndpoint READ lmStudioEndpoint WRITE setLmStudioEndpoint NOTIFY
                   ollamaStatusChanged)
    Q_PROPERTY(QString llamaCppEndpoint READ llamaCppEndpoint WRITE setLlamaCppEndpoint NOTIFY
                   ollamaStatusChanged)
    Q_PROPERTY(QString cloudApiEndpoint READ cloudApiEndpoint WRITE setCloudApiEndpoint NOTIFY
                   ollamaStatusChanged)
    Q_PROPERTY(
        QString ollamaConnectionStatus READ ollamaConnectionStatus NOTIFY ollamaStatusChanged)
    Q_PROPERTY(QString ollamaHealthStatus READ ollamaHealthStatus NOTIFY ollamaStatusChanged)
    Q_PROPERTY(QString ollamaHealthSummary READ ollamaHealthSummary NOTIFY ollamaStatusChanged)
    Q_PROPERTY(QString ollamaDiscoveryStatus READ ollamaDiscoveryStatus NOTIFY ollamaStatusChanged)
    Q_PROPERTY(QString localInferenceHealthSummary READ localInferenceHealthSummary NOTIFY
                   ollamaStatusChanged)
    Q_PROPERTY(bool isOnline READ isOnline NOTIFY onlineChanged)
    Q_PROPERTY(int ollamaModelCount READ ollamaModelCount NOTIFY ollamaStatusChanged)
    Q_PROPERTY(QStringList ollamaModelNames READ ollamaModelNames NOTIFY ollamaStatusChanged)
    Q_PROPERTY(
        QString cloudModelDiscoveryError READ cloudModelDiscoveryError NOTIFY ollamaStatusChanged)
    Q_PROPERTY(QStringList installedOllamaModelNames READ installedOllamaModelNames NOTIFY
                   ollamaStatusChanged)
    Q_PROPERTY(QStringList loadedLMStudioModelNames READ loadedLMStudioModelNames NOTIFY
                   ollamaStatusChanged)
    Q_PROPERTY(
        QStringList ollamaModelSummaries READ ollamaModelSummaries NOTIFY ollamaStatusChanged)

    Q_PROPERTY(QString selectedLocalModel READ selectedLocalModel WRITE setSelectedLocalModel NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QString selectedOllamaModel READ selectedOllamaModel WRITE setSelectedOllamaModel
                   NOTIFY localModelSelectionChanged)
    Q_PROPERTY(
        QString activeLocalModelName READ activeLocalModelName NOTIFY localModelSelectionChanged)
    Q_PROPERTY(
        bool proxyEnabled READ proxyEnabled WRITE setProxyEnabled NOTIFY proxySettingsChanged)
    Q_PROPERTY(QString proxyType READ proxyType WRITE setProxyType NOTIFY proxySettingsChanged)
    Q_PROPERTY(QString proxyHost READ proxyHost WRITE setProxyHost NOTIFY proxySettingsChanged)
    Q_PROPERTY(int proxyPort READ proxyPort WRITE setProxyPort NOTIFY proxySettingsChanged)
    Q_PROPERTY(QString proxyUser READ proxyUser WRITE setProxyUser NOTIFY proxySettingsChanged)
    Q_PROPERTY(
        QString proxyPassword READ proxyPassword WRITE setProxyPassword NOTIFY proxySettingsChanged)
    Q_PROPERTY(
        bool proxyPasswordConfigured READ proxyPasswordConfigured NOTIFY proxySettingsChanged)
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
    Q_PROPERTY(QStringList modelRoleIds READ modelRoleIds NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList modelRoleAssignmentSummaries READ modelRoleAssignmentSummaries NOTIFY
                   modelRoleChanged)
    Q_PROPERTY(QStringList modelAdvisorRecommendationSummaries READ
                   modelAdvisorRecommendationSummaries NOTIFY localModelSelectionChanged)
    Q_PROPERTY(QStringList modelAdvisorAvoidSummaries READ modelAdvisorAvoidSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList downloadsCenterSummaries READ downloadsCenterSummaries NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QStringList benchmarkHubSummaries READ benchmarkHubSummaries NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QStringList selectedModelCapabilityLabels READ selectedModelCapabilityLabels NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QVariantList modelCapabilitySettings READ modelCapabilitySettings NOTIFY
                   modelCapabilitySettingsChanged)
    Q_PROPERTY(QString modelCapabilityCompatibilitySummary READ modelCapabilityCompatibilitySummary
                   NOTIFY modelCapabilitySettingsChanged)
    Q_PROPERTY(
        QString modelManagementStatus READ modelManagementStatus NOTIFY localModelSelectionChanged)
    Q_PROPERTY(QString modelManagementSummary READ modelManagementSummary NOTIFY
                   localModelSelectionChanged)
    Q_PROPERTY(QString modelManagementActionAvailability READ modelManagementActionAvailability
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList modelRecommendationSummaries READ modelRecommendationSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList modelRequirementSummaries READ modelRequirementSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString voiceRuntimeMode READ voiceRuntimeMode NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(bool voiceEnabled READ voiceEnabled NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(
        QString voiceReadinessStatus READ voiceReadinessStatus NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(
        QString voiceReadinessSummary READ voiceReadinessSummary NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(
        QStringList voiceReadinessChecks READ voiceReadinessChecks NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList voiceCapabilitySummaries READ voiceCapabilitySummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString textToSpeechStatus READ textToSpeechStatus NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString textToSpeechSummary READ textToSpeechSummary NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString speechToTextStatus READ speechToTextStatus NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString speechToTextSummary READ speechToTextSummary NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString voiceSessionId READ voiceSessionId NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString voiceSessionStatus READ voiceSessionStatus NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString voiceSessionSummary READ voiceSessionSummary NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString voicePipelineStatus READ voicePipelineStatus NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(
        QString voicePipelineSummary READ voicePipelineSummary NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList voicePipelineTraceSummaries READ voicePipelineTraceSummaries NOTIFY
                   runtimeProjectionChanged)
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
                   audioFileSupportedExtensionSummaries NOTIFY runtimeProjectionChanged)
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
    Q_PROPERTY(QString voiceRuntimeStatus READ voiceRuntimeStatus NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString voiceRuntimeSummary READ voiceRuntimeSummary NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList voiceRuntimeCheckSummaries READ voiceRuntimeCheckSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(
        bool voiceRuntimeAvailable READ voiceRuntimeAvailable NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(bool voiceTextToSpeechAvailable READ voiceTextToSpeechAvailable NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(bool voiceSpeechToTextAvailable READ voiceSpeechToTextAvailable NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(
        bool voiceMicrophoneEnabled READ voiceMicrophoneEnabled NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(bool voicePlaybackEnabled READ voicePlaybackEnabled NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(bool voiceLocalOnlyPolicy READ voiceLocalOnlyPolicy NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(bool voiceProcessExecutionEnabled READ voiceProcessExecutionEnabled NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString voiceRuntimeEnvironmentStatus READ voiceRuntimeEnvironmentStatus NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString voiceRuntimeEnvironmentSummary READ voiceRuntimeEnvironmentSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(
        QStringList voiceBinarySummaries READ voiceBinarySummaries NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(
        QStringList voiceModelSummaries READ voiceModelSummaries NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QStringList voiceRuntimePermissionSummaries READ voiceRuntimePermissionSummaries
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString voiceRuntimeSafetyStatus READ voiceRuntimeSafetyStatus NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString voiceRuntimeSafetySummary READ voiceRuntimeSafetySummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList voiceRuntimeSafetyChecks READ voiceRuntimeSafetyChecks NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(bool voiceRuntimeExecutionAllowed READ voiceRuntimeExecutionAllowed NOTIFY
                   runtimeProjectionChanged)
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
    Q_PROPERTY(QString selectedTtsEngine READ selectedTtsEngine WRITE setSelectedTtsEngine NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString kokoroModelPath READ kokoroModelPath WRITE setKokoroModelPath NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(
        QString kokoroVoice READ kokoroVoice WRITE setKokoroVoice NOTIFY voiceConfigurationChanged)
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
    Q_PROPERTY(bool whisperTranscriptionExecutionEnabled READ whisperTranscriptionExecutionEnabled
                   WRITE setWhisperTranscriptionExecutionEnabled NOTIFY voiceConfigurationChanged)
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
    Q_PROPERTY(QString voiceRuntimeReadinessSummary READ voiceRuntimeReadinessSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(
        bool voiceRecordingActive READ voiceRecordingActive NOTIFY voiceRecordingActiveChanged)
    Q_PROPERTY(QString voiceRuntimeHealth READ voiceRuntimeHealth NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(int voiceRuntimeConfiguredCount READ voiceRuntimeConfiguredCount NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(
        int voiceRuntimeMissingCount READ voiceRuntimeMissingCount NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(
        int voiceRuntimeRefusedCount READ voiceRuntimeRefusedCount NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString voiceRuntimePermissionFoundationSummary READ
                   voiceRuntimePermissionFoundationSummary NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString voiceRuntimeSandboxSummary READ voiceRuntimeSandboxSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString voiceRuntimeSafetyReportSummary READ voiceRuntimeSafetyReportSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QStringList voiceRuntimeReadinessChecks READ voiceRuntimeReadinessChecks NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(
        QString whisperRuntimeStatus READ whisperRuntimeStatus NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString whisperRuntimeReadinessSummary READ whisperRuntimeReadinessSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString whisperRuntimePathSummary READ whisperRuntimePathSummary NOTIFY
                   voiceConfigurationChanged)
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
    Q_PROPERTY(QString piperRuntimeStatus READ piperRuntimeStatus NOTIFY voiceConfigurationChanged)
    Q_PROPERTY(QString piperRuntimeReadinessSummary READ piperRuntimeReadinessSummary NOTIFY
                   voiceConfigurationChanged)
    Q_PROPERTY(QString piperRuntimePathSummary READ piperRuntimePathSummary NOTIFY
                   voiceConfigurationChanged)
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
    Q_PROPERTY(
        QVariantMap chatInputCapabilities READ chatInputCapabilities NOTIFY chatMessagesChanged)
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
                   contextExplainabilityVisibleChanged)
    Q_PROPERTY(bool contextExplainabilityVisible READ contextExplainabilityVisible WRITE
                   setContextExplainabilityVisible NOTIFY contextExplainabilityVisibleChanged)
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
    Q_PROPERTY(
        bool semanticRetrievalEnabled READ semanticRetrievalEnabled NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString semanticRetrievalStatus READ semanticRetrievalStatus NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString semanticRetrievalSummary READ semanticRetrievalSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString semanticReadiness READ semanticReadiness NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString embeddingProviderReadiness READ embeddingProviderReadiness NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString embeddingProviderSummary READ embeddingProviderSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(
        QString vectorIndexReadiness READ vectorIndexReadiness NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString vectorIndexSummary READ vectorIndexSummary NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(
        int vectorIndexedItemCount READ vectorIndexedItemCount NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(
        QString semanticProviderMode READ semanticProviderMode NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString selectedSemanticProviderName READ selectedSemanticProviderName NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString semanticProviderReadiness READ semanticProviderReadiness NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(
        QString semanticProviderHealth READ semanticProviderHealth NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString semanticProviderStatusSummary READ semanticProviderStatusSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString semanticActivationReadiness READ semanticActivationReadiness NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString semanticActivationSummary READ semanticActivationSummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList semanticProviderCapabilitySummaries READ
                   semanticProviderCapabilitySummaries NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList semanticActivationRequiredSteps READ semanticActivationRequiredSteps
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList semanticRetrievalReadinessChecks READ semanticRetrievalReadinessChecks
                   NOTIFY runtimeProjectionChanged)
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
    Q_PROPERTY(QString isolatedEmbeddingRuntimeStatus READ isolatedEmbeddingRuntimeStatus NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString isolatedEmbeddingRuntimeHealth READ isolatedEmbeddingRuntimeHealth NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString isolatedEmbeddingRuntimeReadiness READ isolatedEmbeddingRuntimeReadiness
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString isolatedEmbeddingRuntimeSummary READ isolatedEmbeddingRuntimeSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString isolatedEmbeddingRuntimeBoundedState READ
                   isolatedEmbeddingRuntimeBoundedState NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QStringList isolatedEmbeddingRuntimeChecks READ isolatedEmbeddingRuntimeChecks NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(
        QString vectorPersistenceStatus READ vectorPersistenceStatus NOTIFY contextAssemblyChanged)
    Q_PROPERTY(
        QString vectorPersistenceHealth READ vectorPersistenceHealth NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString vectorPersistenceReadiness READ vectorPersistenceReadiness NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString vectorPersistenceSummary READ vectorPersistenceSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString vectorPersistenceBoundedState READ vectorPersistenceBoundedState NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int vectorPersistenceIndexedItemCount READ vectorPersistenceIndexedItemCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QStringList vectorPersistenceChecks READ vectorPersistenceChecks NOTIFY
                   contextAssemblyChanged)
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
    Q_PROPERTY(QString semanticAcceptanceStatus READ semanticAcceptanceStatus NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticAcceptanceReadiness READ semanticAcceptanceReadiness NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticAcceptanceSummary READ semanticAcceptanceSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticAcceptanceBudgetSummary READ semanticAcceptanceBudgetSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticAcceptanceSourceSummary READ semanticAcceptanceSourceSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticAcceptanceArbitrationSummary READ
                   semanticAcceptanceArbitrationSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticAcceptanceFallbackSummary READ semanticAcceptanceFallbackSummary
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int semanticAcceptanceAcceptedCount READ semanticAcceptanceAcceptedCount NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int semanticAcceptanceBudgetCharacters READ semanticAcceptanceBudgetCharacters NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QStringList semanticAcceptanceCandidateSummaries READ
                   semanticAcceptanceCandidateSummaries NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QStringList semanticAcceptanceChecks READ semanticAcceptanceChecks NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticSupplementAssemblyStatus READ semanticSupplementAssemblyStatus NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticSupplementAssemblyReadiness READ semanticSupplementAssemblyReadiness
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticSupplementAssemblySummary READ semanticSupplementAssemblySummary
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticSupplementAssemblyBudgetSummary READ
                   semanticSupplementAssemblyBudgetSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticSupplementAssemblySafetySummary READ
                   semanticSupplementAssemblySafetySummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int semanticSupplementAssemblyBlockCount READ semanticSupplementAssemblyBlockCount
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int semanticSupplementAssemblyBudgetCharacters READ
                   semanticSupplementAssemblyBudgetCharacters NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QStringList semanticSupplementAssemblyChecks READ semanticSupplementAssemblyChecks
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticPromptAuthorityStatus READ semanticPromptAuthorityStatus NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticPromptAuthorityDecisionSummary READ
                   semanticPromptAuthorityDecisionSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticPromptAuthoritySafetySummary READ
                   semanticPromptAuthoritySafetySummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticPromptAuthorityReadinessSummary READ
                   semanticPromptAuthorityReadinessSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticPromptAuthorityFallbackSummary READ
                   semanticPromptAuthorityFallbackSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticPromptAuthorityAuditSummary READ semanticPromptAuthorityAuditSummary
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(int semanticPromptAuthorityWouldIncludeBlockCount READ
                   semanticPromptAuthorityWouldIncludeBlockCount NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QStringList semanticPromptAuthorityChecks READ semanticPromptAuthorityChecks NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(bool semanticPromptInclusionEnabled READ semanticPromptInclusionEnabled WRITE
                   setSemanticPromptInclusionEnabled NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticPromptInclusionStatus READ semanticPromptInclusionStatus NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(QString semanticPromptInclusionSummary READ semanticPromptInclusionSummary NOTIFY
                   contextAssemblyChanged)
    Q_PROPERTY(int semanticPromptInclusionIncludedCount READ semanticPromptInclusionIncludedCount
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticPromptInclusionBudgetSummary READ
                   semanticPromptInclusionBudgetSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticPromptInclusionFallbackSummary READ
                   semanticPromptInclusionFallbackSummary NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QString semanticPromptInclusionAuditSummary READ semanticPromptInclusionAuditSummary
                   NOTIFY contextAssemblyChanged)
    Q_PROPERTY(
        bool semanticPromptInclusionDeterministicAuthorityPreserved READ
            semanticPromptInclusionDeterministicAuthorityPreserved NOTIFY contextAssemblyChanged)
    Q_PROPERTY(QStringList semanticPromptInclusionChecks READ semanticPromptInclusionChecks NOTIFY
                   contextAssemblyChanged)
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
    Q_PROPERTY(int availableToolCount READ availableToolCount NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList availableToolIds READ availableToolIds NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString memoryStatus READ memoryStatus NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString chatHistoryStatus READ chatHistoryStatus NOTIFY runtimeProjectionChanged)
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
    Q_PROPERTY(QString conversationCurrentStorageMode READ conversationCurrentStorageMode NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString conversationFutureStorageMode READ conversationFutureStorageMode NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString conversationMigrationReadiness READ conversationMigrationReadiness NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString conversationMigrationStatusSummary READ conversationMigrationStatusSummary
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString conversationSchemaStatusSummary READ conversationSchemaStatusSummary NOTIFY
                   runtimeProjectionChanged)
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
    Q_PROPERTY(bool conversationExportAvailable READ conversationExportAvailable NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString conversationExportReadinessStatus READ conversationExportReadinessStatus
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString conversationExportReadinessSummary READ conversationExportReadinessSummary
                   NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList conversationExportReadinessChecks READ conversationExportReadinessChecks
                   NOTIFY runtimeProjectionChanged)
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
    Q_PROPERTY(QString memoryRecallPolicyStatus READ memoryRecallPolicyStatus NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString memoryRecallPolicySummary READ memoryRecallPolicySummary NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString memoryRecallQueryText READ memoryRecallQueryText NOTIFY memoryRecallChanged)
    Q_PROPERTY(QString memoryRecallStatus READ memoryRecallStatus NOTIFY memoryRecallChanged)
    Q_PROPERTY(
        QString memoryRecallSummaryText READ memoryRecallSummaryText NOTIFY memoryRecallChanged)
    Q_PROPERTY(int memoryRecallResultCount READ memoryRecallResultCount NOTIFY memoryRecallChanged)
    Q_PROPERTY(QStringList memoryRecallResultSummaries READ memoryRecallResultSummaries NOTIFY
                   memoryRecallChanged)
    Q_PROPERTY(QString contextAssemblyPolicyStatus READ contextAssemblyPolicyStatus NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QString contextAssemblyPolicySummary READ contextAssemblyPolicySummary NOTIFY
                   runtimeProjectionChanged)
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
    Q_PROPERTY(QString memoryMaintenanceStatus READ memoryMaintenanceStatus NOTIFY
                   maintenanceStatusChanged)
    Q_PROPERTY(
        QString chatMaintenanceStatus READ chatMaintenanceStatus NOTIFY maintenanceStatusChanged)
    Q_PROPERTY(QString currentModeName READ currentModeName WRITE setCurrentModeName NOTIFY
                   currentModeChanged)
    Q_PROPERTY(QStringList availableModes READ availableModes NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QString currentPage READ currentPage WRITE setCurrentPage NOTIFY currentPageChanged)
    Q_PROPERTY(QStringList availablePages READ availablePages NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(ChatMessageListModel* chatMessages READ chatMessages NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList memoryEntries READ memoryEntries NOTIFY memoryEntriesChanged)
    Q_PROPERTY(QString themeName READ themeName WRITE setThemeName NOTIFY themeNameChanged)
    Q_PROPERTY(QString configurationProfile READ configurationProfile WRITE setConfigurationProfile
                   NOTIFY configurationProfileChanged)
    Q_PROPERTY(QString appLanguage READ appLanguage WRITE setAppLanguage NOTIFY appLanguageChanged)
    Q_PROPERTY(
        QStringList availableLanguages READ availableLanguages NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(bool companionEnabled READ companionEnabled WRITE setCompanionEnabled NOTIFY
                   companionChanged)
    Q_PROPERTY(bool companionAvailable READ companionAvailable NOTIFY companionChanged)
    Q_PROPERTY(bool companionPaused READ companionPaused NOTIFY companionChanged)
    Q_PROPERTY(bool companionChatVisible READ companionChatVisible WRITE setCompanionChatVisible
                   NOTIFY companionChatVisibleChanged)
    Q_PROPERTY(QString companionStatus READ companionStatus NOTIFY companionChanged)
    Q_PROPERTY(QString companionAvailability READ companionAvailability NOTIFY companionChanged)
    Q_PROPERTY(QString companionPlatformCapability READ companionPlatformCapability NOTIFY
                   companionChanged)
    Q_PROPERTY(
        QString companionPermissionPosture READ companionPermissionPosture NOTIFY companionChanged)
    Q_PROPERTY(QString companionSafetyBoundary READ companionSafetyBoundary NOTIFY companionChanged)
    Q_PROPERTY(QString companionQuickCaptureSummary READ companionQuickCaptureSummary NOTIFY
                   companionChanged)
    Q_PROPERTY(
        QStringList companionActionSummaries READ companionActionSummaries NOTIFY companionChanged)
    Q_PROPERTY(QStringList companionPlatformSummaries READ companionPlatformSummaries NOTIFY
                   companionChanged)
    Q_PROPERTY(
        QStringList companionTraceSummaries READ companionTraceSummaries NOTIFY companionChanged)
    Q_PROPERTY(bool developerModeEnabled READ developerModeEnabled WRITE setDeveloperModeEnabled
                   NOTIFY developerModeChanged)
    Q_PROPERTY(bool agentAutonomousMode READ agentAutonomousMode WRITE setAgentAutonomousMode NOTIFY
                   agentAutonomousModeChanged)
    Q_PROPERTY(QString updateCheckPolicy READ updateCheckPolicy WRITE setUpdateCheckPolicy NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(QString updateCheckUrl READ updateCheckUrl WRITE setUpdateCheckUrl NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(QString notificationPolicy READ notificationPolicy WRITE setNotificationPolicy NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(bool onboardingComplete READ onboardingComplete WRITE setOnboardingComplete NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(int onboardingStepIndex READ onboardingStepIndex NOTIFY onboardingStateChanged)
    Q_PROPERTY(QString onboardingProcessingMode READ onboardingProcessingMode WRITE
                   setOnboardingProcessingMode NOTIFY onboardingStateChanged)
    Q_PROPERTY(QString onboardingErrorText READ onboardingErrorText NOTIFY onboardingStateChanged)
    Q_PROPERTY(QVariantMap onboardingSnapshot READ onboardingState NOTIFY onboardingStateChanged)
    Q_PROPERTY(QString onboardingUseCase READ onboardingUseCase WRITE setOnboardingUseCase NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(QString onboardingAiProvider READ onboardingAiProvider WRITE setOnboardingAiProvider
                   NOTIFY nativeExperienceChanged)
    Q_PROPERTY(QString recoveryDraftText READ recoveryDraftText WRITE setRecoveryDraftText NOTIFY
                   recoveryDraftTextChanged)
    Q_PROPERTY(bool reducedMotionEnabled READ reducedMotionEnabled WRITE setReducedMotionEnabled
                   NOTIFY nativeExperienceChanged)
    Q_PROPERTY(bool highContrastEnabled READ highContrastEnabled WRITE setHighContrastEnabled NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(QString uiDensity READ uiDensity WRITE setUiDensity NOTIFY nativeExperienceChanged)
    Q_PROPERTY(bool notifyModelDownloads READ notifyModelDownloads WRITE setNotifyModelDownloads
                   NOTIFY nativeExperienceChanged)
    Q_PROPERTY(bool notifyModelRemovals READ notifyModelRemovals WRITE setNotifyModelRemovals NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(bool notifyAgentResponses READ notifyAgentResponses WRITE setNotifyAgentResponses
                   NOTIFY nativeExperienceChanged)
    Q_PROPERTY(bool notifySystemUpdates READ notifySystemUpdates WRITE setNotifySystemUpdates NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(bool soundEffectsEnabled READ soundEffectsEnabled WRITE setSoundEffectsEnabled NOTIFY
                   soundEffectsEnabledChanged)
    Q_PROPERTY(
        QString openAiApiKey READ openAiApiKey WRITE setOpenAiApiKey NOTIFY cloudApiKeysChanged)
    Q_PROPERTY(
        QString claudeApiKey READ claudeApiKey WRITE setClaudeApiKey NOTIFY cloudApiKeysChanged)
    Q_PROPERTY(
        QString geminiApiKey READ geminiApiKey WRITE setGeminiApiKey NOTIFY cloudApiKeysChanged)
    Q_PROPERTY(QString deepseekApiKey READ deepseekApiKey WRITE setDeepseekApiKey NOTIFY
                   cloudApiKeysChanged)
    Q_PROPERTY(QString groqApiKey READ groqApiKey WRITE setGroqApiKey NOTIFY cloudApiKeysChanged)
    Q_PROPERTY(
        QString mistralApiKey READ mistralApiKey WRITE setMistralApiKey NOTIFY cloudApiKeysChanged)
    Q_PROPERTY(QString selectedCloudProvider READ selectedCloudProvider WRITE
                   setSelectedCloudProvider NOTIFY selectedCloudProviderChanged)
    Q_PROPERTY(QString webSearchProvider READ webSearchProvider WRITE setWebSearchProvider NOTIFY
                   webSearchSettingsChanged)
    Q_PROPERTY(QString webSearchApiKey READ webSearchApiKey WRITE setWebSearchApiKey NOTIFY
                   webSearchSettingsChanged)
    Q_PROPERTY(int webSearchMaxResults READ webSearchMaxResults WRITE setWebSearchMaxResults NOTIFY
                   webSearchSettingsChanged)
    Q_PROPERTY(QString semanticProvider READ semanticProvider WRITE setSemanticProvider NOTIFY
                   semanticSettingsChanged)
    Q_PROPERTY(QString semanticEmbeddingModel READ semanticEmbeddingModel WRITE
                   setSemanticEmbeddingModel NOTIFY semanticSettingsChanged)
    Q_PROPERTY(QStringList activityTimelineSummaries READ activityTimelineSummaries NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(QStringList notificationCenterSummaries READ notificationCenterSummaries NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(QStringList notificationCategories READ notificationCategories NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(QStringList notificationLifecycleSummaries READ notificationLifecycleSummaries NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(QStringList notificationFilteredSummaries READ notificationFilteredSummaries NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(
        int unreadNotificationCount READ unreadNotificationCount NOTIFY nativeExperienceChanged)
    Q_PROPERTY(QString notificationSearchQuery READ notificationSearchQuery WRITE
                   setNotificationSearchQuery NOTIFY nativeExperienceChanged)
    Q_PROPERTY(QString notificationCategoryFilter READ notificationCategoryFilter WRITE
                   setNotificationCategoryFilter NOTIFY nativeExperienceChanged)
    Q_PROPERTY(bool notificationCenterVisible READ notificationCenterVisible WRITE
                   setNotificationCenterVisible NOTIFY notificationCenterVisibleChanged)
    Q_PROPERTY(QString updateWorkflowState READ updateWorkflowState NOTIFY nativeExperienceChanged)
    Q_PROPERTY(QString lastAssetUrl READ lastAssetUrl NOTIFY nativeExperienceChanged)
    Q_PROPERTY(
        QStringList releaseNotesSummaries READ releaseNotesSummaries NOTIFY nativeExperienceChanged)
    Q_PROPERTY(QStringList aboutSentinelSummaries READ aboutSentinelSummaries NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(QStringList accessibilitySummaries READ accessibilitySummaries NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(QStringList diagnosticsCenterSummaries READ diagnosticsCenterSummaries NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(QStringList exportPreviewSummaries READ exportPreviewSummaries NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(
        QStringList brainInsightSummaries READ brainInsightSummaries NOTIFY nativeExperienceChanged)
    Q_PROPERTY(QStringList recoveryReliabilitySummaries READ recoveryReliabilitySummaries NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(QStringList productPolishSummaries READ productPolishSummaries NOTIFY
                   nativeExperienceChanged)
    Q_PROPERTY(QString selectedSkillProfile READ selectedSkillProfile WRITE setSelectedSkillProfile
                   NOTIFY skillProfileChanged)
    Q_PROPERTY(
        QString selectedSkillProfileName READ selectedSkillProfileName NOTIFY skillProfileChanged)
    Q_PROPERTY(QString selectedSkillProfileSummary READ selectedSkillProfileSummary NOTIFY
                   skillProfileChanged)
    Q_PROPERTY(QString selectedSkillProfileDescription READ selectedSkillProfileDescription NOTIFY
                   skillProfileChanged)
    Q_PROPERTY(QString selectedSkillProfileReadiness READ selectedSkillProfileReadiness NOTIFY
                   skillProfileChanged)
    Q_PROPERTY(QString selectedSkillProfilePolicyPosture READ selectedSkillProfilePolicyPosture
                   NOTIFY skillProfileChanged)
    Q_PROPERTY(QStringList skillProfileIds READ skillProfileIds NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList skillProfileNames READ skillProfileNames NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList skillProfileSummaries READ skillProfileSummaries NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList skillProfileCapabilitySummaries READ skillProfileCapabilitySummaries
                   NOTIFY skillProfileChanged)
    Q_PROPERTY(QStringList skillProfileReadinessChecks READ skillProfileReadinessChecks NOTIFY
                   skillProfileChanged)
    Q_PROPERTY(QStringList skillProfileDeveloperDiagnostics READ skillProfileDeveloperDiagnostics
                   NOTIFY skillProfileChanged)
    Q_PROPERTY(QString selectedWorkspaceId READ selectedWorkspaceId WRITE setSelectedWorkspaceId
                   NOTIFY workspaceChanged)
    Q_PROPERTY(QString selectedWorkspaceName READ selectedWorkspaceName NOTIFY workspaceChanged)
    Q_PROPERTY(QString selectedWorkspaceAccessState READ selectedWorkspaceAccessState NOTIFY
                   workspaceChanged)
    Q_PROPERTY(
        QString workspacePermissionPosture READ workspacePermissionPosture NOTIFY workspaceChanged)
    Q_PROPERTY(QString selectedWorkspaceRootSummary READ selectedWorkspaceRootSummary NOTIFY
                   workspaceChanged)
    Q_PROPERTY(
        QString selectedWorkspaceRootPath READ selectedWorkspaceRootPath NOTIFY workspaceChanged)
    Q_PROPERTY(
        QString workspaceReadinessStatus READ workspaceReadinessStatus NOTIFY workspaceChanged)
    Q_PROPERTY(
        QString workspaceReadinessSummary READ workspaceReadinessSummary NOTIFY workspaceChanged)
    Q_PROPERTY(
        QString workspacePermissionSummary READ workspacePermissionSummary NOTIFY workspaceChanged)
    Q_PROPERTY(QStringList workspaceIds READ workspaceIds NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList workspaceNames READ workspaceNames NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(
        QStringList workspaceSummaries READ workspaceSummaries NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList workspacePermissionPostures READ workspacePermissionPostures NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList workspaceActionPlaceholders READ workspaceActionPlaceholders NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(
        QStringList workspaceReadinessChecks READ workspaceReadinessChecks NOTIFY workspaceChanged)
    Q_PROPERTY(QStringList workspaceBoundaryDiagnostics READ workspaceBoundaryDiagnostics NOTIFY
                   workspaceChanged)
    Q_PROPERTY(QStringList workspaceTemplateNames READ workspaceTemplateNames NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(
        QString workspaceLastActionStatus READ workspaceLastActionStatus NOTIFY workspaceChanged)
    Q_PROPERTY(
        QString workspaceLastActionSummary READ workspaceLastActionSummary NOTIFY workspaceChanged)
    Q_PROPERTY(QStringList attachmentSummaries READ attachmentSummaries NOTIFY attachmentChanged)
    Q_PROPERTY(QString attachmentStatus READ attachmentStatus NOTIFY attachmentChanged)
    Q_PROPERTY(
        QString attachmentPreviewSummary READ attachmentPreviewSummary NOTIFY attachmentChanged)
    Q_PROPERTY(
        QStringList fileChatActionSummaries READ fileChatActionSummaries NOTIFY attachmentChanged)
    Q_PROPERTY(bool localKnowledgeBaseEnabled READ localKnowledgeBaseEnabled WRITE
                   setLocalKnowledgeBaseEnabled NOTIFY workspaceChanged)
    Q_PROPERTY(QString attachmentBehavior READ attachmentBehavior WRITE setAttachmentBehavior NOTIFY
                   workspaceChanged)
    Q_PROPERTY(QString exportDefaultFormat READ exportDefaultFormat WRITE setExportDefaultFormat
                   NOTIFY workspaceChanged)
    Q_PROPERTY(bool exportIncludeTimestamps READ exportIncludeTimestamps WRITE
                   setExportIncludeTimestamps NOTIFY workspaceChanged)
    Q_PROPERTY(bool exportIncludeCitations READ exportIncludeCitations WRITE
                   setExportIncludeCitations NOTIFY workspaceChanged)
    Q_PROPERTY(bool exportAnonymizeNames READ exportAnonymizeNames WRITE setExportAnonymizeNames
                   NOTIFY workspaceChanged)
    Q_PROPERTY(bool exportIncludeModelMetadata READ exportIncludeModelMetadata WRITE
                   setExportIncludeModelMetadata NOTIFY workspaceChanged)
    Q_PROPERTY(
        QString localKnowledgeBaseStatus READ localKnowledgeBaseStatus NOTIFY workspaceChanged)
    Q_PROPERTY(QStringList knowledgeBaseDocumentSummaries READ knowledgeBaseDocumentSummaries NOTIFY
                   workspaceChanged)
    Q_PROPERTY(
        QStringList recentRetrievalSummaries READ recentRetrievalSummaries NOTIFY workspaceChanged)
    Q_PROPERTY(QStringList retrievalExplainabilitySummaries READ retrievalExplainabilitySummaries
                   NOTIFY workspaceChanged)
    Q_PROPERTY(
        QStringList brainWorkspaceSummaries READ brainWorkspaceSummaries NOTIFY workspaceChanged)
    Q_PROPERTY(QStringList exportCenterSummaries READ exportCenterSummaries NOTIFY workspaceChanged)
    Q_PROPERTY(
        QStringList privacyCenterSummaries READ privacyCenterSummaries NOTIFY workspaceChanged)
    Q_PROPERTY(QString defaultPermissionPolicyState READ defaultPermissionPolicyState WRITE
                   setDefaultPermissionPolicyState NOTIFY permissionPolicyChanged)
    Q_PROPERTY(
        QString permissionPolicyStatus READ permissionPolicyStatus NOTIFY permissionPolicyChanged)
    Q_PROPERTY(
        QString permissionPolicySummary READ permissionPolicySummary NOTIFY permissionPolicyChanged)
    Q_PROPERTY(QStringList permissionPolicyStateLabels READ permissionPolicyStateLabels NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList permissionPolicyDomainIds READ permissionPolicyDomainIds NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList permissionPolicyDomainNames READ permissionPolicyDomainNames NOTIFY
                   runtimeProjectionChanged)
    Q_PROPERTY(QStringList permissionPolicyDomainSummaries READ permissionPolicyDomainSummaries
                   NOTIFY permissionPolicyChanged)
    Q_PROPERTY(QStringList permissionPolicyDeveloperDiagnostics READ
                   permissionPolicyDeveloperDiagnostics NOTIFY permissionPolicyChanged)
    Q_PROPERTY(QString toolGatewayStatus READ toolGatewayStatus NOTIFY permissionPolicyChanged)
    Q_PROPERTY(QString toolGatewaySummary READ toolGatewaySummary NOTIFY permissionPolicyChanged)
    Q_PROPERTY(QString toolGatewayPermissionPosture READ toolGatewayPermissionPosture NOTIFY
                   permissionPolicyChanged)
    Q_PROPERTY(int toolGatewayToolCount READ toolGatewayToolCount NOTIFY permissionPolicyChanged)
    Q_PROPERTY(int toolGatewayMetadataSafeCount READ toolGatewayMetadataSafeCount NOTIFY
                   permissionPolicyChanged)
    Q_PROPERTY(int toolGatewayUnavailableCount READ toolGatewayUnavailableCount NOTIFY
                   permissionPolicyChanged)
    Q_PROPERTY(
        int toolGatewayRefusedCount READ toolGatewayRefusedCount NOTIFY permissionPolicyChanged)
    Q_PROPERTY(QStringList toolGatewayToolSummaries READ toolGatewayToolSummaries NOTIFY
                   permissionPolicyChanged)
    Q_PROPERTY(QStringList toolGatewayDeveloperDiagnostics READ toolGatewayDeveloperDiagnostics
                   NOTIFY permissionPolicyChanged)
    Q_PROPERTY(QString agentRuntimeStatus READ agentRuntimeStatus NOTIFY agentRuntimeChanged)
    Q_PROPERTY(QString agentRuntimeSummary READ agentRuntimeSummary NOTIFY agentRuntimeChanged)
    Q_PROPERTY(QString agentRuntimeApprovalPosture READ agentRuntimeApprovalPosture NOTIFY
                   agentRuntimeChanged)
    Q_PROPERTY(int agentRuntimeAgentCount READ agentRuntimeAgentCount NOTIFY agentRuntimeChanged)
    Q_PROPERTY(
        int agentRuntimeReadyAgentCount READ agentRuntimeReadyAgentCount NOTIFY agentRuntimeChanged)
    Q_PROPERTY(int agentRuntimeRefusedAgentCount READ agentRuntimeRefusedAgentCount NOTIFY
                   agentRuntimeChanged)
    Q_PROPERTY(QStringList agentRuntimeAgentSummaries READ agentRuntimeAgentSummaries NOTIFY
                   agentRuntimeChanged)
    Q_PROPERTY(QStringList agentRuntimeReadinessSummaries READ agentRuntimeReadinessSummaries NOTIFY
                   agentRuntimeChanged)
    Q_PROPERTY(QStringList agentRuntimeDeveloperDiagnostics READ agentRuntimeDeveloperDiagnostics
                   NOTIFY agentRuntimeChanged)
    Q_PROPERTY(QString agentPlanId READ agentPlanId NOTIFY agentRuntimeChanged)
    Q_PROPERTY(QString agentPlanGoalSummary READ agentPlanGoalSummary NOTIFY agentRuntimeChanged)
    Q_PROPERTY(QStringList agentPlanSteps READ agentPlanSteps NOTIFY agentRuntimeChanged)
    Q_PROPERTY(
        QStringList agentPlanRequiredTools READ agentPlanRequiredTools NOTIFY agentRuntimeChanged)
    Q_PROPERTY(QStringList agentPlanRequiredPermissions READ agentPlanRequiredPermissions NOTIFY
                   agentRuntimeChanged)
    Q_PROPERTY(
        QString agentPlanEstimatedRisk READ agentPlanEstimatedRisk NOTIFY agentRuntimeChanged)
    Q_PROPERTY(
        QString agentPlanApprovalState READ agentPlanApprovalState NOTIFY agentRuntimeChanged)
    Q_PROPERTY(
        QString agentPlanRefusalReason READ agentPlanRefusalReason NOTIFY agentRuntimeChanged)
    Q_PROPERTY(
        QStringList agentPlanDiagnostics READ agentPlanDiagnostics NOTIFY agentRuntimeChanged)
    Q_PROPERTY(QString controlledTaskActiveSummary READ controlledTaskActiveSummary NOTIFY
                   controlledAgentTasksChanged)
    Q_PROPERTY(bool controlledTaskSessionActive READ controlledTaskSessionActive NOTIFY
                   controlledAgentTasksChanged)
    Q_PROPERTY(QString controlledTaskCurrentStep READ controlledTaskCurrentStep NOTIFY
                   controlledAgentTasksChanged)
    Q_PROPERTY(QString controlledTaskProgressSummary READ controlledTaskProgressSummary NOTIFY
                   controlledAgentTasksChanged)
    Q_PROPERTY(QStringList controlledTaskPlanSteps READ controlledTaskPlanSteps NOTIFY
                   controlledAgentTasksChanged)
    Q_PROPERTY(QStringList controlledTaskQueueSummaries READ controlledTaskQueueSummaries NOTIFY
                   controlledAgentTasksChanged)
    Q_PROPERTY(QStringList controlledTaskTimelineSummaries READ controlledTaskTimelineSummaries
                   NOTIFY controlledAgentTasksChanged)
    Q_PROPERTY(QStringList controlledTaskExplainabilitySummaries READ
                   controlledTaskExplainabilitySummaries NOTIFY controlledAgentTasksChanged)
    Q_PROPERTY(QStringList controlledTaskPermissionSummaries READ controlledTaskPermissionSummaries
                   NOTIFY controlledAgentTasksChanged)
    Q_PROPERTY(QStringList controlledTaskNotificationCategories READ
                   controlledTaskNotificationCategories NOTIFY runtimeProjectionChanged)
    Q_PROPERTY(QStringList controlledTaskExportSummaries READ controlledTaskExportSummaries NOTIFY
                   controlledAgentTasksChanged)
    Q_PROPERTY(QString controlledTaskDiagnosticsSummary READ controlledTaskDiagnosticsSummary NOTIFY
                   controlledAgentTasksChanged)
    Q_PROPERTY(QStringList controlledTaskSafetyGuarantees READ controlledTaskSafetyGuarantees NOTIFY
                   runtimeProjectionChanged)

public:
    DesktopShellViewModel(core::ApplicationController& controller, core::ModeManager& modeManager,
                          core::AppSettings& settings,
                          core::WinTaskbarIntegration* taskbar = nullptr,
                          QObject* parent = nullptr);

    DesktopShellViewModel(DesktopRuntimeClient& client, core::ModeManager& modeManager,
                          core::AppSettings& settings,
                          core::WinTaskbarIntegration* taskbar = nullptr,
                          QObject* parent = nullptr);
    Q_INVOKABLE bool respondToDaemonApproval(bool allow);
    Q_INVOKABLE bool cancelDaemonRun();
    QVariantMap pendingDaemonApproval() const;
    Q_PROPERTY(QString voiceInputSource READ voiceInputSource WRITE setVoiceInputSource NOTIFY voiceInputChanged)
    Q_PROPERTY(QString voiceInputStatus READ voiceInputStatus NOTIFY voiceInputChanged)
    Q_PROPERTY(QString attachmentError READ attachmentError NOTIFY attachmentChanged)
    QString voiceInputSource() const;
    void setVoiceInputSource(const QString& source);
    Q_PROPERTY(QString voiceInputMessage READ voiceInputMessage NOTIFY voiceInputChanged)
    QString voiceInputStatus() const { return voiceInputStatus_; }
    QString voiceInputMessage() const { return voiceStatusMessage(voiceInputStatus_); }
    Q_INVOKABLE QString voiceStatusMessage(const QString& status) const;
    QString attachmentError() const { return attachmentError_; }
    QString daemonConnectionStatus() const;
    bool daemonConnected() const;
    QString providerName() const;
    QString providerStatus() const;
    QString agentStatus() const;
    bool agentLoopActive() const;
    bool agentAwaitingApproval() const;
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
    Q_INVOKABLE void setRoutingModeByName(const QString& routingModeName);
    QStringList availableRoutingModes() const;
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
    QString orchestrationSnapshotStatus() const;
    QString orchestrationSnapshotSummary() const;
    QStringList orchestrationSignals() const;
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
    QString cloudApiEndpoint() const;
    void setCloudApiEndpoint(const QString& endpoint);
    QString ollamaConnectionStatus() const;
    QString ollamaHealthStatus() const;
    QString ollamaHealthSummary() const;
    QString ollamaDiscoveryStatus() const;
    QString localInferenceHealthSummary() const;
    QString activeLocalModelName() const;
    QString selectedOllamaModel() const;
    void setSelectedOllamaModel(const QString& modelName);
    bool proxyEnabled() const;
    void setProxyEnabled(bool enabled);
    QString proxyType() const;
    void setProxyType(const QString& type);
    QString proxyHost() const;
    void setProxyHost(const QString& host);
    int proxyPort() const;
    void setProxyPort(int port);
    QString proxyUser() const;
    void setProxyUser(const QString& user);
    QString proxyPassword() const;
    bool proxyPasswordConfigured() const;
    void setProxyPassword(const QString& password);
    int ollamaModelCount() const;
    bool isOnline() const;
    QStringList ollamaModelNames() const;
    QString cloudModelDiscoveryError() const;
    QStringList installedOllamaModelNames() const;
    QStringList loadedLMStudioModelNames() const;
    QStringList ollamaModelSummaries() const;

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
    QStringList modelRoleIds() const;
    QStringList modelRoleAssignmentSummaries() const;
    Q_INVOKABLE void assignModelRole(const QString& roleId, const QString& modelId);
    QStringList modelAdvisorRecommendationSummaries() const;
    QStringList modelAdvisorAvoidSummaries() const;
    QStringList downloadsCenterSummaries() const;
    QStringList benchmarkHubSummaries() const;
    QStringList selectedModelCapabilityLabels() const;
    QVariantList modelCapabilitySettings() const;
    QString modelCapabilityCompatibilitySummary() const;
    Q_INVOKABLE bool setModelCapabilityOverride(const QString& capabilityId, const QString& value);
    Q_INVOKABLE bool setModelCapabilityNumberOverride(const QString& capabilityId, int value);
    Q_INVOKABLE void resetModelCapabilityOverrides();
    QString modelManagementStatus() const;
    QString modelManagementSummary() const;
    QString modelManagementActionAvailability() const;
    QStringList modelRecommendationSummaries() const;
    QStringList modelRequirementSummaries() const;
    QString voiceRuntimeMode() const;
    bool voiceEnabled() const;
    bool voiceRecordingActive() const;
    QString voiceReadinessStatus() const;
    QString voiceReadinessSummary() const;
    QStringList voiceReadinessChecks() const;
    QStringList voiceCapabilitySummaries() const;
    QString textToSpeechStatus() const;
    QString textToSpeechSummary() const;
    QString speechToTextStatus() const;
    QString speechToTextSummary() const;
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
    QString selectedTtsEngine() const;
    void setSelectedTtsEngine(const QString& engine);
    QString kokoroModelPath() const;
    void setKokoroModelPath(const QString& path);
    QString kokoroVoice() const;
    void setKokoroVoice(const QString& voice);
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
    QVariantMap chatInputCapabilities() const;
    bool promptContextInjectionEnabled() const;
    void setPromptContextInjectionEnabled(bool enabled);
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
    QString conversationWindowStatus() const;
    QString conversationWindowSummary() const;
    QString conversationWindowBudgetSummary() const;
    int conversationWindowBudgetCharacters() const;
    int conversationWindowIncludedMessageCount() const;
    int conversationWindowTruncatedMessageCount() const;
    int conversationWindowOmittedMessageCount() const;
    QString conversationSummaryStatus() const;
    QString conversationSummaryText() const;
    QString conversationSummaryBudgetSummary() const;
    int conversationSummaryBudgetCharacters() const;
    int conversationSummaryBlockCount() const;
    int conversationSummaryMessageCount() const;
    int conversationSummaryOmittedMessageCount() const;
    int conversationSummaryTruncatedBlockCount() const;
    QStringList conversationSummaryBlockSummaries() const;
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
    bool contextExplainabilityEnabled() const;
    bool contextExplainabilityVisible() const;
    void setContextExplainabilityVisible(bool visible);
    QString openAiApiKey() const;
    void setOpenAiApiKey(const QString& key);
    QString claudeApiKey() const;
    void setClaudeApiKey(const QString& key);
    QString geminiApiKey() const;
    void setGeminiApiKey(const QString& key);
    QString deepseekApiKey() const;
    void setDeepseekApiKey(const QString& key);
    QString groqApiKey() const;
    void setGroqApiKey(const QString& key);
    QString mistralApiKey() const;
    void setMistralApiKey(const QString& key);
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
    core::SemanticRetrievalPolicy semanticRetrievalPolicy() const;
    bool semanticRetrievalEnabled() const;
    QString semanticRetrievalStatus() const;
    QString semanticRetrievalSummary() const;
    QString semanticReadiness() const;
    QString embeddingProviderReadiness() const;
    QString embeddingProviderSummary() const;
    QString vectorIndexReadiness() const;
    QString vectorIndexSummary() const;
    int vectorIndexedItemCount() const;
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
    QString semanticCandidateStatus() const;
    QString semanticCandidateSummary() const;
    QString semanticCandidateBudgetSummary() const;
    QString semanticCandidateArbitrationSummary() const;
    int semanticCandidateCount() const;
    int semanticCandidateSelectedCount() const;
    int semanticCandidateExcludedCount() const;
    int semanticCandidateTruncatedCount() const;
    QStringList semanticCandidateParticipationSummaries() const;
    QString hybridRetrievalStatus() const;
    QString hybridRetrievalReadiness() const;
    QString hybridRetrievalSummary() const;
    QStringList hybridRetrievalReadinessChecks() const;
    QString semanticArbitrationStatus() const;
    QString semanticArbitrationReadiness() const;
    QString semanticArbitrationSummary() const;
    QString semanticArbitrationBudgetSummary() const;
    QStringList semanticArbitrationSelectionSummaries() const;
    QStringList semanticArbitrationChecks() const;
    QString embeddingRuntimeReadiness() const;
    QString embeddingRuntimeSummary() const;
    QString embeddingRuntimeBudgetSummary() const;
    QStringList embeddingRuntimeRequirementSummaries() const;
    QStringList embeddingRuntimeConstraintSummaries() const;
    QString isolatedEmbeddingRuntimeStatus() const;
    QString isolatedEmbeddingRuntimeHealth() const;
    QString isolatedEmbeddingRuntimeReadiness() const;
    QString isolatedEmbeddingRuntimeSummary() const;
    QString isolatedEmbeddingRuntimeBoundedState() const;
    QStringList isolatedEmbeddingRuntimeChecks() const;
    QString vectorPersistenceStatus() const;
    QString vectorPersistenceHealth() const;
    QString vectorPersistenceReadiness() const;
    QString vectorPersistenceSummary() const;
    QString vectorPersistenceBoundedState() const;
    int vectorPersistenceIndexedItemCount() const;
    QStringList vectorPersistenceChecks() const;
    QString semanticSearchStatus() const;
    QString semanticSearchReadiness() const;
    QString semanticSearchSummary() const;
    QString semanticSearchBudgetSummary() const;
    QString semanticSearchRuntimeState() const;
    int semanticSearchCandidateCount() const;
    QString semanticSearchArbitrationSummary() const;
    QStringList semanticSearchCandidateSummaries() const;
    QStringList semanticSearchChecks() const;
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
    bool semanticPromptInclusionEnabled() const;
    void setSemanticPromptInclusionEnabled(bool enabled);
    QString semanticPromptInclusionStatus() const;
    QString semanticPromptInclusionSummary() const;
    int semanticPromptInclusionIncludedCount() const;
    QString semanticPromptInclusionBudgetSummary() const;
    QString semanticPromptInclusionFallbackSummary() const;
    QString semanticPromptInclusionAuditSummary() const;
    bool semanticPromptInclusionDeterministicAuthorityPreserved() const;
    QStringList semanticPromptInclusionChecks() const;
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
    QString conversationHistorySummaryText() const;
    QStringList conversationHistorySummaryLines() const;
    int conversationHistoryMessageCount() const;
    QString conversationPersistenceStatus() const;
    QString conversationLastSavedStatus() const;
    QString conversationLastRestoredStatus() const;
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
    QString conversationCurrentStorageMode() const;
    QString conversationFutureStorageMode() const;
    QString conversationMigrationReadiness() const;
    QString conversationMigrationStatusSummary() const;
    QString conversationSchemaStatusSummary() const;
    QString conversationSearchQueryText() const;
    QString conversationSearchStatus() const;
    QString conversationSearchSummaryText() const;
    int conversationSearchResultCount() const;
    QStringList conversationSearchResultSummaries() const;
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
    QString lastMemoryCandidateReviewStatus() const;
    QString lastMemoryCandidateReviewSummary() const;
    QString memoryCommitReadinessStatus() const;
    QString memoryCommitReadinessSummary() const;
    QStringList memoryCommitReadinessChecks() const;
    int memoryCommitPlanCount() const;
    QString memoryCommitTargetSummary() const;
    QStringList memoryCommitCandidateSummaries() const;
    QString lastMemoryCommitStatus() const;
    QString lastMemoryCommitResultSummary() const;
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
    QString currentModeName() const;
    void setCurrentModeName(const QString& modeName);
    QStringList availableModes() const;
    QString currentPage() const;
    void setCurrentPage(const QString& page);
    QStringList availablePages() const;
    ChatMessageListModel* chatMessages();
    QStringList memoryEntries() const;
    Q_PROPERTY(QStringList availableThemes READ availableThemes CONSTANT)
    QStringList availableThemes() const;
    QString themeName() const;
    void setThemeName(const QString& themeName);
    QString configurationProfile() const;
    void setConfigurationProfile(const QString& configurationProfile);
    QString appLanguage() const;
    void setAppLanguage(const QString& language);
    QStringList availableLanguages() const;
    Q_INVOKABLE QString languageDisplayName(const QString& language) const;
    Q_INVOKABLE QVariantMap getLocalModelDetails(const QString& modelName) const;
    Q_INVOKABLE bool checkRuntimeInstalled(const QString& runtimeId) const;
    bool companionEnabled() const;
    void setCompanionEnabled(bool enabled);
    bool companionAvailable() const;
    bool companionPaused() const;
    bool companionChatVisible() const;
    void setCompanionChatVisible(bool visible);
    Q_INVOKABLE void registerMainWindow(quintptr winId);
    Q_INVOKABLE void toggleCompanionChat();
    Q_INVOKABLE void openCompanionChat();
    Q_INVOKABLE void hideCompanionChat();
    void setCompanionNativeAvailable(bool available);
    Q_INVOKABLE void setCompanionPaused(bool paused);
    Q_INVOKABLE void toggleCompanionPause();
    QString companionStatus() const;
    QString companionAvailability() const;
    QString companionPlatformCapability() const;
    QString companionPermissionPosture() const;
    QString companionSafetyBoundary() const;
    QString companionQuickCaptureSummary() const;
    QStringList companionActionSummaries() const;
    QStringList companionPlatformSummaries() const;
    QStringList companionTraceSummaries() const;
    bool developerModeEnabled() const;
    void setDeveloperModeEnabled(bool enabled);
    bool agentAutonomousMode() const;
    void setAgentAutonomousMode(bool enabled);
    QString updateCheckPolicy() const;
    void setUpdateCheckPolicy(const QString& policy);
    QString updateCheckUrl() const;
    void setUpdateCheckUrl(const QString& url);
    QString notificationPolicy() const;
    void setNotificationPolicy(const QString& policy);
    bool onboardingComplete() const;
    void setOnboardingComplete(bool complete);
    int onboardingStepIndex() const;
    QString onboardingProcessingMode() const;
    QString onboardingErrorText() const;
    void setOnboardingProcessingMode(const QString& mode);
    Q_INVOKABLE bool advanceOnboarding(bool skip = false);
    Q_INVOKABLE bool backOnboarding();
    Q_INVOKABLE void reopenOnboarding();
    Q_INVOKABLE QVariantMap onboardingState() const;
    Q_INVOKABLE QVariantList productSettings() const;
    Q_INVOKABLE QStringList productSettingsSections() const;
    Q_INVOKABLE QVariantMap setProductSetting(const QString& id, const QVariant& value);
    Q_INVOKABLE QVariantMap resetProductSetting(const QString& id);
    Q_INVOKABLE QVariantList resetProductSettingsSection(int section);
    Q_INVOKABLE QVariantMap clearWorkspaceSettingOverride(const QString& workspaceId,
                                                          const QString& key);
    Q_INVOKABLE QVariantMap providerSettingsState(const QString& providerId) const;
    Q_INVOKABLE QVariantMap extensionsHubSnapshot() const;
    Q_INVOKABLE QVariantMap extensionsHubActionState() const;
    Q_INVOKABLE bool performExtensionsHubAction(const QString& id, const QString& action);
    Q_INVOKABLE QVariantMap extensionsHubDetail(const QString& id) const;
    Q_INVOKABLE void refreshExtensionsHub();
    Q_INVOKABLE QVariantMap extensionSettingsState(const QString& extensionId) const;
    Q_INVOKABLE QVariantMap performExtensionSettingsAction(const QString& extensionId, int action);
    Q_INVOKABLE QVariantMap speechSettingsState() const;
    Q_INVOKABLE QVariantMap securitySettingsState() const;
    Q_INVOKABLE QVariantMap productPrivacyState() const;
    Q_INVOKABLE QVariantMap productRecoveryState() const;
    Q_PROPERTY(QObject* backupManager READ backupManager CONSTANT)
    QObject* backupManager() const;
    Q_INVOKABLE QVariantMap productBackupAvailability() const;
    Q_INVOKABLE QVariantMap exportProductBackup(const QStringList& domains) const;
    Q_INVOKABLE QVariantMap importProductBackup(const QByteArray& data, const QStringList& domains,
                                                bool replace);
    Q_INVOKABLE QVariantMap clearProductData(const QString& domain);
    Q_INVOKABLE QVariantMap runProductMaintenance();
    Q_INVOKABLE QVariantMap resolveInterruptedModelOperation(const QString& operationId);
    Q_INVOKABLE QVariantMap clearProductCredential(const QString& id);
    Q_INVOKABLE QVariantMap setProductProviderCredential(const QString& providerId,
                                                         const QString& value);
    Q_INVOKABLE QVariantMap setProductPluginCredential(const QString& pluginId,
                                                       const QString& credentialId,
                                                       const QString& value);
    Q_INVOKABLE QVariantMap clearProductPluginCredential(const QString& pluginId,
                                                         const QString& credentialId);
    QString onboardingUseCase() const;
    void setOnboardingUseCase(const QString& useCase);
    QString onboardingAiProvider() const;
    void setOnboardingAiProvider(const QString& provider);
    QString recoveryDraftText() const;
    void setRecoveryDraftText(const QString& text);
    bool reducedMotionEnabled() const;
    void setReducedMotionEnabled(bool enabled);
    Q_PROPERTY(bool reducedTransparencyEnabled READ reducedTransparencyEnabled WRITE setReducedTransparencyEnabled NOTIFY nativeExperienceChanged)
    bool reducedTransparencyEnabled() const;
    void setReducedTransparencyEnabled(bool enabled);
    bool highContrastEnabled() const;
    void setHighContrastEnabled(bool enabled);
    QString uiDensity() const;
    void setUiDensity(const QString& density);
    bool notifyModelDownloads() const;
    void setNotifyModelDownloads(bool enabled);
    bool notifyModelRemovals() const;
    void setNotifyModelRemovals(bool enabled);
    bool notifyAgentResponses() const;
    void setNotifyAgentResponses(bool enabled);
    bool notifySystemUpdates() const;
    void setNotifySystemUpdates(bool enabled);
    bool soundEffectsEnabled() const;
    void setSoundEffectsEnabled(bool enabled);
    QStringList activityTimelineSummaries() const;
    QStringList notificationCenterSummaries() const;
    QStringList notificationCategories() const;
    QStringList notificationLifecycleSummaries() const;
    QStringList notificationFilteredSummaries() const;
    int unreadNotificationCount() const;
    QString notificationSearchQuery() const;
    void setNotificationSearchQuery(const QString& query);
    QString notificationCategoryFilter() const;
    void setNotificationCategoryFilter(const QString& category);
    QString updateWorkflowState() const;
    QString lastAssetUrl() const;
    QStringList releaseNotesSummaries() const;
    QStringList aboutSentinelSummaries() const;
    QStringList accessibilitySummaries() const;
    QStringList diagnosticsCenterSummaries() const;
    QStringList exportPreviewSummaries() const;
    QStringList brainInsightSummaries() const;
    QStringList recoveryReliabilitySummaries() const;
    QStringList productPolishSummaries() const;
    Q_PROPERTY(QString responseProfileInstructions READ responseProfileInstructions WRITE setResponseProfileInstructions NOTIFY skillProfileChanged)
    QString responseProfileInstructions() const;
    void setResponseProfileInstructions(const QString& instructions);
    QString selectedSkillProfile() const;
    void setSelectedSkillProfile(const QString& profileId);
    QString selectedSkillProfileName() const;
    QString selectedSkillProfileSummary() const;
    QString selectedSkillProfileDescription() const;
    QString selectedSkillProfileReadiness() const;
    QString selectedSkillProfilePolicyPosture() const;
    QStringList skillProfileIds() const;
    QStringList skillProfileNames() const;
    QStringList skillProfileSummaries() const;
    QStringList skillProfileCapabilitySummaries() const;
    QStringList skillProfileReadinessChecks() const;
    QStringList skillProfileDeveloperDiagnostics() const;
    QString selectedWorkspaceId() const;
    QString selectedWorkspaceRootPath() const;
    Q_INVOKABLE bool workspaceRequiresLocalProvider() const;
    Q_INVOKABLE QVariantMap currentWorkspaceProfile() const;
    Q_INVOKABLE QStringList presetIds() const;
    Q_INVOKABLE QStringList presetNames() const;
    Q_INVOKABLE bool setWorkspaceProfile(const QString& workspaceId, const QString& presetId,
                                         const QVariantMap& overrides);
    Q_INVOKABLE QString createPreset(const QString& name, const QVariantMap& preferences);
    Q_INVOKABLE bool renamePreset(const QString& presetId, const QString& name);
    Q_INVOKABLE bool updatePreset(const QString& presetId, const QVariantMap& preferences);
    Q_INVOKABLE QString duplicatePreset(const QString& presetId);
    Q_INVOKABLE bool deletePreset(const QString& presetId);
    void setSelectedWorkspaceId(const QString& workspaceId);
    QString selectedWorkspaceName() const;
    QString selectedWorkspaceAccessState() const;
    QString workspacePermissionPosture() const;
    QString selectedWorkspaceRootSummary() const;
    QString workspaceReadinessStatus() const;
    QString workspaceReadinessSummary() const;
    QString workspacePermissionSummary() const;
    QStringList workspaceIds() const;
    QStringList workspaceNames() const;
    QStringList workspaceSummaries() const;
    QStringList workspacePermissionPostures() const;
    QStringList workspaceActionPlaceholders() const;
    QStringList workspaceReadinessChecks() const;
    QStringList workspaceBoundaryDiagnostics() const;
    QStringList workspaceTemplateNames() const;
    QString workspaceLastActionStatus() const;
    QString workspaceLastActionSummary() const;
    QStringList attachmentSummaries() const;
    QString attachmentStatus() const;
    QString attachmentPreviewSummary() const;
    QStringList fileChatActionSummaries() const;
    bool localKnowledgeBaseEnabled() const;
    void setLocalKnowledgeBaseEnabled(bool enabled);
    QString attachmentBehavior() const;
    void setAttachmentBehavior(const QString& behavior);
    QString exportDefaultFormat() const;
    void setExportDefaultFormat(const QString& format);
    bool exportIncludeTimestamps() const;
    void setExportIncludeTimestamps(bool enabled);
    bool exportIncludeCitations() const;
    void setExportIncludeCitations(bool enabled);
    bool exportAnonymizeNames() const;
    void setExportAnonymizeNames(bool enabled);
    bool exportIncludeModelMetadata() const;
    void setExportIncludeModelMetadata(bool enabled);
    QString localKnowledgeBaseStatus() const;
    QStringList knowledgeBaseDocumentSummaries() const;
    QStringList recentRetrievalSummaries() const;
    QStringList retrievalExplainabilitySummaries() const;
    QStringList brainWorkspaceSummaries() const;
    QStringList exportCenterSummaries() const;
    QStringList privacyCenterSummaries() const;
    QString defaultPermissionPolicyState() const;
    void setDefaultPermissionPolicyState(const QString& state);
    QString permissionPolicyStatus() const;
    QString permissionPolicySummary() const;
    QStringList permissionPolicyStateLabels() const;
    QStringList permissionPolicyDomainIds() const;
    QStringList permissionPolicyDomainNames() const;
    QStringList permissionPolicyDomainSummaries() const;
    QStringList permissionPolicyDeveloperDiagnostics() const;
    QString toolGatewayStatus() const;
    QString toolGatewaySummary() const;
    QString toolGatewayPermissionPosture() const;
    int toolGatewayToolCount() const;
    int toolGatewayMetadataSafeCount() const;
    int toolGatewayUnavailableCount() const;
    int toolGatewayRefusedCount() const;
    QStringList toolGatewayToolSummaries() const;
    QStringList toolGatewayDeveloperDiagnostics() const;
    QString agentRuntimeStatus() const;
    QString agentRuntimeSummary() const;
    QString agentRuntimeApprovalPosture() const;
    int agentRuntimeAgentCount() const;
    int agentRuntimeReadyAgentCount() const;
    int agentRuntimeRefusedAgentCount() const;
    QStringList agentRuntimeAgentSummaries() const;
    QStringList agentRuntimeReadinessSummaries() const;
    QStringList agentRuntimeDeveloperDiagnostics() const;
    QString agentPlanId() const;
    QString agentPlanGoalSummary() const;
    QStringList agentPlanSteps() const;
    QStringList agentPlanRequiredTools() const;
    QStringList agentPlanRequiredPermissions() const;
    QString agentPlanEstimatedRisk() const;
    QString agentPlanApprovalState() const;
    QString agentPlanRefusalReason() const;
    QStringList agentPlanDiagnostics() const;
    QString controlledTaskActiveSummary() const;
    bool controlledTaskSessionActive() const;
    QString controlledTaskCurrentStep() const;
    QString controlledTaskProgressSummary() const;
    QStringList controlledTaskPlanSteps() const;
    QStringList controlledTaskQueueSummaries() const;
    QStringList controlledTaskTimelineSummaries() const;
    QStringList controlledTaskExplainabilitySummaries() const;
    QStringList controlledTaskPermissionSummaries() const;
    QStringList controlledTaskNotificationCategories() const;
    QStringList controlledTaskExportSummaries() const;
    QString controlledTaskDiagnosticsSummary() const;
    QStringList controlledTaskSafetyGuarantees() const;

    Q_INVOKABLE bool sendMessage(const QString& message);
    Q_INVOKABLE bool regenerateChatResponse(int userMessageId);
    Q_INVOKABLE bool regenerateLatestChatResponse();
    Q_INVOKABLE bool retryChatResponse(int assistantMessageId);
    Q_INVOKABLE QString editAndResendChatMessage(int userMessageId, const QString& text);
    Q_INVOKABLE QVariantList searchChats(const QString& query) const;
    Q_INVOKABLE bool cancelLocalInference();
    Q_INVOKABLE bool runLocalInference(const QString& prompt, const QString& model);
    Q_INVOKABLE bool requestConversationSummaryGeneration();
    Q_INVOKABLE bool generatePiperTtsFile(const QString& text);
    Q_INVOKABLE bool searchConversation(const QString& query);
    Q_INVOKABLE void clearConversationSearch();
    Q_INVOKABLE bool exportTranscript(const QString& format);
    Q_INVOKABLE void openUpdateModal();
    Q_INVOKABLE bool checkForUpdates();
    Q_INVOKABLE bool startDownload(const QString& assetUrl);
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE bool confirmUpdateDownload();
    Q_INVOKABLE void relaunchApplication();
    Q_INVOKABLE void replayOnboarding();
    Q_INVOKABLE void seedRecoveryDraft(const QString& text);
    Q_INVOKABLE bool pinNotification(const QString& notificationId);
    Q_INVOKABLE bool archiveNotification(const QString& notificationId);
    Q_INVOKABLE bool markNotificationRead(const QString& notificationId);
    Q_INVOKABLE QString latestNotificationId() const;
    Q_INVOKABLE bool clearArchivedNotifications();
    Q_INVOKABLE bool prepareExportPreview(const QString& source, const QString& format);
    Q_INVOKABLE bool exportDiagnostics(const QString& format);
    Q_INVOKABLE bool requestConversationExport(const QString& format);
    Q_INVOKABLE QString createWorkspace(const QString& name, const QString& templateName);
    Q_INVOKABLE bool renameWorkspace(const QString& workspaceId, const QString& name);
    Q_INVOKABLE bool setWorkspaceRoot(const QString& workspaceId, const QString& rootPath);
    Q_INVOKABLE bool openWorkspaceFolder(const QString& rootPath);
    Q_INVOKABLE bool clearWorkspaceRoot();
    Q_INVOKABLE bool archiveWorkspace(const QString& workspaceId);
    Q_INVOKABLE bool deleteWorkspace(const QString& workspaceId);
    Q_INVOKABLE QString duplicateWorkspace(const QString& workspaceId);
    Q_INVOKABLE bool attachFileToChat(const QString& filePath);
    Q_INVOKABLE bool pasteAttachment(const QString& name, const QString& text);
    Q_INVOKABLE bool removeAttachment(const QString& attachmentId);
    Q_INVOKABLE void clearAttachments();
    Q_INVOKABLE bool replaceAttachment(const QString& attachmentId, const QString& filePath);
    Q_INVOKABLE bool addKnowledgeBaseDocument(const QString& filePath);
    Q_INVOKABLE bool removeKnowledgeBaseDocument(const QString& documentId);
    Q_INVOKABLE bool reindexKnowledgeBase();
    Q_INVOKABLE bool clearKnowledgeBase();
    Q_INVOKABLE QString planControlledAgentTask(const QString& goal);
    Q_INVOKABLE bool modifyControlledAgentPlan(const QString& taskId, const QStringList& steps);
    Q_INVOKABLE bool approveControlledAgentTask(const QString& taskId, const QString& choice);
    Q_INVOKABLE bool denyControlledAgentTask(const QString& taskId);
    Q_INVOKABLE bool startControlledAgentTask(const QString& taskId);
    Q_INVOKABLE bool executeControlledAgentStep(const QString& taskId);
    Q_INVOKABLE bool skipControlledAgentStep(const QString& taskId);
    Q_INVOKABLE bool retryControlledAgentStep(const QString& taskId);
    Q_INVOKABLE bool cancelControlledAgentTask(const QString& taskId);
    Q_INVOKABLE bool reorderControlledAgentTask(const QString& taskId, int newIndex);
    Q_INVOKABLE bool setControlledToolPermission(const QString& category, const QString& choice);
    Q_INVOKABLE bool exportControlledAgentTask(const QString& taskId, const QString& format);
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
    Q_INVOKABLE bool runAgentRequest(const QString& request);
    Q_INVOKABLE bool clearMemory();
    Q_INVOKABLE bool clearChat();
    Q_INVOKABLE void addNotification(const QString& category, const QString& title,
                                     const QString& body);
    Q_INVOKABLE void addNotificationWithPriority(const QString& category, const QString& title,
                                                 const QString& body, const QString& priority);
    Q_INVOKABLE bool removeNotificationById(const QString& notificationId);
    Q_INVOKABLE bool markAllNotificationsRead();
    Q_INVOKABLE void toggleNotificationCenter();
    Q_PROPERTY(bool dndEnabled READ dndEnabled WRITE setDndEnabled NOTIFY dndEnabledChanged)
    bool dndEnabled() const;
    void setDndEnabled(bool enabled);
    Q_PROPERTY(QString globalErrorMessage READ globalErrorMessage NOTIFY globalErrorChanged)
    QString globalErrorMessage() const;
    Q_PROPERTY(QString globalErrorSeverity READ globalErrorSeverity NOTIFY globalErrorChanged)
    QString globalErrorSeverity() const;
    Q_PROPERTY(bool globalErrorVisible READ globalErrorVisible NOTIFY globalErrorChanged)
    bool globalErrorVisible() const;
    Q_PROPERTY(QString globalErrorActionLabel READ globalErrorActionLabel NOTIFY globalErrorChanged)
    QString globalErrorActionLabel() const;
    void setGlobalError(const QString& message, const QString& severity = QStringLiteral("Error"),
                        const QString& actionLabel = QString());
    Q_INVOKABLE void dismissGlobalError();
    // Invoked by the global banner's action button; keeps the action policy in C++.
    Q_INVOKABLE void triggerGlobalErrorAction();
    // Surfaces a recoverable desktop initialization problem (for example a repaired settings
    // store) with a generic, non-sensitive message. Never stores raw exception text.
    void reportStartupRecovery(const QString& code);
    // How long the daemon may stay in the never-connected (Unavailable) state before the global
    // banner appears, so a daemon that is still booting does not flash a false alarm. A lost
    // connection (Reconnecting) is always surfaced immediately. Zero disables the grace.
    void setDaemonAvailabilityGracePeriodMs(int milliseconds);
    Q_INVOKABLE bool snoozeNotification(const QString& notificationId, int minutes);
    Q_INVOKABLE bool unsnoozeNotification(const QString& notificationId);
    Q_INVOKABLE bool shouldShowNotification(const QVariantMap& notification) const;
    Q_INVOKABLE bool isChannelMuted(const QString& category) const;
    Q_INVOKABLE void setChannelMuted(const QString& category, bool muted);
    Q_INVOKABLE QStringList mutedChannelNames() const;
    Q_INVOKABLE QVariantMap cursorScreenGeometry();
    bool notificationCenterVisible() const;
    void setNotificationCenterVisible(bool visible);
    Q_INVOKABLE void setModeByName(const QString& modeName);
    Q_INVOKABLE void remember(const QString& key, const QString& value);
    Q_INVOKABLE void applyVoiceConfigurationPaths(const QString& piperBinaryPath,
                                                  const QString& piperModelPath,
                                                  const QString& whisperBinaryPath,
                                                  const QString& whisperModelPath);
    Q_INVOKABLE QString autoDetectVoicePaths();
    Q_INVOKABLE QVariantMap autoDetectVoicePathStatus();
    Q_INVOKABLE void startVoiceCapture(bool testOnly = false);
    Q_INVOKABLE void stopVoiceCapture();
    Q_INVOKABLE void transcribeAudioFile(const QString& path);
    QString selectedCloudProvider() const;
    void setSelectedCloudProvider(const QString& provider);
    QString webSearchProvider() const;
    void setWebSearchProvider(const QString& provider);
    QString webSearchApiKey() const;
    void setWebSearchApiKey(const QString& key);
    int webSearchMaxResults() const;
    void setWebSearchMaxResults(int maxResults);
    QVariantList persistentPermissionGrants() const;
    Q_INVOKABLE bool revokePersistentPermission(const QString& id);
    Q_INVOKABLE bool clearPersistentPermissions();
    QString semanticProvider() const;
    void setSemanticProvider(const QString& provider);
    QString semanticEmbeddingModel() const;
    void setSemanticEmbeddingModel(const QString& model);

    Q_INVOKABLE void manageMemory(const QVariantMap& request);
signals:
    void memoryManagementCompleted(const QVariantMap& result);
    void runtimeProjectionChanged();
    void providerStatusChanged();
    void persistentPermissionGrantsChanged();
    void currentModeChanged();
    void chatMessagesChanged();
    void memoryEntriesChanged();
    void themeNameChanged();
    void configurationProfileChanged();
    void appLanguageChanged();
    void companionChanged();
    void companionChatVisibleChanged();
    void notificationCenterVisibleChanged();
    void dndEnabledChanged();
    void requestCompanionChat();
    void developerModeChanged();
    void agentAutonomousModeChanged();
    void contextExplainabilityVisibleChanged();
    void currentPageChanged();
    void recoveryDraftTextChanged();
    void nativeExperienceChanged();
    void onboardingStateChanged();
    void nativeNotificationRequested(const QString& title, const QString& body,
                                     const QString& category);
    void requestWindowActive(const QString& pageName);
    void maintenanceStatusChanged();
    void agentStatusChanged();
    void agentLoopChanged();
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
    void voiceInputChanged();
    void voiceRecordingActiveChanged();
    void voiceTranscriptionCompleted(const QString& transcript);
    void voiceTestTranscriptionCompleted(const QString& transcript);
    void conversationExportChanged();
    void conversationDuplicateChanged();
    void conversationDeleteChanged();
    void conversationDeleteCompleted(const QString& conversationId, bool succeeded, const QString& summary);
    void memoryCandidatesChanged();
    void memoryRecallChanged();
    void contextAssemblyChanged();
    void agentActivityChanged();
    void modelRoutingChanged();
    void taskPlanChanged();
    void orchestrationSnapshotChanged();
    void runtimeProviderRegistryChanged();
    void localModelSelectionChanged();
    void modelCapabilitySettingsChanged();
    void ollamaStatusChanged();
    void proxySettingsChanged();
    void onlineChanged();
    void modelRoleChanged();
    void localChatInferenceRoutingChanged();
    void localInferenceChanged();
    void voiceConfigurationChanged();
    void promptContextInjectionChanged();
    void skillProfileChanged();
    void workspaceChanged();
    void attachmentChanged();
    void permissionPolicyChanged();
    void agentRuntimeChanged();
    void controlledAgentTasksChanged();
    void updateCheckCompleted(bool available, const QString& version, const QString& releaseNotes,
                              const QString& downloadUrl, qint64 assetSize = 0);
    void requestOpenUpdateModal();
    void updateDownloadProgressChanged(qint64 bytesReceived, qint64 bytesTotal,
                                       double speedBytesPerSec);
    void updateDownloadFinished(bool success, const QString& filePath);
    void selectedCloudProviderChanged();
    void webSearchSettingsChanged();
    void mcpServersChanged();
    void semanticSettingsChanged();
    void cloudApiKeysChanged();
    void soundEffectsEnabledChanged();
    void globalErrorChanged();

private:
    QObject* backupManager_ = nullptr;
    DesktopShellViewModel(DesktopControllerBridge::Source source, core::ModeManager& modeManager,
                          core::AppSettings& settings, core::WinTaskbarIntegration* taskbar,
                          QObject* parent);
    QString onboardingErrorCode_;
    static QString normalizedPageOrDefault(const QString& page);
    void applyGlobalError(const QString& message, const QString& severity,
                          const QString& actionLabel, DesktopErrorCategory category);
    void clearGlobalError();
    void refreshDaemonErrorProjection();
    void showDaemonUnavailableBanner();

    DesktopControllerBridge controller_;
    core::ModeManager& modeManager_;
    core::AppSettings& settings_;
    core::AgentRuntimeService agentRuntimeService_;
    core::CompanionService companionService_;
    core::PermissionPolicyService permissionPolicyService_;
    core::SkillProfileService skillProfileService_;
    core::ToolExecutionGateway toolExecutionGateway_;
    core::WorkspaceService workspaceService_;
    std::unique_ptr<core::LocalRagStore> localRagStore_;
    QList<core::RagDocumentRecord> attachments_;
    QString workspaceLastActionStatus_ = QStringLiteral("Ready");
    QString workspaceLastActionSummary_ = QStringLiteral("No workspace action has run.");
    QString notificationSearchQuery_;
    QString notificationCategoryFilter_ = QStringLiteral("All");
    bool notificationCenterVisible_ = false;
    void setupBackgroundUpdateCheck();
    QString exportPreviewSource_ = QStringLiteral("conversations");
    QString exportPreviewFormat_ = QStringLiteral("Markdown");
    ChatMessageListModel chatMessages_;
    QString currentPage_ = QStringLiteral("Dashboard");
    bool companionNativeAvailable_ = false;
    bool companionPaused_ = false;
    bool companionChatVisible_ = false;
    SystemDictationService systemDictation_{this};
    void deliverVoiceTranscript(const QString& transcript);
    bool localSttReady() const;
    bool voiceTestOnly_ = false;
    // True while this capture runs on native dictation because local Whisper
    // was not ready; stop/cancel must route back to the same backend.
    bool voiceUsingSystemFallback_ = false;
    QString voiceInputStatus_;
    QString lastVoiceTranscript_;
    QString attachmentError_;
    QHash<QString, QString> attachmentPaths_;
    bool attachmentSendPending_ = false;
    bool voiceRecordingActive_ = false;

    core::WinTaskbarIntegration* taskbar_ = nullptr;
    QSystemTrayIcon* trayIcon_ = nullptr;
    QTimer* updateCheckTimer_ = nullptr;
    QTimer* daemonGraceTimer_ = nullptr;
    bool daemonUnavailableShown_ = false;
    QNetworkAccessManager* networkManager_ = nullptr;
    QString lastReleaseUrl_;
    QString lastAssetUrl_;
    QString lastAssetName_;
    qint64 lastAssetSize_ = 0;
    QNetworkReply* downloadReply_ = nullptr;
    QFile* downloadFile_ = nullptr;
    QString downloadPath_;
    qint64 lastBytesReceived_ = 0;
    QElapsedTimer downloadElapsed_;
    QString globalErrorMessage_;
    QString globalErrorSeverity_ = QStringLiteral("Error");
    bool globalErrorVisible_ = false;
    QString globalErrorActionLabel_;
    DesktopErrorCategory globalErrorCategory_ = DesktopErrorCategory::None;
    QSet<QString> notifiedIds_;
    QString lastAgentStatus_;
    QString lastNotifiedCategory_;
    qint64 lastNotificationTime_ = 0;
    QHash<QString, qint64> notificationCooldowns_;
};

} // namespace sentinel::desktop
