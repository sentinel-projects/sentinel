// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include "sentinel/core/agent/AgentLoop.h"
#include "sentinel/core/agent/ClaimGroundingResolver.h"
#include "sentinel/core/agent/ObservationPolicy.h"
#include "sentinel/core/runtime/BuiltInToolProvider.h"
#include "sentinel/core/runtime/IToolExecutor.h"
#include "sentinel/core/runtime/InMemoryToolRegistry.h"
#include "sentinel/core/runtime/RealToolExecutor.h"
#include <algorithm>
#include "sentinel/core/security/StaticApprovalPolicy.h"
#include "sentinel/core/security/StaticSandboxPolicy.h"
#include <QFile>

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <algorithm>
#include <functional>

using namespace sentinel::core;

namespace {
class CallbackIntentPolicy final : public IObservationIntentPolicy {
public:
    std::function<void()> onClassify;
    ObservationIntent classify(const QString&, const QString&,
                               const QList<ToolDescriptor>&) const override {
        onClassify();
        return {};
    }
};

class FixedFilesystemIntent final : public IObservationIntentPolicy {
public:
    ObservationIntent intent;
    ObservationIntent classify(const QString&, const QString&,
                               const QList<ToolDescriptor>&) const override {
        return intent;
    }
};

class ScriptedPlanner final : public IAgentStepPlanner {
public:
    QVector<AgentStepDecision> decisions;
    mutable QVector<QList<AgentStepRecord>> observedHistories;
    mutable int calls = 0;
    mutable QStringList feedbacks;
    bool repeatLast = false;
    bool canonicalFinal = false;
    QList<EvidenceRecord> evidence;
    void setEvidence(const QList<EvidenceRecord>& value) override {
        evidence = value;
    }
    void setPlannerFeedback(const QString& feedback) override {
        feedbacks.append(feedback);
    }
    std::function<void(int)> onCall;

    AgentStepDecision nextStep(const QString& goal,
                               const QList<AgentStepRecord>& history) const override {
        Q_UNUSED(goal);
        observedHistories.append(history);
        const int index = calls++;
        if (onCall) {
            onCall(index);
        }
        if (index < decisions.size()) {
            auto decision = decisions.at(index);
            if (canonicalFinal && decision.kind == AgentStepDecision::Kind::FinalAnswer) {
                ObservationIntent intent;
                intent.requirements = {{ObservationDomain::FileSystem,
                                        {},
                                        EvidenceFreshness::TurnScoped,
                                        ObservationPurpose::Operate}};
                if (auto answer = ClaimGroundingResolver::filesystemFinalAnswer(intent, evidence,
                                                                                decision.answer))
                    decision.answer = *answer;
            }
            return decision;
        }
        if (repeatLast && !decisions.isEmpty()) {
            return decisions.last();
        }
        AgentStepDecision fallback;
        fallback.kind = AgentStepDecision::Kind::FinalAnswer;
        fallback.answer = QStringLiteral("fallback final answer");
        return fallback;
    }
};

class RecordingExecutor final : public IToolExecutor {
public:
    mutable QVector<ToolExecutionRequest> requests;
    QString summary = QStringLiteral("observation-from-tool");
    ToolExecutionStatus status = ToolExecutionStatus::Succeeded;
    ToolFailureCategory failureCategory = ToolFailureCategory::None;

    ToolExecutionResult execute(const ToolExecutionRequest& request) const override {
        requests.append(request);
        ToolExecutionResult result{status, summary};
        result.failureCategory = failureCategory;
        return result;
    }
};

class RecordingHandler final : public IToolHandler {
public:
    QList<ToolExecutionRequest> requests;

    IToolExecutor::Cancel execute(const ToolExecutionRequest& request, const QString&,
                                  const QString&, IToolExecutor::Output,
                                  IToolExecutor::Completion completion) override {
        requests.append(request);
        completion({ToolExecutionStatus::Succeeded, QStringLiteral("observation-from-handler")});
        return {};
    }
};

// Deterministic execution fixture: real scratch-file writes/reads, controlled
// command receipts. The real-model suite independently runs the actual command.
class CodingFixtureApproval final : public IApprovalPolicy {
public:
    bool denyVerification = false;
    ApprovalDecision evaluate(const ToolInvocationPlan& plan) const override {
        if (denyVerification && plan.invocations.first().toolId == "git-diff")
            return {ApprovalStatus::Denied, "Fixture verification denied", {}};
        return {ApprovalStatus::Approved, "Explicitly approved isolated fixture", {}};
    }
};

class CodingTaskHandler final : public IToolHandler {
public:
    QString root;
    int exitCode = 0;
    bool denyReadAfterWrite = false;
    QStringList calls;
    IToolExecutor::Cancel execute(const ToolExecutionRequest& request, const QString&,
                                  const QString&, IToolExecutor::Output,
                                  IToolExecutor::Completion completion) override {
        const auto& invocation = request.plan.invocations.first();
        const auto id = invocation.toolId;
        calls.append(id);
        QString path;
        for (const auto& arg : invocation.arguments)
            if (arg.id == "path")
                path = QDir(root).absoluteFilePath(arg.value);
        path = QDir::cleanPath(path);
        auto observation = std::make_shared<StructuredObservation>();
        observation->data = {{"complete", true}, {"truncated", false}, {"cancelled", false}};
        ToolExecutionResult result{ToolExecutionStatus::Succeeded, "fixture success"};
        if (id == "write-file") {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) ||
                file.write("def add(a, b):\n    return a + b\n") < 0) {
                completion({ToolExecutionStatus::Failed, "fixture write failed"});
                return {};
            }
            result.mutations.append({path});
        } else if (id == "read-file") {
            if (denyReadAfterWrite && calls.contains("write-file")) {
                completion({ToolExecutionStatus::Blocked, "Fixture post-mutation read denied"});
                return {};
            }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                completion({ToolExecutionStatus::Failed, "fixture read failed"});
                return {};
            }
            observation->kind = StructuredObservationKind::FileContent;
            observation->data.insert("path", QFileInfo(path).canonicalFilePath());
            observation->data.insert("content", QString::fromUtf8(file.readAll()));
        } else {
            observation->kind = StructuredObservationKind::Generic;
            observation->data.insert("exitCode", id == "run-command" ? exitCode : 0);
            observation->data.insert("stdout", id == "run-command"
                                                   ? (exitCode ? "TEST_FAILED" : "ALL_TESTS_PASSED")
                                                   : "- return a - b\n+ return a + b");
            observation->data.insert("stderr", "");
            if (id == "run-command" && exitCode)
                result.status = ToolExecutionStatus::Failed;
        }
        result.structuredObservation = observation;
        completion(result);
        return {};
    }
};

