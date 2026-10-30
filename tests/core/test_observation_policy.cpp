// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>
#ifndef Q_MOC_RUN
#include "../support/ScriptedProviderTransport.h"
#endif

#include "sentinel/core/agent/ClaimGroundingResolver.h"
#include "sentinel/core/agent/ObservationPolicy.h"
#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/platform/UserDirectories.h"
#include "sentinel/core/provider/SelectedEndpointChatProvider.h"
#include "sentinel/core/runtime/BuiltInToolProvider.h"
#include "sentinel/core/runtime/IFileSystemService.h"
#include "sentinel/core/runtime/LocalInference.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>

using namespace sentinel::core;

namespace {

class FixedReplyProvider final : public IChatProvider {
public:
    QString name() const override {
        return QStringLiteral("fixed-reply");
    }
    ChatProviderStatus status() const override {
        return ChatProviderStatus::Ready;
    }
    ChatProviderReply sendMessage(const QString&) override {
        if (failures > 0) {
            --failures;
            ChatProviderReply result;
            result.category = ChatProviderErrorCategory::RequestRejected;
            result.completion = ResponseCompletion::Truncated;
            result.errorMessage = QStringLiteral(
                "Provider 'lm-studio': Provider output limit reached; classification is "
                "indeterminate.");
            return result;
        }
        return {true, reply, {}};
    }
    ChatProviderReply sendRequest(const QString& message,
                                  const ChatRequestOptions& options) override {
        ++calls;
        request = options;
        prompt = message;
        if (options.cancellationToken && options.cancellationToken->load()) {
            ChatProviderReply result;
            result.lifecycle = ChatRequestLifecycle::Cancelled;
            result.category = ChatProviderErrorCategory::Cancelled;
            result.errorMessage = QStringLiteral("Cancelled");
            return result;
        }
        if (!responses.isEmpty())
            return responses.takeFirst();
        if (onRequest)
            return onRequest(message, options);
        return sendMessage(message);
    }

    QString reply;
    QString prompt;
    ChatRequestOptions request;
    int calls = 0;
    int failures = 0;
    QList<ChatProviderReply> responses;
    std::function<ChatProviderReply(const QString&, const ChatRequestOptions&)> onRequest;
};

} // namespace

class ObservationPolicyTest final : public QObject {
    Q_OBJECT

private slots:
    void preCancelledOversizeClassificationDoesNotBecomeBudgetFailure() {
        FixedReplyProvider provider;
        auto token = std::make_shared<std::atomic_bool>(true);
        ObservationIntentPolicy policy(&provider, token, {"cloud-api", "fixture"});
        const auto result = policy.classify(QString(100000, 'x'), {}, {});
        QCOMPARE(result.failure, ObservationClassificationFailure::Cancelled);
        QCOMPARE(provider.calls, 0);
        QVERIFY(result.providerFailure);
        QCOMPARE(result.providerFailure->attempts, 0);
    }

