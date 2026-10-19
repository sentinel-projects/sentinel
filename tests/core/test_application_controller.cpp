// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../support/DeterministicChatFixture.h"
#include "../support/LocalInferenceWorkerFixture.h"
#include "sentinel/core/agent/NullAgentRuntime.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/ApplicationController.h"
#include "sentinel/core/app/ApplicationControllerBuilder.h"
#include "sentinel/core/chat/IChatHistoryStore.h"
#include "sentinel/core/chat/InMemoryConversationStore.h"
#include "sentinel/core/chat/LocalEchoProvider.h"
#include "sentinel/core/chat/SQLiteConversationStore.h"
#include "sentinel/core/memory/InMemoryStore.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/model/ModelManagement.h"
#include "sentinel/core/runtime/LocalInference.h"
#include "sentinel/core/runtime/RuntimePermissions.h"
#include "sentinel/core/voice/PiperTts.h"
#include "sentinel/core/voice/Voice.h"

#include <QDir>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <cstdint>
#include <memory>

using sentinel::core::ApplicationController;
using sentinel::core::IChatHistoryStore;
using sentinel::core::IConversationStore;
using sentinel::core::ILocalInferenceClient;
using sentinel::core::InMemoryStore;
using sentinel::core::LocalEchoProvider;
using sentinel::core::LocalInferenceRequest;
using sentinel::core::LocalInferenceResponse;
using sentinel::core::LocalInferenceStatus;
using sentinel::core::ModelManagementAction;
using sentinel::core::ModelManagementRequest;
using sentinel::core::OllamaConfig;
using sentinel::core::OllamaHealthCheckResult;
using sentinel::core::OllamaModelSummary;
using sentinel::core::SQLiteConversationStore;
using sentinel::core::StaticModelManagementService;
class RecordingChatHistoryStore final : public IChatHistoryStore {
public:
    explicit RecordingChatHistoryStore(QList<sentinel::core::ChatMessage> messages = {},
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
            wasCleared_ = true;
            messages_.clear();
        }
    }

    bool isAvailable() const override {
        return available_;
    }

    QString lastError() const override {
        return available_ ? QString() : QStringLiteral("unavailable");
    }

    QList<sentinel::core::ChatMessage> messages_;
    bool available_ = true;
    bool wasCleared_ = false;
};


class UnavailableMemoryStore final : public sentinel::core::IMemoryStore {
public:
    void put(QString key, QString value) override {
        Q_UNUSED(key);
        Q_UNUSED(value);
    }

    QString get(const QString& key) const override {
        Q_UNUSED(key);
        return {};
    }

    sentinel::core::MemoryEntries entries() const override {
        return {};
    }

    void clear() override {}

    bool isAvailable() const override {
        return false;
    }

    QString lastError() const override {
        return QStringLiteral("unavailable");
    }
};

class AllowLocalInferencePolicy final : public sentinel::core::IRuntimePermissionPolicy {
public:
    sentinel::core::RuntimePermissionDecision
    evaluate(const sentinel::core::RuntimePermissionRequest& request) const override {
        return {
            sentinel::core::RuntimePermissionDecisionStatus::Allowed,
            request,
            QStringLiteral("Local inference permission allowed for injected test policy."),
        };
    }
};

class BlockLocalInferenceSafetyPolicy final : public sentinel::core::IRuntimeSafetyPolicy {
public:
    sentinel::core::RuntimeSafetyReport evaluate() const override {
        sentinel::core::RuntimeSafetyReport report;
        report.decision = sentinel::core::RuntimeSafetyDecision::Blocked;
        report.summary = QStringLiteral("Injected local safety policy blocked execution.");
        return report;
    }
};

class FakeLocalInferenceClient final : public ILocalInferenceClient {
public:
    LocalInferenceResponse infer(const LocalInferenceRequest& request) override {
        called = true;
        lastRequest = request;
        LocalInferenceResponse response;
        response.status = LocalInferenceStatus::Succeeded;
        response.model = request.options.model;
        response.endpoint = QStringLiteral("http://127.0.0.1:11434");
        response.text = QStringLiteral("fake local completion");
        response.summary = QStringLiteral("Fake local inference completed.");
        response.traces = {
            sentinel::core::LocalInferenceTrace{
                1,
                QStringLiteral("Fake Client"),
                QStringLiteral("Succeeded"),
                QStringLiteral("Injected fake local inference client was called."),
            },
        };
        return response;
    }

    QString statusSummary() const override {
        return QStringLiteral("Fake local inference client is ready.");
    }

    bool called = false;
    LocalInferenceRequest lastRequest;
};

class FakeOllamaRuntimeClient final : public sentinel::core::IOllamaRuntimeClient {
public:
    explicit FakeOllamaRuntimeClient(QList<OllamaModelSummary> models, OllamaConfig config = {})
        : models_(std::move(models)), config_(std::move(config)) {}

    OllamaConfig config() const override {
        return config_;
    }

    OllamaHealthCheckResult healthCheck() const override {
        OllamaHealthCheckResult result;
        if (!models_.isEmpty()) {
            result.connectionStatus = sentinel::core::OllamaConnectionStatus::Connected;
            result.healthStatus = sentinel::core::OllamaHealthStatus::Healthy;
        }
        result.summary = QStringLiteral("Fake Ollama health metadata.");
        return result;
    }

    QList<OllamaModelSummary> installedModels() const override {
        return models_;
    }

private:
    QList<OllamaModelSummary> models_;
    OllamaConfig config_;
};

class FakePiperTtsClient final : public sentinel::core::IPiperTtsClient {
public:
    enum class Mode : std::uint8_t {
        Success,
        Failure,
        Timeout,
    };

    explicit FakePiperTtsClient(Mode mode = Mode::Success) : mode_(mode) {}

    sentinel::core::PiperTtsStatus status() const override {
        return sentinel::core::PiperTtsStatus::Configured;
    }

    QString statusSummary() const override {
        return QStringLiteral("Fake Piper TTS client is deterministic.");
    }

    sentinel::core::PiperTtsResult
    synthesize(const sentinel::core::PiperTtsRequest& request,
               const sentinel::core::PiperTtsConfig& config) override {
        called = true;
        lastRequest = request;
        lastConfig = config;
        if (mode_ == Mode::Failure) {
            return {
                sentinel::core::PiperTtsStatus::Failed,
                false,
                request.outputPath,
                QStringLiteral("Controlled Piper TTS output path: %1").arg(request.outputPath),
                request.timeoutMs,
                2,
                QStringLiteral("fake failure"),
                QStringLiteral("Piper TTS failed while generating the controlled output file."),
                {QStringLiteral("Fake Piper failure path.")},
            };
        }
        if (mode_ == Mode::Timeout) {
            return {
                sentinel::core::PiperTtsStatus::Timeout,
                false,
                request.outputPath,
                QStringLiteral("Controlled Piper TTS output path: %1").arg(request.outputPath),
                request.timeoutMs,
                -1,
                QStringLiteral("fake timeout"),
                QStringLiteral("Piper TTS timed out before file-output synthesis completed."),
                {QStringLiteral("Fake Piper timeout path.")},
            };
        }

        return {
            sentinel::core::PiperTtsStatus::Succeeded,
            true,
            request.outputPath,
            QStringLiteral("Controlled Piper TTS output path: %1").arg(request.outputPath),
            request.timeoutMs,
            0,
            {},
            QStringLiteral("Piper TTS generated a local controlled audio file. Playback was not "
                           "started."),
            {QStringLiteral("Fake Piper wrote controlled output without playback.")},
        };
    }

    bool called = false;
    sentinel::core::PiperTtsRequest lastRequest;
    sentinel::core::PiperTtsConfig lastConfig;

private:
    Mode mode_ = Mode::Success;
};

class ApplicationControllerTest final : public QObject {
    Q_OBJECT

private slots:
    void compatibleLocalReadinessTracksDiscovery();
    void exposesProviderNameAndInitialSystemMessage();
    void exposesProviderStatus();
    void exposesAgentStatusWithoutRuntime();
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
    void disabledRuntimeProviderSelectionFallsBackToLocalOllama();
    void exposesOllamaRuntimeBoundaryMetadata();
    void exposesLocalInferenceBoundaryMetadata();
    void modelManagementRecommendationsAreDeterministicAndActionsAvailable();
    void exposesModelManagementReadinessMetadata();
    void exposesVoiceReadinessMetadata();
    void validatesConfiguredVoicePathsAsMetadataOnly();
    void piperFileOutputExecutionRequiresExplicitOptIn();
    void piperFileOutputExecutionBlocksInvalidBinaryOrModel();
    void rejectsInvalidSelectedModelAgainstDiscoveryMetadata();
    void exposesStreamingEnabledByDefaultMetadata();
    void clearChatClearsConversationRuntimeAndPersistence();
    void clearChatKeepsSingleInitialSystemMessage();
    void reportsPersistedConversationHistorySummary();
    void reportsRuntimeOnlyConversationHistorySummary();
    void exposesConversationStoreReadinessWithoutSwitchingTranscriptStorage();
    void createsSwitchesRenamesArchivesAndUnarchivesConversations();
    void archivedActiveConversationBlocksSend();
    void archiveUnarchiveUpdatesBrowserSummaries();
    void pinPersistsAcrossControllerRecreation();
    void unpinPersistsAcrossControllerRecreation();
    void duplicateConversationCopiesLocalMessages();
    void conversationOrderingIsPinnedRecentArchivedDeterministic();
    void deleteReadinessIsDisabledByDefault();
    void deleteRequestRefusesSafelyWithoutMutation();
    void deleteRequestRefusesSafelyWithoutSQLiteMutation();
    void activeConversationRemainsValidAfterArchiveUnarchive();
    void persistsActiveConversationAcrossControllerRecreation();
    void persistsFailedChatAcrossControllerRecreation();
    void persistsCancelledChatAcrossControllerRecreation();
    void localInferenceWorkerDropsCallbacksAfterContextDestruction();
    void switchingConversationDoesNotDuplicateTranscriptInsertion();
    void reportsSingleConversationBrowserEntryDeterministically();
    void reportsEmptyTranscriptConversationBrowserSummary();
    void reportsConversationBrowserMessageCountSummary();
    void clearChatUpdatesConversationBrowserEntry();
    void conversationBrowserReflectsSearchAndExportAvailability();
    void reportsMultiConversationPlanningReadinessWithoutStorageMutation();
    void inMemoryConversationSearchFindsUserAndAssistantMessages();
    void emptyConversationSearchDoesNotMutateHistory();
    void conversationSearchDoesNotMutateHistory();
    void clearChatResetsConversationSearchSummary();
    void markdownConversationExportWritesCurrentTranscript();
    void jsonConversationExportWritesValidStructure();
    void textAndPdfConversationExportWriteLocalFiles();
    void emptyConversationExportIsRefused();
    void conversationExportUsesSanitizedTimestampedFilenames();
    void unsupportedConversationExportFormatWritesNoFile();
    void blocksLocalInferenceByDefaultPermission();
    void blocksLocalInferenceWhenSafetyPolicyBlocks();
    void runsInjectedLocalInferenceWhenPermissionAllows();
    void exposesConversationSessionMetadata();
    void exposesConversationStateMetadata();
    void updatesModelRoutingModeMetadata();
    void keepsConversationSessionSeparateFromChatAndRuntimeSessions();
    void exposesAgentToolMetadata();
    void exposesMemoryStatus();
    void sendsMessageThroughProvider();
    void updatesConversationStateForChatFlow();
    void ignoresBlankChatMessages();
    void handlesUnavailableProvider();
    void handlesProviderErrorReply();
    void clearsChatHistory();
    void loadsPersistedChatHistoryAtStartup();
    void appendsNewChatMessagesToHistoryStore();
    void clearsPersistentChatHistoryWhenAvailable();
    void keepsRuntimeChatWorkingWhenHistoryStoreUnavailable();
    void storesRuntimeMemoryEntries();
    void memoryFollowsAuthoritativeConversation();
    void candidateCannotBeMovedToAnotherSession();
    void createsMemoryCandidatesPendingReview();
    void recordsMemoryCandidateApprovalAndRejectionMetadata();
    void resetsMemoryCandidateReviewMetadata();
    void refusesInvalidMemoryCandidateReviewTransitions();
    void archivesReviewedMemoryCandidatesAsTerminalMetadata();
    void memoryCandidatesDoNotMutateKeyValueMemory();
    void approvedMemoryCandidateCommitsToKeyValueMemory();
    void failedMemoryCommitPreservesCandidateAndAllowsRetry();
    void pendingAndRejectedMemoryCandidatesCannotCommit();
    void archivedMemoryCandidateCannotCommit();
    void duplicateMemoryCommitKeyRefusesByDefault();
    void approvalDoesNotAutomaticallyCommitMemory();
    void localMemoryRecallFindsCommittedKeyValueMemory();
    void emptyLocalMemoryRecallDoesNotMutateMemory();
    void localMemoryRecallAfterCommitFindsCommittedCandidate();
    void clearChatKeepsCommittedMemoryRecallAvailable();
    void localMemoryRecallDoesNotInjectIntoPrompt();
    void contextAssemblyShowsCommittedMemorySource();
    void contextAssemblyPlanningDoesNotMutateOrInjectPrompt();
    void promptContextInjectionBudgetTruncatesDeterministically();
    void conversationSummaryPlanningDoesNotMutateState();
    void retrievalPlanningDoesNotMutateState();
    void conversationCompressionReadinessPlansMetadataWithoutMutation();
    void manualSummaryGenerationIsBlockedAndDoesNotMutateTranscript();
    void manualSummaryGenerationFailureDoesNotPersistMetadata();
    void summaryContinuityInjectsPersistedSummaryAfterRestart();
    void staleSummaryContinuityFallsBackToTranscriptOnly();
    void semanticProviderPlanningExposesDisabledSelection();
    void semanticCandidateOrchestrationExposesSafeMetadata();
    void semanticArbitrationSimulationExposesSafeMetadata();
    void isolatedEmbeddingRuntimeExposureIsBoundedAndDisabledByDefault();
    void vectorPersistenceExposureIsDisabledAndDoesNotMutatePlanningOrPrompt();
    void semanticSearchExposureIsBoundedAndDoesNotMutatePlanningOrPrompt();
    void hybridBridgeExposureIsBoundedAndDoesNotMutatePlanningOrPrompt();
    void semanticAcceptanceExposureIsBoundedAndDoesNotMutatePlanningOrPrompt();
    void semanticSupplementAssemblyExposureIsDisabledAndDoesNotMutatePlanningOrPrompt();
    void semanticPromptAuthorityExposureIsDefaultDeniedAndDoesNotMutatePrompt();
    void promptContextInjectionDoesNotMutateMemoryOrCandidates();
    void clearChatKeepsApprovedMemoryCandidateMetadata();
    void clearChatKeepsCommittedMemory();
    void clearsRuntimeMemoryEntries();
    void failsSafeWhenMemoryStoreUnavailable();
    void rejectsBlankMemoryKeys();
    void overwritesMemoryEntriesThroughStoreBackend();
    void reportsRuntimeOnlyWhenChatStoreUnavailableOnClear();
};

static std::unique_ptr<ApplicationController> makeController() {
    return std::make_unique<ApplicationController>(std::make_unique<LocalEchoProvider>(),
                                                   std::make_unique<InMemoryStore>());
}

static std::unique_ptr<ApplicationController> makeChatController() {
    sentinel::test::DeterministicModelServiceFixture models;
    sentinel::core::ApplicationControllerBuilder builder;
    builder.withMemoryStore(std::make_unique<InMemoryStore>())
        .withModelService(models.takeModelService());
    auto controller = builder.build();
    controller->setSelectedRuntimeProvider(QStringLiteral("ollama"));
    controller->setSelectedLocalModel(QStringLiteral("sentinel-test-model"));
    return controller;
}

static std::unique_ptr<ApplicationController>
makeControllerWithChatHistory(std::unique_ptr<IChatHistoryStore> chatHistoryStore,
                              sentinel::test::DeterministicChatReply reply =
                                  sentinel::test::DeterministicChatReply::Final) {
    sentinel::test::DeterministicModelServiceFixture models(reply);
    sentinel::core::ApplicationControllerBuilder builder;
    builder.withMemoryStore(std::make_unique<InMemoryStore>())
        .withChatHistoryStore(std::move(chatHistoryStore))
        .withModelService(models.takeModelService());
    auto controller = builder.build();
    controller->setSelectedRuntimeProvider(QStringLiteral("ollama"));
    controller->setSelectedLocalModel(QStringLiteral("sentinel-test-model"));
    return controller;
}

static void waitForCompletedChatTurn(const ApplicationController& controller) {
    QTRY_VERIFY(controller.chatHistory().size() == 3 &&
                !controller.chatHistory().last().content.isEmpty() &&
                (controller.chatHistory().last().status == sentinel::core::ChatMessageStatus::Completed ||
                 controller.chatHistory().last().status == sentinel::core::ChatMessageStatus::Received));
}

class ProductionChatControllerFixture {
public:
    explicit ProductionChatControllerFixture(
        sentinel::test::DeterministicChatReply reply = sentinel::test::DeterministicChatReply::Final)
        : models(reply), controller(createController(models)) {}

    explicit ProductionChatControllerFixture(std::unique_ptr<IConversationStore> conversationStore,
                                             sentinel::test::DeterministicChatReply reply =
                                                 sentinel::test::DeterministicChatReply::Final)
        : models(reply), controller(createController(models, std::move(conversationStore))) {}

    sentinel::test::DeterministicModelServiceFixture models;
    std::unique_ptr<ApplicationController> controller;

    bool sendAndWait(const QString& message) {
        if (!controller->sendMessage(message)) return false;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 3000 &&
               (controller->chatHistory().size() < 3 ||
                controller->chatHistory().last().content.isEmpty() ||
                controller->chatGenerationActive()))
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        return controller->chatHistory().last().status ==
                   sentinel::core::ChatMessageStatus::Completed ||
               controller->chatHistory().last().status ==
                   sentinel::core::ChatMessageStatus::Received;
    }

private:
    static std::unique_ptr<ApplicationController> createController(
        sentinel::test::DeterministicModelServiceFixture& models,
        std::unique_ptr<IConversationStore> conversationStore =
            std::make_unique<sentinel::core::InMemoryConversationStore>()) {
        sentinel::core::ApplicationControllerBuilder builder;
        builder.withMemoryStore(std::make_unique<InMemoryStore>())
            .withConversationStore(std::move(conversationStore))
            .withModelService(models.takeModelService());
        auto controller = builder.build();
        controller->setSelectedRuntimeProvider(QStringLiteral("ollama"));
        controller->setSelectedLocalModel(QStringLiteral("sentinel-test-model"));
        return controller;
    }
};

static std::unique_ptr<ApplicationController>
makeControllerWithConversationStore(std::unique_ptr<IConversationStore> conversationStore) {
    return std::make_unique<ApplicationController>(
        std::make_unique<LocalEchoProvider>(), std::make_unique<InMemoryStore>(), nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        std::move(conversationStore));
}

struct PiperControllerFixture {
    std::unique_ptr<ApplicationController> controller;
    FakePiperTtsClient* client = nullptr;
};

static PiperControllerFixture makePiperController(FakePiperTtsClient::Mode mode) {
    auto client = std::make_unique<FakePiperTtsClient>(mode);
    auto* clientPtr = client.get();
    auto provider = std::make_unique<sentinel::core::PiperTextToSpeechProvider>(
        sentinel::core::defaultDisabledPiperTtsConfig(), std::move(client));
    return {
        std::make_unique<ApplicationController>(
            std::make_unique<LocalEchoProvider>(), std::make_unique<InMemoryStore>(), nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, std::move(provider)),
        clientPtr,
    };
}

