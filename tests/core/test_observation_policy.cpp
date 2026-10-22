// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include "sentinel/core/agent/ClaimGroundingResolver.h"
#include "sentinel/core/agent/ObservationPolicy.h"
#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/platform/UserDirectories.h"
#include "sentinel/core/runtime/BuiltInToolProvider.h"
#include "sentinel/core/runtime/IFileSystemService.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
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
        return {true, reply, {}};
    }
    ChatProviderReply sendRequest(const QString& message,
                                  const ChatRequestOptions& options) override {
        request = options;
        prompt = message;
        if (options.cancellationToken && options.cancellationToken->load()) {
            ChatProviderReply result;
            result.lifecycle = ChatRequestLifecycle::Cancelled;
            result.category = ChatProviderErrorCategory::Cancelled;
            result.errorMessage = QStringLiteral("Cancelled");
            return result;
        }
        return sendMessage(message);
    }

    QString reply;
    QString prompt;
    ChatRequestOptions request;
};

} // namespace

class ObservationPolicyTest final : public QObject {
    Q_OBJECT

private slots:
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
    void classifierPropagatesCancellationWithoutOtherPayload() {
        FixedReplyProvider provider;
        auto token = std::make_shared<std::atomic_bool>(true);
        ObservationIntentPolicy policy(&provider, token);
        const auto intent = policy.classify(QStringLiteral("List the workspace"), {}, {});
        QCOMPARE(provider.request.cancellationToken, token);
        QCOMPARE(provider.request.deadlineMs, 60000);
        QCOMPARE(provider.request.maxOutputTokens, 2048);
        QVERIFY(intent.indeterminate);
        QCOMPARE(intent.error, QStringLiteral("Cancelled"));
        QVERIFY(provider.request.tools.isEmpty());
        QVERIFY(!provider.request.nativeToolCalling);
        QVERIFY(!provider.request.structuredOutput);
        QVERIFY(provider.request.priorToolCalls.isEmpty());
        QVERIFY(provider.request.toolResults.isEmpty());
        QVERIFY(provider.prompt.contains(QStringLiteral("Return ONLY JSON")));
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