AgentStepDecision toolDecision(const QString& toolId, const QString& argValue = QStringLiteral("x"),
                               ToolRiskLevel risk = ToolRiskLevel::Low) {
    AgentStepDecision decision;
    decision.kind = AgentStepDecision::Kind::ToolCall;
    decision.toolId = toolId;
    decision.toolName = toolId;
    decision.riskLevel = risk;
    decision.thought = QStringLiteral("use %1").arg(toolId);
    decision.arguments.append(ToolInvocationArgument{QStringLiteral("command"), argValue});
    return decision;
}

AgentStepDecision finalDecision(const QString& answer) {
    AgentStepDecision decision;
    decision.kind = AgentStepDecision::Kind::FinalAnswer;
    decision.answer = answer;
    return decision;
}

StaticSandboxPolicy permissiveSandbox() {
    return StaticSandboxPolicy(QSet<QString>{QStringLiteral("tool.metadata.read"),
                                             QStringLiteral("tool.risk.medium"),
                                             QStringLiteral("tool.risk.high")});
}

} // namespace

class AgentLoopTest final : public QObject {
    Q_OBJECT

private slots:
    void codingTaskFinalization_data() {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<bool>("async");
        for (const auto& scenario :
             {"complete", "test-fails", "repeated-read", "read-after-write", "missing-diff",
              "stale-after-write", "provider-extra-read", "cancel-final", "prose-final",
              "wrong-diff", "test-before-edit", "relative-write", "provider-extra-test",
              "recover-missing-diff", "cancel-recovery", "denied-recovery",
              "recover-missing-multiple", "stale-denied", "verification-budget"})
            for (bool async : {false, true})
                QTest::newRow(
                    qPrintable(QString::fromLatin1(scenario) + (async ? "-async" : "-sync")))
                    << QString::fromLatin1(scenario) << async;
    }