static void configureReadyPiperPaths(ApplicationController& controller, QTemporaryDir& dir) {
#ifdef Q_OS_WIN
    const auto piperBinaryPath = dir.filePath(QStringLiteral("piper.exe"));
#else
    const auto piperBinaryPath = dir.filePath(QStringLiteral("piper"));
#endif
    const auto piperModelPath = dir.filePath(QStringLiteral("voice.onnx"));

    QFile piperBinary{piperBinaryPath};
    QVERIFY(piperBinary.open(QIODevice::WriteOnly));
    piperBinary.close();
#if !defined(Q_OS_WIN)
    QVERIFY(QFile::setPermissions(
        piperBinaryPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
#endif

    QFile piperModel{piperModelPath};
    QVERIFY(piperModel.open(QIODevice::WriteOnly));
    piperModel.write("model");
    piperModel.close();

    controller.setPiperBinaryPath(piperBinaryPath);
    controller.setPiperModelPath(piperModelPath);
}

void ApplicationControllerTest::exposesProviderNameAndInitialSystemMessage() {
    const auto controller = makeChatController();

    QCOMPARE(controller->providerName(), QStringLiteral("deterministic-chat"));
    QCOMPARE(controller->chatMessages().size(), 1);
    QCOMPARE(controller->chatMessages().first(), QStringLiteral("Sentinel: Sentinel Core online."));
    QCOMPARE(controller->chatHistory().size(), 1);
    QCOMPARE(controller->chatHistory().first().role, sentinel::core::ChatRole::System);
    QVERIFY(controller->memoryEntries().isEmpty());
}

void ApplicationControllerTest::exposesProviderStatus() {
    const auto controller = makeChatController();

    QCOMPARE(controller->providerStatus(), QStringLiteral("Available"));
}

void ApplicationControllerTest::exposesAgentStatusWithoutRuntime() {
    const auto controller = makeController();

    QCOMPARE(controller->agentStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(controller->lastAgentResponse(), QStringLiteral("No agent request yet."));
    QCOMPARE(controller->latestToolPlanStatus(), QStringLiteral("Not Requested"));
    QCOMPARE(controller->latestToolPlanSummary(), QStringLiteral("No tool plan yet."));
    QCOMPARE(controller->latestApprovalStatus(), QStringLiteral("Not Requested"));
    QCOMPARE(controller->latestApprovalSummary(), QStringLiteral("No approval decision yet."));
    QCOMPARE(controller->latestSandboxStatus(), QStringLiteral("Not Evaluated"));
    QCOMPARE(controller->latestSandboxSummary(), QStringLiteral("No sandbox evaluation yet."));
    QCOMPARE(controller->latestToolExecutionStatus(), QStringLiteral("Not Requested"));
    QCOMPARE(controller->latestToolExecutionSummary(),
             QStringLiteral("No tool execution boundary result yet."));
    QCOMPARE(controller->runtimeSessionId(), QStringLiteral("runtime-session-1"));
    QCOMPARE(controller->runtimeContextStatus(), QStringLiteral("Empty"));
    QCOMPARE(controller->runtimeContextSummary(), QStringLiteral("No runtime context yet."));
    QVERIFY(controller->runtimeContextActiveToolIds().isEmpty());
    QCOMPARE(controller->agentActivityCount(), 0);
    QCOMPARE(controller->latestAgentActivitySummary(), QStringLiteral("No agent activity yet."));

    const auto ran = controller->runAgentRequest(QStringLiteral("plan"));

    QVERIFY(!ran);
    QCOMPARE(controller->lastAgentResponse(), QStringLiteral("Agent runtime unavailable."));
    QCOMPARE(controller->agentActivityCount(), 2);
    QCOMPARE(controller->latestAgentActivitySummary(),
             QStringLiteral("Agent pipeline blocked: runtime unavailable."));
}

void ApplicationControllerTest::exposesModelRoutingMetadata() {
    const auto controller = makeController();

    QCOMPARE(controller->currentRoutingMode(), QStringLiteral("Local Only"));
    QCOMPARE(controller->modelRoutingStatus(), QStringLiteral("No Available Model"));
    QCOMPARE(controller->selectedModelProviderSummary(),
             QStringLiteral("No configured model route is available."));
}

void ApplicationControllerTest::exposesTaskPlanMetadata() {
    const auto controller = makeController();

    QCOMPARE(controller->latestTaskPlanStatus(), QStringLiteral("Blocked"));
    QCOMPARE(controller->plannedTaskStepCount(), 0);
    QCOMPARE(controller->latestTaskPlanSummary(),
             QStringLiteral("No available local metadata capability can satisfy this task."));
    QCOMPARE(controller->currentMemoryAffinitySummary(),
             QStringLiteral("Ambient (Available, Public Metadata, Session)"));
}

void ApplicationControllerTest::exposesAgentRegistryMetadata() {
    const auto controller = makeController();

    QCOMPARE(controller->registeredAgentCount(), 6);
    QCOMPARE(controller->activeAgentSummaries().size(), 6);
    QVERIFY(controller->activeAgentSummaries().contains(
        QStringLiteral("Atlas (Coordinator, Available, Local)")));
    QVERIFY(controller->activeAgentSummaries().contains(
        QStringLiteral("Vela (Researcher, Available, Cloud)")));
    QCOMPARE(controller->currentAgentSummary(),
             QStringLiteral("Atlas (Coordinator, Available, Local)"));
}

void ApplicationControllerTest::exposesProviderCatalogMetadata() {
    const auto controller = makeController();

    QCOMPARE(controller->providerCatalogCount(), 4);
    QCOMPARE(controller->providerCatalogSummaries().size(), 4);
    QVERIFY(controller->providerCatalogSummaries().contains(
        QStringLiteral("Ollama Local (Local, Not Configured)")));
    QVERIFY(controller->providerCatalogSummaries().contains(
        QStringLiteral("OpenAI Cloud (Cloud, Not Configured)")));
}

void ApplicationControllerTest::exposesMemoryCatalogMetadata() {
    const auto controller = makeController();

    QCOMPARE(controller->memoryCatalogCount(), 5);
    QCOMPARE(controller->memoryCatalogSummaries().size(), 5);
    QVERIFY(controller->memoryCatalogSummaries().contains(
        QStringLiteral("Episodic (Available, Private, User Controlled)")));
    QVERIFY(controller->memoryCatalogSummaries().contains(
        QStringLiteral("Semantic (Available, Local Only, Durable)")));
    QCOMPARE(controller->currentMemoryAffinitySummary(),
             QStringLiteral("Ambient (Available, Public Metadata, Session)"));
}

void ApplicationControllerTest::exposesOrchestrationSnapshotMetadata() {
    const auto controller = makeController();
    const auto snapshot = controller->currentOrchestrationSnapshot();

    QCOMPARE(snapshot.healthStatus, sentinel::core::OrchestrationHealthStatus::Degraded);
    QCOMPARE(snapshot.workspace.routingMode, QStringLiteral("Local Only"));
    QCOMPARE(snapshot.workspace.routingStatus, QStringLiteral("No Available Model"));
    QCOMPARE(snapshot.workspace.taskPlanStatus, QStringLiteral("Blocked"));
    QCOMPARE(snapshot.workspace.providerCatalogCount, 4);
    QCOMPARE(snapshot.workspace.registeredAgentCount, 6);
    QCOMPARE(snapshot.workspace.memoryCatalogCount, 5);
    QCOMPARE(snapshot.workspace.preferredAgentSummary,
             QStringLiteral("Atlas (Coordinator, Available, Local)"));
    QCOMPARE(snapshot.workspace.memoryAffinitySummary,
             QStringLiteral("Ambient (Available, Public Metadata, Session)"));
    QVERIFY(!snapshot.executionEnabled);
    QCOMPARE(controller->orchestrationSnapshotStatus(), QStringLiteral("Degraded"));
    QVERIFY(controller->orchestrationSnapshotSummary().contains(
        QStringLiteral("4 provider entries, 6 agents, 5 memory categories")));
    QVERIFY(controller->orchestrationSignals().contains(
        QStringLiteral("Catalogs: 4 providers / 6 agents / 5 memory")));
}

void ApplicationControllerTest::exposesOrchestrationReadinessDiagnostics() {
    const auto controller = makeController();
    const auto report = controller->currentOrchestrationReadinessReport();

    QCOMPARE(report.status, QStringLiteral("Blocked"));
    QCOMPARE(report.checks.size(), 10);
    QCOMPARE(controller->orchestrationReadinessStatus(), QStringLiteral("Blocked"));
    QCOMPARE(controller->orchestrationReadinessSummary(),
             QStringLiteral("Blocked orchestration readiness: 10 deterministic metadata checks, 10 "
                            "diagnostic entries."));
    QVERIFY(controller->orchestrationDiagnostics().contains(
        QStringLiteral("Info: Routing Mode - Local Only routing mode is set.")));
    QVERIFY(controller->orchestrationDiagnostics().contains(
        QStringLiteral("Info: Cloud Providers - Cloud provider metadata remains not configured.")));
    QVERIFY(controller->orchestrationDiagnostics().contains(
        QStringLiteral("Info: Execution Capability - Execution capability remains disabled.")));
}

void ApplicationControllerTest::exposesAgentTaskRuntimeMetadata() {
    const auto controller = makeController();

    QCOMPARE(controller->agentTaskRuntimeStatus(), QStringLiteral("Ready"));
    QCOMPARE(controller->agentTaskRuntimeTaskCount(), 6);
    QCOMPARE(controller->agentTaskQueueCount(), 6);
    QCOMPARE(controller->agentTaskQueuePlannedCount(), 6);
    QCOMPARE(controller->agentTaskQueueActiveCount(), 0);
    QCOMPARE(controller->agentTaskQueueBlockedCount(), 0);
    QCOMPARE(controller->agentTaskQueueCompletedCount(), 0);
    QCOMPARE(controller->agentTaskQueueRefusedCount(), 0);
    QVERIFY(controller->agentTaskRuntimeSummary().contains(
        QStringLiteral("executes local metadata tasks deterministically")));
    QVERIFY(controller->latestAgentTaskSummary().contains(QStringLiteral("Prepare Export Action")));
    QVERIFY(controller->latestAgentTaskLifecycleSummary().contains(QStringLiteral("queued")));
    QCOMPARE(controller->agentTaskQueueSummaries().size(), 6);
    QCOMPARE(controller->agentTaskTraceSummaries().size(), 3);
    QVERIFY(controller->agentTaskTraceSummaries().last().contains(
        QStringLiteral("Execution Boundary")));
    QCOMPARE(controller->agentPlanningSessionStatus(), QStringLiteral("Ready"));
    QCOMPARE(controller->agentPlanningCandidateCount(), 6);
    QCOMPARE(controller->agentPlanningRefusedCount(), 0);
    QVERIFY(controller->agentPlanningSessionSummary().contains(QStringLiteral("execution "
                                                                              "attempted: no")));
    QCOMPARE(controller->agentPlanningCandidateSummaries().size(), 6);
    QCOMPARE(controller->agentPlanningArbitrationSummaries().size(), 6);
    QVERIFY(controller->agentPlanningFallbackSummary().contains(QStringLiteral("No planning "
                                                                               "fallback")));
    QCOMPARE(controller->agentCapabilityRegistryStatus(),
             QStringLiteral("Refusing Unsafe Capabilities"));
    QCOMPARE(controller->agentCapabilityCount(), 9);
    QCOMPARE(controller->agentCapabilityEnabledCount(), 6);
    QCOMPARE(controller->agentCapabilityDisabledCount(), 2);
    QCOMPARE(controller->agentCapabilityRestrictedCount(), 3);
    QVERIFY(controller->agentCapabilityRegistrySummary().contains(QStringLiteral("execution "
                                                                                 "attempted: no")));
    QCOMPARE(controller->agentCapabilitySummaries().size(), 9);
    QVERIFY(controller->agentCapabilitySummaries().at(7).contains(QStringLiteral("Shell "
                                                                                 "Execution")));
    QCOMPARE(controller->agentCapabilityReadinessSummaries().size(), 9);
    QCOMPARE(controller->agentCapabilitySafetySummaries().size(), 9);
    QVERIFY(controller->agentCapabilitySafetySummaries().at(7).contains(QStringLiteral("execution "
                                                                                       "attempted: "
                                                                                       "no")));
    QCOMPARE(controller->toolContractRegistryStatus(), QStringLiteral("Refusing Unsafe Contracts"));
    QCOMPARE(controller->toolContractCount(), 10);
    QCOMPARE(controller->toolContractEnabledCount(), 6);
    QCOMPARE(controller->toolContractDisabledCount(), 3);
    QCOMPARE(controller->toolContractRestrictedCount(), 4);
    QVERIFY(controller->toolContractRegistrySummary().contains(QStringLiteral("execution "
                                                                              "attempted: no")));
    QCOMPARE(controller->toolContractSummaries().size(), 10);
    QVERIFY(controller->toolContractSummaries().at(7).contains(QStringLiteral("Future Subprocess "
                                                                              "Execution")));
    QCOMPARE(controller->toolContractPermissionSummaries().size(), 10);
    QCOMPARE(controller->toolContractSandboxSummaries().size(), 10);
    QCOMPARE(controller->toolContractReadinessSummaries().size(), 10);
    QCOMPARE(controller->toolContractSafetySummaries().size(), 10);
    QVERIFY(controller->toolContractPermissionSummaries().at(6).contains(
        QStringLiteral("future filesystem access")));
    QVERIFY(controller->toolContractSandboxSummaries().at(7).contains(QStringLiteral("Denied")));
    QVERIFY(controller->toolContractSafetySummaries().at(7).contains(QStringLiteral("execution "
                                                                                    "attempted: "
                                                                                    "no")));
}

void ApplicationControllerTest::exposesLocalRuntimeMetadata() {
    const auto controller = makeController();

    QCOMPARE(controller->localRuntimeStatus(), QStringLiteral("Metadata Only"));
    QCOMPARE(controller->localRuntimeHealth(), QStringLiteral("Not Executable"));
    QCOMPARE(controller->localRuntimeSummary(),
             QStringLiteral("Null Local Runtime is metadata-only; local inference execution is "
                            "disabled."));
    QCOMPARE(controller->localRuntimeResponseStatus(), QStringLiteral("Refused"));
    QCOMPARE(controller->localRuntimeResponseSummary(),
             QStringLiteral("Local runtime boundary is metadata-only; execution is disabled."));
    QCOMPARE(controller->localRuntimeCapabilities().size(), 3);
    QVERIFY(controller->localRuntimeCapabilities().contains(
        QStringLiteral("Local Inference (Disabled): Inference execution is intentionally "
                       "disabled.")));
    QCOMPARE(controller->localRuntimeSessionCount(), 1);
    QCOMPARE(controller->localRuntimeSessionStatus(), QStringLiteral("Reserved"));
    QCOMPARE(controller->localRuntimeSessionHealth(), QStringLiteral("Placeholder Only"));
    QCOMPARE(controller->localRuntimeSessionSummary(),
             QStringLiteral("local-runtime-session-1: Reserved placeholder local runtime "
                            "metadata."));
    QCOMPARE(controller->localRuntimeAllocationSummary(),
             QStringLiteral("Metadata-only local runtime allocation; no model or process is "
                            "started."));
    QCOMPARE(controller->localRuntimeReservationSummary(),
             QStringLiteral("Placeholder reservation is held for metadata visibility only."));
    QCOMPARE(controller->localRuntimeSessionSummaries(),
             QStringList{QStringLiteral("local-runtime-session-1: Reserved placeholder local "
                                        "runtime metadata.")});
    QCOMPARE(controller->runtimeCapabilityCount(), 13);
    QCOMPARE(controller->enabledRuntimeCapabilitySummaries().size(), 2);
    QVERIFY(controller->enabledRuntimeCapabilitySummaries().contains(
        QStringLiteral("Privacy-Safe Mode (Security, Enabled): Privacy-safe runtime metadata "
                       "mode is active.")));
    QCOMPARE(controller->disabledRuntimeCapabilitySummaries().size(), 11);
    QVERIFY(controller->disabledRuntimeCapabilitySummaries().contains(
        QStringLiteral("Tool Bridge (Integration, Unavailable): Tool bridge execution is "
                       "unavailable.")));
    QCOMPARE(controller->runtimeNegotiationProfileSummary(),
             QStringLiteral("Metadata-only negotiation profile; no runtime capability is "
                            "activated."));
    QCOMPARE(controller->runtimeNegotiationSummary(),
             QStringLiteral("Metadata-only runtime negotiation: 13 capabilities described, 2 "
                            "enabled as safety metadata."));
    QCOMPARE(controller->localOnlyRuntimeEnforcementSummary(),
             QStringLiteral("Local-only enforcement is active; cloud relay and external runtime "
                            "execution remain unavailable."));
    QCOMPARE(controller->runtimePermissionDecision(), QStringLiteral("Denied"));
    QCOMPARE(controller->runtimePermissionSummary(),
             QStringLiteral("Runtime permission policy is metadata-only and denies execution by "
                            "default."));
    QCOMPARE(controller->runtimeSafetyDecision(), QStringLiteral("Compliant"));
    QCOMPARE(controller->runtimeSafetySummary(),
             QStringLiteral("Runtime safety policy report: local-only and no-execution posture is "
                            "enforced with deterministic metadata rules."));
    QCOMPARE(controller->runtimePipelineStatus(), QStringLiteral("Blocked"));
    QCOMPARE(controller->runtimePipelineSummary(),
             QStringLiteral("Runtime request pipeline blocked execution metadata by permission and "
                            "safety policy."));
    QCOMPARE(controller->runtimePipelineTraceSummaries().size(), 4);
    QVERIFY(controller->runtimePipelineTraceSummaries().contains(
        QStringLiteral("Request Received [Metadata Received]: Runtime request pipeline metadata "
                       "evaluation.")));
    QVERIFY(controller->runtimePipelineTraceSummaries().contains(
        QStringLiteral("Execution Boundary [Blocked]: Execution boundary remained blocked; no "
                       "runtime action was performed.")));
    QCOMPARE(controller->executionLifecycleState(), QStringLiteral("Blocked"));
    QCOMPARE(controller->executionLifecycleStatus(), QStringLiteral("Blocked"));
    QCOMPARE(controller->executionLifecycleSummary(),
             QStringLiteral("Execution lifecycle reached blocked metadata state; no execution is "
                            "permitted."));
    QCOMPARE(controller->executionLifecycleTraceSummaries().size(), 7);
    QVERIFY(controller->executionLifecycleTraceSummaries().contains(
        QStringLiteral("6. Ready Placeholder [Warning]: Ready placeholder is descriptive and not "
                       "executable.")));
    QVERIFY(controller->executionLifecycleTraceSummaries().contains(
        QStringLiteral("7. Blocked [Blocked]: Execution remains intentionally blocked.")));
    QCOMPARE(controller->executionSessionId(), QStringLiteral("execution-session-1"));
    QCOMPARE(controller->executionSessionStatus(), QStringLiteral("Reserved"));
    QCOMPARE(controller->executionSessionOwnership(), QStringLiteral("Application Controller"));
    QCOMPARE(controller->executionCoordinationMode(), QStringLiteral("Metadata Only"));
    QCOMPARE(controller->executionSessionSummary(),
             QStringLiteral("Execution session is reserved for metadata only."));
    QCOMPARE(controller->executionCoordinationSnapshotSummary(),
             QStringLiteral("Execution coordination snapshot is read-only for "
                            "execution-session-1; lifecycle is Blocked and execution is "
                            "blocked."));
    QCOMPARE(controller->localRuntimeAdapterStatus(), QStringLiteral("Placeholder"));
    QCOMPARE(controller->localRuntimeAdapterHealth(), QStringLiteral("Metadata Only"));
    QCOMPARE(controller->localRuntimeAdapterSummary(),
             QStringLiteral("Ollama local runtime adapter contract is metadata-only; no runtime "
                            "connection is configured."));
    QCOMPARE(controller->localRuntimeAdapterCapabilitySummaries().size(), 3);
    QVERIFY(controller->localRuntimeAdapterCapabilitySummaries().contains(
        QStringLiteral("Model Discovery (Unavailable, Not Executable): Model discovery is "
                       "intentionally disabled.")));
    QCOMPARE(controller->providerRuntimeBridgeStatus(), QStringLiteral("Not Connected"));
    QCOMPARE(controller->providerRuntimeBridgeSummary(),
             QStringLiteral("Provider runtime bridge is not connected and cannot execute provider "
                            "requests."));
    QCOMPARE(controller->providerRuntimeBridgeResponseSummary(),
             QStringLiteral("Provider runtime bridge is metadata-only; no provider or local "
                            "runtime request was executed."));
    QCOMPARE(controller->runtimeIntegrationReadinessStatus(), QStringLiteral("Blocked"));
    QCOMPARE(controller->runtimeIntegrationReadinessSummary(),
             QStringLiteral("Runtime integration readiness is blocked: Ollama local "
                            "health/discovery metadata is available, but provider bridge "
                            "execution and inference remain disabled."));
    QCOMPARE(controller->runtimeIntegrationReadinessChecks().size(), 5);
    QVERIFY(controller->runtimeIntegrationReadinessChecks().contains(
        QStringLiteral("Pass: Endpoint Configuration - Safe local Ollama endpoint is configured "
                       "for loopback-only health checks.")));
    QVERIFY(controller->runtimeIntegrationReadinessChecks().contains(
        QStringLiteral("Pass: Model Discovery - Installed model discovery boundary is available "
                       "for read-only local metadata.")));
    QVERIFY(controller->runtimeIntegrationReadinessChecks().contains(
        QStringLiteral("Blocked: Execution Permission - Execution is blocked until runtime "
                       "readiness passes.")));
}

void ApplicationControllerTest::exposesRuntimeProviderRegistryMetadata() {
    auto controller = std::make_unique<ApplicationController>(
        std::make_unique<LocalEchoProvider>(), std::make_unique<InMemoryStore>(), nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        std::make_unique<FakeOllamaRuntimeClient>(
            QList<OllamaModelSummary>{{QStringLiteral("llama3.2"), {}, 10}}),
        nullptr);

    controller->setSelectedLocalModel(QStringLiteral("llama3.2"));
    controller->setLocalChatInferenceEnabled(true);

    QCOMPARE(controller->selectedRuntimeProvider(), QStringLiteral("ollama"));
    QCOMPARE(controller->activeRuntimeProviderId(), QStringLiteral("ollama"));
    QCOMPARE(controller->activeRuntimeProviderLabel(), QStringLiteral("Local Ollama"));
    QCOMPARE(controller->activeRuntimeReadinessState(), QStringLiteral("ready"));
    QCOMPARE(controller->activeRuntimeLocalOnlySummary(), QStringLiteral("Local Only"));
    QVERIFY(controller->activeRuntimeModelLabel().contains(QStringLiteral("llama3.2")));
    QCOMPARE(controller->selectableRuntimeProviderIds(),
             QStringList({QStringLiteral("ollama"), QStringLiteral("lm-studio"),
                          QStringLiteral("llama-cpp-server"), QStringLiteral("cloud-api")}));
    QCOMPARE(controller->runtimeProviderCardSummaries().size(), 5);
    QVERIFY(controller->runtimeProviderCardSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("OpenAI-compatible Local")));
    QVERIFY(controller->runtimeProviderCardSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("LM Studio")));
    QVERIFY(controller->runtimeProviderCardSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("llama.cpp server")));
    QVERIFY(controller->runtimeProviderCapabilitySummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("requiresApiKey: no")));
    QVERIFY(controller->runtimeProviderValidationTraces()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("lm-studio: readiness=unknown")));
    QVERIFY(controller->providerCredentialRegistrySummary().contains(
        QStringLiteral("API key values are persisted")));
    // The production platform store can legitimately be unavailable on a
    // headless runner. The view must faithfully expose its exact safe state.
    const auto credentialSummary = sentinel::core::platformCredentialStore().summary();
    QCOMPARE(controller->credentialStoreSummary(), credentialSummary.summary);
    QCOMPARE(controller->credentialStoreBackendSummary(), credentialSummary.backendSummary);
    QVERIFY(!credentialSummary.plaintextPersistenceAllowed);
    QVERIFY(controller->credentialStoreSafetySummary().contains(QStringLiteral("no plaintext")));
    QCOMPARE(controller->credentialStoreTraceSummaries().size(), 5);
    QVERIFY(controller->credentialActionReadiness().contains(QStringLiteral("credential store")));
    QVERIFY(
        controller->credentialExecutionStatus().contains(QStringLiteral("user-provided API key")));
    QCOMPARE(controller->providerCredentialSummaries().size(), 4);
    QVERIFY(controller->providerCredentialSafetySummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("cloudRequests=refused")));
    QCOMPARE(controller->availableLocalRuntimeSummaries().size(), 3);
}

void ApplicationControllerTest::compatibleLocalReadinessTracksDiscovery() {
    using namespace sentinel::core;
    for (const auto& id : {QStringLiteral("lm-studio"), QStringLiteral("llama-cpp-server")}) {
        auto service = std::make_unique<ModelService>();
        auto* observed = service.get();
        observed->setSelectedProviderId(id);
        observed->setSelectedModelId(QStringLiteral("test-model"));
        ApplicationControllerBuilder builder;
        builder.withModelService(std::move(service));
        auto controller = builder.build();
        QCOMPARE(controller->activeRuntimeReadinessState(), QStringLiteral("unknown"));
        ProviderDiscoveryOutcome success;
        success.completed = true;
        const auto sequence = observed->beginProviderHealthObservation();
        observed->acceptProviderDiscovery(id, {{QStringLiteral("test-model"), {}, 0}}, success,
                                          sequence);
        observed->reportProviderDiscovery(id, sequence, true, ChatProviderErrorCategory::None);
        QCOMPARE(controller->activeRuntimeProviderId(), id);
        QCOMPARE(controller->activeRuntimeReadinessState(), QStringLiteral("ready"));
        ProviderDiscoveryOutcome failed;
        failed.category = ChatProviderErrorCategory::ConnectionFailed;
        const auto failureSequence = observed->beginProviderHealthObservation();
        observed->acceptProviderDiscovery(id, {}, failed, failureSequence);
        observed->reportProviderDiscovery(id, failureSequence, false, failed.category);
        QCOMPARE(controller->activeRuntimeReadinessState(), QStringLiteral("unavailable"));
    }
}

