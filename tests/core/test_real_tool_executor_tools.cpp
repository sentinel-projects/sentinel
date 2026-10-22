// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScopeGuard>
#include <QStandardPaths>

#include "sentinel/core/agent/ClaimGroundingResolver.h"
#include "sentinel/core/chat/SQLiteChatHistoryStore.h"
#include "sentinel/core/memory/InMemoryStore.h"
#include "sentinel/core/runtime/AlarmStore.h"
#include "sentinel/core/runtime/BuiltInToolProvider.h"
#include "sentinel/core/runtime/IFileSystemService.h"
#include "sentinel/core/runtime/InMemoryToolRegistry.h"
#include "sentinel/core/runtime/RealToolExecutor.h"
#include "sentinel/core/runtime/ToolExecutionGateway.h"
#include "sentinel/core/security/ResourceAuthorizationResolver.h"
#include "sentinel/core/security/StaticSandboxPolicy.h"

using namespace sentinel::core;
namespace {

ToolInvocationPlan approvedPlan(const QString& toolId,
                                const QList<ToolInvocationArgument>& arguments,
                                ToolRiskLevel risk = ToolRiskLevel::Low) {
    ToolInvocationPlan plan;
    plan.status = ToolInvocationPlanStatus::Planned;
    plan.summary = QStringLiteral("test plan");
    plan.invocations.append(PlannedToolInvocation{toolId,
                                                  toolId,
                                                  QStringLiteral("test"),
                                                  QStringLiteral("test"),
                                                  risk,
                                                  ToolExecutionMode::Local,
                                                  arguments,
                                                  {}});
    return plan;
}

ToolInvocationArgument intArgument(const QString& id, int value) {
    ToolInvocationArgument argument{id, QString::number(value)};
    argument.jsonValue = QJsonValue(value);
    return argument;
}

ToolExecutionResult runTool(RealToolExecutor& executor, const QString& toolId,
                            const QList<ToolInvocationArgument>& arguments,
                            const QStringList& knownToolIds,
                            const QString& workspace = QDir::currentPath(),
                            QObject* callbackContext = nullptr, const QString& sessionId = {}) {
    Q_UNUSED(knownToolIds)
    InMemoryToolRegistry registry;
    if (!BuiltInToolProvider::registerTools(registry, executor))
        return {ToolExecutionStatus::Failed, QStringLiteral("Built-in tools did not register.")};

    ToolExecutionGateway gateway(&registry);
    ToolInvocationPlan plan = approvedPlan(toolId, arguments);

    const auto validation = gateway.validatePlan(plan);
    if (validation.status != ToolExecutionStatus::Succeeded) {
        if (qEnvironmentVariableIsSet("SENTINEL_TEST_TRACE")) {
            QStringList ids;
            for (const auto& tool : registry.listTools())
                ids.append(tool.id);
            qWarning().noquote() << toolId << "validate"
                                 << static_cast<int>(validation.status) << validation.summary
                                 << "| registered:" << ids.join(QLatin1Char(','));
        }
        return validation;
    }

    // Mirror AgentLoop: capture the authoritative descriptor, then resolve
    // and authorize resources before the gateway hands the call to a handler.
    for (auto& invocation : plan.invocations) {
        const auto registration = registry.findRegistration(invocation.toolId);
        if (registration)
            invocation.descriptorSnapshot =
                std::make_shared<const ToolDescriptor>(registration->descriptor);
    }

    // Resolve then authorize resources before the gateway
    // hands the invocation to the registered handler. A security denial maps to
    // Blocked; every other resource failure maps to InvalidArguments, matching
    // the labels AgentLoop reports for a blocked step.
    const auto resourceFailure = [](const QString& reason, FileSystemFailure failure) {
        const bool security = failure == FileSystemFailure::PermissionDenied ||
                              failure == FileSystemFailure::ResourceChanged ||
                              failure == FileSystemFailure::SymlinkEscape ||
                              failure == FileSystemFailure::UnsafeParent ||
                              failure == FileSystemFailure::SecurityBoundaryViolation;
        return ToolExecutionResult{security ? ToolExecutionStatus::Blocked
                                            : ToolExecutionStatus::InvalidArguments,
                                   reason};
    };
    for (auto& invocation : plan.invocations) {
        if (!invocation.descriptorSnapshot)
            continue;
        auto resolved = ResourceAuthorizationResolver::resolve(*invocation.descriptorSnapshot,
                                                               invocation, workspace, nullptr);
        if (!resolved.ok())
            return resourceFailure(resolved.reason, resolved.failure);
        invocation.resourceSnapshot = std::make_shared<const ResourceAuthorizationSnapshot>(
            std::move(resolved.snapshot));
        auto authorized = ResourceAuthorizationResolver::authorize(*invocation.resourceSnapshot,
                                                                   nullptr, nullptr, {});
        if (!authorized.ok())
            return resourceFailure(authorized.reason, authorized.failure);
        invocation.resourceSnapshot = std::make_shared<const ResourceAuthorizationSnapshot>(
            std::move(authorized.snapshot));
    }

    ToolExecutionResult result;
    bool completed = false;
    const ApprovalDecision approval{ApprovalStatus::Approved, QStringLiteral("test"), {}};
    // Built-in descriptors currently use the default metadata capability.
    const StaticSandboxPolicy sandboxPolicy{QSet<QString>{QStringLiteral("tool.metadata.read")}};
    // The returned cancellation handle owns the active process execution, so it
    // must stay alive until the completion callback fires.
    const IToolExecutor::Cancel active = gateway.executeAsync(
        ToolExecutionRequest{
            plan,
            approval,
            sandboxPolicy.evaluate(plan, approval),
            knownToolIds,
            callbackContext,
        },
        executor, sessionId, {}, {}, [&](ToolExecutionResult value) {
            result = std::move(value);
            completed = true;
        });

    // Process-backed tools finish from a QProcess terminal signal; immediate
    // tools already completed inline above.
    const bool waited = QTest::qWaitFor([&completed] { return completed; }, 30000);
    if (qEnvironmentVariableIsSet("SENTINEL_TEST_TRACE")) {
        QString trace = result.summary;
        trace.replace(QLatin1Char('\n'), QLatin1Char('|'));
        qWarning().noquote() << toolId << static_cast<int>(result.status) << "completed:"
                             << completed << "waited:" << waited << trace;
    }
    if (!completed)
        return {ToolExecutionStatus::Blocked,
                QStringLiteral("Tool execution did not complete in time.")};
    return result;
}

} // namespace