    void codingTaskFinalization() {
        QFETCH(QString, scenario);
        QFETCH(bool, async);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto root = QFileInfo(directory.path()).canonicalFilePath();
        const auto path = root + "/calc.py";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("def add(a, b):\n    return a - b\n");
        file.close();
        auto policy = std::make_shared<FixedFilesystemIntent>();
        policy->intent.requirements = {{ObservationDomain::FileSystem, path,
                                        EvidenceFreshness::TurnScoped, ObservationPurpose::Operate},
                                       {ObservationDomain::FileSystem, path,
                                        EvidenceFreshness::TurnScoped, ObservationPurpose::Inspect},
                                       {ObservationDomain::ProcessExecution, "python3 test_calc.py",
                                        EvidenceFreshness::TurnScoped, ObservationPurpose::Operate},
                                       {ObservationDomain::Workspace, root,
                                        EvidenceFreshness::TurnScoped,
                                        ObservationPurpose::Inspect}};
        policy->intent.requirements.last().toolId = "git-diff";
        policy->intent.requirements[1].toolId = "read-file";
        if (scenario == "recover-missing-multiple" || scenario == "verification-budget") {
            for (const auto& name : scenario == "verification-budget"
                                        ? QStringList{"test_calc.py", "extra_one.py"}
                                        : QStringList{"test_calc.py"}) {
                QFile observed(root + "/" + name);
                QVERIFY(observed.open(QIODevice::WriteOnly));
                observed.write("fixture verification\n");
                observed.close();
                ObservationRequirement required{ObservationDomain::FileSystem, root + "/" + name};
                required.toolId = "read-file";
                policy->intent.requirements.append(required);
            }
        }
        ScriptedPlanner planner;
        planner.canonicalFinal = scenario != "prose-final";
        auto read = toolDecision("read-file");
        read.arguments = {{"path", path}};
        auto write = toolDecision("write-file");
        write.arguments = {{"path", path}, {"content", "def add(a, b):\n    return a + b\n"}};
        if (scenario == "relative-write")
            write.arguments[0].value = "./calc.py";
        auto command = toolDecision("run-command", "python3 test_calc.py");
        auto diff = toolDecision(scenario == "wrong-diff" ? "git-status" : "git-diff");
        diff.arguments = {{"path", root}};
        auto final = finalDecision("All tests succeeded and the requested diff was verified.");
        final.grounding = GroundingMode::Verified;
        final.groundingDeclared = true;
        planner.decisions = scenario == "test-before-edit"
                                ? QVector<AgentStepDecision>{read, command, write}
                                : QVector<AgentStepDecision>{read, write, command};
        if (scenario != "stale-after-write" && scenario != "stale-denied" &&
            scenario != "recover-missing-multiple" && scenario != "verification-budget")
            planner.decisions.append(read);
        if (scenario != "missing-diff" && scenario != "recover-missing-diff" &&
            scenario != "cancel-recovery" && scenario != "denied-recovery" &&
            scenario != "recover-missing-multiple" && scenario != "verification-budget")
            planner.decisions.append(diff);
        if (scenario == "repeated-read" || scenario == "provider-extra-read") {
            auto alias = read;
            alias.arguments = {{"path", "./calc.py"}};
            planner.decisions.append(alias);
        }
        if (scenario == "provider-extra-test")
            planner.decisions.append(command);
        planner.decisions.append(final);
        planner.decisions.append(final);
        auto handler = std::make_shared<CodingTaskHandler>();
        handler->root = root;
        handler->denyReadAfterWrite = scenario == "stale-denied";
        handler->exitCode = scenario == "test-fails" ? 1 : 0;
        RealToolExecutor executor;
        InMemoryToolRegistry registry;
        InMemoryToolRegistry source;
        QVERIFY(BuiltInToolProvider::registerTools(source, executor));
        for (const auto& id :
             {"read-file", "write-file", "run-command", "git-diff", "git-status"}) {
            const auto descriptor = source.findToolById(id);
            QVERIFY(descriptor);
            QVERIFY(registry.registerTool(IToolRegistry::Registration{*descriptor, handler}));
        }
        CodingFixtureApproval approval;
        auto sandbox = permissiveSandbox();
        approval.denyVerification = scenario == "denied-recovery";
        QStringList knownTools{"read-file", "write-file", "run-command", "git-diff", "git-status"};
        if (scenario == "missing-diff" || scenario == "wrong-diff")
            knownTools.removeAll("git-diff");
        AgentLoop loop(planner, executor, approval, sandbox, knownTools);
        loop.setToolRegistry(&registry);
        loop.setResourceScope(root);
        loop.setObservationIntentPolicy(policy);
        AgentContextInput::WorkspaceContext workspace;
        workspace.rootPath = root;
        loop.setWorkspaceContext(workspace);
        bool cancel = false;
        planner.onCall = [&](int index) {
            if (scenario == "cancel-final" &&
                planner.decisions.at(qMin(index, int(planner.decisions.size()) - 1)).kind ==
                    AgentStepDecision::Kind::FinalAnswer)
                cancel = true;
        };
        loop.setCancelQuery([&] { return cancel; });
        loop.setStatusCallback([&](const QString& status) {
            if (scenario == "cancel-recovery" && status == "Planning requested final verification.")
                cancel = true;
        });
        AgentLoopState state;
        int terminals = 0;
        if (async) {
            loop.runAsync("Correct calc.py, run python3 test_calc.py, reread calc.py and inspect "
                          "git-diff before finalizing",
                          "coding-final", this, [&](const AgentLoopState& result) {
                              state = result;
                              ++terminals;
                          });
            QTRY_COMPARE(terminals, 1);
            QCoreApplication::processEvents();
            QCOMPARE(terminals, 1);
        } else {
            state = loop.run("Correct calc.py, run python3 test_calc.py, reread calc.py and "
                             "inspect git-diff before finalizing",
                             "coding-final");
        }
        const bool success = scenario == "complete" || scenario == "read-after-write" ||
                             scenario == "repeated-read" || scenario == "provider-extra-read" ||
                             scenario == "relative-write" || scenario == "provider-extra-test" ||
                             scenario == "recover-missing-diff" ||
                             scenario == "recover-missing-multiple" ||
                             scenario == "stale-after-write" || scenario == "prose-final";
        if (success) {
            QVERIFY2(state.phase == AgentLoopPhase::Completed, qPrintable(state.abortReason));
            QCOMPARE(handler->calls.count("run-command"), 1);
            QCOMPARE(handler->calls.count("read-file"),
                     scenario == "recover-missing-multiple" ? 3 : 2);
            QCOMPARE(handler->calls.count("git-diff"), 1);
            if (scenario == "prose-final") {
                QCOMPARE(
                    state.finalAnswer,
                    QStringLiteral("All tests succeeded and the requested diff was verified."));
            } else {
                QVERIFY(state.finalAnswer.contains("ALL_TESTS_PASSED"));
                QVERIFY(state.finalAnswer.contains("git-diff"));
                QVERIFY(state.finalAnswer.contains("return a + b"));
            }
            QVERIFY(
                std::any_of(state.evidence.cbegin(), state.evidence.cend(), [](const auto& item) {
                    return item.scope == EvidenceScope::Operation &&
                           item.outcome == EvidenceOutcome::Verified;
                }));
            if (scenario == "repeated-read" || scenario == "provider-extra-read")
                QCOMPARE(state.repeatedReadRepairs, 1);
        } else if (scenario == "cancel-final" || scenario == "cancel-recovery")
            QCOMPARE(state.phase, AgentLoopPhase::Cancelled);
        else {
            QVERIFY(state.phase != AgentLoopPhase::Completed);
            QVERIFY(state.finalAnswer.isEmpty());
        }
        if (scenario == "recover-missing-diff" || scenario == "cancel-recovery" ||
            scenario == "denied-recovery") {
            QCOMPARE(state.finalVerificationRecoveries, 1);
            QCOMPARE(handler->calls.count("git-diff"), scenario == "recover-missing-diff" ? 1 : 0);
        }
        QVERIFY(handler->calls.count("write-file") == 1);
        QVERIFY(handler->calls.count("run-command") == 1);
        if (scenario == "recover-missing-multiple")
            QCOMPARE(state.finalVerificationRecoveries, 3);
        if (scenario == "verification-budget")
            QCOMPARE(state.finalVerificationRecoveries, 0);
        if (scenario == "stale-after-write" || scenario == "stale-denied")
            QVERIFY(
                std::any_of(state.evidence.cbegin(), state.evidence.cend(), [](const auto& item) {
                    return item.toolId == "read-file" && item.outcome == EvidenceOutcome::Stale;
                }));
    }