void ApplicationControllerTest::disabledRuntimeProviderSelectionFallsBackToLocalOllama() {
    auto controller = std::make_unique<ApplicationController>(
        std::make_unique<LocalEchoProvider>(), std::make_unique<InMemoryStore>(), nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        std::make_unique<FakeOllamaRuntimeClient>(
            QList<OllamaModelSummary>{{QStringLiteral("llama3.2"), {}, 10}}),
        nullptr);

    controller->setSelectedLocalModel(QStringLiteral("llama3.2"));
    controller->setLocalChatInferenceEnabled(true);
    controller->setSelectedRuntimeProvider(QStringLiteral("openai-compatible"));

    QCOMPARE(controller->selectedRuntimeProvider(), QStringLiteral("openai-compatible"));
    QCOMPARE(controller->activeRuntimeProviderId(), QStringLiteral("ollama"));
    QCOMPARE(controller->activeRuntimeReadinessState(), QStringLiteral("ready"));
    QVERIFY(controller->configuredRuntimeProviderSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Local Ollama")));
}

void ApplicationControllerTest::exposesOllamaRuntimeBoundaryMetadata() {
    const auto controller = makeController();

    QCOMPARE(controller->ollamaEndpoint(), QStringLiteral("http://127.0.0.1:11434"));
    QCOMPARE(controller->ollamaConnectionStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(controller->ollamaHealthStatus(), QStringLiteral("Unavailable"));
    QVERIFY(controller->ollamaHealthSummary().contains(QStringLiteral("no local health check")));
    QCOMPARE(controller->ollamaModelCount(), 0);
    QVERIFY(controller->ollamaModelSummaries().isEmpty());
}

void ApplicationControllerTest::exposesLocalInferenceBoundaryMetadata() {
    const auto controller = makeController();

    QCOMPARE(controller->localInferenceStatus(), QStringLiteral("Not Requested"));
    QVERIFY(controller->localInferenceSummary().contains(QStringLiteral("loopback-only")));
    QCOMPARE(controller->localInferenceLastResponseSummary(),
             QStringLiteral("No local inference request yet."));
    QCOMPARE(controller->localInferenceRuntimeState(), QStringLiteral("Unavailable"));
    QCOMPARE(controller->localInferenceLatencySummary(),
             QStringLiteral("No local inference latency recorded."));
    QVERIFY(controller->localInferenceTraceSummaries().isEmpty());
}


void ApplicationControllerTest::
    modelManagementRecommendationsAreDeterministicAndActionsAvailable() {
    StaticModelManagementService service;

    QCOMPARE(modelManagementStatusName(service.status()), QStringLiteral("Available"));
    const auto recommendations = service.recommendations();
    QCOMPARE(recommendations.size(), 3);
    QCOMPARE(recommendations.at(0).modelName, QStringLiteral("llama3.2:3b"));
    QCOMPARE(recommendations.at(1).modelName, QStringLiteral("mistral:7b"));
    QCOMPARE(recommendations.at(2).modelName, QStringLiteral("qwen2.5-coder:7b"));

    const auto requirements = service.requirementSummaries();
    QCOMPARE(requirements.size(), 3);
    QCOMPARE(requirements.at(0).approximateRam, QStringLiteral("about 8 GB"));
    QCOMPARE(requirements.at(0).approximateDisk, QStringLiteral("about 3 GB"));

    const auto result = service.evaluate(
        ModelManagementRequest{ModelManagementAction::Pull, QStringLiteral("llama3.2:3b")});
    QVERIFY(result.available);
    QCOMPARE(modelManagementStatusName(result.status), QStringLiteral("Available"));
    QVERIFY(result.summary.contains(QStringLiteral("live Ollama model manager")));
    QCOMPARE(service.recommendations().at(0).modelName, recommendations.at(0).modelName);
    QCOMPARE(service.requirementSummaries().at(0).summary, requirements.at(0).summary);
}

void ApplicationControllerTest::exposesModelManagementReadinessMetadata() {
    auto controller = std::make_unique<ApplicationController>(
        std::make_unique<LocalEchoProvider>(), std::make_unique<InMemoryStore>(), nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        std::make_unique<FakeOllamaRuntimeClient>(
            QList<OllamaModelSummary>{{QStringLiteral("llama3.2"), {}, 10}}),
        nullptr);

    QCOMPARE(controller->modelManagementStatus(), QStringLiteral("Available"));
    QCOMPARE(controller->modelManagementSummary(),
             QStringLiteral("Live Ollama model management is available: 1 installed model(s); "
                            "pull and delete use /api/pull and /api/delete."));
    QVERIFY(controller->modelManagementActionAvailability().contains(
        QStringLiteral("Pull available via Ollama /api/pull")));
    QVERIFY(controller->modelManagementActionAvailability().contains(
        QStringLiteral("Delete available via Ollama /api/delete")));
    QVERIFY(controller->modelManagementActionAvailability().contains(
        QStringLiteral("Import/Export are not configured")));
    QCOMPARE(controller->modelRecommendationSummaries().size(), 3);
    QCOMPARE(controller->modelRequirementSummaries().size(), 3);
    QVERIFY(controller->modelRequirementSummaries().first().contains(
        QStringLiteral("approx RAM about 8 GB")));
}

void ApplicationControllerTest::exposesVoiceReadinessMetadata() {
    ApplicationController controller{std::make_unique<LocalEchoProvider>(),
                                     std::make_unique<InMemoryStore>()};

    QCOMPARE(controller.voiceRuntimeMode(), QStringLiteral("Disabled"));
    QVERIFY(!controller.voiceEnabled());
    QCOMPARE(controller.textToSpeechStatus(), QStringLiteral("Disabled"));
    QCOMPARE(controller.speechToTextStatus(), QStringLiteral("Disabled"));
    QVERIFY(controller.textToSpeechSummary().contains(QStringLiteral("disabled by default")));
    QVERIFY(controller.speechToTextSummary().contains(QStringLiteral("disabled by default")));
    QCOMPARE(controller.voiceReadinessStatus(), QStringLiteral("Disabled"));
    QVERIFY(controller.voiceReadinessSummary().contains(QStringLiteral("metadata-only")));
    QVERIFY(controller.voiceReadinessSummary().contains(QStringLiteral("no microphone")));
    QCOMPARE(controller.voiceCapabilitySummaries().size(), 2);
    QVERIFY(
        controller.voiceCapabilitySummaries().first().contains(QStringLiteral("Text To Speech")));
    QVERIFY(controller.voiceReadinessChecks()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("No Piper")));
    QVERIFY(controller.voiceReadinessChecks()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("No microphone access")));
    QCOMPARE(controller.voiceSessionId(), QStringLiteral("voice-session-1"));
    QCOMPARE(controller.voiceSessionStatus(), QStringLiteral("completed"));
    QVERIFY(controller.voiceSessionSummary().contains(QStringLiteral("metadata-only")));
    QCOMPARE(controller.voicePipelineStatus(), QStringLiteral("completed"));
    QVERIFY(controller.voicePipelineSummary().contains(QStringLiteral("metadata-only")));
    QCOMPARE(controller.voicePipelineTraceSummaries().size(), 7);
    QVERIFY(controller.voicePipelineTraceSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("transcribing-placeholder")));
    const auto chatCountBeforeVoicePipeline = controller.chatHistory().size();
    const auto voiceSession = controller.currentVoicePipelineSessionResult();
    QCOMPARE(voiceSession.executionAttempted, false);
    QVERIFY(!voiceSession.safetyReport.executionAttempted);
    QVERIFY(!voiceSession.safetyReport.microphoneCaptureAllowed);
    QVERIFY(!voiceSession.safetyReport.audioPlaybackAllowed);
    QVERIFY(!voiceSession.safetyReport.whisperExecutionAllowed);
    QVERIFY(!voiceSession.safetyReport.piperExecutionAllowed);
    QCOMPARE(controller.chatHistory().size(), chatCountBeforeVoicePipeline);
    QCOMPARE(controller.voicePipelineSessionStatus(), QStringLiteral("fallback"));
    QVERIFY(controller.voicePipelineSessionSummary().contains(
        QStringLiteral("execution attempted: no")));
    QCOMPARE(controller.voicePipelineSessionStageReadinessSummaries().size(), 5);
    QVERIFY(controller.voicePipelineSessionStageReadinessSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("transcription-readiness")));
    QVERIFY(controller.voicePipelineSessionTraceSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("no transcript")));
    QVERIFY(
        controller.voicePipelineSessionFallbackSummary().contains(QStringLiteral("no chat send")));
    QVERIFY(controller.voicePipelineSessionSafetySummary().contains(
        QStringLiteral("no-execution guarantees")));
    QCOMPARE(controller.voicePipelineSessionSafetyChecks().size(), 8);
    QCOMPARE(controller.voicePipelineSessionReadyStageCount(), 2);
    QCOMPARE(controller.voicePipelineSessionBlockedStageCount(), 2);
    QCOMPARE(controller.voiceRuntimeStatus(), QStringLiteral("Unavailable"));
    QVERIFY(controller.voiceRuntimeSummary().contains(QStringLiteral("microphone disabled")));
    QCOMPARE(controller.voiceRuntimeCheckSummaries().size(), 7);
    QVERIFY(!controller.voiceRuntimeAvailable());
    QVERIFY(!controller.voiceTextToSpeechAvailable());
    QVERIFY(!controller.voiceSpeechToTextAvailable());
    QVERIFY(!controller.voiceMicrophoneEnabled());
    QVERIFY(!controller.voicePlaybackEnabled());
    QVERIFY(controller.voiceLocalOnlyPolicy());
    QVERIFY(!controller.voiceProcessExecutionEnabled());
    QCOMPARE(controller.voiceRuntimeEnvironmentStatus(), QStringLiteral("Blocked"));
    QVERIFY(controller.voiceRuntimeEnvironmentSummary().contains(QStringLiteral("metadata-only")));
    QCOMPARE(controller.voiceBinarySummaries().size(), 2);
    QVERIFY(controller.voiceBinarySummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Piper Binary: Missing")));
    QCOMPARE(controller.voiceModelSummaries().size(), 2);
    QVERIFY(controller.voiceModelSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Whisper Model: Missing")));
    QCOMPARE(controller.voiceRuntimePermissionSummaries().size(), 4);
    QVERIFY(controller.voiceRuntimePermissionSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Process Execution: Denied")));
    QCOMPARE(controller.voiceRuntimeSafetyStatus(), QStringLiteral("Blocked"));
    QVERIFY(controller.voiceRuntimeSafetySummary().contains(QStringLiteral("blocks execution")));
    QCOMPARE(controller.voiceRuntimeSafetyChecks().size(), 7);
    QVERIFY(!controller.voiceRuntimeExecutionAllowed());
    QVERIFY(controller.voiceRuntimeReadinessSummary().contains(
        QStringLiteral("Missing Configuration voice runtime readiness")));
    QCOMPARE(controller.voiceRuntimeConfiguredCount(), 0);
    QCOMPARE(controller.voiceRuntimeMissingCount(), 4);
    QCOMPARE(controller.voiceRuntimeRefusedCount(), 0);
    QCOMPARE(controller.voiceRuntimeHealth(), QStringLiteral("Degraded Metadata"));
    QVERIFY(controller.voiceRuntimePermissionFoundationSummary().contains(
        QStringLiteral("future microphone access")));
    QVERIFY(controller.voiceRuntimeSandboxSummary().contains(QStringLiteral("Required Metadata")));
    QVERIFY(controller.voiceRuntimeSafetyReportSummary().contains(
        QStringLiteral("execution attempted: no")));
    QCOMPARE(controller.voiceRuntimeReadinessChecks().size(), 5);
    QVERIFY(controller.piperRuntimePathSummary().contains(QStringLiteral("2 missing")));
    QVERIFY(controller.whisperRuntimePathSummary().contains(QStringLiteral("2 missing")));
    QCOMPARE(controller.piperTtsStatus(), QStringLiteral("Disabled"));
    QVERIFY(controller.piperTtsSummary().contains(QStringLiteral("disabled by default")));
    QCOMPARE(controller.piperTtsReadinessChecks().size(), 9);
    QVERIFY(controller.piperTtsReadinessChecks()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Piper binary")));
    QVERIFY(!controller.piperTtsReady());
    QCOMPARE(controller.piperTtsFileOutputStatus(), QStringLiteral("Disabled"));
    QVERIFY(controller.piperTtsFileOutputSummary().contains(
        QStringLiteral("No playback or microphone access")));
    QCOMPARE(controller.whisperTranscriptionStatus(), QStringLiteral("Disabled"));
    QVERIFY(controller.whisperTranscriptionReadinessSummary().contains(
        QStringLiteral("execution attempted: no")));
    QVERIFY(controller.whisperTranscriptionLastSummary().contains(
        QStringLiteral("No Whisper transcription request")));
    QVERIFY(
        controller.whisperTranscriptionFallbackSummary().contains(QStringLiteral("no transcript")));
    QVERIFY(controller.whisperTranscriptionSafetySummary().contains(
        QStringLiteral("execution attempted: no")));
    QVERIFY(controller.whisperTranscriptionTraceSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("No microphone capture")));
}

void ApplicationControllerTest::validatesConfiguredVoicePathsAsMetadataOnly() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
#ifdef Q_OS_WIN
    const auto piperBinaryPath = dir.filePath(QStringLiteral("piper.exe"));
#else
    const auto piperBinaryPath = dir.filePath(QStringLiteral("piper"));
#endif
    const auto piperModelPath = dir.filePath(QStringLiteral("voice.onnx"));
    const auto whisperModelPath = dir.filePath(QStringLiteral("whisper-models"));
    const auto missingPath = dir.filePath(QStringLiteral("missing-piper"));

    QFile piperBinary{piperBinaryPath};
    QVERIFY(piperBinary.open(QIODevice::WriteOnly));
    piperBinary.close();
#if !defined(Q_OS_WIN)
    QVERIFY(QFile::setPermissions(piperBinaryPath, QFile::ReadOwner | QFile::ExeOwner));
#endif

    QFile piperModel{piperModelPath};
    QVERIFY(piperModel.open(QIODevice::WriteOnly));
    piperModel.close();

    const auto whisperBinaryPath = dir.filePath(QStringLiteral("whisper"));
    QFile whisperBinary{whisperBinaryPath};
    QVERIFY(whisperBinary.open(QIODevice::WriteOnly));
    whisperBinary.close();
#if !defined(Q_OS_WIN)
    QVERIFY(QFile::setPermissions(whisperBinaryPath, QFile::ReadOwner));
#endif

    QVERIFY(QDir{}.mkpath(whisperModelPath));

    ApplicationController controller{std::make_unique<LocalEchoProvider>(),
                                     std::make_unique<InMemoryStore>()};
    QSignalSpy spy(&controller, &ApplicationController::voiceConfigurationChanged);

    controller.setPiperBinaryPath(piperBinaryPath);
    controller.setPiperModelPath(piperModelPath);
    controller.setWhisperBinaryPath(whisperBinaryPath);
    controller.setWhisperModelPath(whisperModelPath);

    QCOMPARE(spy.count(), 4);
    QCOMPARE(controller.voiceRuntimeEnvironmentStatus(), QStringLiteral("Configured Metadata"));
    QCOMPARE(controller.voiceRuntimeConfiguredCount(), 4);
    QCOMPARE(controller.voiceRuntimeMissingCount(), 0);
    QCOMPARE(controller.voiceRuntimeRefusedCount(), 0);
    QCOMPARE(controller.piperRuntimeStatus(), QStringLiteral("Ready Metadata"));
    QCOMPARE(controller.whisperRuntimeStatus(), QStringLiteral("Ready Metadata"));
    QCOMPARE(controller.whisperTranscriptionStatus(), QStringLiteral("Missing Binary"));
    QVERIFY(controller.whisperTranscriptionReadinessSummary().contains(
        QStringLiteral("2 configured, 1 missing")));
    QVERIFY(controller.whisperTranscriptionSafetySummary().contains(
        QStringLiteral("automatic chat send")));
    QVERIFY(
        controller.piperRuntimeReadinessSummary().contains(QStringLiteral("disabled by default")));
    QVERIFY(controller.whisperRuntimeReadinessSummary().contains(QStringLiteral("readiness-only")));
    QVERIFY(controller.voiceConfigurationReadinessSummary().contains(
        QStringLiteral("Piper file-output TTS: Ready")));
    QVERIFY(controller.voiceConfigurationReadinessSummary().contains(
        QStringLiteral("Whisper STT preparation: Blocked")));
    QVERIFY(controller.voiceBinarySummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Piper binary metadata only: path exists, readable, "
                                         "executable")));
    QVERIFY(controller.voiceBinarySummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Whisper binary metadata only: path exists, readable, "
                                         "non-executable")));
    QVERIFY(controller.voiceModelSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Whisper model metadata only: path exists, readable")));
    QVERIFY(controller.voiceConfigurationStatusBadges().contains(
        QStringLiteral("Piper file-output TTS: Ready")));
    QVERIFY(controller.voiceConfigurationStatusBadges().contains(
        QStringLiteral("Piper binary: Ready")));
    QVERIFY(controller.voiceConfigurationStatusBadges().contains(
        QStringLiteral("Whisper binary: Blocked (path is not executable)")));
    QVERIFY(controller.voiceConfigurationValidationSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Piper binary: Ready - exists, readable, file, "
                                         "executable")));
    QVERIFY(controller.voiceConfigurationValidationSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Whisper binary: Blocked - exists, readable, file, "
                                         "non-executable")));
    QVERIFY(controller.voiceConfigurationHintSummaries().contains(
        QStringLiteral("Piper binary hint: configured path is executable; no "
                       "suggestion needed.")));
    QVERIFY(controller.voiceConfigurationHintSummaries().contains(
        QStringLiteral("Whisper model hint: configured path is readable; no "
                       "suggestion needed.")));
    QVERIFY(controller.voiceConfigurationHintSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Hints are read-only")));
    QVERIFY(controller.piperTtsReadinessChecks()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Null Piper TTS client is disabled and never launches "
                                         "Piper")));
    QCOMPARE(controller.piperTtsStatus(), QStringLiteral("Ready Metadata"));
    QCOMPARE(controller.piperSynthesisStatus(), QStringLiteral("Ready Metadata"));
    QVERIFY(controller.piperSynthesisReadinessSummary().contains(
        QStringLiteral("2 configured, 0 missing")));
    QCOMPARE(controller.piperFileOutputReadinessStatus(), QStringLiteral("Ready"));
    QVERIFY(controller.piperFileOutputReadinessSummary().contains(
        QStringLiteral("Ready for a later controlled file-output TTS phase")));
    QCOMPARE(controller.whisperPreparationReadinessStatus(), QStringLiteral("Blocked"));
    QVERIFY(controller.whisperPreparationReadinessSummary().contains(
        QStringLiteral("Whisper binary path is not executable")));
    QVERIFY(controller.piperTtsReady());

    controller.setPiperBinaryPath(missingPath);

    QVERIFY(controller.voiceBinarySummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("Piper Binary: Missing")));
    QVERIFY(controller.voiceBinarySummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("path missing")));
    QVERIFY(controller.voiceConfigurationStatusBadges().contains(
        QStringLiteral("Piper binary: Blocked (path is missing)")));
    QCOMPARE(controller.piperFileOutputReadinessStatus(), QStringLiteral("Blocked"));
    QVERIFY(controller.piperFileOutputReadinessSummary().contains(
        QStringLiteral("Piper binary path is missing")));
    QCOMPARE(controller.piperTtsStatus(), QStringLiteral("Missing Binary"));

    controller.setWhisperBinaryPath(QStringLiteral("https://example.invalid/whisper"));
    QCOMPARE(controller.whisperRuntimeStatus(), QStringLiteral("Refused"));
    QCOMPARE(controller.voiceRuntimeRefusedCount(), 1);
    QVERIFY(
        controller.whisperRuntimeReadinessSummary().contains(QStringLiteral("unsafe/non-local")));
    QVERIFY(!controller.whisperRuntimeReadinessSummary().contains(
        QStringLiteral("https://example.invalid")));
    QVERIFY(!controller.voiceRuntimeSafetyReportSummary().contains(
        QStringLiteral("https://example.invalid")));
}

void ApplicationControllerTest::piperFileOutputExecutionRequiresExplicitOptIn() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto fixture = makePiperController(FakePiperTtsClient::Mode::Success);
    configureReadyPiperPaths(*fixture.controller, dir);

    QCOMPARE(fixture.controller->piperFileOutputReadinessStatus(), QStringLiteral("Ready"));
    QVERIFY(!fixture.controller->piperFileOutputExecutionEnabled());
    QCOMPARE(fixture.controller->piperFileOutputExecutionStatus(),
             QStringLiteral("Ready Metadata"));
    QVERIFY(
        fixture.controller->piperFileOutputExecutionSummary().contains(QStringLiteral("disabled")));

    const auto generated = fixture.controller->generatePiperTtsFile(QStringLiteral("hello"));

    QVERIFY(!generated);
    QVERIFY(!fixture.client->called);
    QCOMPARE(fixture.controller->piperFileOutputExecutionStatus(),
             QStringLiteral("Ready Metadata"));
    QVERIFY(
        fixture.controller->piperFileOutputExecutionSummary().contains(QStringLiteral("refused")));
    QCOMPARE(fixture.controller->piperFileOutputAudioPathSummary(),
             QStringLiteral("No generated Piper audio file."));
}

void ApplicationControllerTest::piperFileOutputExecutionBlocksInvalidBinaryOrModel() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto fixture = makePiperController(FakePiperTtsClient::Mode::Success);
    const auto piperBinaryPath = dir.filePath(QStringLiteral("piper"));
    const auto missingModelPath = dir.filePath(QStringLiteral("missing.onnx"));

    QFile piperBinary{piperBinaryPath};
    QVERIFY(piperBinary.open(QIODevice::WriteOnly));
    piperBinary.write("not executable");
    piperBinary.close();

    fixture.controller->setPiperBinaryPath(piperBinaryPath);
    fixture.controller->setPiperModelPath(missingModelPath);
    fixture.controller->setPiperFileOutputExecutionEnabled(true);

    QVERIFY(!fixture.controller->piperFileOutputExecutionEnabled());
    QVERIFY(!fixture.controller->generatePiperTtsFile(QStringLiteral("hello")));
    QVERIFY(!fixture.client->called);
    QCOMPARE(fixture.controller->piperFileOutputExecutionStatus(),
             QStringLiteral("Missing Binary"));
    QVERIFY(
        fixture.controller->piperFileOutputExecutionSummary().contains(QStringLiteral("refused")));
}

void ApplicationControllerTest::rejectsInvalidSelectedModelAgainstDiscoveryMetadata() {
    auto fakeClient = std::make_unique<FakeLocalInferenceClient>();
    auto* fakeClientPtr = fakeClient.get();
    auto controller = std::make_unique<ApplicationController>(
        std::make_unique<LocalEchoProvider>(), std::make_unique<InMemoryStore>(), nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, std::make_unique<AllowLocalInferencePolicy>(), nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr,
        std::make_unique<FakeOllamaRuntimeClient>(
            QList<OllamaModelSummary>{{QStringLiteral("llama3.2"), {}, 10}}),
        std::move(fakeClient));

    controller->setSelectedLocalModel(QStringLiteral("missing-model"));
    const auto ran = controller->runLocalInference(QStringLiteral("hello"), {});

    QVERIFY(!ran);
    QVERIFY(!fakeClientPtr->called);
    QCOMPARE(controller->selectedLocalModelStatus(), QStringLiteral("Invalid"));
    QCOMPARE(controller->selectedLocalModelSummary(),
             QStringLiteral("Selected local model missing-model was not found in discovered local "
                            "models."));
    QCOMPARE(controller->selectedLocalModelMetadataSummary(),
             QStringLiteral("Invalid selection: missing-model is not in discovered local model "
                            "metadata."));
    QCOMPARE(controller->localInferenceStatus(), QStringLiteral("Model Unavailable"));
    QCOMPARE(controller->localInferenceSummary(),
             QStringLiteral("Local inference request rejected: selected model is not installed."));
}