    void classifierParentDeadline_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto* name :
             {"remaining", "at-expiry", "after-expiry", "cancel", "late-success", "local-long"})
            QTest::newRow(name) << QString(name);
    }
    void classifierParentDeadline() {
        QFETCH(QString, scenario);
        qint64 now = 0;
        auto token = std::make_shared<std::atomic_bool>(false);
        FixedReplyProvider provider;
        QList<int> deadlines;
        provider.onRequest = [&](const auto&, const auto& options) {
            deadlines.append(options.deadlineMs);
            if (provider.calls == 1) {
                now = scenario == "remaining"      ? 59000
                      : scenario == "after-expiry" ? 60001
                      : scenario == "local-long"   ? 900000
                                                   : 60000;
                if (scenario == "cancel")
                    token->store(true);
                ChatProviderReply reply;
                reply.completion = ResponseCompletion::Truncated;
                if (scenario == "late-success") {
                    reply.success = true;
                    reply.completion = ResponseCompletion::Complete;
                    reply.message = R"({"requirements":[]})";
                }
                return reply;
            }
            return ChatProviderReply{true, R"({"requirements":[]})", {}};
        };
        ModelBinding binding{scenario == "local-long" ? "lm-studio" : "cloud-api", "fixture"};
        ObservationIntentPolicy policy(&provider, token, binding, [&] { return now; });
        const auto intent = policy.classify("Explain this fictional scenario", {}, {});
        const bool complete = scenario == "remaining" || scenario == "local-long";
        QCOMPARE(provider.calls, complete ? 2 : 1);
        QCOMPARE(intent.indeterminate, !complete);
        if (scenario == "remaining")
            QCOMPARE(deadlines, QList<int>({60000, 1000}));
        if (scenario == "local-long")
            QCOMPARE(deadlines, QList<int>({0, 0}));
        if (!complete) {
            QVERIFY(intent.providerFailure);
            QCOMPARE(intent.providerFailure->category, scenario == "cancel"
                                                           ? ChatProviderErrorCategory::Cancelled
                                                           : ChatProviderErrorCategory::Timeout);
        }
    }
    void classifierReadmitsOutputReserve_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto* name : {"small-known", "exact-known", "unknown-fallback-refusal",
                                 "unknown-output", "unknown-fits", "known-output-ceiling"})
            QTest::newRow(name) << QString(name);
    }
    void classifierReadmitsOutputReserve() {
        QFETCH(QString, scenario);
        const QString goal = scenario == "unknown-fallback-refusal"
                                 ? QString(9000, 'x')
                                 : QString("Explain a fictional scenario");
        FixedReplyProvider probe;
        probe.reply = R"({"requirements":[]})";
        ObservationIntentPolicy initial(&probe, {}, {"cloud-api", "fixture"}, [] { return 0; });
        const auto admitted = initial.classify(goal, {}, {});
        QVERIFY(!admitted.indeterminate);
        const int input =
            admitted.requestDiagnostics.first().toObject().value("estimated_input_tokens").toInt();
        ModelBinding binding{"cloud-api", "fixture"};
        if (scenario == "small-known" || scenario == "known-output-ceiling")
            binding.capabilities.contextWindow = input + 2048;
        else if (scenario == "exact-known" || scenario == "unknown-output")
            binding.capabilities.contextWindow = input + 4096;
        if (scenario == "known-output-ceiling")
            binding.capabilities.maxOutputTokens = 2048;
        FixedReplyProvider provider;
        provider.failures = 1;
        provider.reply = R"({"requirements":[]})";
        ObservationIntentPolicy policy(&provider, {}, binding, [] { return 0; });
        const auto result = policy.classify(goal, {}, {});
        const bool refused = scenario == "small-known" || scenario == "unknown-fallback-refusal";
        const bool capped = scenario == "known-output-ceiling";
        QCOMPARE(provider.calls, refused || capped ? 1 : 2);
        QCOMPARE(result.indeterminate, refused || capped);
        if (refused) {
            QCOMPARE(result.failure, ObservationClassificationFailure::BudgetRefused);
            QCOMPARE(result.requestDiagnostics.last().toObject().value("provider_invoked").toBool(),
                     false);
        } else if (capped) {
            QCOMPARE(result.failure, ObservationClassificationFailure::OutputTruncated);
        } else {
            QCOMPARE(provider.request.maxOutputTokens, 4096);
        }
        QVERIFY(!QJsonDocument(result.requestDiagnostics).toJson().contains("total_tokens"));
    }
    void transportPolicyRefusalDoesNotCreateReply() {
        sentinel::test::ScopedOnlineNetworkMode restoreMode;
        NetworkPolicyService::instance().setMode(NetworkMode::Offline);
        auto transport = std::make_shared<sentinel::test::ScriptedTransportState>();
        LMStudioConfig config;
        config.endpoint = QUrl("https://fixture.invalid/v1");
        config.apiKey = "PRIVATE_KEY";
        config.networkFactory = std::make_shared<sentinel::test::ScriptedNetworkFactory>(transport);
        auto endpoint = makeSelectedEndpointChatProvider({"cloud-api", "fixture-model"}, config, 0);
        const auto reply = endpoint->sendMessage("PRIVATE_PROMPT");
        QVERIFY(!reply.success);
        QCOMPARE(reply.category, ChatProviderErrorCategory::Offline);
        QVERIFY(transport->requests.isEmpty());
        const auto frames = reply.diagnostics.value("transport_attempts").toArray();
        QCOMPARE(frames.size(), 1);
        const auto frame = frames.first().toObject();
        QCOMPARE(frame.value("policy_allowed").toBool(), false);
        QVERIFY(frame.value("policy_admitted_ms").isNull());
        QVERIFY(frame.value("reply_created_ms").isNull());
        QVERIFY(frame.value("http_status").isNull());
        QVERIFY(frame.value("qt_network_error").isNull());
        QCOMPARE(frame.value("response_completed").toBool(), false);
    }
    void transportRetriesOnlyWhenAllowed_data() {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("allowed-transient") << QString("transient");
        QTest::newRow("deadline-bounded-no-retry") << QString("bounded");
        QTest::newRow("authentication-no-retry") << QString("auth");
    }
    void transportRetriesOnlyWhenAllowed() {
        QFETCH(QString, scenario);
        sentinel::test::ScopedOnlineNetworkMode online;
        auto transport = std::make_shared<sentinel::test::ScriptedTransportState>();
        int calls = 0;
        transport->responder = [&](const auto&) {
            ++calls;
            auto response = sentinel::test::ScriptedHttpResponse::body(
                R"({"choices":[{"message":{"content":"done"}}]})");
            if (scenario == "auth") {
                response.status = 401;
            } else if (calls < 3) {
                response.status = 0;
                response.error = QNetworkReply::ConnectionRefusedError;
            }
            return response;
        };
        ModelBinding binding{"cloud-api", "fixture-model"};
        LMStudioConfig config;
        config.endpoint = QUrl("https://fixture.invalid/v1");
        config.apiKey = "PRIVATE_KEY";
        config.networkFactory = std::make_shared<sentinel::test::ScriptedNetworkFactory>(transport);
        auto endpoint = makeSelectedEndpointChatProvider(binding, config, 0);
        ChatRequestOptions options;
        options.deadlineMs = scenario == "bounded" ? 25 : 0;
        const auto reply = endpoint->sendRequest("PRIVATE_PROMPT", options);
        QCOMPARE(reply.success, scenario == "transient");
        const int expected = scenario == "transient" ? 3 : 1;
        QCOMPARE(calls, expected);
        QCOMPARE(reply.attempts, expected);
        const auto attempts = reply.diagnostics.value("transport_attempts").toArray();
        QCOMPARE(attempts.size(), expected);
        QSet<QString> ids;
        for (const auto& item : attempts) {
            const auto id = item.toObject().value("attempt_id").toString();
            QVERIFY(!QUuid(id).isNull());
            ids.insert(id);
        }
        QCOMPARE(ids.size(), expected);
        QVERIFY(!QJsonDocument(attempts).toJson().contains("PRIVATE_"));
    }
    void classificationInjectedTransportDiagnostics_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto* name :
             {"success", "timeout", "cancel", "transport", "missing-usage", "malformed",
              "connection-no-response", "metadata-stall", "partial-body", "refused",
              "reused-connection", "immediate-success"})
            QTest::newRow(name) << QString(name);
    }
    void classificationInjectedTransportDiagnostics() {
        QFETCH(QString, scenario);
        sentinel::test::ScopedOnlineNetworkMode online;
        auto transport = std::make_shared<sentinel::test::ScriptedTransportState>();
        transport->responder = [scenario](const auto&) {
            auto body = QJsonObject{
                {"choices",
                 QJsonArray{QJsonObject{
                     {"message", QJsonObject{{"content", scenario == "malformed"
                                                             ? "PRIVATE_OUTPUT"
                                                             : "{\"requirements\":[]}"}}}}}}};
            if (scenario == "success")
                body.insert("usage", QJsonObject{{"prompt_tokens", 20},
                                                 {"completion_tokens", 4},
                                                 {"total_tokens", 24}});
            auto response =
                sentinel::test::ScriptedHttpResponse::body(QJsonDocument(body).toJson());
            if (scenario == "timeout" || scenario == "cancel") {
                response.frames.clear();
                response.finish = false;
            }
            if (scenario == "transport") {
                response.status = 0;
                response.error = QNetworkReply::SslHandshakeFailedError;
                response.errorString = "PRIVATE_TRANSPORT";
            }
            if (scenario == "connection-no-response" || scenario == "metadata-stall" ||
                scenario == "partial-body") {
                response.finish = false;
                response.frames = scenario == "partial-body" ? QList<QByteArray>{"PRIVATE_PARTIAL"}
                                                             : QList<QByteArray>{};
                response.status = scenario == "connection-no-response" ? 0 : 200;
                response.emitConnectionSignals = true;
                response.emitRequestSentSignal = true;
                response.emitMetadataSignal = scenario != "connection-no-response";
            }
            if (scenario == "refused") {
                response.status = 0;
                response.error = QNetworkReply::ConnectionRefusedError;
                response.errorString = "PRIVATE_CONNECTION";
            }
            if (scenario == "reused-connection") {
                // A reused socket has no new connect/encrypted signals. Do not
                // infer reuse from their absence in production diagnostics.
                response.emitRequestSentSignal = true;
                response.emitMetadataSignal = true;
            }
            response.immediateFinish = scenario == "immediate-success";
            return response;
        };
        ModelBinding binding{"cloud-api", "fixture-model"};
        LMStudioConfig config;
        config.endpoint = QUrl("https://fixture.invalid/v1");
        config.apiKey = "PRIVATE_KEY";
        config.networkFactory = std::make_shared<sentinel::test::ScriptedNetworkFactory>(transport);
        auto endpoint = makeSelectedEndpointChatProvider(binding, config, 0);
        FixedReplyProvider provider;
        provider.onRequest = [&](const auto& prompt, const auto& options) {
            auto fixtureOptions = options;
            // Exercise real timeout/abort through an injected reply in milliseconds,
            // while separately asserting the unchanged production policy deadline.
            fixtureOptions.deadlineMs = 25;
            return endpoint->sendRequest(prompt, fixtureOptions);
        };
        auto token = std::make_shared<std::atomic_bool>(false);
        if (scenario == "cancel")
            QTimer::singleShot(2, [&] { token->store(true); });
        ObservationIntentPolicy policy(&provider, token, binding);
        const auto intent = policy.classify("PRIVATE_GOAL", "PRIVATE_CONTEXT", {});
        QVERIFY(provider.request.deadlineMs > 0 && provider.request.deadlineMs <= 60000);
        QCOMPARE(transport->requests.size(), 1);
        QCOMPARE(intent.requestDiagnostics.size(), 1);
        const auto record = intent.requestDiagnostics.first().toObject();
        const auto phases = record.value("transport_attempts").toArray();
        QCOMPARE(phases.size(), 1);
        const auto phase = phases.first().toObject();
        QVERIFY(!QUuid(phase.value("attempt_id").toString()).isNull());
        QCOMPARE(phase.value("policy_allowed").toBool(), true);
        QCOMPARE(phase.value("connection_reuse").toString(), QString("unknown"));
        QVERIFY(phase.value("dns_started_ms").isNull());
        QVERIFY(phase.value("tcp_connected_ms").isNull());
        for (const auto* key : {"request_created_ms", "policy_admitted_ms", "reply_created_ms",
                                "socket_connect_started_ms", "tls_encrypted_ms", "request_sent_ms",
                                "metadata_received_ms", "first_body_ms", "qt_finished_ms"}) {
            const auto observed = phase.value(QLatin1String(key));
            QVERIFY(observed.isNull() ||
                    observed.toDouble() <= phase.value("observation_finished_ms").toDouble());
        }
        if (scenario == "connection-no-response" || scenario == "metadata-stall" ||
            scenario == "partial-body") {
            QVERIFY(intent.indeterminate);
            QVERIFY(phase.value("socket_connect_started_ms").isDouble());
            QVERIFY(phase.value("tls_encrypted_ms").isDouble());
            QVERIFY(phase.value("request_sent_ms").isDouble());
            QVERIFY(phase.value("deadline_expired_ms").isDouble());
            QCOMPARE(phase.value("response_completed").toBool(), false);
            QCOMPARE(phase.value("partial_body").toBool(), scenario == "partial-body");
            QCOMPARE(phase.value("metadata_received_ms").isDouble(),
                     scenario != "connection-no-response");
            QCOMPARE(phase.value("http_status").isNull(), scenario == "connection-no-response");
        }
        if (scenario == "transport" || scenario == "refused")
            QCOMPARE(phase.value("qt_network_error").toInt(),
                     int(scenario == "transport" ? QNetworkReply::SslHandshakeFailedError
                                                 : QNetworkReply::ConnectionRefusedError));
        if (scenario == "cancel")
            QVERIFY(phase.value("cancelled_ms").isDouble());
        if (scenario == "cancel")
            QVERIFY(record.value("elapsed_ms").toDouble() < 1000);
        if (scenario == "reused-connection") {
            QVERIFY(phase.value("socket_connect_started_ms").isNull());
            QVERIFY(phase.value("tls_encrypted_ms").isNull());
            QVERIFY(phase.value("request_sent_ms").isDouble());
            QVERIFY(phase.value("metadata_received_ms").isDouble());
        }
        if (scenario == "immediate-success") {
            QVERIFY(phase.value("qt_finished_ms").isNull());
            QVERIFY(phase.value("first_body_ms").isNull());
            QVERIFY(phase.value("response_completed").toBool());
        }
        const auto json = QJsonDocument(intent.requestDiagnostics).toJson();
        QVERIFY(!json.contains("PRIVATE_"));
        QVERIFY(!json.contains("fixture.invalid"));
        QVERIFY(!json.contains("Authorization"));
        QVERIFY(record.value("elapsed_ms").isDouble());
        if (scenario == "success" || scenario == "missing-usage" ||
            scenario == "reused-connection" || scenario == "immediate-success") {
            QVERIFY(!intent.indeterminate);
            QCOMPARE(record.value("http_status").toInt(), 200);
            QCOMPARE(record.contains("total_tokens"), scenario == "success");
            QVERIFY(phase.value("response_completed").toBool());
        } else {
            QVERIFY(intent.indeterminate);
            if (scenario == "timeout") {
                QVERIFY(intent.providerFailure);
                QCOMPARE(intent.providerFailure->category, ChatProviderErrorCategory::Timeout);
                // Response headers already arrived; timeout must not erase them.
                QCOMPARE(intent.providerFailure->httpStatus, 200);
                QVERIFY(record.value("timed_out").toBool());
            } else if (scenario == "cancel") {
                QVERIFY(record.value("cancelled").toBool());
            } else if (scenario == "transport") {
                QCOMPARE(record.value("category").toString(), QString("ConnectionFailed"));
            } else if (scenario == "malformed") {
                QCOMPARE(intent.failure, ObservationClassificationFailure::InvalidJson);
            } else {
                QCOMPARE(intent.failure, ObservationClassificationFailure::ProviderFailure);
            }
        }
    }
    void classificationFailureRetainsSafeRequestEvidence() {
        FixedReplyProvider provider;
        ChatProviderReply reply;
        reply.lifecycle = ChatRequestLifecycle::TimedOut;
        reply.category = ChatProviderErrorCategory::Timeout;
        reply.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        reply.errorMessage = "Synthetic deadline";
        reply.diagnostics = {{"prompt_tokens", 31},
                             {"completion_tokens", 7},
                             {"total_tokens", 38},
                             {"Authorization", "PRIVATE_HEADER"},
                             {"finish_reason", "PRIVATE_OUTPUT"},
                             {"prompt", "PRIVATE_PROMPT"}};
        provider.responses = {reply};
        ModelBinding binding{"cloud-api", "fixture-model"};
        ObservationIntentPolicy policy(&provider, {}, binding);
        const auto intent = policy.classify("PRIVATE_GOAL", "PRIVATE_CONTEXT", {});
        QVERIFY(intent.indeterminate);
        QCOMPARE(provider.calls, 1);
        QVERIFY(intent.providerFailure);
        QCOMPARE(intent.providerFailure->category, ChatProviderErrorCategory::Timeout);
        QCOMPARE(intent.providerFailure->lifecycle, ChatRequestLifecycle::TimedOut);
        QCOMPARE(intent.requestDiagnostics.size(), 1);
        const auto diagnostic = intent.requestDiagnostics.first().toObject();
        QCOMPARE(diagnostic.value("request_id").toString(), reply.requestId);
        QCOMPARE(diagnostic.value("deadline_ms").toInt(), 60000);
        QCOMPARE(diagnostic.value("category").toString(), QString("Timeout"));
        QCOMPARE(diagnostic.value("total_tokens").toInt(), 38);
        QVERIFY(diagnostic.value("timed_out").toBool());
        QVERIFY(diagnostic.value("provider_invoked").toBool());
        QVERIFY(!QUuid(diagnostic.value("trace_id").toString()).isNull());
        const auto json = QJsonDocument(intent.requestDiagnostics).toJson();
        QVERIFY(!json.contains("PRIVATE_"));
        QVERIFY(!json.contains("Authorization"));
    }
    void localizedDesktopClassificationUsesObservedStandardDirectory() {
        FixedReplyProvider provider;
        const auto target =
            QDir(standardUserDirectories().value("desktop").toString()).filePath("example.txt");
        provider.reply = QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{"requirements", QJsonArray{QJsonObject{{"domain", "filesystem"},
                                                                    {"resource", target},
                                                                    {"purpose", "inspect"},
                                                                    {"operation", "none"}}}}})
                .toJson());
        ObservationIntentPolicy policy(&provider);
        const auto intent = policy.classify("Masaüstündeki example.txt dosyasını oku", {},
                                            BuiltInToolProvider::descriptors());
        QVERIFY(!intent.indeterminate);
        QCOMPARE(intent.requirements.size(), 1);
        QCOMPARE(intent.requirements.first().resourceHint, target);
        QVERIFY(provider.prompt.contains(QString::fromUtf8(
            QJsonDocument(standardUserDirectories()).toJson(QJsonDocument::Compact))));
        QVERIFY(provider.prompt.contains("metadata, not permission grants"));
    }
    void insufficientBoundCapacityNeverDispatchesClassification() {
        FixedReplyProvider provider;
        provider.reply = R"({"requirements":[]})";
        ModelBinding binding;
        binding.capabilities.contextWindow = 1024;
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::RuntimeReported;
        ObservationIntentPolicy policy(&provider, {}, binding);
        const auto intent = policy.classify("Read example", {}, BuiltInToolProvider::descriptors());
        QVERIFY(intent.indeterminate);
        QVERIFY(intent.error.startsWith("Context budget refused"));
        QVERIFY(provider.prompt.isEmpty());
        QCOMPARE(intent.requestDiagnostics.size(), 1);
        QVERIFY(!intent.requestDiagnostics.first().toObject().value("provider_invoked").toBool());
    }
    void relativeRequirementsUseFrozenWorkspace() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto root = QFileInfo(workspace.path()).canonicalFilePath();
        FixedReplyProvider provider;
        provider.reply =
            R"({"requirements":[{"domain":"filesystem","resource":"calc.py","purpose":"inspect","tool":"read-file"},{"domain":"workspace","resource":".","purpose":"inspect","tool":"git-diff"}]})";
        ObservationIntentPolicy policy(&provider);
        const auto intent =
            policy.classify("Read calc.py and inspect git-diff", "\nACTIVE WORKSPACE ROOT: " + root,
                            BuiltInToolProvider::descriptors());
        QVERIFY(!intent.indeterminate);
        QCOMPARE(intent.requirements.size(), 2);
        QCOMPARE(intent.requirements.first().resourceHint, root + "/calc.py");
        QCOMPARE(intent.requirements.last().resourceHint, root);
    }
    void genericMutationDoesNotRequireAnInventedImplementation() {
        FixedReplyProvider provider;
        provider.reply =
            R"({"requirements":[{"domain":"filesystem","resource":"/fixture/calc.py","purpose":"operate","tool":"edit-file"}]})";
        ObservationIntentPolicy policy(&provider);
        auto intent = policy.classify("Fix calc.py", {}, BuiltInToolProvider::descriptors());
        QVERIFY(!intent.indeterminate);
        QVERIFY(intent.requirements.first().toolId.isEmpty());
        intent =
            policy.classify("Fix calc.py using edit-file", {}, BuiltInToolProvider::descriptors());
        QCOMPARE(intent.requirements.first().toolId, QStringLiteral("edit-file"));
    }
    void gitFileHintUsesActiveRepositoryScope() {
        QTemporaryDir workspace, external;
        QVERIFY(workspace.isValid());
        QVERIFY(external.isValid());
        const auto root = QFileInfo(workspace.path()).canonicalFilePath();
        const auto outside = QFileInfo(external.path()).canonicalFilePath() + "/calc.py";
        FixedReplyProvider provider;
        QJsonArray requirements;
        for (const auto& path : {root + "/calc.py", outside})
            requirements.append(QJsonObject{{"domain", "workspace"},
                                            {"resource", path},
                                            {"purpose", "inspect"},
                                            {"tool", "git-diff"}});
        provider.reply =
            QString::fromUtf8(QJsonDocument(QJsonObject{{"requirements", requirements}})
                                  .toJson(QJsonDocument::Compact));
        ObservationIntentPolicy policy(&provider);
        const auto intent =
            policy.classify("Inspect the active workspace diff", "\nACTIVE WORKSPACE ROOT: " + root,
                            BuiltInToolProvider::descriptors());
        QVERIFY(!intent.indeterminate);
        QCOMPARE(intent.requirements.size(), 2);
        QCOMPARE(intent.requirements.first().resourceHint, root);
        QCOMPARE(intent.requirements.last().resourceHint, outside);
    }
    void executionRequiresExactRequestedCommand() {
        ObservationIntent intent;
        intent.requirements.append({ObservationDomain::ProcessExecution, "python3 test_calc.py",
                                    EvidenceFreshness::TurnScoped, ObservationPurpose::Operate});
        EvidenceRecord receipt;
        receipt.domain = ObservationDomain::ProcessExecution;
        receipt.scope = EvidenceScope::Operation;
        receipt.outcome = EvidenceOutcome::Verified;
        receipt.resource = "python3 TEST_CALC.py";
        QVERIFY(
            !EvidencePolicy::evaluate(intent, {receipt}, GroundingMode::Verified, true).accepted);
        receipt.resource = "python3 test_calc.py";
        QVERIFY(
            EvidencePolicy::evaluate(intent, {receipt}, GroundingMode::Verified, true).accepted);
    }
    void classifierUsesRegisteredEvidenceContract() {
        FixedReplyProvider provider;
        provider.reply =
            R"({"requirements":[{"domain":"system","resource":"/fixture","purpose":"inspect","tool":"git-diff"},{"domain":"execution","resource":"","query":"python3 test_calc.py","purpose":"inspect","tool":"run-command"}]})";
        ObservationIntentPolicy policy(&provider);
        const auto intent = policy.classify("Run test and inspect git-diff", {},
                                            BuiltInToolProvider::descriptors());
        QVERIFY(!intent.indeterminate);
        QCOMPARE(intent.requirements.size(), 2);
        QCOMPARE(intent.requirements.first().domain, ObservationDomain::Workspace);
        QCOMPARE(intent.requirements.last().purpose, ObservationPurpose::Operate);
        QCOMPARE(intent.requirements.last().resourceHint, QStringLiteral("python3 test_calc.py"));
    }
    void exactRequestedToolCannotBeSubstituted() {
        ObservationIntent intent;
        ObservationRequirement requirement{ObservationDomain::Workspace, "/fixture"};
        requirement.toolId = "git-diff";
        intent.requirements.append(requirement);
        EvidenceRecord evidence;
        evidence.domain = ObservationDomain::Workspace;
        evidence.resource = "/fixture";
        evidence.outcome = EvidenceOutcome::Verified;
        evidence.scope = EvidenceScope::ExactResource;
        evidence.toolId = "git-status";
        QVERIFY(
            !EvidencePolicy::evaluate(intent, {evidence}, GroundingMode::Verified, true).accepted);
        evidence.toolId = "git-diff";
        QVERIFY(
            EvidencePolicy::evaluate(intent, {evidence}, GroundingMode::Verified, true).accepted);
    }
    void classifierPreservesBoundedTaskRequirements_data() {
        QTest::addColumn<int>("count");
        QTest::addColumn<QString>("tool");
        QTest::addColumn<bool>("valid");
        QTest::newRow("five-requirements") << 5 << "git-diff" << true;
        QTest::newRow("eight-requirements") << 8 << "git-diff" << true;
        QTest::newRow("nine-rejected") << 9 << "git-diff" << false;
        QTest::newRow("unknown-tool-rejected") << 1 << "invented-tool" << false;
    }
    void classifierPreservesBoundedTaskRequirements() {
        QFETCH(int, count);
        QFETCH(QString, tool);
        QFETCH(bool, valid);
        auto provider = std::make_shared<FixedReplyProvider>();
        QJsonArray requirements;
        for (int i = 0; i < count; ++i)
            requirements.append(QJsonObject{{"domain", "workspace"},
                                            {"resource", "/fixture"},
                                            {"purpose", "inspect"},
                                            {"operation", "none"},
                                            {"tool", tool}});
        provider->reply =
            QString::fromUtf8(QJsonDocument(QJsonObject{{"requirements", requirements}})
                                  .toJson(QJsonDocument::Compact));
        ObservationIntentPolicy policy(provider.get());
        const auto intent =
            policy.classify("Inspect requested diff", {}, BuiltInToolProvider::descriptors());
        QCOMPARE(!intent.indeterminate, valid);
        if (valid) {
            QCOMPARE(intent.requirements.size(), count);
            QCOMPARE(intent.requirements.first().toolId, tool);
        } else
            QVERIFY(intent.requirements.isEmpty());
    }

    void optInLiveClassification() {
        const auto model = qEnvironmentVariable("SENTINEL_LIVE_CLASSIFIER_MODEL");
        if (model.isEmpty())
            QSKIP("Explicit opt-in required for production local-provider probe");
        ModelService service;
        ModelBinding binding;
        binding.providerId = "lm-studio";
        binding.modelId = model;
        binding.capabilities.contextWindow = 8192;
        auto provider = service.constructProvider(binding);
        QVERIFY(provider);
        ObservationIntentPolicy policy(provider.get());
        const auto intent = policy.classify(
            "Read calc.cpp in the active workspace and explain the actual behavior of add. "
            "Do not modify files or run commands.",
            "\nACTIVE WORKSPACE ROOT: /tmp/sentinel-v325-fixture",
            BuiltInToolProvider::descriptors());
        qInfo() << "Live classification indeterminate:" << intent.indeterminate
                << "requirements:" << intent.requirements.size();
        QVERIFY2(!intent.indeterminate, qPrintable(intent.error));
        QVERIFY(!intent.requirements.isEmpty());
        QCOMPARE(intent.requirements.first().claimType, ClaimType::None);
    }

    void explanationReferences_data() {
        QTest::addColumn<QString>("callId");
        QTest::addColumn<QString>("quote");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("observed") << "run:1" << "return a - b;" << true;
        QTest::newRow("invented-call") << "other-run:1" << "return a - b;" << false;
        QTest::newRow("invented-quote") << "run:1" << "return a + b;" << false;
        QTest::newRow("empty") << "run:1" << "" << false;
        QTest::newRow("double-escaped-newline") << "run:1" << "}\\n// literal" << true;
        QTest::newRow("escaped-fabrication") << "run:1" << "}\\nUNOBSERVED" << false;
        QTest::newRow("literal-backslash-preferred") << "run:1" << "// literal \\n" << true;
    }
    void explanationReferences() {
        QFETCH(QString, callId);
        QFETCH(QString, quote);
        QFETCH(bool, accepted);
        auto observation = std::make_shared<StructuredObservation>();
        observation->kind = StructuredObservationKind::FileContent;
        observation->data = {{"path", "/fixture/calc.cpp"},
                             {"content", "int add(int a,int b) { return a - b; }\n// literal \\n"}};
        EvidenceRecord evidence;
        evidence.domain = ObservationDomain::FileSystem;
        evidence.outcome = EvidenceOutcome::Verified;
        evidence.toolCallId = "run:1";
        evidence.resource = "/fixture/calc.cpp";
        evidence.stepIndex = 1;
        evidence.structuredObservation = observation;
        const auto proposed = QString::fromUtf8(
            QJsonDocument(
                QJsonObject{
                    {"explanations",
                     QJsonArray{QJsonObject{
                         {"interpretation", "add subtracts its second argument from the first."},
                         {"evidence",
                          QJsonArray{QJsonObject{{"call_id", callId}, {"quote", quote}}}}}}}})
                .toJson(QJsonDocument::Compact));
        const auto result = ClaimGroundingResolver::filesystemExplanation({}, {evidence}, proposed);
        QCOMPARE(result.has_value(), accepted);
        if (result) {
            QVERIFY(result->rendered.contains("add subtracts"));
            QVERIFY(result->rendered.contains("/fixture/calc.cpp"));
            const auto roundtrip =
                ClaimGroundingResolver::filesystemExplanation({}, {evidence}, result->canonical);
            QVERIFY(roundtrip);
            QCOMPARE(roundtrip->canonical, result->canonical);
            auto later = evidence;
            later.stepIndex = 2;
            later.toolCallId = "run:2";
            QVERIFY(
                !ClaimGroundingResolver::filesystemExplanation({}, {evidence, later}, proposed));
            auto denied = evidence;
            denied.outcome = EvidenceOutcome::Denied;
            QVERIFY(!ClaimGroundingResolver::filesystemExplanation({}, {denied}, proposed));
        }
    }
    void codingFinalUsesActualExecutionReceipt() {
        ObservationIntent intent;
        intent.requirements.append({ObservationDomain::FileSystem,
                                    QStringLiteral("/fixture/calc.py"),
                                    EvidenceFreshness::TurnScoped, ObservationPurpose::Operate});
        EvidenceRecord operation;
        operation.toolId = QStringLiteral("edit-file");
        operation.toolCallId = QStringLiteral("edit:1");
        operation.domain = ObservationDomain::FileSystem;
        operation.resource = QStringLiteral("/fixture/calc.py");
        operation.scope = EvidenceScope::Operation;
        operation.outcome = EvidenceOutcome::Verified;
        EvidenceRecord execution;
        execution.toolId = QStringLiteral("run-command");
        execution.toolCallId = QStringLiteral("exec:1");
        execution.domain = ObservationDomain::ProcessExecution;
        execution.resource = QStringLiteral("python3 test_calc.py");
        execution.outcome = EvidenceOutcome::Verified;
        auto receipt = std::make_shared<StructuredObservation>();
        receipt->kind = StructuredObservationKind::Generic;
        receipt->data = {{QStringLiteral("exitCode"), 0},
                         {QStringLiteral("stdout"), QStringLiteral("TEST_OK")},
                         {QStringLiteral("stderr"), QString()},
                         {QStringLiteral("complete"), true}};
        execution.structuredObservation = receipt;
        const auto final = ClaimGroundingResolver::filesystemFinalAnswer(
            intent, {operation, execution}, QStringLiteral("FABRICATED_ASSERTION"));
        QVERIFY(final);
        QVERIFY(final->contains(QStringLiteral("TEST_OK")));
        QVERIFY(!final->contains(QStringLiteral("FABRICATED_ASSERTION")));
        auto json = QJsonDocument::fromJson(final->toUtf8()).object();
        QCOMPARE(json.value(QStringLiteral("executions"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("exit_code"))
                     .toInt(),
                 0);
        auto failedReceipt = std::make_shared<StructuredObservation>(*receipt);
        failedReceipt->data.insert(QStringLiteral("exitCode"), 1);
        execution.outcome = EvidenceOutcome::Failed;
        execution.structuredObservation = failedReceipt;
        const auto failed = ClaimGroundingResolver::filesystemFinalAnswer(
            intent, {operation, execution}, QStringLiteral("ALL TESTS PASSED"));
        QVERIFY(failed);
        json = QJsonDocument::fromJson(failed->toUtf8()).object();
        QCOMPARE(json.value(QStringLiteral("executions"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("exit_code"))
                     .toInt(),
                 1);
        QVERIFY(!failed->contains(QStringLiteral("ALL TESTS PASSED")));
    }

    void relativeReadEvidenceUsesAuthoritativeResolvedPath() {
        auto tools = BuiltInToolProvider::descriptors();
        const auto descriptor = *std::find_if(tools.begin(), tools.end(), [](const auto& t) {
            return t.id == QStringLiteral("read-file");
        });
        QTemporaryDir workspace;
        const auto path = workspace.filePath(QStringLiteral("math.h"));
        PlannedToolInvocation call;
        call.toolId = descriptor.id;
        call.arguments.append(
            {QStringLiteral("path"), QStringLiteral("math.h"), QStringLiteral("math.h")});
        auto observation = std::make_shared<const StructuredObservation>(StructuredObservation{
            StructuredObservationKind::FileContent,
            {{QStringLiteral("path"), path},
             {QStringLiteral("content"), QStringLiteral("int add(int a,int b); ")}}});
        const auto records = EvidencePolicy::record(
            descriptor, call, ToolExecutionStatus::Succeeded, QStringLiteral("read completed"), 1,
            QStringLiteral("call"), observation);
        QVERIFY(!records.isEmpty());
        QCOMPARE(records.first().resource, path);
        auto external = descriptor;
        external.source = ToolSource::Plugin;
        const auto untrusted =
            EvidencePolicy::record(external, call, ToolExecutionStatus::Succeeded, {}, 1,
                                   QStringLiteral("other"), observation);
        QCOMPARE(untrusted.first().resource, QStringLiteral("math.h"));
    }
    void coverageKeepsExplicitScopeAndUsesOnlyDeclaredFallbacks() {
        StructuredObservation observation;
        observation.kind = StructuredObservationKind::DirectoryListing;
        observation.data = {{"scope", "/observed"}, {"root", "/other"}, {"path", "/third"}};
        QCOMPARE(FileSystemCoverage::fromObservation(observation).scope, QString("/observed"));
        observation.data.remove("scope");
        QCOMPARE(FileSystemCoverage::fromObservation(observation).scope, QString("/other"));
        observation.data.remove("root");
        QCOMPARE(FileSystemCoverage::fromObservation(observation).scope, QString("/third"));
        observation.data.remove("path");
        QVERIFY(FileSystemCoverage::fromObservation(observation).scope.isEmpty());
    }
    void filesystemAbsenceMatrix_data() {
        QTest::addColumn<QString>("condition");
        QTest::addColumn<int>("expected");
        using V = ClaimVerdict;
        for (const auto& name : {"A-excluded", "D-unknown", "E-truncated", "F-cancelled",
                                 "G-permission", "H-nonrecursive", "I-subdirectory", "N-symlink",
                                 "N-unknown-symlink", "N-root-skipped-symlink"})
            QTest::newRow(name) << QString(name) << static_cast<int>(V::Unknown);
        QTest::newRow("B-present") << QString("B-present") << static_cast<int>(V::Contradicted);
        QTest::newRow("C-empty-complete")
            << QString("C-empty-complete") << static_cast<int>(V::Supported);
        QTest::newRow("J-positive-partial")
            << QString("J-positive-partial") << static_cast<int>(V::Supported);
        QTest::newRow("K-recursive-complete")
            << QString("K-recursive-complete") << static_cast<int>(V::Supported);
        QTest::newRow("L-hidden-directory")
            << QString("L-hidden-directory") << static_cast<int>(V::Contradicted);
        QTest::newRow("M-env-excluded")
            << QString("M-env-excluded") << static_cast<int>(V::Unknown);
        QTest::newRow("M-env-complete")
            << QString("M-env-complete") << static_cast<int>(V::Supported);
    }
    void filesystemAbsenceMatrix() {
        QFETCH(QString, condition);
        QFETCH(int, expected);
        ObservationRequirement claim;
        claim.claimType = ClaimType::HiddenEntriesExist;
        claim.claimId = "hidden";
        claim.resourceHint = "/workspace";
        claim.recursive = true;
        auto observation = std::make_shared<StructuredObservation>();
        observation->kind = StructuredObservationKind::PathMatches;
        observation->data = {{"scope", "/workspace"},      {"root", "/workspace"},
                             {"complete", true},           {"hiddenEntriesPolicy", "included"},
                             {"truncated", false},         {"cancelled", false},
                             {"permissionLimited", false}, {"issues", QJsonArray{}},
                             {"recursive", true},          {"maxDepth", 128},
                             {"resultLimit", 100},         {"followSymlinks", false},
                             {"skippedSymlinks", 0},       {"hiddenEntries", QJsonArray{}},
                             {"matches", QJsonArray{}},    {"pattern", "*"}};
        auto& d = observation->data;
        if (condition == "A-excluded") {
            d["hiddenEntriesPolicy"] = "excluded";
        }
        if (condition == "D-unknown") {
            d.remove("hiddenEntriesPolicy");
        }
        if (condition == "E-truncated") {
            d["truncated"] = true;
        }
        if (condition == "F-cancelled") {
            d["cancelled"] = true;
        }
        if (condition == "G-permission") {
            d["permissionLimited"] = true;
        }
        if (condition == "H-nonrecursive") {
            d["recursive"] = false;
        }
        if (condition == "I-subdirectory") {
            d["scope"] = "/workspace/src";
            d["root"] = "/workspace/src";
        }
        if (condition == "N-unknown-symlink") {
            d.remove("followSymlinks");
        }
        if (condition == "N-root-skipped-symlink") {
            claim.recursive = false;
            d["skippedSymlinks"] = 1;
        }
        if (condition == "N-symlink") {
            d["skippedSymlinks"] = 1;
        }
        if (condition == "B-present") {
            d["hiddenEntries"] = QJsonArray{"/workspace/.hidden.txt"};
        }
        if (condition == "L-hidden-directory") {
            d["hiddenEntries"] = QJsonArray{"/workspace/.hidden-dir"};
        }
        bool asserted = false;
        if (condition == "J-positive-partial") {
            observation->kind = StructuredObservationKind::DirectoryListing;
            d["path"] = "/workspace";
            d["complete"] = false;
            d["truncated"] = true;
            d["entries"] = QJsonArray{QJsonObject{{"name", "visible.txt"}, {"type", "file"}}};
            claim.claimType = ClaimType::FileExists;
            claim.resourceHint = "/workspace/visible.txt";
            asserted = true;
        }
        if (condition.startsWith("M-")) {
            claim.claimType = ClaimType::PathPatternExists;
            claim.claimQuery = ".env";
            d["pattern"] = ".env";
            if (condition == "M-env-excluded")
                d["hiddenEntriesPolicy"] = "excluded";
        }
        EvidenceRecord evidence;
        evidence.domain = ObservationDomain::FileSystem;
        evidence.outcome = EvidenceOutcome::Partial;
        evidence.structuredObservation = observation;
        evidence.toolCallId = "actual-observation";
        QCOMPARE(static_cast<int>(
                     ClaimGroundingResolver::resolve(claim, {evidence}, {claim.claimId, asserted})
                         .verdict),
                 expected);
    }
    void realTraversalReportsHiddenDirectoriesAndSkippedSymlinks() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QDir root(workspace.path());
        QVERIFY(root.mkpath(".hidden-dir"));
        QFile file(root.filePath(".hidden-dir/nested.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("fixture");
        file.close();
        QtFileSystemService service;
        const auto allowed = service.resolve(root.path(), root.path(), FileSystemAccess::Read);
        QVERIFY(allowed.ok());
        const auto excluded =
            service.traverseFiles(*allowed.value, false, 5000, [](const QString&) { return true; });
        QVERIFY(excluded.ok());
        QVERIFY(excluded.value->files.isEmpty());
        const auto included =
            service.traverseFiles(*allowed.value, true, 5000, [](const QString&) { return true; });
        QVERIFY(included.ok());
        QVERIFY(included.value->status.complete);
        QCOMPARE(included.value->files.size(), 1);
        QFile visible(root.filePath("visible.txt"));
        QVERIFY(visible.open(QIODevice::WriteOnly));
        visible.write("fixture");
        visible.close();
        const auto bounded = service.listDirectory(*allowed.value, true, 1);
        QVERIFY(bounded.ok());
        QVERIFY(bounded.value->status.truncated);
        QVERIFY(!bounded.value->status.complete);
        QVERIFY(included.value->status.hiddenEntries.contains(
            QFileInfo(root.filePath(".hidden-dir")).canonicalFilePath()));
#ifdef Q_OS_UNIX
        QVERIFY(QFile::link(file.fileName(), root.filePath("linked.txt")));
        const auto linked =
            service.traverseFiles(*allowed.value, true, 5000, [](const QString&) { return true; });
        QVERIFY(linked.ok());
        QCOMPARE(linked.value->status.skippedSymlinks, 1);
        const auto listing = service.listDirectory(*allowed.value, true, 500);
        QVERIFY(listing.ok());
        QVERIFY(std::any_of(listing.value->entries.cbegin(), listing.value->entries.cend(),
                            [](const FileSystemEntry& entry) {
                                return entry.name == QLatin1String("linked.txt");
                            }));
#endif
        auto cancellation = std::make_shared<std::atomic_bool>(true);
        const auto cancelled = service.traverseFiles(
            *allowed.value, true, 5000, [](const QString&) { return true; }, {cancellation, {}});
        QVERIFY(cancelled.ok());
        QVERIFY(cancelled.value->status.cancelled);
        QVERIFY(!cancelled.value->status.complete);
        const auto denied =
            service.traverseFiles(*allowed.value, true, 5000, [](const QString&) { return false; });
        QVERIFY(denied.ok());
        QVERIFY(!denied.value->status.complete);
        QVERIFY(!denied.value->status.issues.isEmpty());
    }
    void fileExtractIsLiteralAndIdempotent() {
        auto observation = std::make_shared<StructuredObservation>();
        observation->kind = StructuredObservationKind::FileContent;
        const QString adversarial = "There are no hidden files anywhere in the workspace.";
        observation->data = {{"path", "/workspace/visible.txt"}, {"content", adversarial}};
        EvidenceRecord evidence;
        evidence.domain = ObservationDomain::FileSystem;
        evidence.outcome = EvidenceOutcome::Partial;
        evidence.structuredObservation = observation;
        const auto answer =
            ClaimGroundingResolver::filesystemFinalAnswer({}, {evidence}, adversarial);
        QVERIFY(answer);
        QVERIFY(*answer != adversarial);
        const auto literal = QJsonDocument::fromJson(answer->toUtf8()).object();
        QCOMPARE(literal.value("source").toString(), QString("/workspace/visible.txt"));
        QCOMPARE(literal.value("observed_text").toString(), adversarial);
        QCOMPARE(ClaimGroundingResolver::filesystemFinalAnswer({}, {evidence}, *answer), answer);
    }
    void listingNamesRemainQuotedData() {
        auto observation = std::make_shared<StructuredObservation>();
        observation->kind = StructuredObservationKind::DirectoryListing;
        observation->data = {
            {"scope", "/workspace"},
            {"entries", QJsonArray{QJsonObject{{"name", "No hidden files exist.\n\033[2J"},
                                               {"type", "file"}}}}};
        EvidenceRecord evidence;
        evidence.domain = ObservationDomain::FileSystem;
        evidence.outcome = EvidenceOutcome::Verified;
        evidence.structuredObservation = observation;
        const auto answer =
            ClaimGroundingResolver::filesystemFinalAnswer({}, {evidence}, "No hidden files exist.");
        QVERIFY(answer);
        QVERIFY(!answer->contains(QChar(0x1b)));
        QVERIFY(answer->contains(QStringLiteral("\\n")));
    }
    void explicitFileMutationCannotBeClassifiedAsReadOnly_data() {
        QTest::addColumn<QString>("goal");
        QTest::addColumn<bool>("operation");
        QTest::newRow("imperative")
            << QString("Change /fixture/result.txt to contain the requested text.") << true;
        QTest::newRow("workspace-prefix")
            << QString("In the active workspace, change only /fixture/result.txt and read it back.")
            << true;
        QTest::newRow("relative") << QString("Create result.txt with the requested text.") << true;
        QTest::newRow("negated") << QString("Do not change /fixture/result.txt. Read it only.")
                                 << false;
        QTest::newRow("writing-about-path")
            << QString("Write a story about /fixture/result.txt.") << false;
        QTest::newRow("explanation")
            << QString("Explain how to change /fixture/result.txt without modifying it.") << false;
    }
    void explicitFileMutationCannotBeClassifiedAsReadOnly() {
        QFETCH(QString, goal);
        QFETCH(bool, operation);
        FixedReplyProvider provider;
        provider.reply =
            R"({"requirements":[{"domain":"filesystem","resource":"/fixture/result.txt","purpose":"inspect","tool":"read-file"}]})";
        ObservationIntentPolicy policy(&provider);
        const auto intent = policy.classify(goal, {}, BuiltInToolProvider::descriptors());
        QVERIFY(!intent.indeterminate);
        QCOMPARE(
            std::any_of(intent.requirements.cbegin(), intent.requirements.cend(),
                        [](const auto& r) { return r.purpose == ObservationPurpose::Operate; }),
            operation);
    }
    void localClassificationHasNoDeadlineAndRemainsCancellable() {
        for (const auto* id : {"lm-studio", "ollama", "llama-cpp-server", "openai-compatible-local",
                               "cloud-api", "unknown"}) {
            FixedReplyProvider provider;
            auto token = std::make_shared<std::atomic_bool>(false);
            provider.onRequest = [&](const auto&, const auto&) {
                token->store(true);
                ChatProviderReply reply;
                reply.category = ChatProviderErrorCategory::Cancelled;
                reply.errorMessage = "Cancelled";
                return reply;
            };
            ObservationIntentPolicy policy(&provider, token, {id, "fixture"}, [] { return 0; });
            const auto intent = policy.classify("List the workspace", {}, {});
            QCOMPARE(provider.calls, 1);
            QCOMPARE(provider.request.deadlineMs,
                     QString(id) == "cloud-api" || QString(id) == "unknown" ? 60000 : 0);
            QCOMPARE(provider.request.cancellationToken, token);
            QCOMPARE(intent.error, QString("Cancelled"));
        }
    }
    void classificationRetriesOnceAfterTypedTruncation() {
        FixedReplyProvider provider;
        provider.failures = 1;
        provider.reply = QStringLiteral("{\"requirements\":[]}");
        ObservationIntentPolicy policy(&provider);
        const auto intent = policy.classify("List the workspace", {}, {});
        QCOMPARE(provider.calls, 2);
        QVERIFY(!intent.indeterminate);
        QVERIFY(intent.requirements.isEmpty());
        QCOMPARE(intent.requestDiagnostics.size(), 2);
        QCOMPARE(intent.requestDiagnostics.at(0).toObject().value("trace_id"),
                 intent.requestDiagnostics.at(1).toObject().value("trace_id"));
        QCOMPARE(intent.requestDiagnostics.at(1).toObject().value("output_budget_tokens").toInt(),
                 4096);
    }
    void classificationStopsAfterTheSingleRetry() {
        FixedReplyProvider provider;
        provider.failures = 5;
        ObservationIntentPolicy policy(&provider);
        const auto intent = policy.classify("List the workspace", {}, {});
        QCOMPARE(provider.calls, 2);
        QVERIFY(intent.indeterminate);
        QCOMPARE(intent.failure, ObservationClassificationFailure::OutputTruncated);
    }
    void cancelledClassificationNeverRetries() {
        FixedReplyProvider provider;
        provider.failures = 5;
        auto token = std::make_shared<std::atomic_bool>(true);
        ObservationIntentPolicy policy(&provider, token);
        const auto intent = policy.classify("List the workspace", {}, {});
        QCOMPARE(provider.calls, 0);
        QVERIFY(intent.indeterminate);
        QCOMPARE(intent.error, QStringLiteral("Cancelled"));
    }
    void classifierPropagatesCancellationWithoutOtherPayload() {
        FixedReplyProvider provider;
        auto token = std::make_shared<std::atomic_bool>(true);
        ObservationIntentPolicy policy(&provider, token);
        const auto intent = policy.classify("List the workspace", {}, {});
        QCOMPARE(provider.calls, 0);
        QVERIFY(provider.prompt.isEmpty());
        QVERIFY(intent.indeterminate);
        QCOMPARE(intent.error, QString("Cancelled"));
    }

    void classifierPreservesWorkspaceAfterLongConversation() {
        FixedReplyProvider provider;
        provider.reply = QStringLiteral("{\"requirements\":[]}");
        ObservationIntentPolicy policy(&provider);
        policy.classify(
            "Explain calc.cpp",
            QString(4000, QLatin1Char('x')) + "\nACTIVE WORKSPACE ROOT: /fixture/current", {});
        QVERIFY(provider.prompt.contains("ACTIVE WORKSPACE ROOT: /fixture/current"));
        QVERIFY(provider.prompt.contains("operation:none, never exists"));
        QVERIFY(provider.prompt.size() < 2500);
    }

    void responseContract_data() {
        QTest::addColumn<QString>("reply");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("bare") << QStringLiteral("{\"requirements\":[]}") << true;
        QTest::newRow("whole-fence")
            << QStringLiteral("```json\n{\"requirements\":[]}\n```") << true;
        QTest::newRow("prose") << QStringLiteral("Result: {\"requirements\":[]}") << false;
        QTest::newRow("empty") << QString{} << false;
        QTest::newRow("reasoning-only")
            << QStringLiteral("<think>Need to inspect the workspace</think>") << false;
        QTest::newRow("malformed") << QStringLiteral("{\"requirements\":[}") << false;
        QTest::newRow("wrong-schema") << QStringLiteral("{\"intent\":\"filesystem\"}") << false;
        QTest::newRow("invalid-domain")
            << QStringLiteral("{\"requirements\":[{\"domain\":\"invented\"}]}") << false;
    }

    void responseContract() {
        QFETCH(QString, reply);
        QFETCH(bool, accepted);
        FixedReplyProvider provider;
        provider.reply = reply;
        ObservationIntentPolicy policy(&provider);
        const auto intent = policy.classify(QStringLiteral("List the workspace"), {}, {});
        QCOMPARE(!intent.indeterminate, accepted);
        QVERIFY(intent.requirements.isEmpty());
    }

    void acceptsSingleFencedJsonObject() {
        FixedReplyProvider provider;
        provider.reply = QStringLiteral("```json\n{\"requirements\":[{\"domain\":\"filesystem\","
                                        "\"resource\":\"\",\"purpose\":\"inspect\","
                                        "\"operation\":\"none\",\"query\":\"\"}]}\n```");
        ObservationIntentPolicy policy(&provider);

        const auto intent = policy.classify(QStringLiteral("List the workspace"), {}, {});

        QVERIFY(!intent.indeterminate);
        QCOMPARE(intent.requirements.size(), 1);
        QCOMPARE(intent.requirements.first().domain, ObservationDomain::FileSystem);
        QCOMPARE(intent.requirements.first().purpose, ObservationPurpose::Inspect);
    }

    void rejectsFencedJsonWithSurroundingProse() {
        FixedReplyProvider provider;
        provider.reply = QStringLiteral("Here is the result:\n```json\n{\"requirements\":[]}\n```");
        ObservationIntentPolicy policy(&provider);

        const auto intent = policy.classify(QStringLiteral("List the workspace"), {}, {});

        QVERIFY(intent.indeterminate);
    }
};

QTEST_GUILESS_MAIN(ObservationPolicyTest)

#include "test_observation_policy.moc"