class RealToolExecutorToolsTest final : public QObject {
    Q_OBJECT

private slots:
    void authorizedReadRejectsReplacedResourcesAndKeepsBounds() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QtFileSystemService fs;
        const auto filename = workspace.filePath("evidence.txt");
        QFile original(filename);
        QVERIFY(original.open(QIODevice::WriteOnly));
        QCOMPARE(original.write("trusted content"), qint64(15));
        original.close();
        const auto authorized = fs.resolve(filename, workspace.path(), FileSystemAccess::Read);
        QVERIFY(authorized.ok());
        const auto bounded = fs.readFile(*authorized.value, 7);
        QVERIFY(bounded.ok());
        QCOMPARE(bounded.value->content, QByteArray("trusted"));
        QVERIFY(bounded.value->truncated);
        const auto complete = fs.readFile(*authorized.value, 15);
        QVERIFY(complete.ok());
        QVERIFY(complete.value->complete);
        QVERIFY(QFile::rename(filename, filename + ".original"));
        QFile replacement(filename);
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        replacement.write("untrusted replacement");
        replacement.close();
        const auto changed = fs.readFile(*authorized.value, 100);
        QCOMPARE(changed.failure, FileSystemFailure::ResourceChanged);
        QVERIFY(!changed.value.has_value());
        QVERIFY(!fs.readFile(*authorized.value, -1).ok());
#if defined(Q_OS_UNIX)
        QVERIFY(QFile::remove(filename));
        QVERIFY(QFile::link(filename + ".original", filename));
        const auto link = fs.readFile(*authorized.value, 100);
        QVERIFY(!link.ok());
        QVERIFY(!link.value.has_value());
        QVERIFY(QFile::remove(filename));
        QVERIFY(QFile::rename(filename + ".original", filename));
        const auto parentAuthorization =
            fs.resolve(filename, workspace.path(), FileSystemAccess::Read);
        QVERIFY(parentAuthorization.ok());
        const auto parent = QFileInfo(filename).absolutePath();
        QVERIFY(QDir().rename(parent, parent + "-moved"));
        auto cleanupParent = qScopeGuard([parent] {
            QFile::remove(parent);
            QDir().rename(parent + "-moved", parent);
        });
        QVERIFY(QFile::link(parent + "-moved", parent));
        const auto escapedParent = fs.readFile(*parentAuthorization.value, 100);
        QVERIFY(!escapedParent.ok());
        QVERIFY(!escapedParent.value.has_value());
#endif
    }
    void registeredFilesystemHandlerUsesFrozenWorkspace() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QFile file(workspace.filePath(QStringLiteral("frozen-evidence.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("frozen workspace evidence");
        file.close();
        const auto processDirectory = QDir::currentPath();
        QVERIFY(processDirectory != workspace.path());
        RealToolExecutor executor;
        QObject callbackContext;
        for (int path = 0; path < 3; ++path) {
            const auto result = runTool(executor, QStringLiteral("list-directory"),
                                        {{QStringLiteral("path"), QStringLiteral(".")}}, {},
                                        workspace.path(), path == 2 ? &callbackContext : nullptr,
                                        path == 0 ? QString{} : QStringLiteral("frozen-session"));
            QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
            QVERIFY(result.summary.contains(QStringLiteral("frozen-evidence.txt")));
            QCOMPARE(QDir::currentPath(), processDirectory);
        }
    }

    void registeredProcessHandlerUsesFrozenWorkspace() {
#ifdef Q_OS_WIN
        QSKIP("pwd fixture is Unix-only.");
#endif
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        RealToolExecutor executor;
        const auto result = runTool(executor, QStringLiteral("run-command"),
                                    {{QStringLiteral("command"), QStringLiteral("pwd")},
                                     {QStringLiteral("workdir"), QStringLiteral(".")}},
                                    {}, workspace.path());
        if (result.status == ToolExecutionStatus::Blocked &&
            (result.summary.contains(QStringLiteral("sandbox"), Qt::CaseInsensitive) ||
             result.sandbox.enforcement == SandboxEnforcement::Failed ||
             result.failureCategory == ToolFailureCategory::RuntimeUnavailable))
            QSKIP("Host cannot enforce the required process sandbox.");
        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QFileInfo(workspace.path()).canonicalFilePath()));
        QVERIFY(result.structuredObservation);
        const auto receipt = result.structuredObservation->data;
        QCOMPARE(receipt.value(QStringLiteral("exitCode")).toInt(), 0);
        QVERIFY(receipt.value(QStringLiteral("stdout"))
                    .toString()
                    .contains(QFileInfo(workspace.path()).canonicalFilePath()));
        QVERIFY(!receipt.value(QStringLiteral("timedOut")).toBool());
        QVERIFY(!receipt.value(QStringLiteral("stdoutTruncated")).toBool());
        QVERIFY(receipt.value(QStringLiteral("complete")).toBool());
    }

    void pythonToolchainRunsButCannotReadOutsideWorkspace() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS toolchain runtime regression.");
#else
        if (QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty())
            QSKIP("Python is not installed.");
        QTemporaryDir workspace;
        QTemporaryDir outside;
        QVERIFY(workspace.isValid());
        QVERIFY(outside.isValid());
        QFile secret(outside.filePath(QStringLiteral("private.txt")));
        QVERIFY(secret.open(QIODevice::WriteOnly));
        secret.write("DO_NOT_EXPOSE");
        secret.close();
        QFile script(workspace.filePath(QStringLiteral("test.py")));
        QVERIFY(script.open(QIODevice::WriteOnly));
        const auto encoded =
            QJsonDocument(QJsonArray{secret.fileName()}).toJson(QJsonDocument::Compact);
        script.write("import json, pathlib\ntry:\n    pathlib.Path(json.loads(");
        const auto quoted =
            QJsonDocument(QJsonArray{QString::fromUtf8(encoded)}).toJson(QJsonDocument::Compact);
        script.write(quoted.mid(1, quoted.size() - 2));
        script.write(
            ")[0]).read_text()\nexcept PermissionError:\n    print('SANDBOX_PRESERVED')\nelse:\n   "
            " raise AssertionError('outside workspace was readable')\n");
        script.close();
        RealToolExecutor executor;
        const auto result = runTool(
            executor, QStringLiteral("run-command"),
            {{QStringLiteral("command"), QStringLiteral("python3 test.py")}}, {}, workspace.path());
        if (result.status == ToolExecutionStatus::Blocked &&
            (result.summary.contains(QStringLiteral("sandbox"), Qt::CaseInsensitive) ||
             result.sandbox.enforcement == SandboxEnforcement::Failed ||
             result.failureCategory == ToolFailureCategory::RuntimeUnavailable))
            QSKIP("Host cannot enforce the required process sandbox.");
        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("SANDBOX_PRESERVED")));
        QVERIFY(!result.summary.contains(QStringLiteral("DO_NOT_EXPOSE")));