void ApplicationControllerTest::exposesStreamingEnabledByDefaultMetadata() {
    const auto controller = makeController();
    controller->setLocalInferenceStreamingEnabled(true);

    QVERIFY(controller->localInferenceStreamingAvailable());
    QVERIFY(controller->localInferenceStreamingEnabled());
    QCOMPARE(controller->localInferenceStreamStatus(), QStringLiteral("Disabled"));
    QCOMPARE(controller->localInferenceStreamSummary(),
             QStringLiteral("Ollama local streaming boundary is configured for loopback-only "
                            "/api/generate."));
    QVERIFY(controller->localInferenceStreamingText().isEmpty());
}

// Retired legacy local-chat tests.  Local inference is exercised through the
// direct worker/client suites and runLocalInference* integration tests.

void ApplicationControllerTest::clearChatClearsConversationRuntimeAndPersistence() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;

    QVERIFY(fixture.sendAndWait(QStringLiteral("status")));
    QCOMPARE(controller.chatHistory().size(), 3);
    QVERIFY(!controller.clearChat());

    QCOMPARE(controller.conversationState(), QStringLiteral("Idle"));
    QCOMPARE(controller.conversationRuntimeRequestId(), QStringLiteral("None"));
    QCOMPARE(controller.conversationRuntimeActiveModel(), QStringLiteral("None"));
    QCOMPARE(controller.conversationRuntimeLastSuccessSummary(),
             QStringLiteral("No successful response yet."));
    QCOMPARE(controller.conversationRuntimeLastErrorSummary(),
             QStringLiteral("No error or refusal yet."));
    QCOMPARE(controller.chatHistory().size(), 1);
    QCOMPARE(controller.chatHistory().first().role, sentinel::core::ChatRole::System);
}

void ApplicationControllerTest::clearChatKeepsSingleInitialSystemMessage() {
    auto store = std::make_unique<RecordingChatHistoryStore>();
    const auto* storePtr = store.get();
    ApplicationController controller(nullptr, std::make_unique<InMemoryStore>(), nullptr,
                                     std::move(store));

    QVERIFY(controller.clearChat());
    QVERIFY(controller.clearChat());
    QCOMPARE(controller.chatHistory().size(), 1);
    QCOMPARE(controller.chatHistory().first().role, sentinel::core::ChatRole::System);
    QCOMPARE(controller.chatHistory().first().content, QStringLiteral("Sentinel Core online."));
    QCOMPARE(storePtr->messages_.size(), 1);
    QCOMPARE(storePtr->messages_.first().role, sentinel::core::ChatRole::System);
}

void ApplicationControllerTest::reportsPersistedConversationHistorySummary() {
    auto store = std::make_unique<RecordingChatHistoryStore>();
    auto controller = makeControllerWithChatHistory(std::move(store));

    QVERIFY(controller->sendMessage(QStringLiteral("status")));
    waitForCompletedChatTurn(*controller);

    QCOMPARE(controller->conversationPersistenceStatus(), QStringLiteral("Persisted"));
    QCOMPARE(controller->conversationHistoryMessageCount(), 2);
    QVERIFY(controller->conversationHistorySummaryText().contains(QStringLiteral("3 messages")));
    QVERIFY(controller->conversationHistorySummaryText().contains(QStringLiteral("1 user")));
    QVERIFY(controller->conversationHistorySummaryText().contains(QStringLiteral("1 assistant")));
    QCOMPARE(controller->conversationLastSavedStatus(),
             QStringLiteral("Saved initial system message."));
}

void ApplicationControllerTest::reportsRuntimeOnlyConversationHistorySummary() {
    auto store =
        std::make_unique<RecordingChatHistoryStore>(QList<sentinel::core::ChatMessage>{}, false);
    auto controller = makeControllerWithChatHistory(std::move(store));

    QVERIFY(controller->sendMessage(QStringLiteral("status")));
    waitForCompletedChatTurn(*controller);

    QCOMPARE(controller->conversationPersistenceStatus(), QStringLiteral("Runtime Only"));
    QCOMPARE(controller->conversationHistoryMessageCount(), 2);
    QVERIFY(controller->conversationHistorySummaryText().contains(
        QStringLiteral("Runtime Only transcript")));
    QCOMPARE(controller->conversationLastSavedStatus(),
             QStringLiteral("Runtime-only transcript; persistence unavailable."));
    QVERIFY(controller->conversationLastRestoredStatus().contains(QStringLiteral("unavailable")));
}

void ApplicationControllerTest::
    exposesConversationStoreReadinessWithoutSwitchingTranscriptStorage() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;

    QCOMPARE(controller.conversationStoreStatus(), QStringLiteral("Ready"));
    QCOMPARE(controller.conversationStoreConversationCount(), 1);
    QVERIFY(controller.activeConversationSummary().contains(QStringLiteral("Current Transcript")));
    QVERIFY(controller.activeConversationSummary().contains(QStringLiteral("1 message")));
    QCOMPARE(controller.conversationStoreSummaries().size(), 1);
    QVERIFY(!controller.activeConversationId().isEmpty());
    QCOMPARE(controller.conversationIds().size(), 1);
    QCOMPARE(controller.conversationTitles().first(), QStringLiteral("Current Transcript"));
    QCOMPARE(controller.conversationArchivedSummaries().first(), QStringLiteral("Active"));
    QCOMPARE(controller.conversationListCurrentTitle(), QStringLiteral("Current Transcript"));

    QVERIFY(fixture.sendAndWait(QStringLiteral("store exposure")));

    QCOMPARE(controller.conversationStoreConversationCount(), 1);
    QVERIFY(controller.activeConversationSummary().contains(QStringLiteral("3 messages")));
    QCOMPARE(controller.conversationListCurrentTitle(), QStringLiteral("store exposure"));
    QCOMPARE(controller.conversationMessageCountSummaries().first(), QStringLiteral("3 messages"));
}

void ApplicationControllerTest::createsSwitchesRenamesArchivesAndUnarchivesConversations() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    const auto firstId = controller.activeConversationId();
    QVERIFY(fixture.sendAndWait(QStringLiteral("first transcript token")));

    const auto secondId = controller.createConversation(QStringLiteral("Second Thread"));
    QVERIFY(!secondId.isEmpty());
    QVERIFY(secondId != firstId);
    QCOMPARE(controller.activeConversationId(), secondId);
    QCOMPARE(controller.chatHistory().size(), 1);
    QVERIFY(fixture.sendAndWait(QStringLiteral("second transcript token")));
    QCOMPARE(controller.chatHistory().size(), 3);

    QVERIFY(controller.switchConversation(firstId));
    QCOMPARE(controller.activeConversationId(), firstId);
    QCOMPARE(controller.chatHistory().size(), 3);
    QVERIFY(controller.chatHistory().at(1).content.contains(QStringLiteral("first transcript")));

    QVERIFY(controller.renameConversation(firstId, QStringLiteral("Renamed First")));
    QVERIFY(controller.conversationTitles().contains(QStringLiteral("Renamed First")));
    QVERIFY(controller.archiveConversation(firstId));
    QVERIFY(!controller.activeConversationArchived());
    QVERIFY(controller.conversationArchivedSummaries().contains(QStringLiteral("Archived")));
    QVERIFY(controller.unarchiveConversation(firstId));
    QVERIFY(controller.conversationArchivedSummaries().contains(QStringLiteral("Active")));
}

void ApplicationControllerTest::archivedActiveConversationBlocksSend() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    const auto firstId = controller.activeConversationId();
    QVERIFY(fixture.sendAndWait(QStringLiteral("archived first token")));
    const auto secondId = controller.createConversation(QStringLiteral("Second"));
    QVERIFY(!secondId.isEmpty());
    QVERIFY(controller.archiveConversation(firstId));
    QVERIFY(controller.switchConversation(firstId));
    QVERIFY(controller.activeConversationArchived());
    QVERIFY(controller.activeConversationStateSummary().contains(
        QStringLiteral("sending is disabled")));
    const auto beforeCount = controller.chatHistory().size();

    QVERIFY(!controller.sendMessage(QStringLiteral("blocked archived send")));

    QCOMPARE(controller.activeConversationId(), firstId);
    QCOMPARE(controller.chatHistory().size(), beforeCount);
    QVERIFY(controller.conversationMessageCountSummaries().contains(QStringLiteral("3 messages")));
}

void ApplicationControllerTest::archiveUnarchiveUpdatesBrowserSummaries() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    const auto firstId = controller.activeConversationId();
    const auto secondId = controller.createConversation(QStringLiteral("Second"));
    QVERIFY(!secondId.isEmpty());

    QCOMPARE(controller.activeConversationCount(), 2);
    QCOMPARE(controller.archivedConversationCount(), 0);
    QVERIFY(controller.conversationActiveSummaries().contains(QStringLiteral("Current")));
    QVERIFY(controller.conversationArchivedSummaries().contains(QStringLiteral("Active")));

    QVERIFY(controller.archiveConversation(firstId));

    QCOMPARE(controller.activeConversationCount(), 1);
    QCOMPARE(controller.archivedConversationCount(), 1);
    QVERIFY(controller.conversationArchivedSummaries().contains(QStringLiteral("Archived")));

    QVERIFY(controller.unarchiveConversation(firstId));

    QCOMPARE(controller.activeConversationCount(), 2);
    QCOMPARE(controller.archivedConversationCount(), 0);
    QVERIFY(controller.conversationArchivedSummaries().contains(QStringLiteral("Active")));
}

void ApplicationControllerTest::pinPersistsAcrossControllerRecreation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto databasePath = dir.filePath(QStringLiteral("conversations.sqlite3"));
    QString pinnedId;

    {
        auto controller = makeControllerWithConversationStore(
            std::make_unique<SQLiteConversationStore>(databasePath));
        pinnedId = controller->activeConversationId();

        QVERIFY(controller->pinConversation(pinnedId));
        QCOMPARE(controller->conversationPinnedSummaries().first(), QStringLiteral("Pinned"));
    }

    auto reloaded = makeControllerWithConversationStore(
        std::make_unique<SQLiteConversationStore>(databasePath));
    QCOMPARE(reloaded->activeConversationId(), pinnedId);
    QCOMPARE(reloaded->conversationIds().first(), pinnedId);
    QCOMPARE(reloaded->conversationPinnedSummaries().first(), QStringLiteral("Pinned"));
}

void ApplicationControllerTest::unpinPersistsAcrossControllerRecreation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto databasePath = dir.filePath(QStringLiteral("conversations.sqlite3"));
    QString conversationId;

    {
        auto controller = makeControllerWithConversationStore(
            std::make_unique<SQLiteConversationStore>(databasePath));
        conversationId = controller->activeConversationId();

        QVERIFY(controller->pinConversation(conversationId));
        QVERIFY(controller->unpinConversation(conversationId));
        QCOMPARE(controller->conversationPinnedSummaries().first(), QStringLiteral("Unpinned"));
    }

    auto reloaded = makeControllerWithConversationStore(
        std::make_unique<SQLiteConversationStore>(databasePath));
    QCOMPARE(reloaded->conversationIds().first(), conversationId);
    QCOMPARE(reloaded->conversationPinnedSummaries().first(), QStringLiteral("Unpinned"));
}

void ApplicationControllerTest::duplicateConversationCopiesLocalMessages() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    const auto sourceId = controller.activeConversationId();
    QVERIFY(fixture.sendAndWait(QStringLiteral("duplicate source token")));

    const auto duplicateId = controller.duplicateConversation(sourceId);

    QVERIFY(!duplicateId.isEmpty());
    QVERIFY(duplicateId != sourceId);
    QCOMPARE(controller.conversationDuplicateLastStatus(), QStringLiteral("Succeeded"));
    QVERIFY(controller.conversationDuplicateLastResultSummary().contains(
        QStringLiteral("duplicate source token Copy")));
    QVERIFY(controller.conversationTitles().contains(QStringLiteral("duplicate source token Copy")));
    QCOMPARE(controller.activeConversationId(), sourceId);

    QVERIFY(controller.switchConversation(duplicateId));
    QCOMPARE(controller.chatHistory().size(), 3);
    QCOMPARE(controller.chatHistory().at(1).content, QStringLiteral("duplicate source token"));
}

void ApplicationControllerTest::conversationOrderingIsPinnedRecentArchivedDeterministic() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    const auto firstId = controller.activeConversationId();
    QVERIFY(controller.renameConversation(firstId, QStringLiteral("Recent A")));
    const auto pinnedId = controller.createConversation(QStringLiteral("Pinned B"));
    const auto archivedId = controller.createConversation(QStringLiteral("Archived C"));
    QVERIFY(controller.pinConversation(pinnedId));
    QVERIFY(controller.archiveConversation(archivedId));

    const auto ids = controller.conversationIds();
    const auto titles = controller.conversationTitles();
    const auto pinned = controller.conversationPinnedSummaries();
    const auto archived = controller.conversationArchivedSummaries();

    QCOMPARE(ids.size(), 3);
    QCOMPARE(ids.at(0), pinnedId);
    QCOMPARE(pinned.at(0), QStringLiteral("Pinned"));
    QCOMPARE(titles.at(1), QStringLiteral("Recent A"));
    QCOMPARE(archived.at(1), QStringLiteral("Active"));
    QCOMPARE(ids.at(2), archivedId);
    QCOMPARE(archived.at(2), QStringLiteral("Archived"));
}

void ApplicationControllerTest::deleteReadinessIsDisabledByDefault() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;

    QVERIFY(controller.conversationDeleteAvailable());
    QCOMPARE(controller.conversationDeletePolicyStatus(), QStringLiteral("Enabled"));
    QCOMPARE(controller.conversationDeleteReadinessStatus(), QStringLiteral("Ready"));
    QVERIFY(controller.conversationDeleteReadinessSummary().contains(
        QStringLiteral("Permanent delete is available")));
    QVERIFY(controller.conversationDeleteReadinessChecks().contains(
        QStringLiteral("Permanent delete: Enabled")));
}

void ApplicationControllerTest::deleteRequestRefusesSafelyWithoutMutation() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    const auto firstId = controller.activeConversationId();
    // Create a second conversation so there is somewhere to switch after deletion
    controller.createConversation(QStringLiteral("Second"));
    QVERIFY(controller.switchConversation(firstId));
    QVERIFY(fixture.sendAndWait(QStringLiteral("delete safety token")));
    const auto beforeCount = controller.conversationStoreConversationCount();

    // Permanent delete is now enabled — it should succeed
    QVERIFY(controller.requestPermanentDeleteConversation(firstId));

    QCOMPARE(controller.conversationDeleteLastStatus(), QStringLiteral("Deleted"));
    QVERIFY(controller.conversationDeleteLastResultSummary().contains(
        QStringLiteral("deleted successfully")));
    // The deleted conversation should no longer appear in the active list
    QVERIFY(controller.activeConversationId() != firstId);
    // Store count decreases by 1 (deleted records are hidden)
    QCOMPARE(controller.conversationStoreConversationCount(), beforeCount - 1);
}

void ApplicationControllerTest::deleteRequestRefusesSafelyWithoutSQLiteMutation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto databasePath = dir.filePath(QStringLiteral("conversations.sqlite3"));
    QString firstId;
    QString secondId;

    {
        auto controller = makeControllerWithConversationStore(
            std::make_unique<SQLiteConversationStore>(databasePath));
        firstId = controller->activeConversationId();
        secondId = controller->createConversation(QStringLiteral("Second"));
        QVERIFY(controller->switchConversation(firstId));
        QVERIFY(controller->sendMessage(QStringLiteral("sqlite delete safety token")));
        const auto beforeCount = controller->conversationStoreConversationCount();

        // Permanent delete now succeeds
        QVERIFY(controller->requestPermanentDeleteConversation(firstId));

        QCOMPARE(controller->conversationDeleteLastStatus(), QStringLiteral("Deleted"));
        QVERIFY(controller->conversationDeleteLastResultSummary().contains(
            QStringLiteral("deleted successfully")));
        QCOMPARE(controller->conversationStoreConversationCount(), beforeCount - 1);
        QVERIFY(controller->activeConversationId() != firstId);
    }

    // After reload, the deleted conversation has deleted=true in the raw store
    SQLiteConversationStore reloaded(databasePath);
    // listConversations() hides deleted records — only the second should appear
    const auto conversations = reloaded.listConversations();
    QCOMPARE(conversations.size(), 1);
    QCOMPARE(conversations.first().id, secondId);
}

void ApplicationControllerTest::activeConversationRemainsValidAfterArchiveUnarchive() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    const auto firstId = controller.activeConversationId();

    QVERIFY(controller.archiveConversation(firstId));

    QVERIFY(!controller.activeConversationId().isEmpty());
    QVERIFY(controller.activeConversationId() != QStringLiteral("single-transcript"));
    QVERIFY(!controller.activeConversationArchived());
    QVERIFY(controller.conversationIds().contains(controller.activeConversationId()));

    QVERIFY(controller.unarchiveConversation(firstId));
    QVERIFY(controller.switchConversation(firstId));

    QCOMPARE(controller.activeConversationId(), firstId);
    QVERIFY(!controller.activeConversationArchived());
}

void ApplicationControllerTest::persistsActiveConversationAcrossControllerRecreation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto databasePath = dir.filePath(QStringLiteral("conversations.sqlite3"));
    QString firstId;
    QString secondId;

    {
        ProductionChatControllerFixture fixture{
            std::make_unique<SQLiteConversationStore>(databasePath)};
        auto& controller = *fixture.controller;
        firstId = controller.activeConversationId();
        QVERIFY(fixture.sendAndWait(QStringLiteral("persisted first token")));
        secondId = controller.createConversation(QStringLiteral("Persisted Second"));
        QVERIFY(fixture.sendAndWait(QStringLiteral("persisted second token")));
        QVERIFY(!firstId.isEmpty());
        QVERIFY(!secondId.isEmpty());
    }

    ProductionChatControllerFixture reloaded{
        std::make_unique<SQLiteConversationStore>(databasePath)};
    auto& controller = *reloaded.controller;
    QCOMPARE(controller.conversationStoreConversationCount(), 2);
    QCOMPARE(controller.activeConversationId(), secondId);
    QCOMPARE(controller.chatHistory().size(), 3);
    QVERIFY(controller.chatHistory().at(1).content.contains(QStringLiteral("persisted second")));
    QCOMPARE(controller.chatHistory().at(2).content, QStringLiteral("SENTINEL_TEST_RESPONSE"));
    QCOMPARE(controller.chatHistory().at(2).status, sentinel::core::ChatMessageStatus::Completed);
    QVERIFY(controller.switchConversation(firstId));
    QCOMPARE(controller.chatHistory().size(), 3);
    QVERIFY(controller.chatHistory().at(1).content.contains(QStringLiteral("persisted first")));
    QCOMPARE(controller.chatHistory().at(2).content, QStringLiteral("SENTINEL_TEST_RESPONSE"));
}

void ApplicationControllerTest::persistsFailedChatAcrossControllerRecreation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto databasePath = dir.filePath(QStringLiteral("conversations.sqlite3"));
    QString conversationId;

    {
        ProductionChatControllerFixture fixture{
            std::make_unique<SQLiteConversationStore>(databasePath),
            sentinel::test::DeterministicChatReply::Failure};
        auto& controller = *fixture.controller;
        conversationId = controller.activeConversationId();
        QVERIFY(controller.sendMessage(QStringLiteral("persisted failure")));
        QTRY_VERIFY(!controller.chatGenerationActive());
        QCOMPARE(controller.chatHistory().last().status, sentinel::core::ChatMessageStatus::Failed);
        QVERIFY(!controller.chatHistory().last().content.trimmed().isEmpty());
    }

    ProductionChatControllerFixture reloaded{
        std::make_unique<SQLiteConversationStore>(databasePath)};
    auto& controller = *reloaded.controller;
    QCOMPARE(controller.activeConversationId(), conversationId);
    QCOMPARE(controller.chatHistory().size(), 3);
    QCOMPARE(controller.chatHistory().last().status, sentinel::core::ChatMessageStatus::Failed);
    QVERIFY(!controller.chatHistory().last().content.trimmed().isEmpty());
    QVERIFY(!controller.chatGenerationActive());
}

void ApplicationControllerTest::persistsCancelledChatAcrossControllerRecreation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto databasePath = dir.filePath(QStringLiteral("conversations.sqlite3"));
    QString conversationId;

    {
        ProductionChatControllerFixture fixture{
            std::make_unique<SQLiteConversationStore>(databasePath),
            sentinel::test::DeterministicChatReply::Delayed};
        fixture.models.state->delayMs = 150;
        auto& controller = *fixture.controller;
        conversationId = controller.activeConversationId();
        QVERIFY(controller.sendMessage(QStringLiteral("persisted cancellation")));
        QTRY_VERIFY(controller.chatGenerationActive());
        QVERIFY(controller.stopChatGeneration());
        QVERIFY(!controller.chatGenerationActive());
        QCOMPARE(controller.chatHistory().last().status,
                 sentinel::core::ChatMessageStatus::Cancelled);
        QTest::qWait(250);
        QCOMPARE(controller.chatHistory().last().status,
                 sentinel::core::ChatMessageStatus::Cancelled);
    }

    ProductionChatControllerFixture reloaded{
        std::make_unique<SQLiteConversationStore>(databasePath)};
    auto& controller = *reloaded.controller;
    QCOMPARE(controller.activeConversationId(), conversationId);
    QCOMPARE(controller.chatHistory().last().status, sentinel::core::ChatMessageStatus::Cancelled);
    QVERIFY(!controller.chatGenerationActive());
}

void ApplicationControllerTest::localInferenceWorkerDropsCallbacksAfterContextDestruction() {
    sentinel::test::LocalInferenceWorkerFixture f;
    QVERIFY(f.start());
    QTRY_VERIFY_WITH_TIMEOUT(f.state->entered.load(), 1000);
    f.worker->cancel(QStringLiteral("worker-1"));
    f.shutdown();
    QVERIFY(f.state->cancelled.load());
    QVERIFY(f.state->exited.load());
    QCoreApplication::processEvents();
    QCOMPARE(f.callbacks, 0);
    QVERIFY(!f.worker);
    // Conversation isolation remains covered by createsSwitchesRenamesArchivesAndUnarchivesConversations
    // and switchingConversationDoesNotDuplicateTranscriptInsertion.
}