    void citedExplanationCompletesThroughReadGateway() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto path = workspace.filePath("calc.cpp");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("int add(int a,int b) { return a - b; }");
        file.close();
        const auto observedPath = QFileInfo(path).canonicalFilePath();
        ScriptedPlanner planner;
        auto read = toolDecision("read-file");
        read.arguments = {{"path", path}};
        auto final = finalDecision(QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{"explanations", QJsonArray{QJsonObject{
                                                 {"interpretation", "add subtracts b from a."},
                                                 {"evidence", QJsonArray{QJsonObject{
                                                                  {"call_id", "observed"},
                                                                  {"source", observedPath},
                                                                  {"quote", "return a - b;"}}}}}}}})
                .toJson(QJsonDocument::Compact)));
        final.grounding = GroundingMode::Verified;
        final.groundingDeclared = true;
        planner.decisions = {read, final};
        RealToolExecutor executor;
        InMemoryToolRegistry registry;
        QVERIFY(BuiltInToolProvider::registerTools(registry, executor));
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        AgentLoop loop(planner, executor, approval, sandbox, {"read-file"});
        loop.setToolRegistry(&registry);
        loop.setResourceScope(workspace.path());
        loop.setToolCallIdProvider([](int) { return QStringLiteral("observed"); });
        AgentContextInput::WorkspaceContext context;
        context.rootPath = workspace.path();
        loop.setWorkspaceContext(context);
        const auto state = loop.run("Read calc.cpp and explain add", "fixture-explanation");
        QVERIFY2(state.phase == AgentLoopPhase::Completed, qPrintable(state.abortReason));
        QCOMPARE(planner.calls, 2);
        QCOMPARE(state.evidence.size(), 1);
        QVERIFY(state.finalAnswer.contains("Interpretation: add subtracts b from a."));
        QVERIFY(state.finalAnswer.contains("return a - b;"));
        QVERIFY(state.finalAnswer.contains(observedPath));
    }
    void indeterminateClassificationNeverPlans_data() {
        QTest::addColumn<bool>("async");
        QTest::newRow("sync") << false;
        QTest::newRow("async") << true;
    }
    void indeterminateClassificationNeverPlans() {
        QFETCH(bool, async);
        ScriptedPlanner planner;
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        AgentLoop loop(planner, executor, approval, sandbox, QStringList{});
        auto policy = std::make_shared<FixedFilesystemIntent>();
        policy->intent.indeterminate = true;
        policy->intent.error =
            QStringLiteral("Client deadline expired; upstream cancellation unconfirmed.");
        loop.setObservationIntentPolicy(policy);
        AgentLoopState result;
        int terminals = 0;
        if (async) {
            loop.runAsync(QStringLiteral("Inspect workspace"), QStringLiteral("bounded"), this,
                          [&](const AgentLoopState& state) {
                              result = state;
                              ++terminals;
                          });
            QTest::qWait(25);
            QCOMPARE(terminals, 1);
        } else {
            result = loop.run(QStringLiteral("Inspect workspace"), QStringLiteral("bounded"));
        }
        QCOMPARE(result.phase, AgentLoopPhase::Failed);
        QVERIFY(result.finalAnswer.isEmpty());
        QVERIFY(result.abortReason.contains(QStringLiteral("deadline")));
        QCOMPARE(planner.calls, 0);
        QVERIFY(executor.requests.isEmpty());
    }
    void classifiedObservationDoesNotCompleteWithoutEvidence() {
        ScriptedPlanner planner;
        auto final = finalDecision("Invented file explanation");
        final.grounding = GroundingMode::Verified;
        final.groundingDeclared = true;
        planner.decisions = {final};
        planner.repeatLast = true;
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        AgentLoop loop(planner, executor, approval, sandbox, {});
        auto policy = std::make_shared<FixedFilesystemIntent>();
        ObservationRequirement requirement;
        requirement.domain = ObservationDomain::FileSystem;
        requirement.resourceHint = "/fixture/calc.cpp";
        policy->intent.requirements.append(requirement);
        loop.setObservationIntentPolicy(policy);
        const auto state = loop.run("Explain calc.cpp", "classified-no-evidence");
        QVERIFY(planner.calls > 0);
        QVERIFY(state.phase != AgentLoopPhase::Completed);
        QVERIFY(state.finalAnswer.isEmpty());
        QVERIFY(executor.requests.isEmpty());
    }

    void rejectedFinalReportsPolicyReasonWithoutCompleting() {
        ScriptedPlanner planner;
        auto answer = finalDecision(QStringLiteral("The file exists."));
        answer.grounding = GroundingMode::Verified;
        answer.groundingDeclared = true;
        planner.decisions = {answer, answer};
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        AgentLoop loop(planner, executor, approval, sandbox, QStringList{});
        auto policy = std::make_shared<FixedFilesystemIntent>();
        policy->intent.requirements.append(
            {ObservationDomain::FileSystem, QStringLiteral("missing.txt"),
             EvidenceFreshness::TurnScoped, ObservationPurpose::Inspect});
        loop.setObservationIntentPolicy(policy);
        const auto state =
            loop.run(QStringLiteral("Inspect missing.txt"), QStringLiteral("diagnostic"));
        QCOMPARE(state.phase, AgentLoopPhase::Failed);
        QCOMPARE(state.terminalReason, AgentTerminalReason::UnableToComplete);
        QVERIFY(state.abortReason.contains(QStringLiteral("lacks fresh")));
        QVERIFY(state.finalAnswer.isEmpty());
        QVERIFY(executor.requests.isEmpty());
        QCOMPARE(planner.calls, 2);
    }

    void cancellationDuringInitialClassificationIsTerminalOnce() {
        ScriptedPlanner planner;
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        AgentLoop loop(planner, executor, approval, sandbox, QStringList{});
        auto policy = std::make_shared<CallbackIntentPolicy>();
        policy->onClassify = [&] { loop.cancelAsync(); };
        loop.setObservationIntentPolicy(policy);
        int terminals = 0;
        loop.runAsync(QStringLiteral("List the workspace"), QStringLiteral("initial-cancel"), this,
                      [&](const AgentLoopState& state) {
                          ++terminals;
                          QCOMPARE(state.phase, AgentLoopPhase::Cancelled);
                      });
        QTest::qWait(25);
        QCOMPARE(terminals, 1);
        QCOMPARE(planner.calls, 0);
        QVERIFY(executor.requests.isEmpty());
    }

    void hiddenNegativeClaimReproduction_data() {
        QTest::addColumn<QString>("finalText");
        QTest::addColumn<bool>("contextMode");
        QTest::newRow("original") << QString("No hidden files were found.") << false;
        QTest::newRow("universal") << QString("There are no hidden files.") << false;
        QTest::newRow("dotfiles") << QString("No dotfiles were found.") << false;
        QTest::newRow("context-bypass") << QString("No hidden files exist.") << true;
    }
    void hiddenNegativeClaimReproduction() {
        QFETCH(QString, finalText);
        QFETCH(bool, contextMode);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QDir root(workspace.path());
        QVERIFY(root.mkpath("normal"));
        QVERIFY(root.mkpath(".hidden-dir"));
        for (const auto& name :
             {"visible.txt", ".hidden.txt", "normal/file.txt", ".hidden-dir/nested.txt"}) {
            QFile file(root.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("fixture");
        }
        ScriptedPlanner planner;
        auto invocation = toolDecision("list-directory");
        invocation.arguments = {{"path", workspace.path()}};
        auto final = finalDecision(finalText);
        final.grounding = contextMode ? GroundingMode::Context : GroundingMode::Verified;
        final.groundingDeclared = true;
        planner.decisions = {invocation, final};
        planner.repeatLast = true;
        RealToolExecutor executor;
        InMemoryToolRegistry registry;
        QVERIFY(BuiltInToolProvider::registerTools(registry, executor));
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        AgentLoop loop(planner, executor, approval, sandbox, {"list-directory"});
        loop.setToolRegistry(&registry);
        loop.setResourceScope(workspace.path());
        AgentContextInput::WorkspaceContext context;
        context.rootPath = workspace.path();
        loop.setWorkspaceContext(context);
        const auto state = loop.run("List the workspace");
        QVERIFY(!state.evidence.isEmpty());
        const auto observation = state.evidence.first().structuredObservation;
        QVERIFY(observation);
        QCOMPARE(observation->kind, StructuredObservationKind::DirectoryListing);
        QCOMPARE(observation->data.value("includeHidden").toBool(), false);
        QVERIFY(QFileInfo::exists(root.filePath(".hidden.txt")));
        QVERIFY(state.phase != AgentLoopPhase::Completed);
        QVERIFY(state.finalAnswer != finalText);
    }
    void typedHiddenAbsenceThroughGateway_data() {
        QTest::addColumn<bool>("hiddenExists");
        QTest::addColumn<bool>("includeHidden");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("A-hidden-excluded") << true << false << false;
        QTest::newRow("B-hidden-included-present") << true << true << false;
        QTest::newRow("C-included-complete-absent") << false << true << true;
    }
    void typedHiddenAbsenceThroughGateway() {
        QFETCH(bool, hiddenExists);
        QFETCH(bool, includeHidden);
        QFETCH(bool, accepted);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        if (hiddenExists) {
            QFile file(QDir(workspace.path()).filePath(".hidden.txt"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("hidden");
        }
        auto policy = std::make_shared<FixedFilesystemIntent>();
        ObservationRequirement claim;
        claim.domain = ObservationDomain::FileSystem;
        claim.claimType = ClaimType::HiddenEntriesExist;
        claim.claimId = "hidden";
        claim.resourceHint = QFileInfo(workspace.path()).canonicalFilePath();
        policy->intent.requirements.append(claim);
        ScriptedPlanner planner;
        auto tool = toolDecision("list-directory");
        tool.arguments = {{"path", workspace.path()},
                          {"includeHidden", includeHidden ? "true" : "false"}};
        tool.arguments.last().jsonValue = QJsonValue(includeHidden);
        auto final = finalDecision("No hidden files exist.");
        final.grounding = GroundingMode::Verified;
        final.groundingDeclared = true;
        final.claims.append({claim.claimId, false});
        planner.decisions = {tool, final};
        planner.repeatLast = true;
        planner.onCall = [&](int index) {
            if (index == 0)
                return;
            const auto history = planner.observedHistories.last();
            QVERIFY(!history.isEmpty());
            EvidenceRecord evidence;
            evidence.structuredObservation = history.first().structuredObservation;
            evidence.domain = ObservationDomain::FileSystem;
            evidence.outcome = EvidenceOutcome::Verified;
            evidence.toolCallId = "observed";
            if (const auto representation = ClaimGroundingResolver::filesystemFinalAnswer(
                    policy->intent, {evidence}, final.answer))
                planner.decisions[1].answer = *representation;
        };
        RealToolExecutor executor;
        InMemoryToolRegistry registry;
        QVERIFY(BuiltInToolProvider::registerTools(registry, executor));
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        AgentLoop loop(planner, executor, approval, sandbox, {"list-directory"});
        loop.setToolRegistry(&registry);
        loop.setResourceScope(workspace.path());
        loop.setObservationIntentPolicy(policy);
        AgentContextInput::WorkspaceContext context;
        context.rootPath = workspace.path();
        loop.setWorkspaceContext(context);
        const auto state = loop.run("Are there no hidden entries in this exact directory?");
        QCOMPARE(state.phase == AgentLoopPhase::Completed, accepted);
        QVERIFY(!state.evidence.isEmpty());
        if (accepted) {
            QCOMPARE(state.finalClaims.size(), 1);
            QCOMPARE(state.finalClaims.first().value, false);
            QVERIFY(state.finalAnswer.contains("not found"));
        }
    }
    void runsToolThenFinalAnswer() {
        ScriptedPlanner planner;
        planner.decisions = {toolDecision(QStringLiteral("run-command")),
                             finalDecision(QStringLiteral("all done"))};
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();

        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("run-command")});
        const auto state = loop.run(QStringLiteral("list files"));

        QCOMPARE(state.phase, AgentLoopPhase::Completed);
        QCOMPARE(state.steps.size(), 1);
        QCOMPARE(state.finalAnswer, QStringLiteral("all done"));
        QCOMPARE(executor.requests.size(), 1);
        QCOMPARE(state.steps.first().statusText, QStringLiteral("Succeeded"));
        QVERIFY(state.steps.first().succeeded);
    }

    void repeatedReadGetsOnePlanningRepairWithoutReexecution_data() {
        QTest::addColumn<bool>("recover");
        QTest::addColumn<bool>("async");
        QTest::newRow("sync-final") << true << false;
        QTest::newRow("sync-bounded") << false << false;
        QTest::newRow("async-final") << true << true;
        QTest::newRow("async-bounded") << false << true;
    }

    void repeatedReadGetsOnePlanningRepairWithoutReexecution() {
        QFETCH(bool, recover);
        QFETCH(bool, async);
        ScriptedPlanner planner;
        const auto read = toolDecision(QStringLiteral("read-file"));
        planner.decisions = {read, read,
                             recover ? finalDecision(QStringLiteral("observed result")) : read};
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("read-file")});
        AgentLoopState state;
        bool finished = false;
        if (async) {
            loop.runAsync(QStringLiteral("Inspect file"), QStringLiteral("read-repair"), this,
                          [&](const AgentLoopState& result) {
                              state = result;
                              finished = true;
                          });
            QTRY_VERIFY(finished);
        } else {
            state = loop.run(QStringLiteral("Inspect file"));
        }
        QCOMPARE(state.phase, recover ? AgentLoopPhase::Completed : AgentLoopPhase::Stuck);
        QCOMPARE(state.repeatedReadRepairs, 1);
        QCOMPARE(executor.requests.size(), 1);
        QCOMPARE(planner.calls, 3);
    }

    void repeatedMutationGetsOneBoundedRepairWithoutReexecution() {
        ScriptedPlanner planner;
        const auto write = toolDecision(QStringLiteral("write-file"));
        planner.decisions = {write, write, finalDecision(QStringLiteral("write observed"))};
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("write-file")});
        const auto state = loop.run(QStringLiteral("Write file"));
        QCOMPARE(state.phase, AgentLoopPhase::Stuck);
        QCOMPARE(state.repeatedReadRepairs, 0);
        QCOMPARE(state.repeatedActionRepairs, 1);
        QCOMPARE(executor.requests.size(), 1);
        QVERIFY(state.finalAnswer.isEmpty());
    }

    void repeatedFailedStepGetsOneBoundedRepairWithoutReexecution_data() {
        QTest::addColumn<bool>("recover");
        QTest::addColumn<bool>("async");
        QTest::newRow("sync-final") << true << false;
        QTest::newRow("sync-repeat") << false << false;
        QTest::newRow("async-final") << true << true;
        QTest::newRow("async-repeat") << false << true;
    }

    void repeatedFailedStepGetsOneBoundedRepairWithoutReexecution() {
        QFETCH(bool, recover);
        QFETCH(bool, async);
        ScriptedPlanner planner;
        const auto stat = toolDecision(QStringLiteral("read-file"));
        // First call fails with NotFound; the model repeats it unchanged. The
        // repeat is never re-executed; one repair teaches that the failed result
        // is already the answer. A second repeat still stops fail-closed.
        planner.decisions = {stat, stat, recover ? finalDecision(QStringLiteral("path is absent"))
                                                 : stat};
        planner.repeatLast = true;
        RecordingExecutor executor;
        executor.status = ToolExecutionStatus::Failed;
        executor.failureCategory = ToolFailureCategory::NotFound;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("read-file")});
        AgentLoopState state;
        bool finished = false;
        if (async) {
            loop.runAsync(QStringLiteral("Inspect missing file"),
                          QStringLiteral("failed-repeat"), this,
                          [&](const AgentLoopState& result) {
                              state = result;
                              finished = true;
                          });
            QTRY_VERIFY(finished);
        } else {
            state = loop.run(QStringLiteral("Inspect missing file"));
        }
        QCOMPARE(state.phase, recover ? AgentLoopPhase::Completed : AgentLoopPhase::Stuck);
        QCOMPARE(state.repeatedFailureRepairs, 1);
        QCOMPARE(executor.requests.size(), 1);
        const bool hasFailureRepair = std::any_of(
            planner.feedbacks.cbegin(), planner.feedbacks.cend(), [](const QString& feedback) {
                return feedback.contains(QStringLiteral("A NotFound result is definitive")) &&
                       feedback.contains(QStringLiteral("already returned Failed"));
            });
        QVERIFY(hasFailureRepair);
        if (!recover)
            QVERIFY(state.finalAnswer.isEmpty());
    }

    void multipleToolResultsRequireAcceptedFinal() {
        ScriptedPlanner planner;
        planner.decisions = {toolDecision(QStringLiteral("first")),
                             toolDecision(QStringLiteral("second")),
                             finalDecision(QStringLiteral("verified sequence"))};
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("first"), QStringLiteral("second")});
        const auto state = loop.run(QStringLiteral("Inspect two resources"));
        QCOMPARE(state.phase, AgentLoopPhase::Completed);
        QCOMPARE(state.steps.size(), 2);
        QCOMPARE(executor.requests.size(), 2);
        QCOMPARE(planner.observedHistories.at(2).size(), 2);
        QCOMPARE(state.finalAnswer, QStringLiteral("verified sequence"));
    }

    void feedsObservationToNextPlanningCall() {
        ScriptedPlanner planner;
        planner.decisions = {toolDecision(QStringLiteral("run-command")),
                             finalDecision(QStringLiteral("done"))};
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();

        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("run-command")});
        loop.run(QStringLiteral("goal"));

        QCOMPARE(planner.observedHistories.size(), 2);
        QCOMPARE(planner.observedHistories.at(1).size(), 1);
        QVERIFY(planner.observedHistories.at(1).first().observation.contains(
            QStringLiteral("observation-from-tool")));
    }

    void bindsFilesystemAuthorizationToConfiguredWorkspaceRoot() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto descriptors = BuiltInToolProvider::descriptors();
        const auto descriptor =
            std::find_if(descriptors.cbegin(), descriptors.cend(), [](const ToolDescriptor& item) {
                return item.id == QLatin1String("list-directory");
            });
        QVERIFY(descriptor != descriptors.cend());

        auto handler = std::make_shared<RecordingHandler>();
        InMemoryToolRegistry registry;
        QVERIFY(registry.registerTool({*descriptor, handler}));
        ScriptedPlanner planner;
        auto list = toolDecision(QStringLiteral("list-directory"), QStringLiteral("."));
        list.arguments.first().id = QStringLiteral("path");
        EvidenceRecord observation;
        observation.domain = ObservationDomain::FileSystem;
        observation.toolId = QStringLiteral("list-directory");
        observation.resource = QFileInfo(workspace.path()).canonicalFilePath();
        observation.outcome = EvidenceOutcome::Verified;
        const auto representation = ClaimGroundingResolver::filesystemFinalAnswer(
            {}, {observation}, QStringLiteral("done"));
        QVERIFY(representation);
        planner.decisions = {list, finalDecision(*representation)};
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();

        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("list-directory")});
        loop.setToolRegistry(&registry);
        AgentContextInput::WorkspaceContext context;
        context.id = QStringLiteral("workspace");
        context.rootPath = workspace.path();
        loop.setWorkspaceContext(context);
        const auto state = loop.run(QStringLiteral("list the workspace"));

        QCOMPARE(state.phase, AgentLoopPhase::Completed);
        QCOMPARE(handler->requests.size(), 1);
        QVERIFY(handler->requests.first().plan.invocations.first().resourceSnapshot);
        QCOMPARE(
            handler->requests.first().plan.invocations.first().resourceSnapshot->workingDirectory,
            QFileInfo(workspace.path()).canonicalFilePath());
    }

    void stopsAtIterationLimit() {
        ScriptedPlanner planner;
        planner.decisions = {toolDecision(QStringLiteral("run-command"), QStringLiteral("one")),
                             toolDecision(QStringLiteral("run-command"), QStringLiteral("two")),
                             toolDecision(QStringLiteral("run-command"), QStringLiteral("three"))};
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();

        AgentLoop::Config config;
        config.maxIterations = 3;
        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("run-command")}, config);
        const auto state = loop.run(QStringLiteral("goal"));

        QCOMPARE(state.phase, AgentLoopPhase::Failed);
        QCOMPARE(state.steps.size(), 3);
        QVERIFY(state.abortReason.contains(QStringLiteral("Iteration limit")));
    }

    void doomLoopRequiresTrailingRepetition() {
        DoomLoopDetector detector{DoomLoopDetector::Config{}};
        for (const auto& action : {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"),
                                   QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")})
            detector.recordAction(QStringLiteral("session"), action);
        QVERIFY(!detector.isStuck(QStringLiteral("session")));
        for (const auto& action : {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")})
            detector.recordAction(QStringLiteral("session"), action);
        QVERIFY(detector.isStuck(QStringLiteral("session")));
        detector.reset(QStringLiteral("session"));
        QVERIFY(!detector.isStuck(QStringLiteral("session")));
    }

    void consecutiveBlockedBudgetResetsAfterProductiveStep() {
        ScriptedPlanner planner;
        const auto unknownTool = [](const QString& id) {
            AgentStepDecision decision;
            decision.kind = AgentStepDecision::Kind::ToolCall;
            decision.toolId = id;
            decision.toolName = id;
            return decision;
        };
        planner.decisions = {
            unknownTool(QStringLiteral("missing-1")), toolDecision(QStringLiteral("run-command")),
            unknownTool(QStringLiteral("missing-2")), unknownTool(QStringLiteral("missing-3")),
            unknownTool(QStringLiteral("missing-4"))};
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();
        AgentLoop::Config config;
        config.maxConsecutiveBlockedSteps = 3;
        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("run-command")}, config);
        const auto state = loop.run(QStringLiteral("recover from invalid requests"));
        QCOMPARE(state.phase, AgentLoopPhase::Failed);
        QVERIFY(state.abortReason.contains(QStringLiteral("consecutive blocked")));
        QCOMPARE(state.blockedSteps, 4);
        QCOMPARE(state.consecutiveBlockedSteps, 3);
        QCOMPARE(executor.requests.size(), 1);
    }

    void pausesForApprovalAndResumes() {
        ScriptedPlanner planner;
        planner.decisions = {toolDecision(QStringLiteral("run-command"),
                                          QStringLiteral("rm -rf /tmp/probe"), ToolRiskLevel::High),
                             finalDecision(QStringLiteral("finished"))};
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();

        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("run-command")});
        auto state = loop.run(QStringLiteral("clean temp"));

        QCOMPARE(state.phase, AgentLoopPhase::AwaitingApproval);
        QCOMPARE(executor.requests.size(), 0);
        QVERIFY(!state.pendingApprovalPlan.invocations.isEmpty());

        AgentLoop resumed(planner, executor, approval, sandbox,
                          QStringList{QStringLiteral("run-command")});
        state = resumed.resume(state, true);

        QCOMPARE(state.phase, AgentLoopPhase::Completed);
        QCOMPARE(executor.requests.size(), 1);
        QCOMPARE(state.steps.size(), 1);
        QVERIFY(state.steps.first().succeeded);
    }

    void recordsDenialObservationWhenUserDenies() {
        ScriptedPlanner planner;
        planner.decisions = {toolDecision(QStringLiteral("run-command"), QStringLiteral(" risky "),
                                          ToolRiskLevel::High),
                             finalDecision(QStringLiteral("aborted by user denial"))};
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();

        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("run-command")});
        auto state = loop.run(QStringLiteral("risky goal"));

        QCOMPARE(state.phase, AgentLoopPhase::AwaitingApproval);

        AgentLoop resumed(planner, executor, approval, sandbox,
                          QStringList{QStringLiteral("run-command")});
        state = resumed.resume(state, false);

        QCOMPARE(state.phase, AgentLoopPhase::Failed);
        QCOMPARE(executor.requests.size(), 0);
        QCOMPARE(state.steps.size(), 1);
        QVERIFY(!state.steps.first().succeeded);
        QVERIFY(state.steps.first().observation.contains(
            QStringLiteral("User denied execution in chat.")));
        QVERIFY(state.abortReason.contains(QStringLiteral("User denied")));
    }

    void detectsDoomLoop() {
        ScriptedPlanner planner;
        planner.decisions = {toolDecision(QStringLiteral("run-command"))};
        planner.repeatLast = true;
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();

        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("run-command")});
        const auto state = loop.run(QStringLiteral("goal"));

        QCOMPARE(state.phase, AgentLoopPhase::Stuck);
        QVERIFY(state.abortReason.contains(QStringLiteral("repeated the same")));
        QCOMPARE(state.steps.size(), 1);
    }

    void honorsCancelQueryBetweenSteps() {
        ScriptedPlanner planner;
        planner.decisions = {toolDecision(QStringLiteral("run-command"))};
        planner.repeatLast = true;
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();

        bool cancelNow = false;
        planner.onCall = [&cancelNow](int index) {
            if (index == 1) {
                cancelNow = true;
            }
        };

        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("run-command")});
        loop.setCancelQuery([&cancelNow] { return cancelNow; });
        const auto state = loop.run(QStringLiteral("goal"));

        QCOMPARE(state.phase, AgentLoopPhase::Cancelled);
        QCOMPARE(state.steps.size(), 1);
        QCOMPARE(executor.requests.size(), 1);
        QVERIFY(state.abortReason.contains(QStringLiteral("cancelled")));
    }

    void recordsUnknownToolObservation() {
        ScriptedPlanner planner;
        planner.decisions = {toolDecision(QStringLiteral("warp-drive")),
                             finalDecision(QStringLiteral("recovered"))};
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();

        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("run-command")});
        const auto state = loop.run(QStringLiteral("goal"));

        QCOMPARE(state.phase, AgentLoopPhase::Completed);
        QCOMPARE(executor.requests.size(), 0);
        QCOMPARE(state.steps.size(), 1);
        QVERIFY(state.steps.first().observation.contains(
            QStringLiteral("Unknown tool requested: warp-drive")));
    }

    void executesWithoutApprovalInAutonomousMode() {
        ScriptedPlanner planner;
        planner.decisions = {toolDecision(QStringLiteral("run-command"), QStringLiteral("deploy"),
                                          ToolRiskLevel::High),
                             finalDecision(QStringLiteral("deployed"))};
        RecordingExecutor executor;
        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();

        AgentLoop::Config config;
        config.autonomousMode = true;
        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("run-command")}, config);
        const auto state = loop.run(QStringLiteral("deploy"));

        QCOMPARE(state.phase, AgentLoopPhase::Completed);
        QCOMPARE(executor.requests.size(), 1);
        QCOMPARE(state.steps.size(), 1);
    }

    void truncatesLargeObservations() {
        ScriptedPlanner planner;
        planner.decisions = {toolDecision(QStringLiteral("run-command")),
                             finalDecision(QStringLiteral("done"))};

        QStringList bigLines;
        for (int i = 0; i < 500; ++i) {
            bigLines.append(QStringLiteral("line-%1-%2").arg(i).arg(QString(96, QChar('x'))));
        }
        RecordingExecutor executor;
        executor.summary = bigLines.join(QLatin1Char('\n'));

        StaticApprovalPolicy approval;
        auto sandbox = permissiveSandbox();

        AgentLoop::Config config;
        config.observationMaxBytes = 4096;
        AgentLoop loop(planner, executor, approval, sandbox,
                       QStringList{QStringLiteral("run-command")}, config);
        const auto state = loop.run(QStringLiteral("goal"));

        QCOMPARE(state.phase, AgentLoopPhase::Completed);
        QVERIFY(state.steps.first().observation.toUtf8().size() < 5000);
        QVERIFY(state.steps.first().observation.contains(QStringLiteral("omitted")));
        QVERIFY(state.steps.first().observation.contains(QStringLiteral("Output truncated:")));
        QVERIFY(state.steps.first().observation.contains(QStringLiteral("Full output:")));
        QVERIFY(state.steps.first().observation.contains(QStringLiteral("instead of repeating")));
    }
};

QTEST_MAIN(AgentLoopTest)
#include "test_agent_loop.moc"