#endif
    }

    void readFileReturnsNumberedLines() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sample.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("alpha\nbeta\ngamma\n");
        file.close();

        // The executor scopes reads to the current working directory.
        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("read-file"),
                    {ToolInvocationArgument{QStringLiteral("path"), QStringLiteral("sample.txt")}},
                    QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("1: alpha")));
        QVERIFY(result.summary.contains(QStringLiteral("3: gamma")));
        QVERIFY(result.summary.contains(QStringLiteral("4: ")));
        QVERIFY(!result.summary.contains(QStringLiteral("5: ")));
    }

    void gitInspectionUsesFixedSandboxedArguments() {
        if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
            QSKIP("Git is not installed.");
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QProcess setup;
        setup.setWorkingDirectory(workspace.path());
        setup.start(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("--quiet")});
        QVERIFY(setup.waitForFinished());
        QCOMPARE(setup.exitCode(), 0);
        QFile file(workspace.filePath(QStringLiteral("fixture.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("original\n");
        file.close();
        RealToolExecutor executor;
        const auto status =
            runTool(executor, QStringLiteral("git-status"),
                    {{QStringLiteral("path"), QStringLiteral(".")}}, {}, workspace.path());
        if (status.status == ToolExecutionStatus::Blocked &&
            (status.summary.contains(QStringLiteral("sandbox"), Qt::CaseInsensitive) ||
             status.sandbox.enforcement == SandboxEnforcement::Failed ||
             status.failureCategory == ToolFailureCategory::RuntimeUnavailable))
            QSKIP("Host cannot enforce the required process sandbox.");
        QCOMPARE(status.status, ToolExecutionStatus::Succeeded);
        QVERIFY(status.summary.contains(QStringLiteral("?? fixture.txt")));
        QVERIFY(status.structuredObservation);
        QCOMPARE(status.structuredObservation->data.value(QStringLiteral("exitCode")).toInt(), 0);
        setup.start(QStringLiteral("git"),
                    {QStringLiteral("add"), QStringLiteral("--"), QStringLiteral("fixture.txt")});
        QVERIFY(setup.waitForFinished());
        QCOMPARE(setup.exitCode(), 0);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("modified\n");
        file.close();
        const auto diff =
            runTool(executor, QStringLiteral("git-diff"),
                    {{QStringLiteral("path"), QStringLiteral(".")}}, {}, workspace.path());
        QCOMPARE(diff.status, ToolExecutionStatus::Succeeded);
        QVERIFY(diff.summary.contains(QStringLiteral("-original")));
        QVERIFY(diff.summary.contains(QStringLiteral("+modified")));
        const auto staged =
            runTool(executor, QStringLiteral("git-diff"),
                    {{QStringLiteral("path"), QStringLiteral(".")},
                     {QStringLiteral("staged"), QStringLiteral("true"), QJsonValue(true)}},
                    {}, workspace.path());
        QCOMPARE(staged.status, ToolExecutionStatus::Succeeded);
        QVERIFY(staged.summary.contains(QStringLiteral("+original")));
        setup.start(QStringLiteral("git"),
                    {QStringLiteral("config"), QStringLiteral("filter.test.clean"),
                     QStringLiteral("sentinel-filter-must-never-run")});
        QVERIFY(setup.waitForFinished());
        QCOMPARE(setup.exitCode(), 0);
        const auto filtered =
            runTool(executor, QStringLiteral("git-diff"),
                    {{QStringLiteral("path"), QStringLiteral(".")}}, {}, workspace.path());
        QCOMPARE(filtered.status, ToolExecutionStatus::Blocked);
        QCOMPARE(filtered.failureCategory, ToolFailureCategory::SecurityDenied);
        QVERIFY(filtered.summary.contains(QStringLiteral("filters")));
        const auto escaped =
            runTool(executor, QStringLiteral("git-status"),
                    {{QStringLiteral("path"), workspace.path() + QStringLiteral("/../")}}, {},
                    workspace.path());
        QCOMPARE(escaped.status, ToolExecutionStatus::Blocked);

        // Fixed Git argument construction and strict argument schema validation:
        // Extraneous or hostile parameters are refused as invalid arguments.
        const auto injectionAttempt =
            runTool(executor, QStringLiteral("git-status"),
                    {{QStringLiteral("path"), QStringLiteral(".")},
                     {QStringLiteral("command"), QStringLiteral("echo injected")},
                     {QStringLiteral("args"), QStringLiteral("--upload-pack=injected")},
                     {QStringLiteral("extra"), QStringLiteral("; touch injected.txt")}},
                    {}, workspace.path());
        QCOMPARE(injectionAttempt.status, ToolExecutionStatus::InvalidArguments);
        QVERIFY(!QFileInfo::exists(workspace.filePath(QStringLiteral("injected.txt"))));

        // Refuse non-directory paths for repository inspection:
        const auto fileNotDir =
            runTool(executor, QStringLiteral("git-status"),
                    {{QStringLiteral("path"), QStringLiteral("fixture.txt")}}, {},
                    workspace.path());
        QCOMPARE(fileNotDir.status, ToolExecutionStatus::InvalidArguments);

        // Verification of no unexpected filesystem mutations:
        QFile checkFile(workspace.filePath(QStringLiteral("fixture.txt")));
        QVERIFY(checkFile.open(QIODevice::ReadOnly));
        QCOMPARE(checkFile.readAll(), QByteArray("modified\n"));
    }

    void statPathUsesAuthorizedFilesystem() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile file(dir.filePath(QStringLiteral("stat.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("hello"), 5);
        file.close();
        const auto previous = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));
        const auto restore = qScopeGuard([&] { QDir::setCurrent(previous); });
        RealToolExecutor executor;
        const auto inspect = [&](const QString& path) {
            return runTool(executor, QStringLiteral("stat-path"),
                           {ToolInvocationArgument{QStringLiteral("path"), path}}, {});
        };
        const auto result = inspect(QStringLiteral("stat.txt"));
        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.structuredObservation);
        QCOMPARE(result.structuredObservation->kind, StructuredObservationKind::FileSystemFact);
        QCOMPARE(result.structuredObservation->data.value(QStringLiteral("size")).toInt(), 5);
        QVERIFY(result.structuredObservation->data.value(QStringLiteral("regularFile")).toBool());
        ObservationRequirement claim;
        claim.claimId = "stat-file";
        claim.claimType = ClaimType::FileExists;
        claim.resourceHint = file.fileName();
        EvidenceRecord evidence;
        evidence.toolCallId = "stat-tool";
        evidence.outcome = EvidenceOutcome::Verified;
        evidence.structuredObservation = result.structuredObservation;
        QCOMPARE(ClaimGroundingResolver::resolve(claim, {evidence}, {claim.claimId, true}).verdict,
                 ClaimVerdict::Supported);
        auto incomplete = std::make_shared<StructuredObservation>(*result.structuredObservation);
        incomplete->data.remove("regularFile");
        evidence.structuredObservation = incomplete;
        QCOMPARE(ClaimGroundingResolver::resolve(claim, {evidence}, {claim.claimId, false}).verdict,
                 ClaimVerdict::Unknown);

        const auto directory = inspect(QStringLiteral("."));
        QCOMPARE(directory.status, ToolExecutionStatus::Succeeded);
        QVERIFY(directory.structuredObservation->data.value(QStringLiteral("directory")).toBool());
        const auto missing = inspect(QStringLiteral("missing.txt"));
        QCOMPARE(missing.status, ToolExecutionStatus::Failed);
        QVERIFY(missing.structuredObservation);
        QCOMPARE(missing.structuredObservation->fileSystemFailure, FileSystemFailure::NotFound);
        const auto escaped = inspect(dir.path() + QStringLiteral("/../outside.txt"));
        QCOMPARE(escaped.status, ToolExecutionStatus::Blocked);
    }

    void readFilePageEvidenceMatchesReturnedContent() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile file(dir.filePath(QStringLiteral("pages.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("first\nsecond\nthird\n");
        file.close();
        const auto previous = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));
        const auto restore = qScopeGuard([&] { QDir::setCurrent(previous); });
        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("read-file"),
                    {ToolInvocationArgument{QStringLiteral("path"), QStringLiteral("pages.txt")},
                     intArgument(QStringLiteral("limit"), 1)},
                    {});
        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.structuredObservation);
        const auto data = result.structuredObservation->data;
        QCOMPARE(data.value(QStringLiteral("content")).toString(), QStringLiteral("first"));
        QCOMPARE(data.value(QStringLiteral("returnedLines")).toInt(), 1);
        QVERIFY(!data.value(QStringLiteral("complete")).toBool());
        QVERIFY(data.value(QStringLiteral("truncated")).toBool());
        QVERIFY(!result.summary.contains(QStringLiteral("second")));
    }

    void readFileOffsetOutOfRangeFailsGracefully() {
        QTemporaryDir dir;
        QFile file(dir.filePath(QStringLiteral("tiny.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("only line\n");
        file.close();

        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("read-file"),
                    {ToolInvocationArgument{QStringLiteral("path"), QStringLiteral("tiny.txt")},
                     intArgument(QStringLiteral("offset"), 42)},
                    QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        // Paging past the end succeeds with no content and reports an
        // incomplete view instead of inventing lines.
        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.isEmpty());
        QVERIFY(result.structuredObservation);
        QVERIFY(result.structuredObservation->data.value(QStringLiteral("content"))
                    .toString()
                    .isEmpty());
        QCOMPARE(result.structuredObservation->data.value(QStringLiteral("complete")).toBool(),
                 false);
        QCOMPARE(result.structuredObservation->data.value(QStringLiteral("truncated")).toBool(),
                 true);
    }

    void readDirectoryListsEntries() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.filePath(QStringLiteral("nested"))).mkpath(QStringLiteral(".")));

        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto result = runTool(
            executor, QStringLiteral("list-directory"),
            {ToolInvocationArgument{QStringLiteral("path"), QStringLiteral(".")}}, QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("nested")));
    }

    void readFileRejectsDirectoryWithPathGuidance() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.filePath(QStringLiteral("nested"))).mkpath(QStringLiteral(".")));

        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto result = runTool(
            executor, QStringLiteral("read-file"),
            {ToolInvocationArgument{QStringLiteral("path"), QStringLiteral("nested")}},
            QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Failed);
        QVERIFY(result.summary.contains(QStringLiteral("list-directory")));
    }

    void editFileAppliesReplacement() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("code.cpp"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("int main() {\n    return 1;\n}\n");
        file.close();

        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto result = runTool(
            executor, QStringLiteral("edit-file"),
            {ToolInvocationArgument{QStringLiteral("path"), QStringLiteral("code.cpp")},
             ToolInvocationArgument{QStringLiteral("oldString"), QStringLiteral("return 1;")},
             ToolInvocationArgument{QStringLiteral("newString"), QStringLiteral("return 0;")}},
            QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("edit-file: Edited")));

        QFile updated(path);
        QVERIFY(updated.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(updated.readAll());
        QVERIFY(content.contains(QStringLiteral("return 0;")));
        QVERIFY(!content.contains(QStringLiteral("return 1;")));
    }

    void editFileCreatesNewFileWithEmptyOldString() {
        QTemporaryDir dir;
        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto result = runTool(
            executor, QStringLiteral("edit-file"),
            {ToolInvocationArgument{QStringLiteral("path"), QStringLiteral("fresh.md")},
             ToolInvocationArgument{QStringLiteral("oldString"), QString()},
             ToolInvocationArgument{QStringLiteral("newString"), QStringLiteral("# hello")}},
            QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("fresh.md"))));
    }

    void grepFindsMatchesUnderDirectory() {
        QTemporaryDir dir;
        QFile a(dir.filePath(QStringLiteral("a.txt")));
        QVERIFY(a.open(QIODevice::WriteOnly | QIODevice::Text));
        a.write("keep this line\nnothing here\n");
        a.close();
        QFile b(dir.filePath(QStringLiteral("b.txt")));
        QVERIFY(b.open(QIODevice::WriteOnly | QIODevice::Text));
        b.write("also keep this one\n");
        b.close();

        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto result = runTool(
            executor, QStringLiteral("grep"),
            {ToolInvocationArgument{QStringLiteral("pattern"), QStringLiteral("keep this")}},
            QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("a.txt:1: keep this line")));
        QVERIFY(result.summary.contains(QStringLiteral("b.txt:")));
        QVERIFY(result.structuredObservation);
        QCOMPARE(result.structuredObservation->data.value(QStringLiteral("matchCount")).toInt(), 2);
    }

    void globListsMatchingFiles() {
        QTemporaryDir dir;
        QVERIFY(QFile(dir.filePath(QStringLiteral("one.cpp"))).open(QIODevice::WriteOnly));
        QVERIFY(QFile(dir.filePath(QStringLiteral("two.cpp"))).open(QIODevice::WriteOnly));
        QVERIFY(QFile(dir.filePath(QStringLiteral("three.txt"))).open(QIODevice::WriteOnly));

        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("glob"),
                    {ToolInvocationArgument{QStringLiteral("pattern"), QStringLiteral("*.cpp")}},
                    QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("one.cpp")));
        QVERIFY(result.summary.contains(QStringLiteral("two.cpp")));
        QVERIFY(!result.summary.contains(QStringLiteral("three.txt")));
    }

    void todoToolsAreSessionIsolatedAndBounded() {
        RealToolExecutor executor;
        const auto workspace = QDir::currentPath();
        const auto read = [&](const QString& session) {
            return runTool(executor, QStringLiteral("todo-read"), {}, {}, workspace, nullptr,
                           session);
        };
        const QString todos =
            QStringLiteral("[{\"content\":\"test project\",\"status\":\"pending\"}]");
        const auto saved = runTool(
            executor, QStringLiteral("todo-write"),
            {{QStringLiteral("todos"), todos, QJsonDocument::fromJson(todos.toUtf8()).array()}}, {},
            workspace, nullptr, QStringLiteral("session-a"));
        QCOMPARE(saved.status, ToolExecutionStatus::Succeeded);
        QVERIFY(saved.structuredObservation);
        QCOMPARE(saved.structuredObservation->data.value(QStringLiteral("todos")).toArray().size(),
                 1);
        const auto other = read(QStringLiteral("session-b"));
        QCOMPARE(other.status, ToolExecutionStatus::Succeeded);
        QVERIFY(
            other.structuredObservation->data.value(QStringLiteral("todos")).toArray().isEmpty());
        QVERIFY(read(QStringLiteral("session-a")).summary.contains(QStringLiteral("test project")));
        QCOMPARE(read({}).status, ToolExecutionStatus::Blocked);
        const auto invalid = runTool(
            executor, QStringLiteral("todo-write"),
            {{QStringLiteral("todos"), QString(),
              QJsonArray{QJsonObject{{QStringLiteral("content"), QStringLiteral("bad")},
                                     {QStringLiteral("status"), QStringLiteral("invalid")}}}}},
            {}, workspace, nullptr, QStringLiteral("session-a"));
        QCOMPARE(invalid.status, ToolExecutionStatus::InvalidArguments);
        QVERIFY(read(QStringLiteral("session-a")).summary.contains(QStringLiteral("test project")));
    }

    void setAlarmAndListAlarmsRoundTrip() {
        QTemporaryDir dir;
        auto alarmStore = std::make_shared<AlarmStore>(dir.filePath(QStringLiteral("alarms.json")));

        RealToolExecutor executor;
        executor.setAlarmStore(alarmStore);

        const auto setResult =
            runTool(executor, QStringLiteral("set-alarm"),
                    {ToolInvocationArgument{QStringLiteral("time"), QStringLiteral("23:59")},
                     ToolInvocationArgument{QStringLiteral("label"), QStringLiteral("test alarm")}},
                    QStringList{});

        QCOMPARE(setResult.status, ToolExecutionStatus::Succeeded);
        QVERIFY(setResult.summary.contains(QStringLiteral("Alarm scheduled")));

        const auto listResult = runTool(executor, QStringLiteral("list-alarms"), {}, QStringList{});
        QCOMPARE(listResult.status, ToolExecutionStatus::Succeeded);
        QVERIFY(listResult.summary.contains(QStringLiteral("test alarm")));
    }

    void setAlarmRejectsUnparseableTime() {
        RealToolExecutor executor;
        executor.setAlarmStore(std::make_shared<AlarmStore>(QString()));

        const auto result =
            runTool(executor, QStringLiteral("set-alarm"),
                    {ToolInvocationArgument{QStringLiteral("time"), QStringLiteral("next tuesday")},
                     ToolInvocationArgument{QStringLiteral("label"), QStringLiteral("x")}},
                    QStringList{});

        QCOMPARE(result.status, ToolExecutionStatus::InvalidArguments);
        QVERIFY(result.summary.contains(QStringLiteral("Could not parse time")));
    }

    void alarmStorePersistsAndTakesDue() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("alarms.json"));
        {
            AlarmStore store(path);
            store.schedule(QDateTime::currentDateTime().addSecs(-10), QStringLiteral("past"));
            store.schedule(QDateTime::currentDateTime().addSecs(3600), QStringLiteral("future"));
        }

        AlarmStore reloaded(path);
        QCOMPARE(reloaded.active().size(), 2);

        const auto due = reloaded.takeDue(QDateTime::currentDateTime());
        QCOMPARE(due.size(), 1);
        QCOMPARE(due.first().label, QStringLiteral("past"));
        QCOMPARE(reloaded.active().size(), 1);
    }

    void rejectsPathOutsideWorkspace() {
        QTemporaryDir dir;
        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("read-file"),
                    {ToolInvocationArgument{QStringLiteral("path"), QStringLiteral("/etc/passwd")}},
                    QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Blocked);
        QVERIFY(result.summary.contains(QStringLiteral("Filesystem access denied for /etc/passwd")));
    }

    void deleteFileRemovesFileAndRefusesDirectories() {
        QTemporaryDir dir;
        QVERIFY(QFile(dir.filePath(QStringLiteral("doomed.txt"))).open(QIODevice::WriteOnly));
        QVERIFY(QDir(dir.filePath(QStringLiteral("keepdir"))).mkpath(QStringLiteral(".")));

        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto fileResult =
            runTool(executor, QStringLiteral("delete-file"),
                    {ToolInvocationArgument{QStringLiteral("path"), QStringLiteral("doomed.txt")}},
                    QStringList{});
        const auto dirResult =
            runTool(executor, QStringLiteral("delete-file"),
                    {ToolInvocationArgument{QStringLiteral("path"), QStringLiteral("keepdir")}},
                    QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(fileResult.status, ToolExecutionStatus::Succeeded);
        QVERIFY(fileResult.summary.contains(QStringLiteral("delete-file: Deleted")));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("doomed.txt"))));
        QCOMPARE(dirResult.status, ToolExecutionStatus::Failed);
        QVERIFY(dirResult.summary.contains(QStringLiteral("Refusing to delete a directory")));
        QVERIFY(QDir(dir.filePath(QStringLiteral("keepdir"))).exists());
    }

    void deleteFileRefusesPathOutsideWorkspace() {
        QTemporaryDir dir;
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        const auto protectedPath = outside.filePath(QStringLiteral("protected.txt"));
        QFile protectedFile(protectedPath);
        QVERIFY(protectedFile.open(QIODevice::WriteOnly));
        QCOMPARE(protectedFile.write("protected"), qint64(9));
        protectedFile.close();
        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("delete-file"),
                    {ToolInvocationArgument{QStringLiteral("path"), protectedPath}}, QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Blocked);
        QVERIFY(result.summary.contains(
            QStringLiteral("Filesystem access denied for %1").arg(protectedPath)));
        QVERIFY(protectedFile.open(QIODevice::ReadOnly));
        QCOMPARE(protectedFile.readAll(), QByteArray("protected"));
    }

    void moveFileRenamesWithinWorkspace() {
        QTemporaryDir dir;
        const QString source = dir.filePath(QStringLiteral("old-name.txt"));
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("payload\n");
        file.close();

        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto result = runTool(
            executor, QStringLiteral("move-file"),
            {ToolInvocationArgument{QStringLiteral("source"), QStringLiteral("old-name.txt")},
             ToolInvocationArgument{QStringLiteral("destination"), QStringLiteral("new-name.txt")}},
            QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("move-file: Moved")));
        QVERIFY(!QFile::exists(source));
        QFile moved(dir.filePath(QStringLiteral("new-name.txt")));
        QVERIFY(moved.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(moved.readAll()), QStringLiteral("payload\n"));
    }

    void moveFileRefusesExistingDestination() {
        QTemporaryDir dir;
        QVERIFY(QFile(dir.filePath(QStringLiteral("a.txt"))).open(QIODevice::WriteOnly));
        QVERIFY(QFile(dir.filePath(QStringLiteral("b.txt"))).open(QIODevice::WriteOnly));

        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto result = runTool(
            executor, QStringLiteral("move-file"),
            {ToolInvocationArgument{QStringLiteral("source"), QStringLiteral("a.txt")},
             ToolInvocationArgument{QStringLiteral("destination"), QStringLiteral("b.txt")}},
            QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Failed);
        QVERIFY(result.summary.contains(QStringLiteral("Already exists")));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("a.txt"))));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("b.txt"))));
    }

    void cancelAlarmRemovesScheduledAlarm() {
        QTemporaryDir dir;
        auto alarmStore = std::make_shared<AlarmStore>(dir.filePath(QStringLiteral("alarms.json")));
        const auto entry = alarmStore->schedule(QDateTime::currentDateTime().addSecs(3600),
                                                QStringLiteral("will cancel"));

        RealToolExecutor executor;
        executor.setAlarmStore(alarmStore);

        const auto cancelResult =
            runTool(executor, QStringLiteral("cancel-alarm"),
                    {ToolInvocationArgument{QStringLiteral("id"), entry.id}}, QStringList{});
        QCOMPARE(cancelResult.status, ToolExecutionStatus::Succeeded);
        QVERIFY(cancelResult.summary.contains(
            QStringLiteral("cancel-alarm: Alarm %1 cancelled").arg(entry.id)));
        QCOMPARE(alarmStore->active().size(), 0);

        const auto missingResult = runTool(
            executor, QStringLiteral("cancel-alarm"),
            {ToolInvocationArgument{QStringLiteral("id"), QStringLiteral("nope")}}, QStringList{});
        QVERIFY(missingResult.summary.contains(QStringLiteral("No active alarm with id nope")));
    }

    void openUrlRejectsNonHttpScheme() {
        RealToolExecutor executor;
        const auto result = runTool(
            executor, QStringLiteral("open-url"),
            {ToolInvocationArgument{QStringLiteral("url"), QStringLiteral("file:///etc/passwd")}},
            QStringList{});

        QCOMPARE(result.status, ToolExecutionStatus::InvalidArguments);
        QVERIFY(result.summary.contains(QStringLiteral("Only http and https URLs can be opened")));
    }

    void currentTimeReportsUtcAndEpoch() {
        RealToolExecutor executor;
        const auto result = runTool(executor, QStringLiteral("current-time"), {}, QStringList{});

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("current-time:")));
        QVERIFY(result.summary.contains(QStringLiteral("UTC:")));
        QVERIFY(result.summary.contains(QStringLiteral("Epoch seconds:")));
        QVERIFY(
            result.summary.contains(QString::number(QDateTime::currentDateTime().date().year())));
    }

    void systemInfoReportsPlatform() {
        RealToolExecutor executor;
        const auto result = runTool(executor, QStringLiteral("system-info"), {}, QStringList{});

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("OS:")));
        QVERIFY(result.summary.contains(QStringLiteral("CPU architecture:")));
        QVERIFY(result.summary.contains(QStringLiteral("Hostname:")));
        QVERIFY(result.structuredObservation);
        const auto paths = result.structuredObservation->data.value("knownDirectories").toObject();
        QCOMPARE(paths.value("desktop").toString(),
                 QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
        QCOMPARE(paths.value("home").toString(), QDir::homePath());
        QVERIFY(result.summary.contains("knownDirectories"));
    }

    void processListReportsProcessLines() {
        RealToolExecutor executor;
        const auto result = runTool(executor, QStringLiteral("process-list"), {}, QStringList{});

        QVERIFY(result.status == ToolExecutionStatus::Succeeded ||
                result.status == ToolExecutionStatus::Failed);
        QVERIFY(result.summary.contains(QStringLiteral("process-list:")));
    }

    void clipboardToolsFailGracefullyWithoutGuiSession() {
        // With a GUI session present the tools round-trip through the real
        // clipboard; without one they must report unavailability instead of
        // crashing. The original clipboard content is restored afterwards.
        const auto* guiApp = qobject_cast<const QGuiApplication*>(QCoreApplication::instance());
        if (!guiApp) {
            RealToolExecutor executor;
            const auto readResult =
                runTool(executor, QStringLiteral("clipboard-read"), {}, QStringList{});
            QCOMPARE(readResult.status, ToolExecutionStatus::Succeeded);
            QVERIFY(
                readResult.summary.contains(QStringLiteral("unavailable without a GUI session")));
            return;
        }

        QClipboard* clipboard = QGuiApplication::clipboard();
        const QString original = clipboard->text();
        const QString sample = QStringLiteral("sentinel-clipboard-test-42");
        clipboard->setText(sample);
        QCoreApplication::processEvents();
        if (clipboard->text() != sample)
            QSKIP("The active macOS test session does not provide a round-trippable clipboard.");

        RealToolExecutor executor;
        const auto writeResult =
            runTool(executor, QStringLiteral("clipboard-write"),
                    {ToolInvocationArgument{QStringLiteral("text"), sample}}, QStringList{});
        QCOMPARE(writeResult.status, ToolExecutionStatus::Succeeded);
        QVERIFY(writeResult.summary.contains(QStringLiteral("Copied 26 character(s)")));

        const auto readResult =
            runTool(executor, QStringLiteral("clipboard-read"), {}, QStringList{});
        QCOMPARE(readResult.status, ToolExecutionStatus::Succeeded);
        QVERIFY(readResult.summary.contains(sample));

        clipboard->setText(original);
    }

    void memorySearchFindsSnapshotEntries() {
        RealToolExecutor executor;
        InMemoryStore memory;
        const sentinel::core::MemoryAccessContext context{"session", "project", {"project"}, false};
        memory.writeScopedMemory({sentinel::core::MemoryScope::Session, "shopping", "buy oat milk"},
                                 context);
        executor.setMemoryAccessContext(context);
        executor.setSearchStores(&memory, nullptr);

        const auto result =
            runTool(executor, QStringLiteral("memory-search"),
                    {ToolInvocationArgument{QStringLiteral("query"), QStringLiteral("milk")}},
                    QStringList{});

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("match(es) for 'milk'")));
        QVERIFY(result.summary.contains(QStringLiteral("shopping")));
        QVERIFY(result.summary.contains(QStringLiteral("buy oat milk")));
        QVERIFY(result.summary.contains(QStringLiteral("untrusted historical data")));

        const auto noMatch = runTool(
            executor, QStringLiteral("memory-search"),
            {ToolInvocationArgument{QStringLiteral("query"), QStringLiteral("yzk")}}, QStringList{});
        QVERIFY(noMatch.summary.contains(QStringLiteral("No memory entries match 'yzk'")));
        sentinel::core::PlannedToolInvocation child;
        child.arguments = {ToolInvocationArgument{"query", "milk"}};
        child.runtimeSessionId = "delegated-child";
        QString cwd;
        const auto childResult = executor.executeMemorySearch(child, cwd);
        QCOMPARE(childResult.status, ToolExecutionStatus::Failed);
        QCOMPARE(childResult.failureCategory,
                 sentinel::core::ToolFailureCategory::PermissionDenied);
        QVERIFY(!childResult.summary.contains("oat milk"));
        auto other = context;
        other.sessionId = "other-session";
        other.workspaceId = "other-project";
        other.authorizedWorkspaceIds = {"other-project"};
        memory.writeScopedMemory(
            {sentinel::core::MemoryScope::Project, "shopping", "milk PROJECT_B"}, other);
        executor.setMemoryContextResolver([context, other](const QString& id) {
            return id == context.sessionId ? context
                   : id == other.sessionId ? other
                                           : sentinel::core::MemoryAccessContext{};
        });
        executor.setMemoryAccessContext(
            other); // A changed active selection must not reroute an old request.
        child.runtimeSessionId = context.sessionId;
        const auto original = executor.executeMemorySearch(child, cwd);
        QVERIFY(original.summary.contains("oat milk"));
        QVERIFY(!original.summary.contains("PROJECT_B"));
        child.runtimeSessionId = other.sessionId;
        const auto otherResult = executor.executeMemorySearch(child, cwd);
        QVERIFY(otherResult.summary.contains("PROJECT_B"));
        QVERIFY(!otherResult.summary.contains("oat milk"));
        executor.setMemoryAccessContext({});
        const auto denied = runTool(executor, QStringLiteral("memory-search"),
                                    {ToolInvocationArgument{"query", "milk"},
                                     ToolInvocationArgument{"workspace_id", "project"},
                                     ToolInvocationArgument{"session_id", "session"}},
                                    QStringList{});
        QCOMPARE(denied.status, ToolExecutionStatus::InvalidArguments);
        QVERIFY(!denied.summary.contains("oat milk"));
        const auto missingContext =
            runTool(executor, QStringLiteral("memory-search"),
                    {ToolInvocationArgument{"query", "milk"}}, QStringList{});
        QCOMPARE(missingContext.status, ToolExecutionStatus::Failed);
        QCOMPARE(missingContext.failureCategory,
                 sentinel::core::ToolFailureCategory::PermissionDenied);
        QVERIFY(!missingContext.summary.contains("oat milk"));
    }

    void memorySearchWithoutSnapshotIsGraceful() {
        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("memory-search"),
                    {ToolInvocationArgument{QStringLiteral("query"), QStringLiteral("anything")}},
                    QStringList{});

        QCOMPARE(result.status, ToolExecutionStatus::Failed);
        QCOMPARE(result.failureCategory, ToolFailureCategory::RuntimeUnavailable);
        QVERIFY(result.summary.contains(QStringLiteral("Memory store is unavailable")));
    }

    void appLaunchRedirectsDomainNamesToOpenUrl() {
        // Domains must never be launched as applications; the executor guides
        // the caller to open-url instead of spawning anything.
        RealToolExecutor executor;
        const auto result = runTool(
            executor, QStringLiteral("app-launch"),
            {ToolInvocationArgument{QStringLiteral("app"), QStringLiteral("sahibinden.com")}},
            QStringList{});

        if (result.failureCategory == ToolFailureCategory::Unsupported) {
            // Linux registers app-launch as platform-unsupported, so the
            // gateway refuses the call before any launch happens.
            QCOMPARE(result.status, ToolExecutionStatus::Failed);
            QVERIFY(result.summary.contains(QStringLiteral("unsupported on this platform")));
            return;
        }
        QCOMPARE(result.status, ToolExecutionStatus::InvalidArguments);
        QVERIFY(result.summary.contains(QStringLiteral("open-url")));
    }

    void applyPatchUpdatesExistingFile() {
        QTemporaryDir dir;
        QFile file(dir.filePath(QStringLiteral("app.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("alpha\nbeta\ngamma\n");
        file.close();

        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        const QString patch = QStringLiteral("--- a/app.txt\n"
                                             "+++ b/app.txt\n"
                                             "@@ -1,3 +1,3 @@\n"
                                             " alpha\n"
                                             "-beta\n"
                                             "+BETA\n"
                                             " gamma\n");

        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("apply-patch"),
                    {ToolInvocationArgument{QStringLiteral("patch"), patch}}, QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("app.txt: applied")));

        QFile updated(dir.filePath(QStringLiteral("app.txt")));
        QVERIFY(updated.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(updated.readAll()), QStringLiteral("alpha\nBETA\ngamma"));
    }

    void applyPatchAddsAndDeletesFiles() {
        QTemporaryDir dir;
        QVERIFY(QFile(dir.filePath(QStringLiteral("old.txt"))).open(QIODevice::WriteOnly));

        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        const QString patch = QStringLiteral("--- /dev/null\n"
                                             "+++ b/new.txt\n"
                                             "@@ -0,0 +1,2 @@\n"
                                             "+first\n"
                                             "+second\n"
                                             "--- a/old.txt\n"
                                             "+++ /dev/null\n"
                                             "@@ -1 +0,0 @@\n"
                                             "-content\n");

        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("apply-patch"),
                    {ToolInvocationArgument{QStringLiteral("patch"), patch}}, QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("new.txt: applied")));
        QVERIFY(result.summary.contains(QStringLiteral("old.txt: applied")));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("old.txt"))));

        QFile created(dir.filePath(QStringLiteral("new.txt")));
        QVERIFY(created.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(created.readAll()), QStringLiteral("first\nsecond"));
    }

    void applyPatchReportsContextMismatch() {
        QTemporaryDir dir;
        QFile file(dir.filePath(QStringLiteral("app.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("one\ntwo\nthree\n");
        file.close();

        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        const QString patch = QStringLiteral("--- a/app.txt\n"
                                             "+++ b/app.txt\n"
                                             "@@ -1,3 +1,3 @@\n"
                                             " wrong-context\n"
                                             "-two\n"
                                             "+TWO\n"
                                             " three\n");

        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("apply-patch"),
                    {ToolInvocationArgument{QStringLiteral("patch"), patch}}, QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Failed);
        QVERIFY(result.summary.contains(QStringLiteral("did not match")));

        // The file must remain untouched.
        QFile unchanged(dir.filePath(QStringLiteral("app.txt")));
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(unchanged.readAll()), QStringLiteral("one\ntwo\nthree\n"));
    }

    void applyPatchRejectsInvalidPatchText() {
        RealToolExecutor executor;
        const auto result = runTool(executor, QStringLiteral("apply-patch"),
                                    {ToolInvocationArgument{QStringLiteral("patch"),
                                                            QStringLiteral("this is not a patch")}},
                                    QStringList{});

        QCOMPARE(result.status, ToolExecutionStatus::InvalidArguments);
        QVERIFY(result.summary.contains(QStringLiteral("No file sections found")));
    }

    void listCodeDefinitionsExtractsSymbols() {
        QTemporaryDir dir;
        QFile file(dir.filePath(QStringLiteral("sample.py")));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("class Greeter:\n"
                   "    def greet(self):\n"
                   "        return 1\n"
                   "\n"
                   "def main():\n"
                   "    pass\n");
        file.close();

        const auto oldCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));

        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("list-code-definitions"),
                    {ToolInvocationArgument{QStringLiteral("path"), QStringLiteral("sample.py")}},
                    QStringList{});

        QVERIFY(QDir::setCurrent(oldCwd));

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("class Greeter")));
        QVERIFY(result.summary.contains(QStringLiteral("def greet")));
        QVERIFY(result.summary.contains(QStringLiteral("def main")));
    }

    void historySearchFindsSnapshotEntries() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SQLiteChatHistoryStore history(dir.filePath(QStringLiteral("history.sqlite3")));
        QVERIFY(history.isAvailable());
        ChatMessage user;
        user.id = 1;
        user.role = ChatRole::User;
        user.content = QStringLiteral("bisiklet tamir etmem lazım");
        user.timestamp = QDateTime::currentDateTimeUtc();
        history.appendMessage(user);
        QVERIFY2(history.lastError().isEmpty(), qPrintable(history.lastError()));
        ChatMessage assistant;
        assistant.id = 2;
        assistant.role = ChatRole::Assistant;
        assistant.content = QStringLiteral("hangi parça sorunlu?");
        assistant.timestamp = QDateTime::currentDateTimeUtc();
        history.appendMessage(assistant);
        QVERIFY2(history.lastError().isEmpty(), qPrintable(history.lastError()));
        RealToolExecutor executor;
        executor.setSearchStores(nullptr, &history);

        const auto result =
            runTool(executor, QStringLiteral("history-search"),
                    {ToolInvocationArgument{QStringLiteral("query"), QStringLiteral("bisiklet")}},
                    QStringList{});

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QVERIFY(result.summary.contains(QStringLiteral("match(es) for 'bisiklet'")));
        QVERIFY(result.summary.contains(QStringLiteral("[user] bisiklet")));

        const auto missing =
            runTool(executor, QStringLiteral("history-search"),
                    {ToolInvocationArgument{QStringLiteral("query"), QStringLiteral("xyzzy")}},
                    QStringList{});
        QVERIFY(missing.summary.contains(QStringLiteral("No history entries match 'xyzzy'")));
    }

    void historySearchWithoutSnapshotIsGraceful() {
        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("history-search"),
                    {ToolInvocationArgument{QStringLiteral("query"), QStringLiteral("anything")}},
                    QStringList{});

        QCOMPARE(result.status, ToolExecutionStatus::Failed);
        QCOMPARE(result.failureCategory, ToolFailureCategory::RuntimeUnavailable);
        QVERIFY(result.summary.contains(QStringLiteral("Chat history store is unavailable")));
    }

    void spawnAgentRunsInjectedSubagent() {
        RealToolExecutor executor;
        QStringList receivedTasks;
        executor.setSubagentRunner([&receivedTasks](const QString& task) {
            receivedTasks.append(task);
            return QStringLiteral("subagent answer for: %1").arg(task);
        });

        const auto result = runTool(
            executor, QStringLiteral("spawn-agent"),
            {ToolInvocationArgument{QStringLiteral("task"), QStringLiteral("count the TODOs")},
             ToolInvocationArgument{QStringLiteral("purpose"),
                                    QStringLiteral("parallel_verification")},
             ToolInvocationArgument{QStringLiteral("workspace"), QStringLiteral(".")}},
            QStringList{});

        QCOMPARE(result.status, ToolExecutionStatus::Succeeded);
        QCOMPARE(receivedTasks.size(), 1);
        QCOMPARE(receivedTasks.first(), QStringLiteral("count the TODOs"));
        QVERIFY(result.summary.contains(QStringLiteral("subagent finished")));
        QVERIFY(result.summary.contains(QStringLiteral("subagent answer for: count the TODOs")));
    }

    void spawnAgentWithoutRunnerIsGraceful() {
        RealToolExecutor executor;
        const auto result = runTool(
            executor, QStringLiteral("spawn-agent"),
            {ToolInvocationArgument{QStringLiteral("task"), QStringLiteral("anything")},
             ToolInvocationArgument{QStringLiteral("purpose"), QStringLiteral("specialist_review")},
             ToolInvocationArgument{QStringLiteral("workspace"), QStringLiteral(".")}},
            QStringList{});

        QCOMPARE(result.status, ToolExecutionStatus::Blocked);
        QVERIFY(result.summary.contains(QStringLiteral("No subagent runner is configured")));
    }

    void spawnAgentRequiresPurpose() {
        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("spawn-agent"),
                    {ToolInvocationArgument{QStringLiteral("task"), QStringLiteral("anything")}},
                    QStringList{});
        QCOMPARE(result.status, ToolExecutionStatus::InvalidArguments);
        QVERIFY(result.summary.contains(QStringLiteral("$.purpose")));
    }

    void spawnAgentRequiresTask() {
        RealToolExecutor executor;
        const auto result =
            runTool(executor, QStringLiteral("spawn-agent"),
                    {ToolInvocationArgument{QStringLiteral("purpose"),
                                            QStringLiteral("independent_research")}},
                    QStringList{});
        QCOMPARE(result.status, ToolExecutionStatus::InvalidArguments);
        QVERIFY(result.summary.contains(QStringLiteral("$.task")));
    }

    void runCommandDockerSandboxReportsMissingDocker() {
        // Resolve/cache the real sandbox launcher before isolating PATH.
        const PlatformProcessSandbox backend;
        Q_UNUSED(backend)
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto originalPath = qgetenv("PATH");
        const auto restorePath = qScopeGuard([&] { qputenv("PATH", originalPath); });
        QVERIFY(qputenv("PATH", workspace.path().toLocal8Bit()));
        QVERIFY(QStandardPaths::findExecutable("docker").isEmpty());
        RealToolExecutor executor;
        const auto result = runTool(
            executor, "run-command",
            {{"command", "fixture-must-not-execute"}, {"sandbox", "docker"}}, {}, workspace.path());
        QCOMPARE(result.status, ToolExecutionStatus::Failed);
        QVERIFY2(result.summary.contains("docker CLI is not available"),
                 qPrintable(result.summary));
        QVERIFY(result.sandbox.enforcement != SandboxEnforcement::Enforced);
    }

    void runCommandDockerFixtureContracts_data() {
        QTest::addColumn<QString>("command");
        QTest::addColumn<ToolExecutionStatus>("expectedStatus");
        QTest::addColumn<QString>("expectedSummary");
        QTest::newRow("success") << QString("fixture-success") << ToolExecutionStatus::Succeeded
                                 << QString("DOCKER_FIXTURE_OK");
        QTest::newRow("daemon-unavailable")
            << QString("fixture-daemon-unavailable") << ToolExecutionStatus::Failed
            << QString("daemon is not running");
        QTest::newRow("command-failure")
            << QString("fixture-command-failure") << ToolExecutionStatus::Failed
            << QString("DOCKER_FIXTURE_FAILURE");
    }

    void runCommandDockerFixtureContracts() {
        QFETCH(QString, command);
        QFETCH(ToolExecutionStatus, expectedStatus);
        QFETCH(QString, expectedSummary);
        const PlatformProcessSandbox backend;
        Q_UNUSED(backend)
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const QString program = workspace.filePath(
#ifdef Q_OS_WIN
            "docker.exe"
#else
            "docker"
#endif
        );
        QVERIFY(QFile::copy(QString::fromUtf8(DOCKER_CLI_FIXTURE), program));
        QVERIFY(
            QFile::setPermissions(program, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto originalPath = qgetenv("PATH");
        const auto restorePath = qScopeGuard([&] { qputenv("PATH", originalPath); });
        QVERIFY(qputenv("PATH", workspace.path().toLocal8Bit()));
        QCOMPARE(QFileInfo(QStandardPaths::findExecutable("docker")).canonicalFilePath(),
                 QFileInfo(program).canonicalFilePath());
        RealToolExecutor executor;
        const auto result =
            runTool(executor, "run-command", {{"command", command}, {"sandbox", "docker"}}, {},
                    workspace.path());
#ifdef Q_OS_WIN
        // The production Windows backend cannot enforce filesystem/network
        // confinement. Preserve its fail-closed contract instead of relaxing it.
        QCOMPARE(result.status, ToolExecutionStatus::Failed);
        QVERIFY(result.sandbox.enforcement != SandboxEnforcement::Enforced);
        QVERIFY(result.summary.contains("Sandbox denied launch"));
#else
        QCOMPARE(result.status, expectedStatus);
        QVERIFY2(result.summary.contains(expectedSummary), qPrintable(result.summary));
        QCOMPARE(result.sandbox.enforcement, SandboxEnforcement::Enforced);
        QVERIFY(result.sandbox.networkDenied);
        QVERIFY(result.sandbox.processTreeControlled);
#endif
    }

    void browserToolsReportMissingNodeGracefully() {
        // Playwright tools require Node.js (npx). Without it they must return
        // install guidance rather than crash.
        const bool hasNode =
            QProcess::execute(QStringLiteral("npx"), {QStringLiteral("--version")}) == 0;
        if (hasNode) {
            QSKIP("npx is installed on this machine; the missing-node branch cannot run.");
        }

        RealToolExecutor executor;
        const auto screenshot = runTool(
            executor, QStringLiteral("browser-screenshot"),
            {ToolInvocationArgument{QStringLiteral("url"), QStringLiteral("https://example.com")}},
            QStringList{});
        QCOMPARE(screenshot.status, ToolExecutionStatus::Succeeded);
        QVERIFY(screenshot.summary.contains(QStringLiteral("Node.js")));

        const auto pdf = runTool(
            executor, QStringLiteral("browser-pdf"),
            {ToolInvocationArgument{QStringLiteral("url"), QStringLiteral("https://example.com")}},
            QStringList{});
        QCOMPARE(pdf.status, ToolExecutionStatus::Succeeded);
        QVERIFY(pdf.summary.contains(QStringLiteral("Node.js")));
    }

    void browserToolsRequireUrl() {
        RealToolExecutor executor;
        const auto screenshot =
            runTool(executor, QStringLiteral("browser-screenshot"), {}, QStringList{});
        QCOMPARE(screenshot.status, ToolExecutionStatus::InvalidArguments);
        QVERIFY(screenshot.summary.contains(QStringLiteral("$.url")));

        const auto pdf = runTool(executor, QStringLiteral("browser-pdf"), {}, QStringList{});
        QCOMPARE(pdf.status, ToolExecutionStatus::InvalidArguments);
        QVERIFY(pdf.summary.contains(QStringLiteral("$.url")));
    }
};

QTEST_MAIN(RealToolExecutorToolsTest)
#include "test_real_tool_executor_tools.moc"