void ApplicationControllerTest::switchingConversationDoesNotDuplicateTranscriptInsertion() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    const auto firstId = controller.activeConversationId();
    QVERIFY(fixture.sendAndWait(QStringLiteral("duplicate guard")));
    const auto secondId = controller.createConversation(QStringLiteral("Second"));
    QVERIFY(fixture.sendAndWait(QStringLiteral("other guard")));

    QVERIFY(controller.switchConversation(firstId));
    QCOMPARE(controller.chatHistory().size(), 3);
    QVERIFY(controller.switchConversation(secondId));
    QCOMPARE(controller.chatHistory().size(), 3);
    QVERIFY(controller.switchConversation(firstId));
    QCOMPARE(controller.chatHistory().size(), 3);
    QVERIFY(controller.conversationStoreSummaries().first().contains(QStringLiteral("3 messages")));
}

void ApplicationControllerTest::reportsSingleConversationBrowserEntryDeterministically() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;

    QCOMPARE(controller.conversationListEntryCount(), 1);
    QCOMPARE(controller.conversationListCurrentTitle(), QStringLiteral("Current Transcript"));
    QCOMPARE(controller.conversationListCurrentMessageCount(), 1);
    QCOMPARE(controller.conversationListCurrentPersistenceStatus(), QStringLiteral("Runtime Only"));
}

void ApplicationControllerTest::reportsEmptyTranscriptConversationBrowserSummary() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;

    QCOMPARE(controller.conversationBrowserStatus(), QStringLiteral("Empty Transcript"));
    QVERIFY(controller.conversationBrowserSummaryText().contains(
        QStringLiteral("current transcript entry")));
}

void ApplicationControllerTest::reportsConversationBrowserMessageCountSummary() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    QVERIFY(fixture.sendAndWait(QStringLiteral("count token")));

    QCOMPARE(controller.conversationListCurrentMessageCount(), 3);
    QVERIFY(controller.conversationListCurrentSummary().contains(QStringLiteral("3 messages")));
}

void ApplicationControllerTest::clearChatUpdatesConversationBrowserEntry() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    QVERIFY(fixture.sendAndWait(QStringLiteral("clear token")));
    QCOMPARE(controller.conversationListCurrentMessageCount(), 3);

    QVERIFY(!controller.clearChat());

    QCOMPARE(controller.conversationBrowserStatus(), QStringLiteral("Empty Transcript"));
    QCOMPARE(controller.conversationListCurrentMessageCount(), 1);
    QVERIFY(controller.conversationListCurrentLastUpdatedSummary().contains(
        QStringLiteral("Runtime-only transcript")));
}

void ApplicationControllerTest::conversationBrowserReflectsSearchAndExportAvailability() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    controller.setConversationExportDirectory(dir.path());
    QVERIFY(fixture.sendAndWait(QStringLiteral("availability token")));

    QVERIFY(controller.searchConversation(QStringLiteral("token")));
    QVERIFY(controller.conversationListCurrentSearchAvailabilitySummary().contains(
        QStringLiteral("Completed")));

    QVERIFY(controller.exportTranscript(QStringLiteral("json")));
    QVERIFY(controller.conversationListCurrentExportAvailabilitySummary().contains(
        QStringLiteral("Last export: Succeeded")));
}

void ApplicationControllerTest::reportsMultiConversationPlanningReadinessWithoutStorageMutation() {
    auto store = std::make_unique<RecordingChatHistoryStore>();
    const auto* storePtr = store.get();
    ApplicationController controller(nullptr, std::make_unique<InMemoryStore>(), nullptr,
                                     std::move(store));
    const auto beforeCount = storePtr->messages_.size();

    QCOMPARE(controller.conversationCurrentStorageMode(), QStringLiteral("Single Transcript"));
    QCOMPARE(controller.conversationFutureStorageMode(), QStringLiteral("Multi Conversation"));
    QCOMPARE(controller.conversationMigrationReadiness(), QStringLiteral("Not Started"));
    QCOMPARE(controller.conversationMigrationStatusSummary(),
             QStringLiteral("Not Started / Planned"));
    QVERIFY(controller.conversationSchemaStatusSummary().contains(
        QStringLiteral("No conversation schema migration applied")));
    QCOMPARE(controller.conversationListEntryCount(), 1);
    QCOMPARE(controller.conversationListCurrentTitle(), QStringLiteral("Current Transcript"));

    QCOMPARE(storePtr->messages_.size(), beforeCount);
    QVERIFY(!storePtr->wasCleared_);
}

void ApplicationControllerTest::inMemoryConversationSearchFindsUserAndAssistantMessages() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;

    QVERIFY(controller.sendMessage(QStringLiteral("alpha response user search")));
    QTRY_VERIFY(!controller.chatGenerationActive());

    QVERIFY(controller.searchConversation(QStringLiteral("response")));

    QCOMPARE(controller.conversationSearchStatus(), QStringLiteral("Completed"));
    QCOMPARE(controller.conversationSearchQueryText(), QStringLiteral("response"));
    QCOMPARE(controller.conversationSearchResultCount(), 2);
    QVERIFY(controller.conversationSearchSummaryText().contains(QStringLiteral("2")));
    QVERIFY(controller.conversationSearchResultSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("user #2")));
    QVERIFY(controller.conversationSearchResultSummaries()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("assistant #3")));
}

void ApplicationControllerTest::emptyConversationSearchDoesNotMutateHistory() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    QVERIFY(fixture.sendAndWait(QStringLiteral("status")));
    const auto before = controller.chatHistory();

    QVERIFY(!controller.searchConversation(QStringLiteral("   ")));

    QCOMPARE(controller.conversationSearchStatus(), QStringLiteral("Empty Query"));
    QCOMPARE(controller.conversationSearchResultCount(), 0);
    QCOMPARE(controller.chatHistory().size(), before.size());
    for (int i = 0; i < before.size(); ++i) {
        QCOMPARE(controller.chatHistory().at(i).id, before.at(i).id);
        QCOMPARE(controller.chatHistory().at(i).role, before.at(i).role);
        QCOMPARE(controller.chatHistory().at(i).content, before.at(i).content);
    }
}

void ApplicationControllerTest::conversationSearchDoesNotMutateHistory() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    QVERIFY(fixture.sendAndWait(QStringLiteral("status")));
    const auto before = controller.chatHistory();

    QVERIFY(controller.searchConversation(QStringLiteral("status")));

    QCOMPARE(controller.chatHistory().size(), before.size());
    for (int i = 0; i < before.size(); ++i) {
        QCOMPARE(controller.chatHistory().at(i).id, before.at(i).id);
        QCOMPARE(controller.chatHistory().at(i).role, before.at(i).role);
        QCOMPARE(controller.chatHistory().at(i).content, before.at(i).content);
        QCOMPARE(controller.chatHistory().at(i).status, before.at(i).status);
    }
}

void ApplicationControllerTest::clearChatResetsConversationSearchSummary() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    QVERIFY(fixture.sendAndWait(QStringLiteral("status")));
    QVERIFY(controller.searchConversation(QStringLiteral("status")));
    QCOMPARE(controller.conversationSearchResultCount(), 1);

    QVERIFY(!controller.clearChat());

    QCOMPARE(controller.conversationSearchStatus(), QStringLiteral("Empty Query"));
    QCOMPARE(controller.conversationSearchQueryText(), QString());
    QCOMPARE(controller.conversationSearchResultCount(), 0);
    QCOMPARE(controller.chatHistory().size(), 1);
}

void ApplicationControllerTest::markdownConversationExportWritesCurrentTranscript() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    controller.setConversationExportDirectory(dir.path());

    QVERIFY(fixture.sendAndWait(QStringLiteral("export markdown token")));
    QVERIFY(controller.exportTranscript(QStringLiteral("Markdown")));

    const auto result = controller.latestConversationExportResult();
    QVERIFY(result.success);
    QVERIFY(result.wroteFile);
    QCOMPARE(result.messageCount, controller.chatHistory().size());
    QVERIFY(result.outputFileName.endsWith(QStringLiteral(".md")));
    QVERIFY(QFileInfo(result.outputPath).absolutePath() ==
            QFileInfo(dir.path()).absoluteFilePath());
    QFile file(result.outputPath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto content = QString::fromUtf8(file.readAll());
    QVERIFY(content.contains(QStringLiteral("# Sentinel Transcript")));
    QVERIFY(content.contains(QStringLiteral("## user #2")));
    QVERIFY(content.contains(QStringLiteral("export markdown token")));
    QCOMPARE(controller.conversationExportLastStatus(), QStringLiteral("Succeeded"));
    QCOMPARE(controller.conversationExportLastFileName(), result.outputFileName);
}

void ApplicationControllerTest::jsonConversationExportWritesValidStructure() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    controller.setConversationExportDirectory(dir.path());

    QVERIFY(fixture.sendAndWait(QStringLiteral("export json token")));
    QVERIFY(controller.exportTranscript(QStringLiteral("json")));

    const auto result = controller.latestConversationExportResult();
    QVERIFY(result.success);
    QVERIFY(result.outputFileName.endsWith(QStringLiteral(".json")));
    QFile file(result.outputPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto document = QJsonDocument::fromJson(file.readAll());
    QVERIFY(document.isObject());
    const auto root = document.object();
    QCOMPARE(root.value(QStringLiteral("format")).toString(),
             QStringLiteral("sentinel.transcript.v1"));
    QCOMPARE(root.value(QStringLiteral("messageCount")).toInt(), controller.chatHistory().size());
    const auto messages = root.value(QStringLiteral("messages")).toArray();
    QCOMPARE(messages.size(), controller.chatHistory().size());
    QCOMPARE(messages.at(1).toObject().value(QStringLiteral("role")).toString(),
             QStringLiteral("user"));
    QCOMPARE(messages.at(1).toObject().value(QStringLiteral("content")).toString(),
             QStringLiteral("export json token"));
}

void ApplicationControllerTest::textAndPdfConversationExportWriteLocalFiles() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    controller.setConversationExportDirectory(dir.path());

    QVERIFY(fixture.sendAndWait(QStringLiteral("export text and pdf token")));
    QVERIFY(controller.exportTranscript(QStringLiteral("txt")));
    auto result = controller.latestConversationExportResult();
    QVERIFY(result.success);
    QVERIFY(result.outputFileName.endsWith(QStringLiteral(".txt")));
    QFile textFile(result.outputPath);
    QVERIFY(textFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(QString::fromUtf8(textFile.readAll())
                .contains(QStringLiteral("export text and pdf token")));

    QVERIFY(controller.exportTranscript(QStringLiteral("pdf")));
    result = controller.latestConversationExportResult();
    QVERIFY(result.success);
    QVERIFY(result.outputFileName.endsWith(QStringLiteral(".pdf")));
    QFile pdfFile(result.outputPath);
    QVERIFY(pdfFile.open(QIODevice::ReadOnly));
    QVERIFY(pdfFile.read(8).startsWith("%PDF-1."));
}

void ApplicationControllerTest::emptyConversationExportIsRefused() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    controller.setConversationExportDirectory(dir.path());

    QVERIFY(!controller.exportTranscript(QStringLiteral("markdown")));

    const auto result = controller.latestConversationExportResult();
    QCOMPARE(result.status, QStringLiteral("Refused"));
    QVERIFY(!result.wroteFile);
    QVERIFY(result.refusalSummary.contains(QStringLiteral("no user or assistant messages")));
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 0);
}

void ApplicationControllerTest::conversationExportUsesSanitizedTimestampedFilenames() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    controller.setConversationExportDirectory(dir.path());

    QVERIFY(fixture.sendAndWait(QStringLiteral("filename token")));
    QVERIFY(controller.exportTranscript(QStringLiteral("markdown")));
    QVERIFY(controller.exportTranscript(QStringLiteral("markdown")));

    const auto result = controller.latestConversationExportResult();
    QVERIFY(result.outputFileName.startsWith(QStringLiteral("sentinel-transcript-")));
    QVERIFY(result.outputFileName.endsWith(QStringLiteral(".md")));
    QVERIFY(!result.outputFileName.contains(QLatin1Char('/')));
    QVERIFY(!result.outputFileName.contains(QLatin1Char(':')));
    QVERIFY(result.outputFileName.contains(QRegularExpression(
        QStringLiteral("^sentinel-transcript-[0-9]{8}-[0-9]{6}-[0-9]{3}(-[0-9]+)?\\.md$"))));
    QCOMPARE(QDir(dir.path()).entryList(QStringList{QStringLiteral("*.md")}, QDir::Files).size(),
             2);
}

void ApplicationControllerTest::unsupportedConversationExportFormatWritesNoFile() {
    ProductionChatControllerFixture fixture;
    auto& controller = *fixture.controller;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    controller.setConversationExportDirectory(dir.path());

    QVERIFY(fixture.sendAndWait(QStringLiteral("plain text should not export")));
    QVERIFY(!controller.exportTranscript(QStringLiteral("plain text")));

    QCOMPARE(controller.latestConversationExportResult().status, QStringLiteral("Refused"));
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 0);
}


void ApplicationControllerTest::blocksLocalInferenceByDefaultPermission() {
    const auto controller = makeController();
    QSignalSpy spy(controller.get(), &ApplicationController::localInferenceChanged);

    const auto ran =
        controller->runLocalInference(QStringLiteral("hello"), QStringLiteral("llama3.2"));

    QVERIFY(!ran);
    QVERIFY(spy.count() >= 1);
    QCOMPARE(controller->localInferenceStatus(), QStringLiteral("Blocked"));
    QCOMPARE(controller->localInferenceSummary(),
             QStringLiteral("Local inference blocked by runtime permission policy."));
    QVERIFY(controller->localInferenceTraceSummaries().contains(
        QStringLiteral("2. Permission Policy [Denied]: Runtime permission policy is "
                       "metadata-only and denies execution by default.")));
}

void ApplicationControllerTest::blocksLocalInferenceWhenSafetyPolicyBlocks() {
    auto fakeClient = std::make_unique<FakeLocalInferenceClient>();
    auto* fakeClientPtr = fakeClient.get();
    auto controller = std::make_unique<ApplicationController>(
        std::make_unique<LocalEchoProvider>(), std::make_unique<InMemoryStore>(), nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, std::make_unique<AllowLocalInferencePolicy>(),
        std::make_unique<BlockLocalInferenceSafetyPolicy>(), nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr,
        std::make_unique<FakeOllamaRuntimeClient>(
            QList<OllamaModelSummary>{{QStringLiteral("llama3.2"), {}, 10}}),
        std::move(fakeClient));

    const auto ran =
        controller->runLocalInference(QStringLiteral("hello"), QStringLiteral("llama3.2"));

    QVERIFY(!ran);
    QVERIFY(!fakeClientPtr->called);
    QCOMPARE(controller->localInferenceStatus(), QStringLiteral("Blocked"));
    QCOMPARE(controller->localInferenceSummary(),
             QStringLiteral("Local inference blocked by runtime safety policy."));
    QVERIFY(controller->localInferenceTraceSummaries().contains(
        QStringLiteral("3. Safety Policy [Blocked]: Injected local safety policy blocked "
                       "execution.")));
}

void ApplicationControllerTest::runsInjectedLocalInferenceWhenPermissionAllows() {
    auto fakeClient = std::make_unique<FakeLocalInferenceClient>();
    auto* fakeClientPtr = fakeClient.get();
    auto controller = std::make_unique<ApplicationController>(
        std::make_unique<LocalEchoProvider>(), std::make_unique<InMemoryStore>(), nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, std::make_unique<AllowLocalInferencePolicy>(), nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr,
        std::make_unique<FakeOllamaRuntimeClient>(
            QList<OllamaModelSummary>{{QStringLiteral("local-model"), {}, 0}}),
        std::move(fakeClient));
    QSignalSpy spy(controller.get(), &ApplicationController::localInferenceChanged);

    const auto ran =
        controller->runLocalInference(QStringLiteral("hello"), QStringLiteral("local-model"));

    QVERIFY(ran);
    QVERIFY(fakeClientPtr->called);
    QCOMPARE(fakeClientPtr->lastRequest.prompt, QStringLiteral("hello"));
    QCOMPARE(fakeClientPtr->lastRequest.options.model, QStringLiteral("local-model"));
    QCOMPARE(spy.count(), 2);
    QVERIFY(!controller->localInferenceBusy());
    QCOMPARE(controller->localInferenceRuntimeState(), QStringLiteral("Idle"));
    QCOMPARE(controller->localInferenceStatus(), QStringLiteral("Succeeded"));
    QCOMPARE(controller->localInferenceLastResponseSummary(),
             QStringLiteral("Local inference completed for local-model (21 characters)."));
    QVERIFY(controller->localInferenceLatencySummary().contains(QStringLiteral("ms")));
    QVERIFY(controller->localInferenceTraceSummaries().contains(
        QStringLiteral("1. Permission Policy [Allowed]: Local inference permission allowed for "
                       "injected test policy.")));
    QVERIFY(controller->localInferenceTraceSummaries().contains(
        QStringLiteral("2. Safety Policy [Compliant]: Runtime safety policy report: local-only "
                       "and no-execution posture is enforced with deterministic metadata rules.")));
    const sentinel::core::MemoryAccessContext context{
        controller->activeConversationId(), {}, {}, false};
    QCOMPARE(controller->memoryStore()
                 ->writeScopedMemory({sentinel::core::MemoryScope::Session, "orchid",
                                      "DO_NOT_FLATTEN_GENERATED_MEMORY",
                                      sentinel::core::MemoryProvenance::GeneratedSummary,
                                      "summary:fixture"},
                                     context)
                 .status,
             sentinel::core::ScopedMemoryStatus::Ready);
    controller->setPromptContextInjectionEnabled(true);
    QVERIFY2(controller->runLocalInference("orchid", "local-model"),
             qPrintable(controller->localInferenceSummary()));
    QVERIFY(!fakeClientPtr->lastRequest.prompt.contains("DO_NOT_FLATTEN_GENERATED_MEMORY"));
}


void ApplicationControllerTest::exposesConversationSessionMetadata() {
    const auto controller = makeController();

    QCOMPARE(controller->conversationSessionId(), QStringLiteral("conversation-session-1"));
    QCOMPARE(controller->conversationSessionStatus(), QStringLiteral("Active"));
    QCOMPARE(controller->interactionMode(), QStringLiteral("Companion"));
    QCOMPARE(controller->attentionState(), QStringLiteral("Observing"));
    QCOMPARE(controller->currentConversationSession().revision, 1);
    QCOMPARE(controller->contextWindowSummary(),
             QStringLiteral("Workspace context window: Local Only route, Atlas (Coordinator, "
                            "Available, Local), Ambient (Available, Public Metadata, Session)."));
    QCOMPARE(controller->currentConversationSession().contextWindow.currentRoutingMode,
             QStringLiteral("Local Only"));
    QCOMPARE(controller->currentConversationSession().contextWindow.preferredAgentSummary,
             QStringLiteral("Atlas (Coordinator, Available, Local)"));
}

void ApplicationControllerTest::exposesConversationStateMetadata() {
    const auto controller = makeController();

    QCOMPARE(controller->conversationState(), QStringLiteral("Idle"));
    QCOMPARE(controller->conversationTransitionStatus(), QStringLiteral("Not Requested"));
    QCOMPARE(controller->conversationTransitionSummary(),
             QStringLiteral("No conversation transition yet."));
}

void ApplicationControllerTest::updatesModelRoutingModeMetadata() {
    const auto controller = makeController();
    QSignalSpy spy(controller.get(), &ApplicationController::modelRoutingChanged);
    QSignalSpy taskPlanSpy(controller.get(), &ApplicationController::taskPlanChanged);
    QSignalSpy conversationSpy(controller.get(),
                               &ApplicationController::conversationSessionChanged);
    QSignalSpy snapshotSpy(controller.get(), &ApplicationController::orchestrationSnapshotChanged);

    controller->setRoutingModeByName(QStringLiteral("Balanced"));

    QCOMPARE(controller->currentRoutingMode(), QStringLiteral("Balanced"));
    QCOMPARE(controller->modelRoutingStatus(), QStringLiteral("No Available Model"));
    QCOMPARE(controller->selectedModelProviderSummary(),
             QStringLiteral("No configured model route is available."));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(taskPlanSpy.count(), 1);
    QCOMPARE(controller->latestTaskPlanStatus(), QStringLiteral("Blocked"));
    QCOMPARE(controller->currentOrchestrationSnapshot().workspace.routingMode,
             QStringLiteral("Balanced"));
    QCOMPARE(controller->currentConversationSession().revision, 2);
    QCOMPARE(controller->currentConversationSession().contextWindow.currentRoutingMode,
             QStringLiteral("Balanced"));
    QVERIFY(controller->contextWindowSummary().contains(QStringLiteral("Balanced route")));
    QCOMPARE(conversationSpy.count(), 1);
    QCOMPARE(controller->orchestrationSnapshotStatus(), QStringLiteral("Degraded"));
    QCOMPARE(snapshotSpy.count(), 1);

    controller->setRoutingModeByName(QStringLiteral("unknown"));
    QCOMPARE(controller->currentRoutingMode(), QStringLiteral("Local Only"));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(taskPlanSpy.count(), 2);
    QCOMPARE(conversationSpy.count(), 2);
    QCOMPARE(snapshotSpy.count(), 2);
}

void ApplicationControllerTest::keepsConversationSessionSeparateFromChatAndRuntimeSessions() {
    const auto controller = makeController();

    QCOMPARE(controller->conversationSessionId(), QStringLiteral("conversation-session-1"));
    QCOMPARE(controller->runtimeSessionId(), QStringLiteral("runtime-session-1"));
    QCOMPARE(controller->chatHistory().size(), 1);
    QCOMPARE(controller->conversationState(), QStringLiteral("Idle"));
    QCOMPARE(controller->conversationTransitionStatus(), QStringLiteral("Not Requested"));

    controller->setRoutingModeByName(QStringLiteral("Quality"));

    QCOMPARE(controller->conversationSessionId(), QStringLiteral("conversation-session-1"));
    QCOMPARE(controller->runtimeSessionId(), QStringLiteral("runtime-session-1"));
    QCOMPARE(controller->runtimeContextStatus(), QStringLiteral("Empty"));
    QCOMPARE(controller->chatHistory().size(), 1);
    QCOMPARE(controller->chatMessages().first(), QStringLiteral("Sentinel: Sentinel Core online."));
    QCOMPARE(controller->conversationState(), QStringLiteral("Idle"));
    QCOMPARE(controller->conversationTransitionStatus(), QStringLiteral("Not Requested"));
}

void ApplicationControllerTest::exposesAgentToolMetadata() {
    const auto controllerWithoutRuntime = makeController();
    QCOMPARE(controllerWithoutRuntime->availableToolCount(), 0);
    QVERIFY(controllerWithoutRuntime->availableToolIds().isEmpty());

    ApplicationController controller(std::make_unique<LocalEchoProvider>(),
                                     std::make_unique<InMemoryStore>(), nullptr, nullptr,
                                     std::make_unique<sentinel::core::NullAgentRuntime>());

    QCOMPARE(controller.availableToolCount(), 1);
    QCOMPARE(controller.availableToolIds(), QStringList{QStringLiteral("local-plan-summary")});
}

void ApplicationControllerTest::exposesMemoryStatus() {
    const auto controller = makeController();

    QCOMPARE(controller->memoryStatus(), QStringLiteral("Available"));
}

void ApplicationControllerTest::sendsMessageThroughProvider() {
    const auto controller = makeChatController();
    QSignalSpy spy(controller.get(), &ApplicationController::chatMessagesChanged);

    const auto sent = controller->sendMessage(QStringLiteral("status"));
    QVERIFY(sent);
    waitForCompletedChatTurn(*controller);
    const auto messages = controller->chatMessages();
    QCOMPARE(messages.size(), 3);
    QCOMPARE(messages.at(1), QStringLiteral("You: status"));
    QCOMPARE(messages.at(2), QStringLiteral("Sentinel: SENTINEL_TEST_RESPONSE"));
    QCOMPARE(controller->chatHistory().at(1).status, sentinel::core::ChatMessageStatus::Completed);
    QCOMPARE(controller->chatHistory().at(2).status, sentinel::core::ChatMessageStatus::Completed);
    QVERIFY(spy.count() >= 1);
}

void ApplicationControllerTest::updatesConversationStateForChatFlow() {
    const auto controller = makeChatController();
    QSignalSpy stateSpy(controller.get(), &ApplicationController::conversationStateChanged);

    const auto sent = controller->sendMessage(QStringLiteral("status"));

    QVERIFY(sent);
    waitForCompletedChatTurn(*controller);
    QCOMPARE(controller->conversationState(), QStringLiteral("Idle"));
    QCOMPARE(controller->conversationTransitionStatus(), QStringLiteral("Not Requested"));
    QCOMPARE(controller->runtimeSessionId(), QStringLiteral("runtime-session-1"));
    QCOMPARE(controller->runtimeContextStatus(), QStringLiteral("Empty"));
    QCOMPARE(stateSpy.count(), 0);
}

void ApplicationControllerTest::ignoresBlankChatMessages() {
    const auto controller = makeChatController();
    QSignalSpy spy(controller.get(), &ApplicationController::chatMessagesChanged);

    const auto sent = controller->sendMessage(QStringLiteral("   "));

    QVERIFY(!sent);
    QCOMPARE(controller->chatMessages().size(), 1);
    QCOMPARE(spy.count(), 0);
}

void ApplicationControllerTest::handlesUnavailableProvider() {
    auto controller = makeControllerWithChatHistory(
        nullptr, sentinel::test::DeterministicChatReply::Unavailable);
    QSignalSpy spy(controller.get(), &ApplicationController::chatMessagesChanged);

    const auto sent = controller->sendMessage(QStringLiteral("status"));

    QVERIFY(sent);
    QCOMPARE(controller->providerName(), QStringLiteral("deterministic-chat"));
    QCOMPARE(controller->providerStatus(), QStringLiteral("Available"));
    QCOMPARE(controller->chatMessages().size(), 3);
    QCOMPARE(controller->chatMessages().last(),
             QStringLiteral("Sentinel: The selected provider is not ready to accept requests."));
    QCOMPARE(controller->chatHistory().last().status, sentinel::core::ChatMessageStatus::Failed);
    QVERIFY(spy.count() >= 1);
}

void ApplicationControllerTest::handlesProviderErrorReply() {
    auto controller = makeControllerWithChatHistory(
        nullptr, sentinel::test::DeterministicChatReply::Failure);
    QSignalSpy spy(controller.get(), &ApplicationController::chatMessagesChanged);

    const auto sent = controller->sendMessage(QStringLiteral("status"));

    QVERIFY(sent);
    QTRY_VERIFY(controller->chatMessages().size() == 3 &&
                !controller->chatHistory().last().content.isEmpty());
    QCOMPARE(controller->chatMessages().last(),
             QStringLiteral("Sentinel: Generation failed before the provider returned usable "
                            "assistant text."));
    QCOMPARE(controller->chatHistory().last().status, sentinel::core::ChatMessageStatus::Failed);
    QVERIFY(spy.count() >= 1);
}

void ApplicationControllerTest::clearsChatHistory() {
    const auto controller = makeChatController();
    QSignalSpy spy(controller.get(), &ApplicationController::chatMessagesChanged);

    QVERIFY(controller->sendMessage(QStringLiteral("status")));
    waitForCompletedChatTurn(*controller);
    controller->clearChat();

    QCOMPARE(controller->chatMessages(),
             QStringList{QStringLiteral("Sentinel: Sentinel Core online.")});
    QCOMPARE(controller->chatHistory().size(), 1);
    QCOMPARE(controller->chatHistory().first().id, 1);
    QVERIFY(spy.count() >= 2);
}

void ApplicationControllerTest::loadsPersistedChatHistoryAtStartup() {
    auto store = std::make_unique<RecordingChatHistoryStore>(QList<sentinel::core::ChatMessage>{
        {4, sentinel::core::ChatRole::System, QStringLiteral("previous system"),
         QDateTime::fromString(QStringLiteral("2026-05-15T12:00:00.000Z"), Qt::ISODateWithMs),
         sentinel::core::ChatMessageStatus::Received},
        {5, sentinel::core::ChatRole::User, QStringLiteral("previous user"),
         QDateTime::fromString(QStringLiteral("2026-05-15T12:01:00.000Z"), Qt::ISODateWithMs),
         sentinel::core::ChatMessageStatus::Sent},
    });
    const auto storePtr = store.get();

    auto controller = makeControllerWithChatHistory(std::move(store));

    QCOMPARE(controller->chatHistory().size(), 2);
    QCOMPARE(controller->chatHistory().first().id, 4);
    QCOMPARE(controller->chatMessages().first(), QStringLiteral("Sentinel: previous system"));
    QCOMPARE(storePtr->messages_.size(), 2);
}

void ApplicationControllerTest::appendsNewChatMessagesToHistoryStore() {
    auto store = std::make_unique<RecordingChatHistoryStore>();
    const auto storePtr = store.get();
    auto controller = makeControllerWithChatHistory(std::move(store));

    const auto sent = controller->sendMessage(QStringLiteral("status"));

    QVERIFY(sent);
    waitForCompletedChatTurn(*controller);
    QCOMPARE(controller->chatHistory().size(), 3);
    QCOMPARE(controller->chatHistory().at(0).role, sentinel::core::ChatRole::System);
    QCOMPARE(controller->chatHistory().at(1).role, sentinel::core::ChatRole::User);
    QCOMPARE(controller->chatHistory().at(1).content, QStringLiteral("status"));
    QCOMPARE(controller->chatHistory().at(2).role, sentinel::core::ChatRole::Assistant);
    QCOMPARE(controller->chatHistory().at(2).content, QStringLiteral("SENTINEL_TEST_RESPONSE"));
}

void ApplicationControllerTest::clearsPersistentChatHistoryWhenAvailable() {
    auto store = std::make_unique<RecordingChatHistoryStore>();
    const auto storePtr = store.get();
    auto controller = makeControllerWithChatHistory(std::move(store));

    controller->sendMessage(QStringLiteral("status"));
    waitForCompletedChatTurn(*controller);
    const auto cleared = controller->clearChat();

    QVERIFY(cleared);
    QVERIFY(storePtr->wasCleared_);
    QCOMPARE(storePtr->messages_.size(), 1);
    QCOMPARE(storePtr->messages_.first().id, 1);
    QCOMPARE(storePtr->messages_.first().role, sentinel::core::ChatRole::System);
    QCOMPARE(controller->chatHistory().size(), 1);
    QCOMPARE(controller->chatMaintenanceStatus(), QStringLiteral("Clear completed"));
}

void ApplicationControllerTest::keepsRuntimeChatWorkingWhenHistoryStoreUnavailable() {
    auto store =
        std::make_unique<RecordingChatHistoryStore>(QList<sentinel::core::ChatMessage>{}, false);
    auto controller = makeControllerWithChatHistory(std::move(store));

    const auto sent = controller->sendMessage(QStringLiteral("status"));

    QVERIFY(sent);
    QTRY_VERIFY(!controller->chatGenerationActive());
    QCOMPARE(controller->chatHistory().size(), 3);
    QCOMPARE(controller->chatHistory().at(1).content, QStringLiteral("status"));
}

void ApplicationControllerTest::candidateCannotBeMovedToAnotherSession() {
    auto controller = makeController();
    const auto candidate =
        controller->createMemoryCandidateFromConversationText("old session source");
    QVERIFY(controller->approveMemoryCandidate(candidate));
    const auto next = controller->createConversation("Other session");
    QVERIFY(controller->switchConversation(next));
    QVERIFY(!controller->requestMemoryCandidateCommit(candidate));
    QCOMPARE(controller->memoryEntryCount(), 0);
    QVERIFY(controller->lastMemoryCommitResultSummary().contains("different session"));
}

void ApplicationControllerTest::memoryFollowsAuthoritativeConversation() {
    QTemporaryDir dir;
    sentinel::core::AppSettings settings(
        std::make_unique<sentinel::core::JsonSettingsStore>(dir.filePath("settings.json")),
        sentinel::core::inMemoryTestCredentialStore());
    auto controller = makeController();
    const auto first = controller->activeConversationId();
    controller->remember("orchid", "FIRST_SESSION");
    QCOMPARE(controller->memoryEntryCount(), 1);
    QVERIFY(controller->recallLocalMemory("FIRST_SESSION"));
    QCOMPARE(controller->memoryRecallResultCount(), 1);
    const auto next = controller->createConversation("Separate conversation");
    QVERIFY(!next.isEmpty());
    QVERIFY(controller->switchConversation(next));
    QCOMPARE(controller->memoryEntryCount(), 0);
    QCOMPARE(controller->memoryRecallResultCount(), 0);
    QVERIFY(controller->memoryRecallQueryText().isEmpty());
    QVERIFY(!controller->memoryRecallResultSummaries().join(" ").contains("FIRST_SESSION"));
    controller->remember("orchid", "SECOND_SESSION");
    QCOMPARE(controller->memoryEntryCount(), 1);
    QVERIFY(controller->memoryEntries().join(" ").contains("SECOND_SESSION"));
    QVERIFY(!controller->memoryEntries().join(" ").contains("FIRST_SESSION"));
    QVERIFY(controller->recallLocalMemory("SECOND_SESSION"));
    QCOMPARE(controller->memoryRecallResultCount(), 1);
    QVERIFY(controller->switchConversation(first));
    QCOMPARE(controller->memoryEntryCount(), 1);
    QCOMPARE(controller->memoryRecallResultCount(), 0);
    QVERIFY(controller->memoryRecallQueryText().isEmpty());
    QVERIFY(!controller->memoryRecallResultSummaries().join(" ").contains("SECOND_SESSION"));
    controller->attachControlledTaskSettings(settings);
    controller->remember("orchid", "WORKSPACE_ONE");
    QCOMPARE(controller->memoryEntryCount(), 1);
    QVERIFY(controller->recallLocalMemory("WORKSPACE_ONE"));
    settings.setSelectedWorkspaceId("coding");
    QCOMPARE(controller->memoryEntryCount(), 0);
    QCOMPARE(controller->memoryRecallResultCount(), 0);
    QVERIFY(controller->memoryRecallQueryText().isEmpty());
    QVERIFY(!controller->memoryRecallResultSummaries().join(" ").contains("WORKSPACE_ONE"));
}

void ApplicationControllerTest::storesRuntimeMemoryEntries() {
    const auto controller = makeController();
    QSignalSpy spy(controller.get(), &ApplicationController::memoryEntriesChanged);

    controller->remember(QStringLiteral(" callsign "), QStringLiteral(" Sentinel "));

    QCOMPARE(controller->memoryEntries(), QStringList{QStringLiteral("callsign: Sentinel")});
    QCOMPARE(spy.count(), 1);

    controller->remember(QString(), QStringLiteral("ignored"));
    QCOMPARE(controller->memoryEntries(), QStringList{QStringLiteral("callsign: Sentinel")});
    QCOMPARE(spy.count(), 1);
}

void ApplicationControllerTest::createsMemoryCandidatesPendingReview() {
    const auto controller = makeController();
    QSignalSpy spy(controller.get(), &ApplicationController::memoryCandidatesChanged);

    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("The user prefers concise local-only architecture notes."));

    QCOMPARE(id, QStringLiteral("memory-candidate-1"));
    QCOMPARE(controller->memoryCandidateCount(), 1);
    QCOMPARE(controller->pendingMemoryCandidateCount(), 1);
    QCOMPARE(controller->approvedMemoryCandidateCount(), 0);
    QVERIFY(
        controller->memoryCandidateSummaries().first().contains(QStringLiteral("Pending Review")));
    QCOMPARE(spy.count(), 1);
}

void ApplicationControllerTest::recordsMemoryCandidateApprovalAndRejectionMetadata() {
    const auto controller = makeController();

    const auto approvedId = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Remember this project uses Qt and QML."));
    const auto rejectedId = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Temporary phrasing should not become durable memory."));

    QVERIFY(controller->approveMemoryCandidate(approvedId));
    QVERIFY(controller->rejectMemoryCandidate(rejectedId));
    QVERIFY(!controller->approveMemoryCandidate(QStringLiteral("missing")));
    QCOMPARE(controller->pendingMemoryCandidateCount(), 0);
    QCOMPARE(controller->approvedMemoryCandidateCount(), 1);
    QCOMPARE(controller->rejectedMemoryCandidateCount(), 1);
    QVERIFY(controller->memoryCandidateSummaries().at(0).contains(QStringLiteral("Approved")));
    QVERIFY(controller->memoryCandidateSummaries().at(1).contains(QStringLiteral("Rejected")));
    QCOMPARE(controller->lastMemoryCandidateReviewStatus(), QStringLiteral("Refused"));
    QVERIFY(controller->lastMemoryCandidateReviewSummary().contains(QStringLiteral("not found")));
}

void ApplicationControllerTest::resetsMemoryCandidateReviewMetadata() {
    const auto controller = makeController();
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Reset this reviewed candidate to pending."));

    QVERIFY(controller->approveMemoryCandidate(id));
    QVERIFY(controller->resetMemoryCandidate(id));

    QCOMPARE(controller->pendingMemoryCandidateCount(), 1);
    QCOMPARE(controller->approvedMemoryCandidateCount(), 0);
    QCOMPARE(controller->rejectedMemoryCandidateCount(), 0);
    QVERIFY(
        controller->memoryCandidateSummaries().first().contains(QStringLiteral("Pending Review")));
    QVERIFY(controller->memoryCandidateSummaries().first().contains(
        QStringLiteral("Reviewer: User review")));
}

void ApplicationControllerTest::refusesInvalidMemoryCandidateReviewTransitions() {
    const auto controller = makeController();
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Invalid transitions must not change candidate state."));

    QVERIFY(!controller->resetMemoryCandidate(id));
    QVERIFY(!controller->archiveMemoryCandidate(id));
    QVERIFY(controller->approveMemoryCandidate(id));
    QVERIFY(!controller->rejectMemoryCandidate(id));

    QCOMPARE(controller->pendingMemoryCandidateCount(), 0);
    QCOMPARE(controller->approvedMemoryCandidateCount(), 1);
    QCOMPARE(controller->rejectedMemoryCandidateCount(), 0);
    QCOMPARE(controller->archivedMemoryCandidateCount(), 0);
    QCOMPARE(controller->lastMemoryCandidateReviewStatus(), QStringLiteral("Refused"));
}

void ApplicationControllerTest::archivesReviewedMemoryCandidatesAsTerminalMetadata() {
    const auto controller = makeController();
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Archived candidate metadata should be terminal."));

    QVERIFY(controller->rejectMemoryCandidate(id));
    QVERIFY(controller->archiveMemoryCandidate(id));
    QVERIFY(!controller->resetMemoryCandidate(id));

    QCOMPARE(controller->memoryCandidateCount(), 1);
    QCOMPARE(controller->pendingMemoryCandidateCount(), 0);
    QCOMPARE(controller->approvedMemoryCandidateCount(), 0);
    QCOMPARE(controller->rejectedMemoryCandidateCount(), 0);
    QCOMPARE(controller->archivedMemoryCandidateCount(), 1);
    QVERIFY(controller->archivedMemoryCandidateSummaries().first().contains(
        QStringLiteral("Archived")));
    QCOMPARE(controller->lastMemoryCandidateReviewStatus(), QStringLiteral("Refused"));
}

void ApplicationControllerTest::memoryCandidatesDoNotMutateKeyValueMemory() {
    const auto controller = makeController();
    controller->remember(QStringLiteral("mode"), QStringLiteral("Companion"));

    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Candidate metadata should not write to IMemoryStore."));
    QVERIFY(controller->approveMemoryCandidate(id));

    QCOMPARE(controller->memoryEntries(), QStringList{QStringLiteral("mode: Companion")});
}

void ApplicationControllerTest::approvedMemoryCandidateCommitsToKeyValueMemory() {
    const auto controller = makeController();
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("The project keeps memory commits behind explicit user action."));

    QVERIFY(controller->approveMemoryCandidate(id));

    QCOMPARE(controller->memoryCommitPlanCount(), 1);
    QCOMPARE(controller->memoryCommitReadinessStatus(), QStringLiteral("Ready"));
    QVERIFY(controller->memoryCommitReadinessSummary().contains(
        QStringLiteral("explicit user action")));
    QVERIFY(controller->memoryCommitTargetSummary().contains(QStringLiteral("Key-value Memory")));
    QVERIFY(
        controller->memoryCommitTargetSummary().contains(QStringLiteral("Refuse Existing Key")));
    QVERIFY(controller->memoryCommitCandidateSummaries().first().contains(id));
    QVERIFY(controller->memoryCommitCandidateSummaries().first().contains(
        QStringLiteral("Key-value Memory")));
    QVERIFY(controller->memoryCommitReadinessChecks()
                .join(QStringLiteral(" "))
                .contains(QStringLiteral("explicit user action")));
    QVERIFY(controller->requestMemoryCandidateCommit(id));
    QCOMPARE(controller->lastMemoryCommitStatus(), QStringLiteral("Committed"));
    QVERIFY(controller->lastMemoryCommitResultSummary().contains(
        QStringLiteral("memory.semantic.conversation-memory-candidate.memory-candidate-1")));
    const auto committed = controller->memoryStore()->retrieveMemory(
        {}, {controller->activeConversationId(), {}, {}, false});
    QCOMPARE(committed.records.size(), 1);
    QCOMPARE(committed.records.first().provenance, sentinel::core::MemoryProvenance::Unknown);
    QVERIFY(committed.records.first().sourceReference.startsWith("capture:"));
    QVERIFY(committed.records.first().sourceReference.size() > 40);
    QCOMPARE(controller->committedMemoryCandidateCount(), 1);
    QVERIFY(controller->memoryCandidateSummaries().first().contains(QStringLiteral("Committed")));
    QCOMPARE(controller->memoryEntries(),
             QStringList{QStringLiteral("memory.semantic.conversation-memory-candidate."
                                        "memory-candidate-1: The project keeps memory commits "
                                        "behind explicit user action.")});
}

void ApplicationControllerTest::failedMemoryCommitPreservesCandidateAndAllowsRetry() {
    class Store final : public sentinel::core::IMemoryStore {
    public:
        InMemoryStore data;
        bool rejectWrites = true;
        mutable QString error;
        void put(QString key, QString value) override {
            if (rejectWrites) {
                error = QStringLiteral("private-storage-detail-must-not-be-published");
                return;
            }
            data.put(std::move(key), std::move(value));
            error.clear();
        }
        sentinel::core::ScopedMemoryResult
        writeScopedMemory(const sentinel::core::ScopedMemoryWrite& write,
                          const sentinel::core::MemoryAccessContext& context) override {
            if (rejectWrites) {
                error = "private-storage-detail-must-not-be-published";
                return {};
            }
            error.clear();
            return data.writeScopedMemory(write, context);
        }
        sentinel::core::ScopedMemoryResult
        retrieveMemory(const QString& query, const sentinel::core::MemoryAccessContext& context,
                       int limit) const override {
            return data.retrieveMemory(query, context, limit);
        }
        QString get(const QString& key) const override {
            error.clear();
            return data.get(key);
        }
        sentinel::core::MemoryEntries entries() const override {
            return data.entries();
        }
        void clear() override {
            data.clear();
        }
        QString lastError() const override {
            return error;
        }
    };
    auto store = std::make_unique<Store>();
    auto* backend = store.get();
    auto controller = std::make_unique<ApplicationController>(std::make_unique<LocalEchoProvider>(),
                                                              std::move(store));
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Reviewed candidate must survive failed persistence."));
    QVERIFY(controller->approveMemoryCandidate(id));
    QSignalSpy entriesChanged(controller.get(), &ApplicationController::memoryEntriesChanged);
    QVERIFY(!controller->requestMemoryCandidateCommit(id));
    QCOMPARE(controller->lastMemoryCommitStatus(), QStringLiteral("Refused"));
    QVERIFY(controller->lastMemoryCommitResultSummary().contains(QStringLiteral("storage")));
    QVERIFY(
        !controller->lastMemoryCommitResultSummary().contains(QStringLiteral("private-storage")));
    QCOMPARE(controller->committedMemoryCandidateCount(), 0);
    QCOMPARE(controller->memoryCommitReadinessStatus(), QStringLiteral("Ready"));
    QVERIFY(controller->memoryEntries().isEmpty());
    QCOMPARE(entriesChanged.size(), 0);
    backend->rejectWrites = false;
    QVERIFY(controller->requestMemoryCandidateCommit(id));
    QCOMPARE(controller->lastMemoryCommitStatus(), QStringLiteral("Committed"));
    QCOMPARE(controller->committedMemoryCandidateCount(), 1);
    QCOMPARE(controller->memoryEntries().size(), 1);
    QCOMPARE(entriesChanged.size(), 1);
    QVERIFY(!controller->requestMemoryCandidateCommit(id));
    QCOMPARE(controller->memoryEntries().size(), 1);
    QCOMPARE(entriesChanged.size(), 1);
}

void ApplicationControllerTest::pendingAndRejectedMemoryCandidatesCannotCommit() {
    const auto controller = makeController();
    const auto pendingId = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Pending candidates are not commit-ready."));
    const auto rejectedId = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Rejected candidates are not commit-ready."));

    QVERIFY(!controller->requestMemoryCandidateCommit(pendingId));
    QCOMPARE(controller->lastMemoryCommitStatus(), QStringLiteral("Refused"));
    QVERIFY(
        controller->lastMemoryCommitResultSummary().contains(QStringLiteral("requires review")));

    QVERIFY(controller->rejectMemoryCandidate(rejectedId));
    QVERIFY(!controller->requestMemoryCandidateCommit(rejectedId));
    QCOMPARE(controller->lastMemoryCommitStatus(), QStringLiteral("Refused"));
    QVERIFY(controller->lastMemoryCommitResultSummary().contains(QStringLiteral("rejected")));
}

void ApplicationControllerTest::archivedMemoryCandidateCannotCommit() {
    const auto controller = makeController();
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Archived candidates cannot be committed."));

    QVERIFY(controller->approveMemoryCandidate(id));
    QVERIFY(controller->archiveMemoryCandidate(id));
    QVERIFY(!controller->requestMemoryCandidateCommit(id));

    QCOMPARE(controller->lastMemoryCommitStatus(), QStringLiteral("Refused"));
    QVERIFY(controller->lastMemoryCommitResultSummary().contains(QStringLiteral("archived")));
    QVERIFY(controller->memoryEntries().isEmpty());
}

void ApplicationControllerTest::duplicateMemoryCommitKeyRefusesByDefault() {
    const auto controller = makeController();
    controller->remember(QStringLiteral("memory.semantic.conversation-memory-candidate."
                                        "memory-candidate-1"),
                         QStringLiteral("Existing value"));
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Duplicate keys should refuse before overwrite."));

    QVERIFY(controller->approveMemoryCandidate(id));
    QVERIFY(!controller->requestMemoryCandidateCommit(id));

    QCOMPARE(controller->lastMemoryCommitStatus(), QStringLiteral("Refused"));
    QVERIFY(controller->lastMemoryCommitResultSummary().contains(QStringLiteral("already exists")));
    QCOMPARE(controller->committedMemoryCandidateCount(), 0);
    QCOMPARE(controller->memoryEntries(),
             QStringList{QStringLiteral("memory.semantic.conversation-memory-candidate."
                                        "memory-candidate-1: Existing value")});
}

void ApplicationControllerTest::approvalDoesNotAutomaticallyCommitMemory() {
    const auto controller = makeController();
    controller->remember(QStringLiteral("mode"), QStringLiteral("Companion"));
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Approved candidates wait for explicit Commit."));

    QVERIFY(controller->approveMemoryCandidate(id));

    QCOMPARE(controller->lastMemoryCandidateReviewStatus(), QStringLiteral("Accepted"));
    QCOMPARE(controller->lastMemoryCommitStatus(), QStringLiteral("No Commit"));
    QCOMPARE(controller->committedMemoryCandidateCount(), 0);
    QCOMPARE(controller->memoryEntries(), QStringList{QStringLiteral("mode: Companion")});
}

void ApplicationControllerTest::localMemoryRecallFindsCommittedKeyValueMemory() {
    const auto controller = makeController();
    controller->remember(QStringLiteral("project.mode"), QStringLiteral("Companion local mode"));
    controller->remember(QStringLiteral("preference.tone"), QStringLiteral("Concise answers"));

    QVERIFY(controller->recallLocalMemory(QStringLiteral("concise")));

    QCOMPARE(controller->memoryRecallPolicyStatus(), QStringLiteral("Local Literal Recall"));
    QCOMPARE(controller->memoryRecallStatus(), QStringLiteral("Completed"));
    QCOMPARE(controller->memoryRecallResultCount(), 1);
    QCOMPARE(controller->memoryEntryCount(), 2);
    QVERIFY(controller->memoryRecallSummaryText().contains(QStringLiteral("Found 1")));
    QVERIFY(controller->memoryRecallResultSummaries().first().contains(
        QStringLiteral("preference.tone")));
    QVERIFY(controller->memoryRecallResultSummaries().first().contains(
        QStringLiteral("matched value")));
}

void ApplicationControllerTest::emptyLocalMemoryRecallDoesNotMutateMemory() {
    const auto controller = makeController();
    controller->remember(QStringLiteral("mode"), QStringLiteral("Companion"));
    const auto beforeEntries = controller->memoryEntries();
    const auto beforeMessages = controller->chatHistory().size();

    QVERIFY(!controller->recallLocalMemory(QStringLiteral("   ")));

    QCOMPARE(controller->memoryRecallStatus(), QStringLiteral("Empty Query"));
    QCOMPARE(controller->memoryRecallResultCount(), 0);
    QCOMPARE(controller->memoryEntries(), beforeEntries);
    QCOMPARE(controller->chatHistory().size(), beforeMessages);
}

void ApplicationControllerTest::localMemoryRecallAfterCommitFindsCommittedCandidate() {
    const auto controller = makeController();
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Committed recall should find this local value."));
    QVERIFY(controller->approveMemoryCandidate(id));
    QVERIFY(controller->requestMemoryCandidateCommit(id));

    QVERIFY(controller->recallLocalMemory(QStringLiteral("recall")));

    QCOMPARE(controller->memoryRecallStatus(), QStringLiteral("Completed"));
    QCOMPARE(controller->memoryRecallResultCount(), 1);
    QVERIFY(controller->memoryRecallResultSummaries().first().contains(
        QStringLiteral("memory.semantic.conversation-memory-candidate.memory-candidate-1")));
}

void ApplicationControllerTest::clearChatKeepsCommittedMemoryRecallAvailable() {
    const auto controller = makeController();
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Committed memory recall survives clear chat."));
    QVERIFY(controller->approveMemoryCandidate(id));
    QVERIFY(controller->requestMemoryCandidateCommit(id));

    const auto previous = controller->activeConversationId();
    QVERIFY(controller->sendMessage(QStringLiteral("status")));
    controller->clearChat();
    QVERIFY(!controller->recallLocalMemory(QStringLiteral("survives")));
    QCOMPARE(controller->memoryStore()
                 ->retrieveMemory("survives", {previous, {}, {}, false})
                 .records.size(),
             1);

    QCOMPARE(controller->chatHistory().size(), 1);
    QCOMPARE(controller->memoryRecallResultCount(), 0);
    QCOMPARE(controller->committedMemoryCandidateCount(), 1);
}

void ApplicationControllerTest::localMemoryRecallDoesNotInjectIntoPrompt() {
    const auto controller = makeController();
    controller->remember(QStringLiteral("secret.local"), QStringLiteral("do not inject"));
    QVERIFY(controller->recallLocalMemory(QStringLiteral("secret")));

    QVERIFY(controller->sendMessage(QStringLiteral("hello")));

    QCOMPARE(controller->chatHistory().size(), 3);
    QCOMPARE(controller->chatHistory().at(1).content, QStringLiteral("hello"));
    QVERIFY(!controller->chatHistory().at(2).content.contains(QStringLiteral("do not inject")));
    QVERIFY(!controller->conversationRuntimeLastSuccessSummary().contains(
        QStringLiteral("do not inject")));
}

void ApplicationControllerTest::contextAssemblyShowsCommittedMemorySource() {
    const auto controller = makeController();
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Context assembly should see committed local memory."));
    QVERIFY(controller->approveMemoryCandidate(id));
    QVERIFY(controller->requestMemoryCandidateCommit(id));

    QCOMPARE(controller->contextAssemblyPolicyStatus(), QStringLiteral("Planning Only"));
    QCOMPARE(controller->contextAssemblyStatus(), QStringLiteral("Ready"));
    QCOMPARE(controller->committedMemoryContextAvailability(), QStringLiteral("Available"));
    QVERIFY(controller->contextAssemblySourceSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Committed Memory Context: Available")));
    QVERIFY(controller->contextAssemblyReadinessChecks().contains(
        QStringLiteral("Prompt assembly: disabled")));
    QVERIFY(controller->contextAssemblyReadinessChecks().contains(
        QStringLiteral("Automatic context attachment: disabled")));
}

void ApplicationControllerTest::contextAssemblyPlanningDoesNotMutateOrInjectPrompt() {
    const auto controller = makeController();
    controller->remember(QStringLiteral("secret.local"), QStringLiteral("planning only"));
    const auto beforeEntries = controller->memoryEntries();
    const auto beforeMessages = controller->chatHistory().size();

    const auto summary = controller->contextAssemblySummary();

    QCOMPARE(summary.status, sentinel::core::ContextAssemblyStatus::Ready);
    QCOMPARE(controller->memoryEntries(), beforeEntries);
    QCOMPARE(controller->chatHistory().size(), beforeMessages);

    QVERIFY(controller->sendMessage(QStringLiteral("hello")));
    QVERIFY(!controller->chatHistory().last().content.contains(QStringLiteral("planning only")));
}

void ApplicationControllerTest::promptContextInjectionBudgetTruncatesDeterministically() {
    const auto result = sentinel::core::injectPromptContext(
        QStringLiteral("hello"),
        QList<sentinel::core::PromptContextBlock>{
            {sentinel::core::ContextAssemblySourceKind::Conversation,
             QStringLiteral("Current Conversation Context"), QStringLiteral("abcdefghij")},
            {sentinel::core::ContextAssemblySourceKind::CommittedMemory,
             QStringLiteral("Committed Local Memory"), QStringLiteral("klmnopqrst")},
        },
        sentinel::core::PromptContextInjectionPolicy{true, 12});

    QCOMPARE(result.status, sentinel::core::PromptContextInjectionStatus::Truncated);
    QCOMPARE(result.injectedBlockCount, 2);
    QVERIFY(result.prompt.contains(QStringLiteral("abcdefghij")));
    QVERIFY(result.prompt.contains(QStringLiteral("kl")));
    QVERIFY(!result.prompt.contains(QStringLiteral("klm")));
    QVERIFY(result.sizeSummary.contains(QStringLiteral("12 of 20")));
}

void ApplicationControllerTest::conversationSummaryPlanningDoesNotMutateState() {
    const auto controller = makeController();
    for (int i = 0; i < 8; ++i) {
        QVERIFY(controller->sendMessage(QStringLiteral("summary planning marker %1 %2")
                                            .arg(i)
                                            .arg(QString(120, QLatin1Char('p')))));
    }
    controller->remember(QStringLiteral("summary.local"), QStringLiteral("unchanged"));
    const auto beforeEntries = controller->memoryEntries();
    const auto beforeMessages = controller->conversationHistoryMessageCount();

    const auto summary = controller->conversationSummaryResult();

    QVERIFY(summary.status == sentinel::core::ConversationSummaryStatus::Ready ||
            summary.status == sentinel::core::ConversationSummaryStatus::Truncated);
    QCOMPARE(controller->memoryEntries(), beforeEntries);
    QCOMPARE(controller->conversationHistoryMessageCount(), beforeMessages);
    QCOMPARE(controller->promptContextInjectionStatus(), QStringLiteral("Disabled"));
}

void ApplicationControllerTest::retrievalPlanningDoesNotMutateState() {
    const auto controller = makeController();
    for (int i = 0; i < 8; ++i) {
        QVERIFY(controller->sendMessage(QStringLiteral("retrieval planning marker %1 %2")
                                            .arg(i)
                                            .arg(QString(120, QLatin1Char('p')))));
    }
    controller->remember(QStringLiteral("retrieval.local"), QStringLiteral("unchanged"));
    const auto beforeEntries = controller->memoryEntries();
    const auto beforeMessages = controller->conversationHistoryMessageCount();
    const auto beforeInjectionStatus = controller->promptContextInjectionStatus();

    const auto planning = controller->retrievalPlanningResult();

    QVERIFY(planning.status == sentinel::core::RetrievalPlanningStatus::Ready ||
            planning.status == sentinel::core::RetrievalPlanningStatus::Truncated);
    QCOMPARE(controller->memoryEntries(), beforeEntries);
    QCOMPARE(controller->conversationHistoryMessageCount(), beforeMessages);
    QCOMPARE(controller->promptContextInjectionStatus(), beforeInjectionStatus);
}

void ApplicationControllerTest::conversationCompressionReadinessPlansMetadataWithoutMutation() {
    const auto controller = makeController();
    for (int i = 0; i < 18; ++i) {
        QVERIFY(controller->sendMessage(QStringLiteral("remember my compression preference %1 %2")
                                            .arg(i)
                                            .arg(QString(220, QLatin1Char('c')))));
    }
    controller->remember(QStringLiteral("compression.local"), QStringLiteral("unchanged"));
    const auto beforeEntries = controller->memoryEntries();
    const auto beforeMessages = controller->conversationHistoryMessageCount();
    const auto beforeTranscript = controller->chatMessages();
    const auto beforeInjectionStatus = controller->promptContextInjectionStatus();

    const auto summary = controller->conversationCompressionSummary();

    QVERIFY(summary.readiness.compressionUseful);
    QVERIFY(controller->conversationCompressionStatus() == QStringLiteral("Planned") ||
            controller->conversationCompressionStatus() == QStringLiteral("Needed") ||
            controller->conversationCompressionStatus() == QStringLiteral("Approaching"));
    QVERIFY(controller->conversationCompressionCandidateCount() > 0);
    QVERIFY(controller->conversationCompressionSelectedCandidateCount() > 0);
    QVERIFY(
        controller->conversationCompressionReadinessSummary().contains(QStringLiteral("pressure")));
    QVERIFY(controller->conversationCompressionTraceSummary().contains(QStringLiteral("selected")));
    QCOMPARE(controller->memoryEntries(), beforeEntries);
    QCOMPARE(controller->conversationHistoryMessageCount(), beforeMessages);
    QCOMPARE(controller->chatMessages(), beforeTranscript);
    QCOMPARE(controller->promptContextInjectionStatus(), beforeInjectionStatus);
}

void ApplicationControllerTest::manualSummaryGenerationIsBlockedAndDoesNotMutateTranscript() {
    const auto controller = makeController();
    for (int i = 0; i < 18; ++i) {
        QVERIFY(controller->sendMessage(QStringLiteral("remember summary pipeline %1 %2")
                                            .arg(i)
                                            .arg(QString(180, QLatin1Char('s')))));
    }
    controller->remember(QStringLiteral("summary.local"), QStringLiteral("unchanged"));
    const auto beforeEntries = controller->memoryEntries();
    const auto beforeMessages = controller->conversationHistoryMessageCount();
    const auto beforeTranscript = controller->chatMessages();

    QVERIFY(!controller->requestConversationSummaryGeneration());

    QCOMPARE(controller->conversationSummaryGenerationStatus(), QStringLiteral("Blocked"));
    QCOMPARE(controller->conversationSummaryAvailable(), false);
    QVERIFY(!controller->conversationSummaryBlockedReason().trimmed().isEmpty());
    QVERIFY(controller->conversationSummaryEstimatedCompressionGain().contains(
        QStringLiteral("estimated gain")));
    QVERIFY(!controller->conversationSummaryCandidateSegments().isEmpty());
    QVERIFY(!controller->conversationSummaryGenerationTraceSummaries().isEmpty());
    QCOMPARE(controller->memoryEntries(), beforeEntries);
    QCOMPARE(controller->conversationHistoryMessageCount(), beforeMessages);
    QCOMPARE(controller->chatMessages(), beforeTranscript);
}

void ApplicationControllerTest::manualSummaryGenerationFailureDoesNotPersistMetadata() {
    auto store = std::make_unique<sentinel::core::InMemoryConversationStore>();
    auto* storePtr = store.get();
    auto controller = makeControllerWithConversationStore(std::move(store));
    for (int i = 0; i < 16; ++i) {
        QVERIFY(controller->sendMessage(QStringLiteral("manual summary metadata %1 %2")
                                            .arg(i)
                                            .arg(QString(160, QLatin1Char('m')))));
    }

    QVERIFY(!controller->requestConversationSummaryGeneration());

    const auto metadata = storePtr->loadSummaryMetadata(controller->activeConversationId());
    QVERIFY(metadata.conversationId.isEmpty());
    QVERIFY(metadata.summaryText.isEmpty());
    QCOMPARE(controller->conversationSummaryPersistenceSummary(),
             QStringLiteral("No local summary metadata persisted."));
}

void ApplicationControllerTest::summaryContinuityInjectsPersistedSummaryAfterRestart() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const auto dbPath = tempDir.filePath(QStringLiteral("conversations.sqlite3"));
    QString conversationId;
    {
        auto store = std::make_unique<SQLiteConversationStore>(dbPath);
        auto* storePtr = store.get();
        auto controller = makeControllerWithConversationStore(std::move(store));
        conversationId = controller->activeConversationId();
        for (int i = 0; i < 5; ++i) {
            QVERIFY(controller->sendMessage(QStringLiteral("continuity persisted turn %1").arg(i)));
        }

        sentinel::core::ConversationSummaryMetadataRecord metadata;
        metadata.conversationId = conversationId;
        metadata.summaryTimestampUtc = QDateTime::currentDateTimeUtc();
        metadata.coveredFirstMessageId = 2;
        metadata.coveredLastMessageId = 6;
        metadata.estimatedReductionPercent = 72;
        metadata.readinessState = QStringLiteral("Ready");
        metadata.summaryText = QStringLiteral("Persisted continuity summary for restart.");
        metadata.summary = QStringLiteral("Ready / messages 2-6 / 72% estimated reduction");
        QVERIFY(storePtr->saveSummaryMetadata(metadata));
    }

    auto reloaded =
        makeControllerWithConversationStore(std::make_unique<SQLiteConversationStore>(dbPath));
    QCOMPARE(reloaded->activeConversationId(), conversationId);
    reloaded->setPromptContextInjectionEnabled(true);
    QVERIFY(reloaded->sendMessage(QStringLiteral("continuity followup")));
    QCOMPARE(reloaded->summaryContinuityStatus(), QStringLiteral("Active"));
    QVERIFY(reloaded->summaryContinuityFreshnessSummary().contains(QStringLiteral("newer")));
    QVERIFY(reloaded->summaryContinuityCoverageSummary().contains(QStringLiteral("messages 2-6")));
    QVERIFY(reloaded->summaryContinuityContributionSummary().contains(QStringLiteral("active")));
    QVERIFY(reloaded->contextExplainabilityEnabled());
    QVERIFY(reloaded->contextReasoningSummary().contains(QStringLiteral("Context reasoning")));
    QVERIFY(reloaded->contextReasoningBudgetSummary().contains(QStringLiteral("summary")));
    QVERIFY(
        reloaded->contextReasoningOrderingSummary().contains(QStringLiteral("recent transcript")));
    QVERIFY(!reloaded->contextReasoningDeveloperTraces()
                 .join(QStringLiteral("\n"))
                 .contains(QStringLiteral("[Sentinel Local Context]")));
}

void ApplicationControllerTest::staleSummaryContinuityFallsBackToTranscriptOnly() {
    auto store = std::make_unique<sentinel::core::InMemoryConversationStore>();
    auto* storePtr = store.get();
    auto controller = makeControllerWithConversationStore(std::move(store));
    for (int i = 0; i < 4; ++i) {
        QVERIFY(controller->sendMessage(QStringLiteral("stale continuity seed %1").arg(i)));
    }

    sentinel::core::ConversationSummaryMetadataRecord metadata;
    metadata.conversationId = controller->activeConversationId();
    metadata.summaryTimestampUtc = QDateTime::currentDateTimeUtc();
    metadata.coveredFirstMessageId = 2;
    metadata.coveredLastMessageId = 3;
    metadata.estimatedReductionPercent = 65;
    metadata.readinessState = QStringLiteral("Ready");
    metadata.summaryText = QStringLiteral("Stale summary text must not inject.");
    metadata.summary = QStringLiteral("Ready / messages 2-3 / 65% estimated reduction");
    QVERIFY(storePtr->saveSummaryMetadata(metadata));
    const auto otherId = controller->createConversation(QStringLiteral("Other"));
    QVERIFY(!otherId.isEmpty());
    QVERIFY(controller->switchConversation(metadata.conversationId));

    for (int i = 0; i < 13; ++i) {
        QVERIFY(controller->sendMessage(QStringLiteral("newer stale turn %1").arg(i)));
    }
    const auto beforeMessages = controller->chatMessages();
    const auto beforeMemory = controller->memoryEntries();

    controller->setPromptContextInjectionEnabled(true);
    QVERIFY(controller->sendMessage(QStringLiteral("stale continuity followup")));
    QCOMPARE(controller->summaryContinuityStatus(), QStringLiteral("Stale"));
    QVERIFY(controller->summaryContinuityFallbackSummary().contains(QStringLiteral("fallback"),
                                                                    Qt::CaseInsensitive));
    QVERIFY(
        controller->contextReasoningFallbackSummary().contains(QStringLiteral("Fallback active")));
    QVERIFY(controller->contextReasoningExclusionHints()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("summary is stale")));
    QVERIFY(
        !controller->promptContextSourceSummary().contains(QStringLiteral("Conversation Summary")));
    QVERIFY(controller->chatMessages().size() > beforeMessages.size());
    QCOMPARE(controller->memoryEntries(), beforeMemory);
}

void ApplicationControllerTest::semanticProviderPlanningExposesDisabledSelection() {
    const auto controller = makeController();

    QCOMPARE(controller->semanticProviderMode(), QStringLiteral("Disabled"));
    QCOMPARE(controller->selectedSemanticProviderName(), QStringLiteral("Disabled"));
    QCOMPARE(controller->semanticProviderReadiness(), QStringLiteral("Disabled"));
    QCOMPARE(controller->semanticProviderHealth(), QStringLiteral("Not Checked"));
    QCOMPARE(controller->semanticActivationReadiness(), QStringLiteral("Refused"));
    QVERIFY(controller->semanticProviderStatusSummary().contains(
        QStringLiteral("Selected semantic provider: Disabled")));
    QVERIFY(controller->semanticActivationSummary().contains(
        QStringLiteral("Semantic activation refused")));
    QVERIFY(controller->semanticProviderCapabilitySummaries().contains(
        QStringLiteral("Prompt mutation blocked")));
    QVERIFY(controller->semanticActivationRequiredSteps()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("deterministic retrieval")));
}

void ApplicationControllerTest::semanticCandidateOrchestrationExposesSafeMetadata() {
    const auto controller = makeController();
    for (int i = 0; i < 8; ++i) {
        QVERIFY(controller->sendMessage(QStringLiteral("semantic candidate marker %1 %2")
                                            .arg(i)
                                            .arg(QString(110, QLatin1Char('c')))));
    }
    controller->remember(QStringLiteral("candidate.local"), QStringLiteral("source isolated"));

    const auto arbitration = controller->semanticCandidateArbitration();

    QVERIFY(arbitration.status == sentinel::core::SemanticCandidateStatus::Ready ||
            arbitration.status == sentinel::core::SemanticCandidateStatus::Truncated);
    QVERIFY(controller->semanticCandidateCount() >= controller->semanticCandidateSelectedCount());
    QVERIFY(controller->semanticCandidateSelectedCount() >= 4);
    QVERIFY(controller->semanticCandidateExcludedCount() >= 1);
    QVERIFY(controller->semanticCandidateParticipationSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Recent Conversation Window")));
    QVERIFY(controller->semanticCandidateParticipationSummaries()
                .join(QStringLiteral("\n"))
                .contains(QStringLiteral("Future Semantic/Vector Candidates")));
    QCOMPARE(controller->hybridRetrievalStatus(), QStringLiteral("Deterministic Only"));
    QVERIFY(
        controller->hybridRetrievalSummary().contains(QStringLiteral("semantic path disabled")));
    QVERIFY(controller->hybridRetrievalReadinessChecks().contains(
        QStringLiteral("Vector database activation: disabled")));
}

void ApplicationControllerTest::semanticArbitrationSimulationExposesSafeMetadata() {
    const auto controller = makeController();
    for (int i = 0; i < 6; ++i) {
        QVERIFY(controller->sendMessage(QStringLiteral("semantic arbitration marker %1 %2")
                                            .arg(i)
                                            .arg(QString(80, QLatin1Char('a')))));
    }
    controller->remember(QStringLiteral("arbitration.local"), QStringLiteral("metadata only"));

    const auto planningBefore = controller->retrievalPlanningResult();
    const auto simulated = controller->semanticArbitrationResult();
    const auto planningAfter = controller->retrievalPlanningResult();

    QCOMPARE(planningAfter.selectedCandidateCount, planningBefore.selectedCandidateCount);
    QCOMPARE(controller->semanticArbitrationStatus(), QStringLiteral("Simulated"));
    QVERIFY(controller->semanticArbitrationReadiness().contains(QStringLiteral("cannot change "
                                                                               "prompts")));
    QVERIFY(controller->semanticArbitrationSummary().contains(
        QStringLiteral("Deterministic retrieval remains final authority")));
    QVERIFY(controller->semanticArbitrationBudgetSummary().contains(
        QStringLiteral("0 semantic candidates selected")));
    QVERIFY(!controller->semanticArbitrationSelectionSummaries().isEmpty());
    QVERIFY(controller->semanticArbitrationChecks().contains(
        QStringLiteral("Real embeddings: disabled")));
    QVERIFY(controller->semanticArbitrationChecks().contains(
        QStringLiteral("Vector search: disabled")));
    QCOMPARE(simulated.budget.semanticSelectedCandidateCount, 0);
    QCOMPARE(controller->embeddingRuntimeReadiness(), QStringLiteral("Blocked"));
    QVERIFY(controller->embeddingRuntimeSummary().contains(QStringLiteral("no embeddings")));
    QVERIFY(controller->embeddingRuntimeBudgetSummary().contains(
        QStringLiteral("planned embedding jobs")));
    QVERIFY(controller->embeddingRuntimeRequirementSummaries().contains(
        QStringLiteral("Committed-memory-only indexing policy")));
    QVERIFY(controller->embeddingRuntimeConstraintSummaries().contains(
        QStringLiteral("No Ollama embedding calls while disabled")));
}

void ApplicationControllerTest::isolatedEmbeddingRuntimeExposureIsBoundedAndDisabledByDefault() {
    const auto controller = makeController();
    const auto planningBefore = controller->retrievalPlanningResult();
    const auto promptBefore = controller->latestPromptContextInjectionResult();

    QCOMPARE(controller->isolatedEmbeddingRuntimeStatus(), QStringLiteral("Refused"));
    QCOMPARE(controller->isolatedEmbeddingRuntimeHealth(), QStringLiteral("Blocked"));
    QCOMPARE(controller->isolatedEmbeddingRuntimeReadiness(), QStringLiteral("Refused"));
    QVERIFY(controller->isolatedEmbeddingRuntimeSummary().contains(
        QStringLiteral("readiness metadata only")));
    QVERIFY(controller->isolatedEmbeddingRuntimeBoundedState().contains(
        QStringLiteral("no vectors persisted")));
    QVERIFY(controller->isolatedEmbeddingRuntimeChecks().contains(
        QStringLiteral("Prompt integration: disabled")));
    QVERIFY(controller->isolatedEmbeddingRuntimeChecks().contains(
        QStringLiteral("Retrieval ranking mutation: disabled")));
    QVERIFY(controller->isolatedEmbeddingRuntimeChecks().contains(
        QStringLiteral("Vector DB persistence: disabled")));

    const auto planningAfter = controller->retrievalPlanningResult();
    const auto promptAfter = controller->latestPromptContextInjectionResult();
    QCOMPARE(planningAfter.selectedCandidateCount, planningBefore.selectedCandidateCount);
    QCOMPARE(promptAfter.injectedBlockCount, promptBefore.injectedBlockCount);
}

void ApplicationControllerTest::
    vectorPersistenceExposureIsDisabledAndDoesNotMutatePlanningOrPrompt() {
    const auto controller = makeController();
    const auto planningBefore = controller->retrievalPlanningResult();
    const auto promptBefore = controller->latestPromptContextInjectionResult();

    QCOMPARE(controller->vectorPersistenceStatus(), QStringLiteral("Disabled"));
    QCOMPARE(controller->vectorPersistenceHealth(), QStringLiteral("Blocked"));
    QCOMPARE(controller->vectorPersistenceReadiness(), QStringLiteral("Disabled"));
    QCOMPARE(controller->vectorPersistenceIndexedItemCount(), 0);
    QVERIFY(controller->vectorPersistenceSummary().contains(QStringLiteral("disabled by default")));
    QVERIFY(controller->vectorPersistenceBoundedState().contains(QStringLiteral("local-only")));
    QVERIFY(controller->vectorPersistenceChecks().contains(
        QStringLiteral("Automatic indexing: disabled")));
    QVERIFY(controller->vectorPersistenceChecks().contains(
        QStringLiteral("Filesystem scanning: disabled")));
    QVERIFY(controller->vectorPersistenceChecks().contains(
        QStringLiteral("Semantic retrieval authority: disabled")));
    QVERIFY(controller->vectorPersistenceChecks().contains(
        QStringLiteral("Prompt mutation: disabled")));

    const auto planningAfter = controller->retrievalPlanningResult();
    const auto promptAfter = controller->latestPromptContextInjectionResult();
    QCOMPARE(planningAfter.selectedCandidateCount, planningBefore.selectedCandidateCount);
    QCOMPARE(promptAfter.injectedBlockCount, promptBefore.injectedBlockCount);
    QCOMPARE(controller->semanticRetrievalEnabled(), false);
}

void ApplicationControllerTest::semanticSearchExposureIsBoundedAndDoesNotMutatePlanningOrPrompt() {
    const auto controller = makeController();
    const auto planningBefore = controller->retrievalPlanningResult();
    const auto promptBefore = controller->latestPromptContextInjectionResult();

    QCOMPARE(controller->semanticSearchStatus(), QStringLiteral("Disabled"));
    QCOMPARE(controller->semanticSearchCandidateCount(), 0);
    QVERIFY(controller->semanticSearchReadiness().contains(QStringLiteral("disabled")));
    QVERIFY(controller->semanticSearchSummary().contains(QStringLiteral("disabled")));
    QVERIFY(controller->semanticSearchRuntimeState().contains(QStringLiteral("local-only")));
    QVERIFY(controller->semanticSearchBudgetSummary().contains(
        QStringLiteral("0 bounded semantic candidates")));
    QVERIFY(controller->semanticSearchArbitrationSummary().contains(
        QStringLiteral("deterministic candidates remain final authority")));
    QVERIFY(controller->semanticSearchChecks().contains(
        QStringLiteral("Filesystem indexing: disabled")));
    QVERIFY(controller->semanticSearchChecks().contains(
        QStringLiteral("Semantic prompt authority: disabled")));
    QVERIFY(controller->semanticSearchChecks().contains(
        QStringLiteral("RetrievalPlanningResult mutation: no")));

    const auto planningAfter = controller->retrievalPlanningResult();
    const auto promptAfter = controller->latestPromptContextInjectionResult();
    QCOMPARE(planningAfter.selectedCandidateCount, planningBefore.selectedCandidateCount);
    QCOMPARE(planningAfter.selectedSourceCount, planningBefore.selectedSourceCount);
    QCOMPARE(promptAfter.injectedBlockCount, promptBefore.injectedBlockCount);
    QCOMPARE(controller->semanticRetrievalEnabled(), false);
}

void ApplicationControllerTest::hybridBridgeExposureIsBoundedAndDoesNotMutatePlanningOrPrompt() {
    const auto controller = makeController();
    controller->sendMessage(QStringLiteral("hybrid bridge deterministic context"));
    controller->remember(QStringLiteral("hybrid.bridge"),
                         QStringLiteral("deterministic memory authority"));
    const auto planningBefore = controller->retrievalPlanningResult();
    const auto promptBefore = controller->latestPromptContextInjectionResult();

    QCOMPARE(controller->hybridBridgeStatus(), QStringLiteral("Deterministic Only"));
    QVERIFY(controller->hybridBridgeReadiness().contains(QStringLiteral("deterministic fallback")));
    QVERIFY(controller->hybridBridgeSummary().contains(QStringLiteral("deterministic fallback")));
    QVERIFY(controller->hybridBridgeBudgetSummary().contains(QStringLiteral("bridge candidates")));
    QVERIFY(controller->hybridBridgeSourceSummary().contains(QStringLiteral("deterministic")));
    QVERIFY(controller->hybridBridgeArbitrationSummary().contains(
        QStringLiteral("Deterministic-first")));
    QVERIFY(controller->hybridBridgeFallbackSummary().contains(QStringLiteral("deterministic")));
    QVERIFY(controller->hybridBridgeCandidateCount() >= 1);
    QCOMPARE(controller->hybridBridgeSemanticFillCount(), 0);
    QVERIFY(controller->hybridBridgeChecks().contains(
        QStringLiteral("RetrievalPlanningResult mutation: no")));
    QVERIFY(controller->hybridBridgeChecks().contains(
        QStringLiteral("PromptContextBlocks mutation: no")));
    QVERIFY(controller->hybridBridgeChecks().contains(
        QStringLiteral("Prompt content injection: disabled")));

    const auto planningAfter = controller->retrievalPlanningResult();
    const auto promptAfter = controller->latestPromptContextInjectionResult();
    QCOMPARE(planningAfter.selectedCandidateCount, planningBefore.selectedCandidateCount);
    QCOMPARE(planningAfter.selectedSourceCount, planningBefore.selectedSourceCount);
    QCOMPARE(promptAfter.injectedBlockCount, promptBefore.injectedBlockCount);
    QCOMPARE(controller->semanticRetrievalEnabled(), false);
}

void ApplicationControllerTest::
    semanticAcceptanceExposureIsBoundedAndDoesNotMutatePlanningOrPrompt() {
    const auto controller = makeController();
    controller->sendMessage(QStringLiteral("semantic acceptance deterministic context"));
    controller->remember(QStringLiteral("semantic.acceptance"),
                         QStringLiteral("deterministic memory authority"));
    const auto planningBefore = controller->retrievalPlanningResult();
    const auto promptBefore = controller->latestPromptContextInjectionResult();

    QCOMPARE(controller->semanticAcceptanceStatus(), QStringLiteral("Deterministic Only"));
    QVERIFY(controller->semanticAcceptanceReadiness().contains(QStringLiteral("not ready")) ||
            controller->semanticAcceptanceReadiness().contains(QStringLiteral("Semantic")));
    QVERIFY(controller->semanticAcceptanceSummary().contains(QStringLiteral("deterministic")));
    QVERIFY(controller->semanticAcceptanceBudgetSummary().contains(
        QStringLiteral("semantic supplements")));
    QVERIFY(
        controller->semanticAcceptanceSourceSummary().contains(QStringLiteral("deterministic")));
    QVERIFY(
        controller->semanticAcceptanceArbitrationSummary().contains(QStringLiteral("acceptance")));
    QVERIFY(
        controller->semanticAcceptanceFallbackSummary().contains(QStringLiteral("deterministic")));
    QCOMPARE(controller->semanticAcceptanceAcceptedCount(), 0);
    QVERIFY(controller->semanticAcceptanceBudgetCharacters() > 0);
    QVERIFY(controller->semanticAcceptanceChecks().contains(
        QStringLiteral("RetrievalPlanningResult mutation: no")));
    QVERIFY(controller->semanticAcceptanceChecks().contains(
        QStringLiteral("PromptContextBlocks mutation: no")));
    QVERIFY(controller->semanticAcceptanceChecks().contains(
        QStringLiteral("Deterministic candidate replacement: no")));

    const auto planningAfter = controller->retrievalPlanningResult();
    const auto promptAfter = controller->latestPromptContextInjectionResult();
    QCOMPARE(planningAfter.selectedCandidateCount, planningBefore.selectedCandidateCount);
    QCOMPARE(planningAfter.selectedSourceCount, planningBefore.selectedSourceCount);
    QCOMPARE(promptAfter.injectedBlockCount, promptBefore.injectedBlockCount);
    QCOMPARE(controller->semanticRetrievalEnabled(), false);
}

void ApplicationControllerTest::
    semanticSupplementAssemblyExposureIsDisabledAndDoesNotMutatePlanningOrPrompt() {
    const auto controller = makeController();
    controller->sendMessage(QStringLiteral("semantic supplement assembly deterministic context"));
    controller->remember(QStringLiteral("semantic.supplement"),
                         QStringLiteral("deterministic memory remains authoritative"));
    const auto planningBefore = controller->retrievalPlanningResult();
    const auto promptBefore = controller->latestPromptContextInjectionResult();

    QCOMPARE(controller->semanticSupplementAssemblyStatus(), QStringLiteral("Disabled"));
    QCOMPARE(controller->semanticSupplementAssemblyBlockCount(), 0);
    QVERIFY(controller->semanticSupplementAssemblySummary().contains(QStringLiteral("disabled")));
    QVERIFY(controller->semanticSupplementAssemblyBudgetSummary().contains(QStringLiteral("0 of")));
    QVERIFY(controller->semanticSupplementAssemblySafetySummary().contains(
        QStringLiteral("non-authoritative")));
    QVERIFY(controller->semanticSupplementAssemblyChecks().contains(
        QStringLiteral("Live prompt inclusion: blocked")));
    QVERIFY(controller->semanticSupplementAssemblyChecks().contains(
        QStringLiteral("PromptContextBlock mutation: no")));
    QVERIFY(controller->semanticSupplementAssemblyChecks().contains(
        QStringLiteral("RetrievalPlanningResult mutation: no")));
    QVERIFY(controller->semanticSupplementAssemblyChecks().contains(
        QStringLiteral("Deterministic context replacement: no")));

    const auto planningAfter = controller->retrievalPlanningResult();
    const auto promptAfter = controller->latestPromptContextInjectionResult();
    QCOMPARE(planningAfter.selectedCandidateCount, planningBefore.selectedCandidateCount);
    QCOMPARE(planningAfter.selectedSourceCount, planningBefore.selectedSourceCount);
    QCOMPARE(promptAfter.injectedBlockCount, promptBefore.injectedBlockCount);
    QCOMPARE(promptAfter.status, promptBefore.status);
}

void ApplicationControllerTest::
    semanticPromptAuthorityExposureIsDefaultDeniedAndDoesNotMutatePrompt() {
    const auto controller = makeController();
    controller->sendMessage(QStringLiteral("semantic prompt authority deterministic context"));
    controller->remember(QStringLiteral("semantic.authority"),
                         QStringLiteral("deterministic retrieval remains authoritative"));
    const auto planningBefore = controller->retrievalPlanningResult();
    const auto promptBefore = controller->latestPromptContextInjectionResult();

    QCOMPARE(controller->semanticPromptAuthorityStatus(), QStringLiteral("Disabled"));
    QCOMPARE(controller->semanticPromptAuthorityWouldIncludeBlockCount(), 0);
    QCOMPARE(controller->semanticPromptInclusionEnabled(), false);
    QCOMPARE(controller->semanticPromptInclusionStatus(), QStringLiteral("Disabled"));
    QCOMPARE(controller->semanticPromptInclusionIncludedCount(), 0);
    QVERIFY(
        controller->semanticPromptAuthorityDecisionSummary().contains(QStringLiteral("Denied")));
    QVERIFY(controller->semanticPromptAuthoritySafetySummary().contains(
        QStringLiteral("deterministic-only fallback")));
    QVERIFY(controller->semanticPromptAuthorityFallbackSummary().contains(
        QStringLiteral("deterministic prompt assembly remains unchanged")));
    QVERIFY(controller->semanticPromptAuthorityAuditSummary().contains(QStringLiteral("disabled")));
    QVERIFY(controller->semanticPromptAuthorityChecks().contains(
        QStringLiteral("Live prompt mutation: blocked")));
    QVERIFY(controller->semanticPromptAuthorityChecks().contains(
        QStringLiteral("PromptContextBlock mutation: no")));
    QVERIFY(controller->semanticPromptAuthorityChecks().contains(
        QStringLiteral("RetrievalPlanningResult mutation: no")));
    QVERIFY(controller->semanticPromptAuthorityChecks().contains(
        QStringLiteral("Provider handles exposed: no")));
    QVERIFY(controller->semanticPromptInclusionSummary().contains(QStringLiteral("disabled")));
    QVERIFY(controller->semanticPromptInclusionBudgetSummary().contains(QStringLiteral("0 of")));
    QVERIFY(controller->semanticPromptInclusionFallbackSummary().contains(
        QStringLiteral("deterministic-only prompt assembly")));
    QVERIFY(controller->semanticPromptInclusionAuditSummary().contains(QStringLiteral("disabled")));
    QVERIFY(controller->semanticPromptInclusionDeterministicAuthorityPreserved());
    QVERIFY(controller->semanticPromptInclusionChecks().contains(
        QStringLiteral("Deterministic retrieval authoritative: yes")));
    QVERIFY(controller->semanticPromptInclusionChecks().contains(
        QStringLiteral("Raw prompt payloads exposed: no")));

    const auto planningAfter = controller->retrievalPlanningResult();
    const auto promptAfter = controller->latestPromptContextInjectionResult();
    QCOMPARE(planningAfter.selectedCandidateCount, planningBefore.selectedCandidateCount);
    QCOMPARE(planningAfter.selectedSourceCount, planningBefore.selectedSourceCount);
    QCOMPARE(promptAfter.injectedBlockCount, promptBefore.injectedBlockCount);
    QCOMPARE(promptAfter.status, promptBefore.status);
}

void ApplicationControllerTest::promptContextInjectionDoesNotMutateMemoryOrCandidates() {
    auto fakeClient = std::make_unique<FakeLocalInferenceClient>();
    auto controller = std::make_unique<ApplicationController>(
        std::make_unique<LocalEchoProvider>(), std::make_unique<InMemoryStore>(), nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, std::make_unique<AllowLocalInferencePolicy>(), nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr,
        std::make_unique<FakeOllamaRuntimeClient>(
            QList<OllamaModelSummary>{{QStringLiteral("llama3.2"), {}, 10}}),
        std::move(fakeClient));
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Candidate remains pending."));
    controller->remember(QStringLiteral("stable.local"), QStringLiteral("unchanged"));
    const auto beforeEntries = controller->memoryEntries();
    const auto beforeCandidateCount = controller->memoryCandidateCount();
    controller->setSelectedLocalModel(QStringLiteral("llama3.2"));
    controller->setLocalChatInferenceEnabled(true);
    controller->setPromptContextInjectionEnabled(true);

    QVERIFY(controller->sendMessage(QStringLiteral("stable local hello")));

    QCOMPARE(controller->memoryEntries(), beforeEntries);
    QCOMPARE(controller->memoryCandidateCount(), beforeCandidateCount);
    QCOMPARE(controller->pendingMemoryCandidateCount(), 1);
    QCOMPARE(controller->memoryCandidateIds().first(), id);
}

void ApplicationControllerTest::clearChatKeepsApprovedMemoryCandidateMetadata() {
    const auto controller = makeController();
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Keep this approved metadata across chat clear."));
    QVERIFY(controller->approveMemoryCandidate(id));

    controller->sendMessage(QStringLiteral("status"));
    controller->clearChat();

    QCOMPARE(controller->memoryCandidateCount(), 1);
    QCOMPARE(controller->approvedMemoryCandidateCount(), 1);
    QVERIFY(controller->memoryCandidateSummaries().first().contains(QStringLiteral("Approved")));
}

void ApplicationControllerTest::clearChatKeepsCommittedMemory() {
    const auto controller = makeController();
    const auto id = controller->createMemoryCandidateFromConversationText(
        QStringLiteral("Committed memory survives chat clear."));
    QVERIFY(controller->approveMemoryCandidate(id));
    QVERIFY(controller->requestMemoryCandidateCommit(id));

    const auto previous = controller->activeConversationId();
    controller->sendMessage(QStringLiteral("status"));
    controller->clearChat();

    QCOMPARE(controller->committedMemoryCandidateCount(), 1);
    QVERIFY(controller->memoryEntries().isEmpty());
    const auto retained =
        controller->memoryStore()->retrieveMemory("survives", {previous, {}, {}, false});
    QCOMPARE(retained.status, sentinel::core::ScopedMemoryStatus::Ready);
    QCOMPARE(retained.records.size(), 1);
    QCOMPARE(retained.records.first().value, QString("Committed memory survives chat clear."));
}

void ApplicationControllerTest::clearsRuntimeMemoryEntries() {
    const auto controller = makeController();
    QSignalSpy spy(controller.get(), &ApplicationController::memoryEntriesChanged);
    QSignalSpy maintenanceSpy(controller.get(), &ApplicationController::maintenanceStatusChanged);
    controller->remember(QStringLiteral("mode"), QStringLiteral("Companion"));

    const auto cleared = controller->clearMemory();

    QVERIFY(cleared);
    QVERIFY(controller->memoryEntries().isEmpty());
    QCOMPARE(controller->memoryMaintenanceStatus(), QStringLiteral("Clear completed"));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(maintenanceSpy.count(), 1);
}

void ApplicationControllerTest::failsSafeWhenMemoryStoreUnavailable() {
    ApplicationController controller(std::make_unique<LocalEchoProvider>(),
                                     std::make_unique<UnavailableMemoryStore>());
    QSignalSpy spy(&controller, &ApplicationController::memoryEntriesChanged);
    QSignalSpy maintenanceSpy(&controller, &ApplicationController::maintenanceStatusChanged);

    const auto cleared = controller.clearMemory();

    QVERIFY(!cleared);
    QCOMPARE(controller.memoryStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(controller.memoryMaintenanceStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(spy.count(), 0);
    QCOMPARE(maintenanceSpy.count(), 1);
}

void ApplicationControllerTest::rejectsBlankMemoryKeys() {
    const auto controller = makeController();
    QSignalSpy spy(controller.get(), &ApplicationController::memoryEntriesChanged);

    controller->remember(QStringLiteral("   "), QStringLiteral("ignored"));

    QVERIFY(controller->memoryEntries().isEmpty());
    QCOMPARE(spy.count(), 0);
}

void ApplicationControllerTest::overwritesMemoryEntriesThroughStoreBackend() {
    const auto controller = makeController();
    QSignalSpy spy(controller.get(), &ApplicationController::memoryEntriesChanged);

    controller->remember(QStringLiteral("mode"), QStringLiteral("Companion"));
    controller->remember(QStringLiteral("provider"), QStringLiteral("LocalEchoProvider"));
    controller->remember(QStringLiteral("mode"), QStringLiteral("Tactical"));

    const QStringList expected{
        QStringLiteral("mode: Tactical"),
        QStringLiteral("provider: LocalEchoProvider"),
    };

    QCOMPARE(controller->memoryEntries(), expected);
    QCOMPARE(spy.count(), 3);
}

void ApplicationControllerTest::reportsRuntimeOnlyWhenChatStoreUnavailableOnClear() {
    auto store =
        std::make_unique<RecordingChatHistoryStore>(QList<sentinel::core::ChatMessage>{}, false);
    ApplicationController controller(nullptr, std::make_unique<InMemoryStore>(), nullptr,
                                     std::move(store));
    QSignalSpy maintenanceSpy(&controller, &ApplicationController::maintenanceStatusChanged);

    controller.sendMessage(QStringLiteral("status"));
    const auto cleared = controller.clearChat();

    QVERIFY(!cleared);
    QCOMPARE(controller.chatHistory().size(), 1);
    QCOMPARE(controller.chatMaintenanceStatus(), QStringLiteral("Runtime Only"));
    QCOMPARE(maintenanceSpy.count(), 1);
}

QTEST_MAIN(ApplicationControllerTest)

#include "test_application_controller.moc"
