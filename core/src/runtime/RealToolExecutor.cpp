// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/runtime/RealToolExecutor.h"
#include "sentinel/core/chat/IChatHistoryStore.h"
#include "sentinel/core/editor/FuzzyEditor.h"
#include "sentinel/core/mcp/McpService.h"
#include "sentinel/core/runtime/BuiltInToolProvider.h"
#include "sentinel/core/runtime/FileSystemPatch.h"
#include "sentinel/core/runtime/ToolArgumentValidator.h"
#include "sentinel/core/security/ExternalDirectoryGate.h"
#include "sentinel/core/security/PathGuard.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <optional>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#elif defined(Q_OS_WIN)
#define NOMINMAX
#include <windows.h>
#endif

namespace sentinel::core {

namespace {
template <typename T>
ToolExecutionResult fileToolFailure(const QString& tool, const FileSystemResult<T>& result);
FileSystemResult<AuthorizedPath> authorizedFilePath(const IFileSystemService& service,
                                                    const PlannedToolInvocation& invocation,
                                                    const QString& argument, AccessMode access,
                                                    const QString& cwd);
} // namespace

RealToolExecutor::RealToolExecutor() = default;

RealToolExecutor::RealToolExecutor(std::shared_ptr<AlarmStore> alarmStore)
    : alarmStore_(std::move(alarmStore)) {}

void RealToolExecutor::setAlarmStore(std::shared_ptr<AlarmStore> alarmStore) {
    alarmStore_ = std::move(alarmStore);
}

void RealToolExecutor::configureMcpServers(const QList<McpServerConfig>& configs) {
    auto service = std::make_shared<McpService>();
    for (const auto& config : configs) {
        if (config.name.trimmed().isEmpty()) {
            continue;
        }
        service->addServer(config);
    }
    service->connectToAll();
    mcpService_ = std::move(service);
}

void RealToolExecutor::setMcpService(std::shared_ptr<IMcpService> service) {
    mcpService_ = std::move(service);
}

void RealToolExecutor::setSubagentRunner(std::function<QString(const QString& task)> runner) {
    subagentRunner_ = [runner = std::move(runner)](const SubagentAssignment& assignment,
                                                   const QString&) -> ToolExecutionResult {
        return {ToolExecutionStatus::Succeeded, runner(assignment.goal)};
    };
}

void RealToolExecutor::setSubagentRunnerWithContext(
    std::function<ToolExecutionResult(const SubagentAssignment&, const QString&)> runner) {
    subagentRunner_ = std::move(runner);
}

void RealToolExecutor::configureWebSearch(const QString& provider, const QString& apiKey,
                                          int maxResults) {
    webSearchTool_.setSearchProvider(provider);
    webSearchTool_.setApiKey(apiKey);
    webSearchTool_.setMaxResults(maxResults);
}

WebSearchResponse RealToolExecutor::searchWeb(const QString& query) const {
    return webSearchTool_.search(query);
}

namespace {
QString scopedPath(const QString& workingDirectory, const QString& rawPath);
[[maybe_unused]] QString osascriptEscape(const QString& text);
bool looksLikeDomainName(const QString& text);
} // namespace

namespace {
ToolExecutionResult validateCompatibilityRequest(ToolExecutionRequest& request) {
    if (request.plan.status != ToolInvocationPlanStatus::Planned)
        return {ToolExecutionStatus::Succeeded, {}};
    const auto descriptors = BuiltInToolProvider::descriptors();
    for (auto& invocation : request.plan.invocations) {
        const auto found =
            std::find_if(descriptors.begin(), descriptors.end(),
                         [&](const ToolDescriptor& tool) { return tool.id == invocation.toolId; });
        if (found == descriptors.end())
            return {ToolExecutionStatus::UnknownTool,
                    QStringLiteral("Unknown built-in tool: %1").arg(invocation.toolId)};
        const auto validation = ToolArgumentValidator::validate(*found, invocation.arguments);
        if (!validation.valid) {
            QStringList details;
            for (const auto& issue : validation.errors.mid(0, 8))
                details.append(
                    QStringLiteral("%1 [%2]: %3").arg(issue.path, issue.keyword, issue.message));
            return {ToolExecutionStatus::InvalidArguments, details.join(QLatin1Char('\n'))};
        }
        invocation.arguments =
            ToolArgumentValidator::toInvocationArguments(validation.normalizedArguments);
    }
    return {ToolExecutionStatus::Succeeded, {}};
}
} // namespace

IToolExecutor::Cancel RealToolExecutor::executeAsync(const ToolExecutionRequest& originalRequest,
                                                     const QString& sessionId,
                                                     const QString& toolCallId, Output output,
                                                     Completion completion) const {
    ToolExecutionRequest normalizedRequest = originalRequest;
    const auto validation = validateCompatibilityRequest(normalizedRequest);
    if (validation.status != ToolExecutionStatus::Succeeded) {
        completion(validation);
        return {};
    }
    const auto& request = normalizedRequest;
    if (request.plan.status == ToolInvocationPlanStatus::Planned &&
        request.plan.invocations.size() > 1 &&
        (request.approval.status == ApprovalStatus::Approved ||
         request.approval.status == ApprovalStatus::NotRequired) &&
        request.sandbox.status != SandboxStatus::Denied &&
        request.sandbox.status != SandboxStatus::BlockedByApproval) {
        struct Batch {
            ToolExecutionRequest request;
            Output output;
            Completion completion;
            Cancel currentCancel;
            QStringList summaries;
            std::function<void()> next;
            int index = 0;
            bool finished = false;
            bool cancelled = false;
            bool active = false;
        };
        auto batch = std::make_shared<Batch>();
        batch->request = request;
        batch->output = std::move(output);
        batch->completion = std::move(completion);
        std::weak_ptr<Batch> weak = batch;
        batch->next = [this, weak, sessionId, toolCallId] {
            auto current = weak.lock();
            if (!current || current->finished)
                return;
            if (current->cancelled) {
                current->finished = true;
                current->completion(
                    {ToolExecutionStatus::Cancelled, QStringLiteral("Tool execution cancelled.")});
                current->next = {};
                return;
            }
            if (current->index >= current->request.plan.invocations.size()) {
                current->finished = true;
                current->completion({ToolExecutionStatus::Succeeded,
                                     current->summaries.join(QStringLiteral("\n\n"))});
                current->next = {};
                return;
            }
            ToolExecutionRequest one = current->request;
            one.plan.invocations = {current->request.plan.invocations.at(current->index++)};
            current->active = true;
            current->currentCancel = executeAsync(
                one, sessionId, toolCallId, current->output, [weak](ToolExecutionResult result) {
                    auto state = weak.lock();
                    if (!state || state->finished)
                        return;
                    state->active = false;
                    state->currentCancel = {};
                    if (state->cancelled) {
                        state->finished = true;
                        state->completion({ToolExecutionStatus::Cancelled,
                                           QStringLiteral("Tool execution cancelled.")});
                        state->next = {};
                        return;
                    }
                    if (result.status != ToolExecutionStatus::Succeeded &&
                        result.status != ToolExecutionStatus::PlaceholderSucceeded) {
                        state->finished = true;
                        state->completion(std::move(result));
                        state->next = {};
                        return;
                    }
                    state->summaries.append(std::move(result.summary));
                    QTimer::singleShot(0, [weak] {
                        if (auto next = weak.lock(); next && next->next)
                            next->next();
                    });
                });
            if (!current->active)
                current->currentCancel = {};
        };
        batch->next();
        return [batch] {
            if (batch->finished)
                return;
            batch->cancelled = true;
            if (batch->active && batch->currentCancel) {
                batch->currentCancel();
            } else {
                batch->finished = true;
                batch->completion(
                    {ToolExecutionStatus::Cancelled, QStringLiteral("Tool execution cancelled.")});
                batch->next = {};
            }
        };
    }
    if (request.plan.invocations.size() != 1 ||
        request.plan.status != ToolInvocationPlanStatus::Planned ||
        !request.knownToolIds.contains(request.plan.invocations.first().toolId) ||
        (request.approval.status != ApprovalStatus::Approved &&
         request.approval.status != ApprovalStatus::NotRequired) ||
        request.sandbox.status == SandboxStatus::Denied ||
        request.sandbox.status == SandboxStatus::BlockedByApproval) {
        return IToolExecutor::executeAsync(request, sessionId, toolCallId, std::move(output),
                                           std::move(completion));
    }

    const auto& invocation = request.plan.invocations.first();
    auto argument = [&invocation](const QString& id) {
        for (const auto& item : invocation.arguments) {
            if (item.id == id)
                return item.value;
        }
        return QString{};
    };
    const QString toolId = invocation.toolId;
    if (toolId == QLatin1String("voice-transcribe") || toolId == QLatin1String("voice-speak")) {
        const auto stt = sttRuntime_;
        const auto tts = ttsRuntime_;
        const auto cwd = invocation.processSandbox ? invocation.processSandbox->workingDirectory
                                                   : QDir::currentPath();
        std::optional<AuthorizedPath> audioPath;
        if (toolId == QLatin1String("voice-transcribe")) {
            const auto authorized = authorizedFilePath(
                fileSystemService_, invocation, QStringLiteral("path"), AccessMode::Read, cwd);
            if (!authorized.ok()) {
                completion(fileToolFailure(toolId, authorized));
                return {};
            }
            audioPath = *authorized.value;
        }
        const auto speechText = argument(QStringLiteral("text"));
        if (toolId == QLatin1String("voice-speak") && speechText.trimmed().isEmpty()) {
            completion({ToolExecutionStatus::InvalidArguments,
                        QStringLiteral("voice-speak: Text is empty.")});
            return {};
        }
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        QPointer<QObject> context(request.callbackContext);
        auto* worker = QThread::create([this, toolId, stt, tts, audioPath, cwd, speechText,
                                        cancelled, context, completion] {
            ToolExecutionResult result;
            AudioFailure failure = AudioFailure::None;
            QString detail;
            if (toolId == QLatin1String("voice-transcribe")) {
                if (!stt)
                    failure = AudioFailure::RuntimeUnavailable;
                else {
                    const auto verified = fileSystemService_.revalidateAuthorized(*audioPath, cwd);
                    const auto currentGrant = fileSystemService_.resolve(
                        audioPath->canonicalPath, cwd, FileSystemAccess::Read);
                    if (!verified.ok() || !currentGrant.ok() ||
                        currentGrant.value->canonicalPath != audioPath->canonicalPath ||
                        currentGrant.value->deviceId != audioPath->deviceId ||
                        currentGrant.value->fileId != audioPath->fileId)
                        failure = AudioFailure::PermissionDenied;
                    else {
                        const auto file = fileSystemService_.stat(*verified.value);
                        if (!file.ok() || !file.value->regularFile)
                            failure = AudioFailure::UnsupportedFormat;
                        else {
                            const auto transcript =
                                stt->transcribeFile(verified.value->canonicalPath, {}, cancelled);
                            failure = transcript.failure;
                            detail = transcript.detail;
                            if (failure == AudioFailure::None)
                                result.summary = QStringLiteral("voice-transcribe: OK\n%1")
                                                     .arg(transcript.finalText);
                        }
                    }
                }
            } else if (!tts)
                failure = AudioFailure::RuntimeUnavailable;
            else {
                const auto audio =
                    tts->synthesize(SpeechSynthesisRequest{speechText, {}, {}, 1.0}, cancelled);
                failure = audio.failure;
                detail = audio.detail;
                if (cancelled->load() && !audio.filePath.isEmpty())
                    QFile::remove(audio.filePath);
                if (failure == AudioFailure::None && !cancelled->load())
                    result.summary =
                        QStringLiteral("voice-speak: audio generated at %1").arg(audio.filePath);
            }
            if (cancelled->load())
                failure = AudioFailure::Cancelled;
            result.status = failure == AudioFailure::None        ? ToolExecutionStatus::Succeeded
                            : failure == AudioFailure::Cancelled ? ToolExecutionStatus::Cancelled
                                                                 : ToolExecutionStatus::Failed;
            if (failure != AudioFailure::None) {
                result.summary = QStringLiteral("%1: %2. %3")
                                     .arg(toolId, audioFailureName(failure), detail.left(300));
                result.failureCategory =
                    failure == AudioFailure::Timeout     ? ToolFailureCategory::Timeout
                    : failure == AudioFailure::Cancelled ? ToolFailureCategory::Cancelled
                    : failure == AudioFailure::RuntimeUnavailable ||
                            failure == AudioFailure::ModelUnavailable
                        ? ToolFailureCategory::RuntimeUnavailable
                    : failure == AudioFailure::PermissionDenied
                        ? ToolFailureCategory::PermissionDenied
                        : ToolFailureCategory::InternalFailure;
            }
            if (context)
                QMetaObject::invokeMethod(
                    context,
                    [completion, result = std::move(result)]() mutable {
                        completion(std::move(result));
                    },
                    Qt::QueuedConnection);
            else
                completion(std::move(result));
        });
        QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
        return [cancelled] { cancelled->store(true); };
    }
    if (toolId == QLatin1String("spawn-agent")) {
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        QPointer<QObject> context(request.callbackContext);
        auto complete = [context, completion](ToolExecutionResult result) {
            if (context) {
                QMetaObject::invokeMethod(
                    context,
                    [completion, result = std::move(result)]() mutable {
                        completion(std::move(result));
                    },
                    Qt::QueuedConnection);
            } else {
                completion(std::move(result));
            }
        };
        auto* worker = QThread::create([this, request, complete, cancelled] {
            if (cancelled->load()) {
                complete({ToolExecutionStatus::Cancelled,
                          QStringLiteral("Subagent execution cancelled.")});
                return;
            }
            auto result = execute(request);
            if (cancelled->load())
                result = {ToolExecutionStatus::Cancelled,
                          QStringLiteral("Subagent execution cancelled.")};
            complete(std::move(result));
        });
        QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
        return [cancelled] { cancelled->store(true); };
    }
    if (toolId != QLatin1String("run-command")) {
        static const QSet<QString> processTools{
            QStringLiteral("git-status"),    QStringLiteral("git-diff"),
            QStringLiteral("process-list"),  QStringLiteral("app-quit"),
            QStringLiteral("system-notify"), QStringLiteral("browser-screenshot"),
            QStringLiteral("browser-pdf"),   QStringLiteral("app-launch"),
            QStringLiteral("open-url")};
        if (!processTools.contains(toolId))
            return IToolExecutor::executeAsync(request, sessionId, toolCallId, std::move(output),
                                               std::move(completion));
        ProcessRequest process;
        process.sessionId = sessionId;
        process.toolCallId = toolCallId;
        process.timeoutMs = 15000;
        if (!invocation.processSandbox) {
            completion(
                {ToolExecutionStatus::Blocked, QStringLiteral("Process sandbox plan is missing.")});
            return {};
        }
        process.sandbox = invocation.processSandbox;
        process.workingDirectory = invocation.processSandbox->workingDirectory;
        std::function<QString(const ProcessRecord&, const QString&, const QString&)> format;
        QByteArray input;
        std::shared_ptr<QTemporaryDir> outputDirectory;
        if (toolId == QLatin1String("git-status") || toolId == QLatin1String("git-diff")) {
            const auto path =
                authorizedFilePath(fileSystemService_, invocation, QStringLiteral("path"),
                                   AccessMode::Read, process.workingDirectory);
            if (!path.ok()) {
                completion(fileToolFailure(toolId, path));
                return {};
            }
            const auto entry = fileSystemService_.stat(*path.value);
            if (!entry.ok()) {
                completion(fileToolFailure(toolId, entry));
                return {};
            }
            if (!entry.value->directory) {
                completion({ToolExecutionStatus::InvalidArguments,
                            QStringLiteral("Git path must be a repository directory.")});
                return {};
            }
            process.program = QStringLiteral("git");
            process.sandbox->isolateGitConfiguration = true;
            process.workingDirectory = path.value->canonicalPath;
            // No model text becomes a shell command or a Git option.
            process.arguments = {
                QStringLiteral("--no-pager"), QStringLiteral("--no-optional-locks"),
                QStringLiteral("-c"),         QStringLiteral("core.fsmonitor=false"),
                QStringLiteral("-c"),         QStringLiteral("core.untrackedCache=false"),
                QStringLiteral("-c"),         QStringLiteral("color.ui=false")};
            if (toolId == QLatin1String("git-status"))
                process.arguments.append({QStringLiteral("status"),
                                          QStringLiteral("--porcelain=v1"),
                                          QStringLiteral("--untracked-files=normal"),
                                          QStringLiteral("--ignore-submodules=all")});
            else {
                process.arguments.append({QStringLiteral("diff"), QStringLiteral("--no-ext-diff"),
                                          QStringLiteral("--no-textconv"),
                                          QStringLiteral("--ignore-submodules=all")});
                if (argument(QStringLiteral("staged")) == QLatin1String("true"))
                    process.arguments.append(QStringLiteral("--cached"));
                process.arguments.append(QStringLiteral("--"));
            }
            process.environment = QProcessEnvironment::systemEnvironment();
            for (const auto& key : process.environment.keys())
                if (key.startsWith(QStringLiteral("GIT_")))
                    process.environment.remove(key);
            process.environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
            process.environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
            process.environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
#if defined(Q_OS_WIN)
            process.environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("NUL"));
#else
            process.environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"),
                                       QStringLiteral("/dev/null"));
#endif
            format = [toolId](const ProcessRecord& record, const QString& out, const QString& err) {
                return record.state == ProcessState::Exited && record.exitCode == 0
                           ? out
                           : QStringLiteral("%1 failed (exit=%2): %3 %4")
                                 .arg(toolId)
                                 .arg(record.exitCode)
                                 .arg(record.error, err);
            };
        } else if (toolId == QLatin1String("process-list")) {
#if defined(Q_OS_WIN)
            process.program = QStringLiteral("tasklist");
#else
            process.program = QStringLiteral("ps");
            process.arguments = {QStringLiteral("-eo"), QStringLiteral("pid,pcpu,comm")};
#endif
            format = [](const ProcessRecord& record, const QString& out, const QString& err) {
                if (record.timedOut || record.state == ProcessState::Failed)
                    return QStringLiteral("process-list: %1")
                        .arg(record.timedOut          ? QStringLiteral("Timed out.")
                             : record.error.isEmpty() ? err
                                                      : record.error);
                if (out.isEmpty())
                    return QStringLiteral("process-list: No process output was returned.");
                const auto lines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                QStringList shown;
                for (int i = 0; i < lines.size() && i < 40; ++i)
                    shown.append(lines.at(i).left(2000).trimmed());
                return QStringLiteral("process-list: %1 process line(s) (showing first %2):\n%3")
                    .arg(lines.size())
                    .arg(shown.size())
                    .arg(shown.join(QLatin1Char('\n')));
            };
        } else if (toolId == QLatin1String("app-quit")) {
            const QString app = argument(QStringLiteral("app")).trimmed();
            if (app.isEmpty()) {
                completion({ToolExecutionStatus::InvalidArguments,
                            QStringLiteral("app-quit: No app argument provided.")});
                return {};
            }
#if defined(Q_OS_MACOS)
            process.program = QStringLiteral("osascript");
            process.arguments = {QStringLiteral("-e"),
                                 QStringLiteral("quit app \"%1\"").arg(osascriptEscape(app))};
#elif defined(Q_OS_WIN)
            process.program = QStringLiteral("taskkill");
            process.arguments = {QStringLiteral("/IM"),
                                 app.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)
                                     ? app
                                     : app + QStringLiteral(".exe"),
                                 QStringLiteral("/F")};
#else
            process.program = QStringLiteral("pkill");
            process.arguments = {QStringLiteral("-f"), QStringLiteral("-i"), app};
#endif
            format = [app](const ProcessRecord& record, const QString&, const QString& err) {
                const QString error = record.timedOut ? QStringLiteral("Timed out.")
                                      : record.state == ProcessState::Failed ? record.error
                                      : record.exitCode != 0                 ? err
                                                                             : QString();
                return error.isEmpty()
                           ? QStringLiteral("app-quit: Quit requested for '%1'.").arg(app)
                           : QStringLiteral("app-quit: %1 (%2)").arg(error, app);
            };
        } else if (toolId == QLatin1String("app-launch")) {
            const QString app = argument(QStringLiteral("app")).trimmed();
            if (app.isEmpty() || looksLikeDomainName(app)) {
                QString cwd = QDir::currentPath();
                completion(executeAppLaunch(invocation, cwd));
                return {};
            }
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
            completion(
                {ToolExecutionStatus::Blocked,
                 QStringLiteral(
                     "app-launch: detached desktop launch has no process sandbox guarantee.")});
            return {};
#else
            const QString extra = argument(QStringLiteral("args")).trimmed();
#if defined(Q_OS_MACOS)
            process.program = QStringLiteral("open");
            process.arguments = {QStringLiteral("-a"), app};
            if (!extra.isEmpty())
                process.arguments.append(extra);
#else
            process.program = QStringLiteral("cmd.exe");
            process.arguments = {QStringLiteral("/c"), QStringLiteral("start"), QString(), app};
            if (!extra.isEmpty())
                process.arguments.append(extra);
#endif
            format = [app](const ProcessRecord& record, const QString&, const QString& err) {
                const QString error = record.timedOut ? QStringLiteral("Timed out.")
                                      : record.state == ProcessState::Failed ? record.error
                                      : record.exitCode != 0                 ? err
                                                                             : QString();
                return error.isEmpty()
                           ? QStringLiteral("app-launch: Launch requested for '%1'.").arg(app)
                           : QStringLiteral("app-launch: %1 (%2)").arg(error, app);
            };
#endif
        } else if (toolId == QLatin1String("open-url")) {
            const QUrl parsed = QUrl::fromUserInput(argument(QStringLiteral("url")).trimmed());
            if (!parsed.isValid() || (parsed.scheme() != QLatin1String("http") &&
                                      parsed.scheme() != QLatin1String("https"))) {
                completion({ToolExecutionStatus::InvalidArguments,
                            QStringLiteral("open-url: Only http and https URLs can be opened.")});
                return {};
            }
#if defined(Q_OS_MACOS)
            process.program = QStringLiteral("open");
#elif defined(Q_OS_WIN)
            process.program = QStringLiteral("cmd.exe");
            process.arguments = {QStringLiteral("/c"), QStringLiteral("start"), QString()};
#else
            process.program = QStringLiteral("xdg-open");
#endif
            process.arguments.append(parsed.toString(QUrl::FullyEncoded));
            format = [](const ProcessRecord& record, const QString&, const QString& err) {
                if (record.state == ProcessState::Failed || record.exitCode != 0)
                    return QStringLiteral("open-url: Sandbox launch failed: %1")
                        .arg(record.error.isEmpty() ? err : record.error);
                return QStringLiteral("open-url: Browser launch requested.");
            };
        } else if (toolId == QLatin1String("system-notify")) {
            const QString title = argument(QStringLiteral("title")).trimmed();
            const QString message = argument(QStringLiteral("message")).trimmed();
            if (message.isEmpty()) {
                completion({ToolExecutionStatus::InvalidArguments,
                            QStringLiteral("system-notify: No message argument provided.")});
                return {};
            }
#if defined(Q_OS_MACOS)
            process.program = QStringLiteral("osascript");
            process.arguments = {
                QStringLiteral("-e"),
                QStringLiteral("display notification \"%1\" with title \"%2\"")
                    .arg(osascriptEscape(message),
                         osascriptEscape(title.isEmpty() ? QStringLiteral("Sentinel") : title))};
#elif defined(Q_OS_WIN)
            process.program = QStringLiteral("powershell");
            QString escapedTitle = title.isEmpty() ? QStringLiteral("Sentinel") : title;
            escapedTitle.replace(QLatin1Char('\''), QStringLiteral("''"));
            QString cleanMessage = message;
            cleanMessage.replace(QLatin1Char('\''), QStringLiteral("''"));
            const QString script = QStringLiteral(
                "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, "
                "ContentType = WindowsRuntime] | Out-Null;"
                "[Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType "
                "= WindowsRuntime] | Out-Null;"
                "$template = '<toast><visual><binding template=\"ToastGeneric\"><text "
                "id=\"1\">%1</text><text id=\"2\">%2</text></binding></visual></toast>';"
                "$xml = New-Object Windows.Data.Xml.Dom.XmlDocument;$xml.LoadXml($template);"
                "$toast = New-Object Windows.UI.Notifications.ToastNotification($xml);"
                "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('"
                "Sentinel').Show($toast);");
            process.arguments = {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                                 QStringLiteral("-Command"),
                                 script.arg(escapedTitle, cleanMessage)};
#else
            process.program = QStringLiteral("notify-send");
            process.arguments = {title.isEmpty() ? QStringLiteral("Sentinel") : title, message};
#endif
            format = [message](const ProcessRecord& record, const QString&, const QString& err) {
                const QString error = record.timedOut ? QStringLiteral("Timed out.")
                                      : record.state == ProcessState::Failed ? record.error
                                      : record.exitCode != 0                 ? err
                                                                             : QString();
                return error.isEmpty()
                           ? QStringLiteral("system-notify: Notification sent: %1").arg(message)
                           : QStringLiteral("system-notify: %1").arg(error);
            };
        } else if (toolId == QLatin1String("browser-screenshot") ||
                   toolId == QLatin1String("browser-pdf")) {
            const QString mode = toolId == QLatin1String("browser-pdf")
                                     ? QStringLiteral("pdf")
                                     : QStringLiteral("screenshot");
            const QString url = argument(QStringLiteral("url")).trimmed();
            if (url.isEmpty()) {
                completion({ToolExecutionStatus::Succeeded,
                            QStringLiteral("browser-%1: No url argument provided.").arg(mode)});
                return {};
            }
            QString path = argument(QStringLiteral("path")).trimmed();
            if (!path.isEmpty()) {
                completion({ToolExecutionStatus::Blocked,
                            QStringLiteral(
                                "browser-%1: explicit output path lacks filesystem authorization.")
                                .arg(mode)});
                return {};
            }
            outputDirectory = std::make_shared<QTemporaryDir>();
            if (!outputDirectory->isValid()) {
                completion({ToolExecutionStatus::Failed,
                            QStringLiteral("browser-%1: private output directory is unavailable.")
                                .arg(mode)});
                return {};
            }
            process.sandbox->temporaryDirectory = outputDirectory->path();
            path = QDir(outputDirectory->path())
                       .filePath(mode == QLatin1String("pdf") ? QStringLiteral("capture.pdf")
                                                              : QStringLiteral("capture.png"));
            process.program = QStringLiteral("npx");
            process.arguments = {QStringLiteral("-y"), QStringLiteral("playwright"), mode};
            if (mode == QLatin1String("screenshot"))
                process.arguments.append(
                    {QStringLiteral("--wait-for-timeout"), QStringLiteral("2500")});
            process.arguments.append({url, path});
            process.timeoutMs = 300000;
            format = [mode, path](const ProcessRecord& record, const QString&, const QString& err) {
                if (record.timedOut)
                    return QStringLiteral("browser-%1: Playwright timed out after 300 s.")
                        .arg(mode);
                if (record.state == ProcessState::Failed && record.systemPid == 0)
                    return QStringLiteral("browser-%1: Node.js (npx) is required for Playwright "
                                          "browser tools. Install Node.js and run 'npx playwright "
                                          "install chromium' once.")
                        .arg(mode);
                if (record.exitCode != 0 || !QFile::exists(path))
                    return QStringLiteral("browser-%1: Playwright failed: %2")
                        .arg(mode,
                             err.isEmpty() ? QStringLiteral("no output file was produced") : err);
                return QStringLiteral("browser-%1: saved '%2'.").arg(mode, path);
            };
        }
        struct Active {
            std::shared_ptr<ProcessExecutor> executor;
            QString id;
            QByteArray out;
            QByteArray err;
            bool truncated = false;
            Output output;
            Completion completion;
            std::function<QString(const ProcessRecord&, const QString&, const QString&)> format;
            std::shared_ptr<QTemporaryDir> outputDirectory;
        };
        auto active = std::make_shared<Active>();
        active->executor = std::shared_ptr<ProcessExecutor>(
            new ProcessExecutor, [](ProcessExecutor* service) { service->deleteLater(); });
        active->output = std::move(output);
        active->completion = std::move(completion);
        active->format = std::move(format);
        active->outputDirectory = std::move(outputDirectory);
        std::weak_ptr<Active> weak = active;
        const auto launch = [weak, process, input] {
            const auto entry = weak.lock();
            if (!entry)
                return;
            entry->out.clear();
            entry->err.clear();
            entry->truncated = false;
            entry->id = entry->executor->start(
                process,
                [weak](const ProcessRecord& record) {
                    auto current = weak.lock();
                    if (!current || (record.state != ProcessState::Exited &&
                                     record.state != ProcessState::Failed &&
                                     record.state != ProcessState::Cancelled))
                        return;
                    ToolExecutionResult result{
                        record.state == ProcessState::Cancelled ? ToolExecutionStatus::Cancelled
                        : record.state == ProcessState::Exited && record.exitCode == 0
                            ? ToolExecutionStatus::Succeeded
                            : ToolExecutionStatus::Failed,
                        current->format(record, QString::fromUtf8(current->out).trimmed(),
                                        QString::fromUtf8(current->err).trimmed())};
                    if (record.timedOut)
                        result.failureCategory = ToolFailureCategory::Timeout;
                    else if (record.state == ProcessState::Cancelled)
                        result.failureCategory = ToolFailureCategory::Cancelled;
                    else if (record.state == ProcessState::Failed && record.systemPid == 0)
                        result.failureCategory = ToolFailureCategory::RuntimeUnavailable;
                    else if (result.status == ToolExecutionStatus::Failed)
                        result.failureCategory = ToolFailureCategory::InternalFailure;
                    if (result.status == ToolExecutionStatus::Succeeded &&
                        current->outputDirectory &&
                        QDir(current->outputDirectory->path()).entryList(QDir::Files).isEmpty()) {
                        result.status = ToolExecutionStatus::Failed;
                        result.failureCategory = ToolFailureCategory::InternalFailure;
                        result.summary =
                            QStringLiteral("Expected output artifact was not produced.");
                    }
                    const QJsonObject receipt{
                        {QStringLiteral("processId"), record.processId},
                        {QStringLiteral("exitCode"), record.exitCode},
                        {QStringLiteral("stdout"), QString::fromUtf8(current->out)},
                        {QStringLiteral("stderr"), QString::fromUtf8(current->err)},
                        {QStringLiteral("truncated"), current->truncated},
                        {QStringLiteral("timedOut"), record.timedOut},
                        {QStringLiteral("cancelled"), record.state == ProcessState::Cancelled},
                        {QStringLiteral("complete"), record.state == ProcessState::Exited &&
                                                         !record.timedOut && !current->truncated}};
                    result.structuredObservation = std::make_shared<StructuredObservation>(
                        StructuredObservation{StructuredObservationKind::Generic, receipt});
                    result.sandbox = record.sandbox;
                    if (record.exitCode != 0 && (current->err.startsWith("bwrap:") ||
                                                 current->err.startsWith("sandbox-exec:"))) {
                        result.sandbox.enforcement = SandboxEnforcement::Failed;
                        result.sandbox.failureCategory = QStringLiteral("BackendLaunchFailed");
                    }
                    if (result.status == ToolExecutionStatus::Succeeded &&
                        current->outputDirectory &&
                        !QDir(current->outputDirectory->path()).entryList(QDir::Files).isEmpty())
                        current->outputDirectory->setAutoRemove(false);
                    current->completion(std::move(result));
                },
                [weak](const QString& id, ProcessStream stream, const QByteArray& bytes) {
                    auto current = weak.lock();
                    if (!current)
                        return;
                    auto& buffer = stream == ProcessStream::Stdout ? current->out : current->err;
                    const auto remaining = qMax<qsizetype>(0, 65536 - buffer.size());
                    current->truncated |= bytes.size() > remaining;
                    buffer.append(bytes.left(remaining));
                    if (current->output)
                        current->output(id, stream, bytes);
                });
            if (!input.isEmpty()) {
                entry->executor->write(entry->id, input);
                entry->executor->closeWriteChannel(entry->id);
            }
        };
        if (toolId == QLatin1String("git-status") || toolId == QLatin1String("git-diff")) {
            // Git status/diff may invoke clean/process filters even with --no-textconv.
            // Inspect configuration in the same sandbox and refuse configured commands.
            auto preflight = process;
            preflight.arguments = {QStringLiteral("--no-pager"), QStringLiteral("config"),
                                   QStringLiteral("--name-only"), QStringLiteral("--get-regexp"),
                                   QStringLiteral("^filter\\..*\\.(clean|process|smudge)$")};
            active->id = active->executor->start(
                preflight,
                [weak, launch](const ProcessRecord& record) {
                    const auto current = weak.lock();
                    if (!current || (record.state != ProcessState::Exited &&
                                     record.state != ProcessState::Failed &&
                                     record.state != ProcessState::Cancelled))
                        return;
                    if (record.state == ProcessState::Exited && record.exitCode == 1 &&
                        !record.timedOut) {
                        launch();
                        return;
                    }
                    const auto errText = QString::fromUtf8(current->err).trimmed();
                    const auto detail = !errText.isEmpty() ? errText : record.error;
                    ToolExecutionResult failure{
                        record.state == ProcessState::Cancelled ? ToolExecutionStatus::Cancelled
                                                                : ToolExecutionStatus::Blocked,
                        record.exitCode == 0
                            ? QStringLiteral(
                                  "Git inspection refused: repository filters can execute "
                                  "commands. Use an explicitly approved shell operation.")
                            : QStringLiteral("Git filter safety check could not complete: %1")
                                  .arg(detail)};
                    failure.failureCategory =
                        record.state == ProcessState::Cancelled ? ToolFailureCategory::Cancelled
                        : record.timedOut                       ? ToolFailureCategory::Timeout
                                          : ToolFailureCategory::SecurityDenied;
                    failure.sandbox = record.sandbox;
                    if (record.exitCode != 0 && (current->err.startsWith("bwrap:") ||
                                                 current->err.startsWith("sandbox-exec:"))) {
                        failure.sandbox.enforcement = SandboxEnforcement::Failed;
                        failure.sandbox.failureCategory = QStringLiteral("BackendLaunchFailed");
                        failure.failureCategory = ToolFailureCategory::RuntimeUnavailable;
                    } else if (record.sandbox.enforcement == SandboxEnforcement::Failed) {
                        failure.failureCategory = ToolFailureCategory::RuntimeUnavailable;
                    }
                    current->completion(std::move(failure));
                },
                [weak](const QString& /*id*/, ProcessStream stream, const QByteArray& bytes) {
                    auto current = weak.lock();
                    if (!current)
                        return;
                    auto& buffer = stream == ProcessStream::Stdout ? current->out : current->err;
                    const auto remaining = qMax<qsizetype>(0, 65536 - buffer.size());
                    current->truncated |= bytes.size() > remaining;
                    buffer.append(bytes.left(remaining));
                });
        } else {
            launch();
        }
        if (toolId == QLatin1String("app-quit") || toolId == QLatin1String("app-launch") ||
            toolId == QLatin1String("open-url") || toolId == QLatin1String("system-notify"))
            return [active] {};
        return [active] {
            if (active->executor && !active->id.isEmpty())
                active->executor->terminate(active->id);
        };
    }
    const QString command = argument(QStringLiteral("command"));
    if (command.isEmpty()) {
        completion({ToolExecutionStatus::Failed,
                    QStringLiteral("run-command: No command argument provided.")});
        return {};
    }
    bool timeoutOk = false;
    const int suppliedTimeout = argument(QStringLiteral("timeout")).trimmed().toInt(&timeoutOk);
    int timeoutMs = qBound(1000, timeoutOk ? suppliedTimeout : 60000, 600000);
    const QString root = invocation.resourceSnapshot ? invocation.resourceSnapshot->workingDirectory
                                                     : QDir::currentPath();
    QString workdir = root;
    const QString requestedWorkdir = argument(QStringLiteral("workdir")).trimmed();
    if (!requestedWorkdir.isEmpty()) {
        workdir = scopedPath(root, requestedWorkdir);
        if (workdir.isEmpty() || !QDir(workdir).exists()) {
            completion({ToolExecutionStatus::Blocked,
                        QStringLiteral("run-command: workdir is outside the approved workspace.")});
            return {};
        }
    }
    const bool docker =
        argument(QStringLiteral("sandbox")).trimmed().toLower() == QLatin1String("docker");
    ProcessRequest process;
    process.workingDirectory = workdir;
    process.sessionId = sessionId;
    process.toolCallId = toolCallId;
    process.timeoutMs = timeoutMs;
    if (!invocation.processSandbox ||
        invocation.processSandbox->workingDirectory != PathGuard::canonicalPath(root)) {
        completion({ToolExecutionStatus::Blocked,
                    QStringLiteral("run-command: authorized sandbox plan is missing or changed.")});
        return {};
    }
    process.sandbox = invocation.processSandbox;
    if (docker) {
        process.program = QStringLiteral("docker");
        process.arguments = {
            QStringLiteral("run"),       QStringLiteral("--rm"),
            QStringLiteral("--network"), QStringLiteral("none"),
            QStringLiteral("--memory"),  QStringLiteral("2g"),
            QStringLiteral("--cpus"),    QStringLiteral("2"),
            QStringLiteral("-v"),        QStringLiteral("%1:/workspace").arg(workdir),
            QStringLiteral("-w"),        QStringLiteral("/workspace")};
        if (timeoutMs > 60000)
            process.timeoutMs += 120000;
        process.arguments.append(
            {QStringLiteral("alpine:3.20"), QStringLiteral("sh"), QStringLiteral("-c"), command});
    } else {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString pathEnv = env.value(QStringLiteral("PATH"));
#if defined(Q_OS_MACOS)
        if (!pathEnv.contains(QStringLiteral("/opt/homebrew/bin")))
            pathEnv = QStringLiteral("/opt/homebrew/bin:/usr/local/bin:") + pathEnv;
#elif defined(Q_OS_UNIX)
        if (!pathEnv.contains(QStringLiteral("/usr/local/bin")))
            pathEnv = QStringLiteral("/usr/local/bin:") + pathEnv;
#endif
        env.insert(QStringLiteral("PATH"), pathEnv);
        process.environment = env;
#if defined(Q_OS_WIN)
        process.program = QStringLiteral("cmd.exe");
        process.arguments = {QStringLiteral("/c"), command};
#else
        process.program = QStringLiteral("/bin/sh");
        process.arguments = {QStringLiteral("-c"), command};
#endif
    }

    struct ActiveCommand {
        std::shared_ptr<ProcessExecutor> executor;
        QString id;
        QByteArray stdoutBytes;
        QByteArray stderrBytes;
        bool stdoutTruncated = false;
        bool stderrTruncated = false;
        Output output;
        Completion completion;
        bool docker = false;
        int timeoutMs = 0;
    };
    auto active = std::make_shared<ActiveCommand>();
    active->executor = std::shared_ptr<ProcessExecutor>(
        new ProcessExecutor, [](ProcessExecutor* service) { service->deleteLater(); });
    active->output = std::move(output);
    active->completion = std::move(completion);
    active->docker = docker;
    active->timeoutMs = timeoutMs;
    std::weak_ptr<ActiveCommand> weak = active;
    active->id = active->executor->start(
        process,
        [weak](const ProcessRecord& record) {
            auto current = weak.lock();
            if (!current ||
                (record.state != ProcessState::Exited && record.state != ProcessState::Failed &&
                 record.state != ProcessState::Cancelled))
                return;
            const QString out = QString::fromUtf8(current->stdoutBytes).trimmed();
            const QString err = QString::fromUtf8(current->stderrBytes).trimmed();
            QString summary;
            if (record.timedOut) {
                summary =
                    current->docker
                        ? QStringLiteral(
                              "run-command: docker sandbox terminated command after timeout %1 ms.")
                              .arg(current->timeoutMs)
                        : QStringLiteral(
                              "run-command: Shell tool terminated command after exceeding timeout "
                              "%1 ms. Retry with a larger timeout if needed.")
                              .arg(current->timeoutMs);
            } else if (record.state == ProcessState::Cancelled) {
                summary = QStringLiteral("run-command: Command cancelled.");
            } else if (record.state == ProcessState::Failed && record.systemPid == 0) {
                const auto launchFailure = record.sandbox.failureCategory;
                // A docker sandbox that cannot even start is almost always a
                // missing docker CLI, so say that instead of a bare sandbox error.
                const bool missingDocker =
                    current->docker && (launchFailure.isEmpty() ||
                                        launchFailure == QLatin1String("ExecutableUnavailable"));
                summary =
                    missingDocker
                        ? QStringLiteral(
                              "run-command: docker sandbox requested but the docker CLI is not "
                              "available. Install Docker or retry without sandbox=docker.")
                    : !launchFailure.isEmpty()
                        ? QStringLiteral("run-command: Sandbox denied launch (%1).")
                              .arg(launchFailure)
                        : QStringLiteral("run-command: Failed to start the approved command.");
            } else if (current->docker) {
                if (record.exitCode == 0)
                    summary =
                        out.isEmpty() && err.isEmpty()
                            ? QStringLiteral("Command executed in docker sandbox. (exit=0)")
                            : QStringLiteral(
                                  "Command executed in docker sandbox. (exit=0)\n\n[STDOUT]:\n%1")
                                  .arg(out);
                else if (err.contains(QStringLiteral("failed to connect to the docker API")) ||
                         err.contains(QStringLiteral("Is the docker daemon running")))
                    summary =
                        QStringLiteral("run-command: the docker CLI is installed but the daemon is "
                                       "not running. Start Docker (or OrbStack) and retry, or run "
                                       "without sandbox=docker.\nDetails: %1")
                            .arg(err);
                else
                    summary = QStringLiteral("Docker sandbox command failed. "
                                             "(exit=%1)\n\n[STDOUT]:\n%2\n\n[STDERR]:\n%3")
                                  .arg(record.exitCode)
                                  .arg(out, err);
            } else if (record.exitCode == 0) {
                summary =
                    out.isEmpty() && err.isEmpty()
                        ? QStringLiteral("Command executed successfully. (exit=0)")
                        : QStringLiteral("Command executed successfully. (exit=0)\n\n[STDOUT]:\n%1")
                              .arg(out);
                if (!err.isEmpty())
                    summary += QStringLiteral("\n\n[STDERR]:\n%1").arg(err);
            } else {
                summary =
                    QStringLiteral(
                        "Command execution failed. (exit=%1)\n\n[STDOUT]:\n%2\n\n[STDERR]:\n%3")
                        .arg(record.exitCode)
                        .arg(out, err);
            }
            ToolExecutionResult result{
                record.state == ProcessState::Cancelled ? ToolExecutionStatus::Cancelled
                : record.state == ProcessState::Exited && !record.timedOut && record.exitCode == 0
                    ? ToolExecutionStatus::Succeeded
                    : ToolExecutionStatus::Failed,
                summary};
            const bool complete = record.state == ProcessState::Exited && !record.timedOut &&
                                  !current->stdoutTruncated && !current->stderrTruncated;
            const QJsonObject receipt{
                {QStringLiteral("processId"), record.processId},
                {QStringLiteral("exitCode"), record.exitCode},
                {QStringLiteral("stdout"), QString::fromUtf8(current->stdoutBytes)},
                {QStringLiteral("stderr"), QString::fromUtf8(current->stderrBytes)},
                {QStringLiteral("timedOut"), record.timedOut},
                {QStringLiteral("cancelled"), record.state == ProcessState::Cancelled},
                {QStringLiteral("stdoutTruncated"), current->stdoutTruncated},
                {QStringLiteral("stderrTruncated"), current->stderrTruncated},
                {QStringLiteral("complete"), complete}};
            result.structuredObservation = std::make_shared<StructuredObservation>(
                StructuredObservation{StructuredObservationKind::Generic, receipt});
            result.sandbox = record.sandbox;
            if (record.exitCode != 0 && (current->stderrBytes.startsWith("bwrap:") ||
                                         current->stderrBytes.startsWith("sandbox-exec:"))) {
                result.sandbox.enforcement = SandboxEnforcement::Failed;
                result.sandbox.failureCategory = QStringLiteral("BackendLaunchFailed");
            }
            result.failureCategory =
                result.status == ToolExecutionStatus::Cancelled ? ToolFailureCategory::Cancelled
                : record.timedOut                               ? ToolFailureCategory::Timeout
                : result.sandbox.enforcement == SandboxEnforcement::Failed
                    ? ToolFailureCategory::SecurityDenied
                : record.state == ProcessState::Failed ? ToolFailureCategory::RuntimeUnavailable
                : result.status == ToolExecutionStatus::Failed
                    ? ToolFailureCategory::InternalFailure
                    : ToolFailureCategory::None;
            current->completion(std::move(result));
        },
        [weak](const QString& id, ProcessStream stream, const QByteArray& bytes) {
            auto current = weak.lock();
            if (!current)
                return;
            auto& buffer =
                stream == ProcessStream::Stdout ? current->stdoutBytes : current->stderrBytes;
            auto& truncated = stream == ProcessStream::Stdout ? current->stdoutTruncated
                                                              : current->stderrTruncated;
            const auto remaining = qMax<qsizetype>(0, 65536 - buffer.size());
            truncated |= bytes.size() > remaining;
            buffer.append(bytes.left(remaining));
            if (current->output)
                current->output(id, stream, bytes);
        });
    return [active] {
        if (active->executor && !active->id.isEmpty())
            active->executor->terminate(active->id);
    };
}

namespace {

constexpr int kDefaultReadLimit = 2000;
constexpr int kMaxLineLength = 2000;
constexpr int kGrepGlobLimit = 100;
constexpr int kWebFetchPreviewChars = 8000;

bool isSensitiveToolPath(const QString& canonicalPath);

QString getArgument(const PlannedToolInvocation& invocation, const QString& argId) {
    for (const auto& arg : invocation.arguments) {
        if (arg.id == argId) {
            return arg.value;
        }
    }
    return QString();
}

int getIntArgument(const PlannedToolInvocation& invocation, const QString& argId, int fallback) {
    bool ok = false;
    const int value = getArgument(invocation, argId).trimmed().toInt(&ok);
    return ok ? value : fallback;
}

QString scopedPath(const QString& workingDirectory, const QString& rawPath) {
    QString path = rawPath.trimmed();
    if (path.isEmpty()) {
        return QString();
    }
    if (path == QLatin1String("~"))
        path = QDir::homePath();
    else if (path.startsWith(QStringLiteral("~/")))
        path = QDir::home().absoluteFilePath(path.mid(2));
    QFileInfo fileInfo(path);
    if (fileInfo.isRelative()) {
        path = QDir(workingDirectory).absoluteFilePath(path);
    }
    const QString scoped = PathGuard::safePath(workingDirectory, path);
    if (scoped.isEmpty() || isSensitiveToolPath(scoped)) {
        return QString();
    }
    return scoped;
}

bool isSensitiveToolPath(const QString& canonicalPath) {
    static const QStringList sensitiveDirs{
        QStringLiteral(".ssh"),  QStringLiteral(".gnupg"),          QStringLiteral(".aws"),
        QStringLiteral(".kube"), QStringLiteral(".password-store"),
    };
    static const QStringList sensitiveFiles{
        QStringLiteral("id_rsa"),  QStringLiteral("id_ed25519"),       QStringLiteral("id_ecdsa"),
        QStringLiteral("id_dsa"),  QStringLiteral(".git-credentials"), QStringLiteral(".npmrc"),
        QStringLiteral(".pypirc"), QStringLiteral(".netrc"),
    };

    const QFileInfo info(canonicalPath);
    if (sensitiveFiles.contains(info.fileName())) {
        return true;
    }

    const QStringList parts =
        QDir::cleanPath(canonicalPath).split(QDir::separator(), Qt::SkipEmptyParts);
    for (const auto& part : parts) {
        if (sensitiveDirs.contains(part)) {
            return true;
        }
    }
    return false;
}

QString runSynchronousProcess(const QString& program, const QStringList& args,
                              const QString& workingDirectory, int timeoutMs,
                              QString* errorOut = nullptr) {
    QProcess process;
    if (!workingDirectory.isEmpty() && QDir(workingDirectory).exists()) {
        process.setWorkingDirectory(workingDirectory);
    }
    process.start(program, args);
    if (!process.waitForStarted(5000)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to start '%1'.").arg(program);
        }
        return QString();
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(3000);
        if (errorOut) {
            *errorOut = QStringLiteral("Timed out after %1 ms.").arg(QString::number(timeoutMs));
        }
        return QString();
    }
    const QString out = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (process.exitCode() != 0 && errorOut && out.isEmpty()) {
        *errorOut =
            err.isEmpty() ? QStringLiteral("Exited with code %1.").arg(process.exitCode()) : err;
    }
    return out;
}

[[maybe_unused]] QString osascriptEscape(const QString& text) {
    QString escaped = text;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return escaped;
}

QString humanReadableBytes(quint64 bytes) {
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gib >= 1.0) {
        return QStringLiteral("%1 GiB").arg(QString::number(gib, 'f', 1));
    }
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return QStringLiteral("%1 MiB").arg(QString::number(mib, 'f', 0));
}

QClipboard* activeClipboard() {
    const auto* guiApp = qobject_cast<const QGuiApplication*>(QCoreApplication::instance());
    return guiApp ? QGuiApplication::clipboard() : nullptr;
}

QString systemInfoReport() {
    QStringList lines;
    lines.append(QStringLiteral("OS: %1").arg(QSysInfo::prettyProductName()));
    lines.append(QStringLiteral("Kernel: %1").arg(QSysInfo::kernelVersion()));
    lines.append(QStringLiteral("CPU architecture: %1").arg(QSysInfo::currentCpuArchitecture()));
    lines.append(QStringLiteral("Hostname: %1").arg(QSysInfo::machineHostName()));
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty()) {
        user = qEnvironmentVariable("USERNAME");
    }
    if (!user.isEmpty()) {
        lines.append(QStringLiteral("User: %1").arg(user));
    }

    quint64 totalRamBytes = 0;
#if defined(Q_OS_UNIX)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) {
        totalRamBytes = static_cast<quint64>(pages) * static_cast<quint64>(pageSize);
    }
#elif defined(Q_OS_WIN)
    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (GlobalMemoryStatusEx(&memoryStatus)) {
        totalRamBytes = memoryStatus.ullTotalPhys;
    }
#endif
    if (totalRamBytes > 0) {
        lines.append(QStringLiteral("Total RAM: %1").arg(humanReadableBytes(totalRamBytes)));
    }

    const QStorageInfo root = QStorageInfo::root();
    if (root.isValid() && root.isReady()) {
        lines.append(QStringLiteral("Root volume (%1): %2 free of %3")
                         .arg(QDir::toNativeSeparators(root.rootPath()),
                              humanReadableBytes(static_cast<quint64>(root.bytesFree())),
                              humanReadableBytes(static_cast<quint64>(root.bytesTotal()))));
    }
    lines.append(
        QStringLiteral("Workspace: %1").arg(QDir::toNativeSeparators(QDir::currentPath())));
    return lines.join(QLatin1Char('\n'));
}

QString processListReport() {
    QString output;
    QString error;
#if defined(Q_OS_WIN)
    output = runSynchronousProcess(QStringLiteral("tasklist"), {}, QString(), 15000, &error);
#else
    output = runSynchronousProcess(QStringLiteral("ps"),
                                   {QStringLiteral("-eo"), QStringLiteral("pid,pcpu,comm")},
                                   QString(), 15000, &error);
#endif
    if (output.isEmpty()) {
        return error.isEmpty() ? QStringLiteral("process-list: No process output was returned.")
                               : QStringLiteral("process-list: %1").arg(error);
    }

    const auto lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QStringList shown;
    const int maxLines = 40;
    for (int i = 0; i < lines.size() && shown.size() < maxLines; ++i) {
        QString line = lines.at(i).trimmed();
        if (line.size() > kMaxLineLength) {
            line = line.left(kMaxLineLength) + QStringLiteral("... (line truncated)");
        }
        shown.append(line);
    }
    return QStringLiteral("process-list: %1 process line(s) (showing first %2):\n%3")
        .arg(QString::number(lines.size()), QString::number(shown.size()),
             shown.join(QLatin1Char('\n')));
}

// True when the text looks like a website address ("sahibinden.com",
// "www.x.com", "https://x.com") rather than an application name.
bool looksLikeDomainName(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.contains(QLatin1Char(' '))) {
        return false;
    }
    const QString lowered = trimmed.toLower();
    if (lowered.startsWith(QStringLiteral("http://")) ||
        lowered.startsWith(QStringLiteral("https://")) ||
        lowered.startsWith(QStringLiteral("www."))) {
        return true;
    }
    const int dot = trimmed.lastIndexOf(QLatin1Char('.'));
    if (dot < 1 || dot >= trimmed.size() - 2) {
        return false;
    }
    const QString suffix = trimmed.mid(dot + 1);
    if (suffix.size() > 10) {
        return false;
    }
    for (const QChar c : suffix) {
        if (!c.isLetter()) {
            return false;
        }
    }
    return true;
}

// Runs `npx playwright <mode>` for browser-screenshot / browser-pdf. Real
// Playwright CLI integration (ported from SWE-agent's browser bundle idea);
// requires Node.js and npx on PATH, fails gracefully otherwise.
QString runPlaywrightCli(const QString& mode, const QString& url, const QString& outputPath,
                         const QString& extraWait) {
    if (runSynchronousProcess(QStringLiteral("npx"), {QStringLiteral("--version")}, QString(), 5000)
            .isEmpty()) {
        return QStringLiteral(
                   "browser-%1: Node.js (npx) is required for Playwright browser tools. Install "
                   "Node.js and run 'npx playwright install chromium' once.")
            .arg(mode);
    }

    QStringList args{QStringLiteral("-y"), QStringLiteral("playwright"), mode};
    if (!extraWait.isEmpty()) {
        args.append(QStringLiteral("--wait-for-timeout"));
        args.append(extraWait);
    }
    args.append(url);
    args.append(outputPath);

    QProcess process;
    process.start(QStringLiteral("npx"), args);
    if (!process.waitForStarted(10000)) {
        return QStringLiteral("browser-%1: failed to start npx.").arg(mode);
    }
    // npx may fetch the playwright package on first use; allow extra time.
    if (!process.waitForFinished(300000)) {
        process.kill();
        process.waitForFinished(3000);
        return QStringLiteral("browser-%1: Playwright timed out after 300 s.").arg(mode);
    }
    const QString err = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (process.exitCode() != 0 || !QFile::exists(outputPath)) {
        return QStringLiteral("browser-%1: Playwright failed: %2")
            .arg(mode, err.isEmpty() ? QStringLiteral("no output file was produced") : err);
    }
    return QStringLiteral("browser-%1: saved '%2'.").arg(mode, outputPath);
}

// Extracts code symbols (classes, functions, methods) with line numbers for
// common language families. Ported concept from Cline's
// list_code_definition_names tool.
QStringList extractCodeDefinitions(const QString& path, const QByteArray& bytes,
                                   const FileSystemOperationContext& context, bool& cancelled,
                                   bool& truncated) {
    const QString content = QString::fromUtf8(bytes);
    const QStringList lines = content.split(QLatin1Char('\n'));
    const QString suffix = QFileInfo(path).suffix().toLower();

    static const QRegularExpression cxxPattern(QStringLiteral(
        "^\\s*(?:template\\s*<[^>]*>\\s*)?(?:class|struct|enum\\s+class|enum|namespace)\\s+"
        "([A-Za-z_]\\w*)"));
    static const QRegularExpression cxxFunction(
        QStringLiteral("^\\s*(?:[A-Za-z_][\\w:<>,\\*\\&\\s]*)\\s+([A-Za-z_]\\w*)\\s*\\("));
    static const QRegularExpression pythonPattern(
        QStringLiteral("^\\s*(?:async\\s+)?def\\s+([A-Za-z_]\\w*)|^\\s*class\\s+([A-Za-z_]\\w*)"));
    static const QRegularExpression jsPattern(
        QStringLiteral("^\\s*(?:export\\s+)?(?:default\\s+)?(?:async\\s+)?(?:function\\*?|class|"
                       "interface|type|enum)\\s+([A-Za-z_$][\\w$]*)"));
    static const QRegularExpression rustPattern(QStringLiteral(
        "^\\s*(?:pub(?:\\(\\w+\\))?\\s+)?(?:async\\s+)?(?:fn|struct|enum|trait|mod)\\s+"
        "([A-Za-z_]\\w*)"));

    const bool isPython = suffix == QStringLiteral("py");
    const bool isJsLike = suffix == QStringLiteral("js") || suffix == QStringLiteral("jsx") ||
                          suffix == QStringLiteral("ts") || suffix == QStringLiteral("tsx") ||
                          suffix == QStringLiteral("mjs");
    const bool isRust = suffix == QStringLiteral("rs");
    const bool isCxx = suffix == QStringLiteral("c") || suffix == QStringLiteral("cc") ||
                       suffix == QStringLiteral("cpp") || suffix == QStringLiteral("cxx") ||
                       suffix == QStringLiteral("h") || suffix == QStringLiteral("hpp") ||
                       suffix == QStringLiteral("java") || suffix == QStringLiteral("cs");

    QStringList definitions;
    for (int i = 0; i < lines.size(); ++i) {
        if ((i & 127) == 0 && context.isCancelled()) {
            cancelled = true;
            break;
        }
        const QString& line = lines.at(i);
        auto add = [&](const QString& kind, const QString& name) {
            definitions.append(
                QStringLiteral("%1:%2 %3 %4").arg(QString::number(i + 1), kind, name));
        };
        if (isPython) {
            const auto m = pythonPattern.match(line);
            if (m.hasMatch()) {
                if (!m.captured(1).isEmpty()) {
                    add(QStringLiteral("def"), m.captured(1));
                } else {
                    add(QStringLiteral("class"), m.captured(2));
                }
            }
        } else if (isJsLike) {
            const auto m = jsPattern.match(line);
            if (m.hasMatch()) {
                add(QStringLiteral("symbol"), m.captured(1));
            }
        } else if (isRust) {
            const auto m = rustPattern.match(line);
            if (m.hasMatch()) {
                add(QStringLiteral("item"), m.captured(1));
            }
        } else if (isCxx) {
            const auto m = cxxPattern.match(line);
            if (m.hasMatch()) {
                add(QStringLiteral("type"), m.captured(1));
                continue;
            }
            const auto f = cxxFunction.match(line);
            if (f.hasMatch() && f.captured(1) != QStringLiteral("return") &&
                f.captured(1) != QStringLiteral("if") && f.captured(1) != QStringLiteral("for") &&
                f.captured(1) != QStringLiteral("while") &&
                f.captured(1) != QStringLiteral("switch")) {
                add(QStringLiteral("function"), f.captured(1));
            }
        }
        if (definitions.size() >= 100) {
            definitions.append(QStringLiteral("(truncated at 100 definitions)"));
            truncated = true;
            break;
        }
    }
    return definitions;
}

QDateTime parseAlarmTime(const QString& raw) {
    const auto trimmed = raw.trimmed();
    QDateTime parsed = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(trimmed, Qt::ISODate);
    }
    if (parsed.isValid()) {
        return parsed;
    }

    const QRegularExpression timeOnly(QStringLiteral("^(\\d{1,2}):(\\d{2})(?::(\\d{2}))?$"));
    const auto match = timeOnly.match(trimmed);
    if (match.hasMatch()) {
        const auto now = QDateTime::currentDateTime();
        QTime time(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt());
        QDateTime candidate(now.date(), time);
        if (candidate <= now) {
            candidate = candidate.addDays(1);
        }
        return candidate;
    }

    for (const char* format : {"yyyy-MM-dd HH:mm", "dd.MM.yyyy HH:mm", "HH:mm:ss"}) {
        parsed = QDateTime::fromString(trimmed, QString::fromLatin1(format));
        if (parsed.isValid()) {
            return parsed;
        }
    }
    return QDateTime();
}

} // namespace

ToolExecutionResult RealToolExecutor::execute(const ToolExecutionRequest& request) const {
    Q_UNUSED(request)
    return {ToolExecutionStatus::Blocked,
            QStringLiteral("Registered tools must execute through ToolExecutionGateway.")};
}

ToolExecutionResult
RealToolExecutor::executeLocalPlanSummary(const PlannedToolInvocation& invocation,
                                          QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        logs.append(QStringLiteral("Executed: Local Plan Summary"));

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

QString RealToolExecutor::resolveToolPath(const QString& cwd, const QString& raw,
                                          bool write) const {
    const auto result = fileSystemService_.resolve(
        raw, cwd, write ? FileSystemAccess::Write : FileSystemAccess::Read);
    return result.ok() ? result.value->canonicalPath : QString{};
}
namespace {
FileSystemOperationContext operationContext(const PlannedToolInvocation& invocation) {
    return {invocation.cancellation, invocation.toolCancellation};
}
QJsonArray traversalIssues(const TraversalStatus& status) {
    QJsonArray issues;
    for (const auto& issue : status.issues)
        issues.append(QJsonObject{{QStringLiteral("resource"), issue.resource},
                                  {QStringLiteral("failure"), static_cast<int>(issue.failure)}});
    return issues;
}
void addCoverage(QJsonObject& data, const TraversalStatus& status, const QString& scope,
                 bool includeHidden, bool recursive, int resultLimit) {
    data.insert(QStringLiteral("scope"), scope);
    data.insert(QStringLiteral("hiddenEntriesPolicy"),
                includeHidden ? QStringLiteral("included") : QStringLiteral("excluded"));
    data.insert(QStringLiteral("permissionLimited"), !status.issues.isEmpty());
    data.insert(QStringLiteral("recursive"), recursive);
    data.insert(QStringLiteral("followSymlinks"), false);
    data.insert(QStringLiteral("skippedSymlinks"), status.skippedSymlinks);
    data.insert(QStringLiteral("maxDepth"), recursive ? 128 : 0);
    data.insert(QStringLiteral("resultLimit"), resultLimit);
    data.insert(QStringLiteral("enumerationLimit"), 20000);
    data.insert(QStringLiteral("hiddenEntries"), QJsonArray::fromStringList(status.hiddenEntries));
}
template <typename T>
ToolExecutionResult fileToolFailure(const QString& tool, const FileSystemResult<T>& result) {
    auto observation = std::make_shared<StructuredObservation>();
    observation->kind = StructuredObservationKind::FileSystemFailure;
    observation->fileSystemFailure = result.failure;
    observation->fileSystemOperation = result.operation;
    if (tool == QLatin1String("grep"))
        observation->fileSystemOperation = FileSystemOperation::Grep;
    else if (tool == QLatin1String("glob"))
        observation->fileSystemOperation = FileSystemOperation::Glob;
    else if (result.operation == FileSystemOperation::Stat) {
        if (tool == QLatin1String("list-directory"))
            observation->fileSystemOperation = FileSystemOperation::ListDirectory;
        else if (tool == QLatin1String("read-file"))
            observation->fileSystemOperation = FileSystemOperation::ReadFile;
        else if (tool == QLatin1String("write-file"))
            observation->fileSystemOperation = FileSystemOperation::WriteFile;
        else if (tool == QLatin1String("edit-file"))
            observation->fileSystemOperation = FileSystemOperation::EditFile;
        else if (tool == QLatin1String("delete-file"))
            observation->fileSystemOperation = FileSystemOperation::Delete;
        else if (tool == QLatin1String("move-file"))
            observation->fileSystemOperation = FileSystemOperation::Move;
        else if (tool == QLatin1String("glob"))
            observation->fileSystemOperation = FileSystemOperation::Glob;
        else if (tool == QLatin1String("grep"))
            observation->fileSystemOperation = FileSystemOperation::Grep;
    }
    observation->failureResource = result.resource;
    observation->data = {{QStringLiteral("resource"), result.resource}};
    const bool securityFailure = result.failure == FileSystemFailure::PermissionDenied ||
                                 result.failure == FileSystemFailure::ResourceChanged ||
                                 result.failure == FileSystemFailure::SymlinkEscape ||
                                 result.failure == FileSystemFailure::UnsafeParent ||
                                 result.failure == FileSystemFailure::SecurityBoundaryViolation;
    const auto status = securityFailure ? ToolExecutionStatus::Blocked
                        : result.failure == FileSystemFailure::InvalidPath
                            ? ToolExecutionStatus::InvalidArguments
                            : ToolExecutionStatus::Failed;
    QString detail = result.diagnostic;
    if (detail.isEmpty()) {
        if (securityFailure)
            detail = QStringLiteral("Access denied: %1").arg(result.resource);
        else if (result.failure == FileSystemFailure::InvalidPath)
            detail = QStringLiteral("Invalid path");
        else if (result.failure == FileSystemFailure::NotFound)
            detail = QStringLiteral("Not found: %1").arg(result.resource);
        else if (result.failure == FileSystemFailure::AlreadyExists)
            detail = QStringLiteral("Already exists: %1").arg(result.resource);
        else
            detail = QStringLiteral("Filesystem operation failed: %1").arg(result.resource);
    }
    ToolExecutionResult failure{status, QStringLiteral("%1: %2").arg(tool, detail), observation};
    failure.failureCategory =
        result.failure == FileSystemFailure::NotFound ? ToolFailureCategory::NotFound
        : result.failure == FileSystemFailure::PermissionDenied
            ? ToolFailureCategory::PermissionDenied
        : securityFailure                                  ? ToolFailureCategory::SecurityDenied
        : result.failure == FileSystemFailure::InvalidPath ? ToolFailureCategory::InvalidArguments
        : result.failure == FileSystemFailure::Unavailable ? ToolFailureCategory::RuntimeUnavailable
                                                           : ToolFailureCategory::InternalFailure;
    return failure;
}
FileSystemResult<AuthorizedPath> authorizedFilePath(const IFileSystemService& service,
                                                    const PlannedToolInvocation& invocation,
                                                    const QString& argument, AccessMode access,
                                                    const QString& cwd) {
    if (invocation.resourceSnapshot && invocation.resourceSnapshot->authorized &&
        invocation.resourceSnapshot->workingDirectory == PathGuard::canonicalPath(cwd)) {
        for (const auto& resource : invocation.resourceSnapshot->files) {
            if (resource.argument == argument && resource.patchAction.isEmpty() &&
                resource.access == access)
                return service.revalidateAuthorized(resource.path, cwd);
        }
    }
    FileSystemResult<AuthorizedPath> denied;
    denied.failure = FileSystemFailure::PermissionDenied;
    denied.resource = argument;
    denied.diagnostic = QStringLiteral("Pre-execution filesystem authorization is missing.");
    return denied;
}
} // namespace
ToolExecutionResult RealToolExecutor::executeListDirectory(const PlannedToolInvocation& invocation,
                                                           QString& cwd) const {
    const auto path = authorizedFilePath(fileSystemService_, invocation, QStringLiteral("path"),
                                         AccessMode::Read, cwd);
    if (!path.ok())
        return fileToolFailure(QStringLiteral("list-directory"), path);
    const auto listing = fileSystemService_.listDirectory(
        *path.value,
        getArgument(invocation, QStringLiteral("includeHidden")) == QLatin1String("true"), 500,
        operationContext(invocation));
    if (!listing.ok())
        return fileToolFailure(QStringLiteral("list-directory"), listing);
    QJsonArray entries;
    for (const auto& entry : listing.value->entries)
        entries.append(
            QJsonObject{{QStringLiteral("name"), entry.name},
                        {QStringLiteral("type"), entry.directory     ? QStringLiteral("directory")
                                                 : entry.regularFile ? QStringLiteral("file")
                                                                     : QStringLiteral("other")}});
    QJsonObject data{
        {QStringLiteral("path"), listing.resource},
        {QStringLiteral("entries"), entries},
        {QStringLiteral("recursive"), false},
        {QStringLiteral("includeHidden"),
         getArgument(invocation, QStringLiteral("includeHidden")) == QLatin1String("true")},
        {QStringLiteral("complete"), listing.value->status.complete},
        {QStringLiteral("truncated"), listing.value->status.truncated},
        {QStringLiteral("cancelled"), listing.value->status.cancelled},
        {QStringLiteral("issues"), traversalIssues(listing.value->status)}};
    addCoverage(data, listing.value->status, listing.resource,
                getArgument(invocation, QStringLiteral("includeHidden")) == QLatin1String("true"),
                false, 500);
    const QString summary =
        listing.value->status.cancelled
            ? QStringLiteral(
                  "Directory listing stopped after %1 entries. Scope not fully inspected.\n%2")
                  .arg(entries.size())
                  .arg(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)))
            : QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact));
    return {listing.value->status.cancelled ? ToolExecutionStatus::Cancelled
                                            : ToolExecutionStatus::Succeeded,
            summary,
            std::make_shared<StructuredObservation>(
                StructuredObservation{StructuredObservationKind::DirectoryListing, data})};
}
ToolExecutionResult RealToolExecutor::executeGitInspection(const PlannedToolInvocation&,
                                                           QString&) const {
    return {ToolExecutionStatus::Blocked,
            QStringLiteral("Git inspection must use the asynchronous sandboxed process handler.")};
}

ToolExecutionResult RealToolExecutor::executeStatPath(const PlannedToolInvocation& invocation,
                                                      QString& cwd) const {
    const auto path = authorizedFilePath(fileSystemService_, invocation, QStringLiteral("path"),
                                         AccessMode::Read, cwd);
    if (!path.ok())
        return fileToolFailure(QStringLiteral("stat-path"), path);
    const auto entry = fileSystemService_.stat(*path.value);
    if (!entry.ok())
        return fileToolFailure(QStringLiteral("stat-path"), entry);
    const QJsonObject data{{QStringLiteral("path"), entry.resource},
                           {QStringLiteral("exists"), true},
                           {QStringLiteral("directory"), entry.value->directory},
                           {QStringLiteral("regularFile"), entry.value->regularFile},
                           {QStringLiteral("size"), entry.value->size},
                           {QStringLiteral("complete"), true}};
    return {ToolExecutionStatus::Succeeded,
            QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)),
            std::make_shared<StructuredObservation>(
                StructuredObservation{StructuredObservationKind::FileSystemFact, data})};
}

ToolExecutionResult RealToolExecutor::executeReadFile(const PlannedToolInvocation& invocation,
                                                      QString& cwd) const {
    const auto path = authorizedFilePath(fileSystemService_, invocation, QStringLiteral("path"),
                                         AccessMode::Read, cwd);
    if (!path.ok())
        return fileToolFailure(QStringLiteral("read-file"), path);
    const auto read = fileSystemService_.readFile(*path.value, 64 * 1024);
    if (!read.ok()) {
        if (read.failure == FileSystemFailure::NotFile) {
            FileSystemResult<FileRead> rejected;
            rejected.resource = read.resource;
            rejected.failure = FileSystemFailure::NotFile;
            rejected.diagnostic =
                QStringLiteral("Path is a directory; use list-directory to list it.");
            return fileToolFailure(QStringLiteral("read-file"), rejected);
        }
        return fileToolFailure(QStringLiteral("read-file"), read);
    }
    if (read.value->binary) {
        FileSystemResult<FileRead> rejected;
        rejected.resource = read.resource;
        rejected.failure = FileSystemFailure::ReadFailed;
        rejected.diagnostic = QStringLiteral("Cannot read binary file: %1").arg(read.resource);
        return fileToolFailure(QStringLiteral("read-file"), rejected);
    }
    const int offset = getIntArgument(invocation, QStringLiteral("offset"), 1);
    const int limit = getIntArgument(invocation, QStringLiteral("limit"), kDefaultReadLimit);
    const auto lines = QString::fromUtf8(read.value->content).split(QLatin1Char('\n'));
    QStringList output;
    QStringList page;
    bool shortenedLine = false;
    const int start = qMin(offset - 1, static_cast<int>(lines.size()));
    int end = start;
    while (end < lines.size() && page.size() < limit) {
        const auto text = lines.at(end).left(kMaxLineLength);
        shortenedLine |= text.size() < lines.at(end).size();
        page.append(text);
        output.append(QStringLiteral("%1: %2").arg(end + 1).arg(text));
        ++end;
    }
    const bool complete =
        read.value->complete && offset == 1 && end == lines.size() && !shortenedLine;
    const QJsonObject data{{QStringLiteral("path"), read.resource},
                           {QStringLiteral("content"), page.join(QLatin1Char('\n'))},
                           {QStringLiteral("offset"), offset},
                           {QStringLiteral("returnedLines"), page.size()},
                           {QStringLiteral("complete"), complete},
                           {QStringLiteral("truncated"), !complete},
                           {QStringLiteral("cancelled"), false},
                           {QStringLiteral("permissionLimited"), false},
                           {QStringLiteral("issues"), QJsonArray{}}};
    return {ToolExecutionStatus::Succeeded, output.join(QLatin1Char('\n')),
            std::make_shared<StructuredObservation>(
                StructuredObservation{StructuredObservationKind::FileContent, data})};
}
ToolExecutionResult RealToolExecutor::executeWriteFile(const PlannedToolInvocation& invocation,
                                                       QString& cwd) const {
    const auto path = authorizedFilePath(fileSystemService_, invocation, QStringLiteral("path"),
                                         AccessMode::Write, cwd);
    if (!path.ok())
        return fileToolFailure(QStringLiteral("write-file"), path);
    const auto written = fileSystemService_.writeFile(
        *path.value, getArgument(invocation, QStringLiteral("content")).toUtf8());
    if (!written.ok())
        return fileToolFailure(QStringLiteral("write-file"), written);
    return {ToolExecutionStatus::Succeeded,
            QStringLiteral("write-file: Wrote %1 bytes to '%2'.")
                .arg(written.value->bytesWritten)
                .arg(written.resource),
            {},
            written.mutations};
}
ToolExecutionResult RealToolExecutor::executeEditFile(const PlannedToolInvocation& invocation,
                                                      QString& cwd) const {
    const auto path = authorizedFilePath(fileSystemService_, invocation, QStringLiteral("path"),
                                         AccessMode::Write, cwd);
    if (!path.ok())
        return fileToolFailure(QStringLiteral("edit-file"), path);
    const auto oldText = getArgument(invocation, QStringLiteral("oldString"));
    const auto newText = getArgument(invocation, QStringLiteral("newString"));
    const auto stat = fileSystemService_.stat(*path.value);
    if (!stat.ok() && !(stat.failure == FileSystemFailure::NotFound && oldText.trimmed().isEmpty()))
        return fileToolFailure(QStringLiteral("edit-file"), stat);
    if (stat.ok() && stat.value->directory) {
        FileSystemResult<bool> rejected;
        rejected.resource = path.value->canonicalPath;
        rejected.failure = FileSystemFailure::NotFile;
        return fileToolFailure(QStringLiteral("edit-file"), rejected);
    }
    if (!stat.ok()) {
        const auto written = fileSystemService_.writeFile(*path.value, newText.toUtf8(), true);
        if (!written.ok())
            return fileToolFailure(QStringLiteral("edit-file"), written);
        return {ToolExecutionStatus::Succeeded,
                QStringLiteral("edit-file: Created '%1'.").arg(written.resource),
                {},
                written.mutations};
    }
    if (oldText == newText)
        return {ToolExecutionStatus::Failed,
                QStringLiteral("edit-file: oldString and newString are identical.")};
    const auto read = fileSystemService_.readFile(*path.value, 16 * 1024 * 1024);
    if (!read.ok())
        return fileToolFailure(QStringLiteral("edit-file"), read);
    if (!read.value->complete) {
        FileSystemResult<bool> rejected;
        rejected.resource = path.value->canonicalPath;
        rejected.failure = FileSystemFailure::ReadFailed;
        rejected.diagnostic = QStringLiteral("File is too large to edit safely");
        return fileToolFailure(QStringLiteral("edit-file"), rejected);
    }
    if (read.value->content.isEmpty()) {
        FileSystemResult<bool> rejected;
        rejected.resource = path.value->canonicalPath;
        rejected.failure = FileSystemFailure::ReadFailed;
        rejected.diagnostic =
            QStringLiteral("Cannot edit an empty file with the current edit contract");
        return fileToolFailure(QStringLiteral("edit-file"), rejected);
    }
    FuzzyEditRequest request;
    request.filePath = path.value->canonicalPath;
    request.oldString = oldText;
    request.newString = newText;
    request.replaceAll =
        getArgument(invocation, QStringLiteral("replaceAll")) == QLatin1String("true");
    QString output;
    const auto edited =
        FuzzyEditor{}.transform(QString::fromUtf8(read.value->content), request, output);
    if (!edited.success) {
        FileSystemResult<bool> rejected;
        rejected.resource = path.value->canonicalPath;
        rejected.failure = FileSystemFailure::WriteFailed;
        rejected.diagnostic = edited.error;
        return fileToolFailure(QStringLiteral("edit-file"), rejected);
    }
    const auto written = fileSystemService_.writeFile(*path.value, output.toUtf8());
    if (!written.ok())
        return fileToolFailure(QStringLiteral("edit-file"), written);
    return {ToolExecutionStatus::Succeeded,
            QStringLiteral("edit-file: Edited %1 line(s) in '%2'.")
                .arg(edited.linesChanged)
                .arg(path.value->canonicalPath),
            {},
            written.mutations};
}
ToolExecutionResult RealToolExecutor::executeDeleteFile(const PlannedToolInvocation& invocation,
                                                        QString& cwd) const {
    const auto path = authorizedFilePath(fileSystemService_, invocation, QStringLiteral("path"),
                                         AccessMode::Delete, cwd);
    if (!path.ok())
        return fileToolFailure(QStringLiteral("delete-file"), path);
    const auto stat = fileSystemService_.stat(*path.value);
    if (stat.ok() && stat.value->directory) {
        return {
            ToolExecutionStatus::Failed,
            QStringLiteral("delete-file: Refusing to delete a directory: %1").arg(stat.resource)};
    }
    const auto deleted = fileSystemService_.deleteFile(*path.value);
    if (!deleted.ok())
        return fileToolFailure(QStringLiteral("delete-file"), deleted);
    return {ToolExecutionStatus::Succeeded,
            QStringLiteral("delete-file: Deleted '%1'.").arg(deleted.resource),
            {},
            deleted.mutations};
}
ToolExecutionResult RealToolExecutor::executeMoveFile(const PlannedToolInvocation& invocation,
                                                      QString& cwd) const {
    const auto source = authorizedFilePath(fileSystemService_, invocation, QStringLiteral("source"),
                                           AccessMode::Delete, cwd);
    if (!source.ok())
        return fileToolFailure(QStringLiteral("move-file"), source);
    const auto destination = authorizedFilePath(
        fileSystemService_, invocation, QStringLiteral("destination"), AccessMode::Write, cwd);
    if (!destination.ok())
        return fileToolFailure(QStringLiteral("move-file"), destination);
    const auto moved = fileSystemService_.moveFile(*source.value, *destination.value);
    if (!moved.ok())
        return fileToolFailure(QStringLiteral("move-file"), moved);
    return {ToolExecutionStatus::Succeeded,
            QStringLiteral("move-file: Moved '%1' to '%2'.")
                .arg(moved.value->source, moved.value->destination),
            {},
            moved.mutations};
}
ToolExecutionResult RealToolExecutor::executeApplyPatch(const PlannedToolInvocation& invocation,
                                                        QString& cwd) const {
    const QString patch = getArgument(invocation, QStringLiteral("patch"));
    if (patch.trimmed().isEmpty())
        return {ToolExecutionStatus::InvalidArguments,
                QStringLiteral("apply-patch: No patch argument provided.")};
    return applyPatchWithFileSystem(patch, cwd, fileSystemService_, operationContext(invocation),
                                    invocation.resourceSnapshot.get());
}

ToolExecutionResult
RealToolExecutor::executeListCodeDefinitions(const PlannedToolInvocation& invocation,
                                             QString& cwd) const {
    const auto path = authorizedFilePath(fileSystemService_, invocation, QStringLiteral("path"),
                                         AccessMode::Read, cwd);
    if (!path.ok())
        return fileToolFailure(QStringLiteral("list-code-definitions"), path);
    const auto read = fileSystemService_.readFile(*path.value, 2 * 1024 * 1024);
    if (!read.ok())
        return fileToolFailure(QStringLiteral("list-code-definitions"), read);
    bool cancelled = false;
    bool truncated = read.value->truncated;
    const auto definitions = extractCodeDefinitions(
        read.resource, read.value->content, operationContext(invocation), cancelled, truncated);
    const bool complete = read.value->complete && !cancelled && !truncated;
    const QJsonObject data{{QStringLiteral("path"), read.resource},
                           {QStringLiteral("definitions"), QJsonArray::fromStringList(definitions)},
                           {QStringLiteral("complete"), complete},
                           {QStringLiteral("truncated"), truncated},
                           {QStringLiteral("cancelled"), cancelled}};
    const QString summary =
        cancelled ? QStringLiteral(
                        "Definition scan stopped after %1 result(s); file not fully inspected.\n%2")
                        .arg(definitions.size())
                        .arg(definitions.join(QLatin1Char('\n')))
        : truncated
            ? QStringLiteral("Definition scan returned %1 result(s); file not fully inspected.\n%2")
                  .arg(definitions.size())
                  .arg(definitions.join(QLatin1Char('\n')))
        : definitions.isEmpty()
            ? QStringLiteral("list-code-definitions: No definitions found in '%1'.")
                  .arg(read.resource)
            : QStringLiteral("list-code-definitions: %1 definition(s) in '%2':\n%3")
                  .arg(definitions.size())
                  .arg(read.resource, definitions.join(QLatin1Char('\n')));
    return {cancelled ? ToolExecutionStatus::Cancelled : ToolExecutionStatus::Succeeded, summary,
            std::make_shared<StructuredObservation>(
                StructuredObservation{StructuredObservationKind::CodeDefinitions, data})};
}

ToolExecutionResult RealToolExecutor::executeGrep(const PlannedToolInvocation& invocation,
                                                  QString& cwd) const {
    const QString pattern = getArgument(invocation, QStringLiteral("pattern"));
    if (pattern.trimmed().isEmpty())
        return {ToolExecutionStatus::InvalidArguments,
                QStringLiteral("grep: No pattern argument provided.")};
    const QRegularExpression regex(pattern);
    if (!regex.isValid())
        return {ToolExecutionStatus::InvalidArguments,
                QStringLiteral("grep: Invalid regular expression: %1").arg(regex.errorString())};
    const auto path = authorizedFilePath(fileSystemService_, invocation, QStringLiteral("path"),
                                         AccessMode::Read, cwd);
    if (!path.ok())
        return fileToolFailure(QStringLiteral("grep"), path);
    const auto context = operationContext(invocation);
    const bool includeHidden =
        getArgument(invocation, QStringLiteral("includeHidden")) == QLatin1String("true");
    const QString include = getArgument(invocation, QStringLiteral("include")).trimmed();
    int count = 0;
    QStringList output;
    QJsonArray readIssues;
    bool filesComplete = true;
    bool outputCapped = false;
    bool cancelled = false;
    auto onFile = [&](const FileSystemEntry& entry) {
        if (context.isCancelled()) {
            cancelled = true;
            return false;
        }
        if (!include.isEmpty() && !QDir::match(include, entry.name))
            return true;
        auto issue = [&](FileSystemFailure failure) {
            filesComplete = false;
            if (readIssues.size() < 16)
                readIssues.append(
                    QJsonObject{{QStringLiteral("resource"), entry.path},
                                {QStringLiteral("failure"), static_cast<int>(failure)}});
        };
        if (entry.size > 2 * 1024 * 1024) {
            issue(FileSystemFailure::Unavailable);
            return true;
        }
        const auto child = fileSystemService_.resolve(entry.path, cwd, FileSystemAccess::Read);
        if (!child.ok()) {
            issue(child.failure);
            return true;
        }
        const auto read = fileSystemService_.readFile(*child.value, 2 * 1024 * 1024);
        if (!read.ok() || !read.value->complete || read.value->binary) {
            issue(read.ok() ? FileSystemFailure::Unavailable : read.failure);
            return true;
        }
        const auto lines = QString::fromUtf8(read.value->content).split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            if ((i & 127) == 0 && context.isCancelled()) {
                cancelled = true;
                return false;
            }
            if (!regex.match(lines.at(i)).hasMatch())
                continue;
            if (count >= kGrepGlobLimit) {
                outputCapped = true;
                return false;
            }
            output.append(QStringLiteral("%1:%2: %3")
                              .arg(entry.path)
                              .arg(i + 1)
                              .arg(lines.at(i).left(kMaxLineLength)));
            ++count;
        }
        return true;
    };
    const auto tree = fileSystemService_.traverseFiles(
        *path.value, includeHidden, 5000,
        [this, &cwd](const QString& child) { return !resolveToolPath(cwd, child).isEmpty(); },
        context, onFile);
    if (!tree.ok())
        return fileToolFailure(QStringLiteral("grep"), tree);
    cancelled = cancelled || tree.value->status.cancelled;
    const bool truncated = outputCapped || tree.value->status.truncated;
    const bool complete =
        tree.value->status.complete && filesComplete && !cancelled && !outputCapped;
    QJsonArray issues = traversalIssues(tree.value->status);
    for (const auto& issue : readIssues)
        if (issues.size() < 16)
            issues.append(issue);
    QJsonObject data{{QStringLiteral("scope"), path.value->canonicalPath},
                     {QStringLiteral("query"), pattern},
                     {QStringLiteral("matchCount"), count},
                     {QStringLiteral("recursive"), true},
                     {QStringLiteral("includeHidden"), includeHidden},
                     {QStringLiteral("maxDepth"), 128},
                     {QStringLiteral("followSymlinks"), false},
                     {QStringLiteral("complete"), complete},
                     {QStringLiteral("truncated"), truncated},
                     {QStringLiteral("cancelled"), cancelled},
                     {QStringLiteral("issues"), issues}};
    addCoverage(data, tree.value->status, path.value->canonicalPath, includeHidden, true,
                kGrepGlobLimit);
    const QString summary =
        cancelled
            ? QStringLiteral(
                  "Search stopped before the full scope was inspected. %1 match(es) found.\n%2")
                  .arg(count)
                  .arg(output.join(QLatin1Char('\n')))
        : output.isEmpty() ? QStringLiteral("grep: No matches found for '%1' under '%2'.")
                                 .arg(pattern, path.value->canonicalPath)
                           : output.join(QLatin1Char('\n'));
    return {cancelled ? ToolExecutionStatus::Cancelled : ToolExecutionStatus::Succeeded, summary,
            std::make_shared<StructuredObservation>(
                StructuredObservation{StructuredObservationKind::TextSearch, data})};
}
ToolExecutionResult RealToolExecutor::executeGlob(const PlannedToolInvocation& invocation,
                                                  QString& cwd) const {
    const QString pattern = getArgument(invocation, QStringLiteral("pattern"));
    if (pattern.trimmed().isEmpty())
        return {ToolExecutionStatus::InvalidArguments,
                QStringLiteral("glob: No pattern argument provided.")};
    const auto path = authorizedFilePath(fileSystemService_, invocation, QStringLiteral("path"),
                                         AccessMode::Read, cwd);
    if (!path.ok())
        return fileToolFailure(QStringLiteral("glob"), path);
    const bool includeHidden =
        getArgument(invocation, QStringLiteral("includeHidden")) == QLatin1String("true");
    const auto tree = fileSystemService_.traverseFiles(
        *path.value, includeHidden, 5000,
        [this, &cwd](const QString& child) { return !resolveToolPath(cwd, child).isEmpty(); },
        operationContext(invocation));
    if (!tree.ok())
        return fileToolFailure(QStringLiteral("glob"), tree);
    QJsonArray matches;
    QStringList output;
    bool truncated = tree.value->status.truncated;
    bool cancelled = tree.value->status.cancelled;
    const auto context = operationContext(invocation);
    for (const auto& entry : tree.value->files) {
        // Matching already inspected paths is cheap and preserves positive partial evidence.
        if (context.isCancelled() && !tree.value->status.cancelled) {
            cancelled = true;
            break;
        }
        if (!QDir::match(pattern, entry.name) && !QDir::match(pattern, entry.path))
            continue;
        if (matches.size() >= kGrepGlobLimit) {
            truncated = true;
            break;
        }
        matches.append(entry.path);
        output.append(entry.path);
    }
    const bool complete = tree.value->status.complete && !cancelled && !truncated;
    QJsonObject data{{QStringLiteral("root"), path.value->canonicalPath},
                     {QStringLiteral("pattern"), pattern},
                     {QStringLiteral("matches"), matches},
                     {QStringLiteral("recursive"), true},
                     {QStringLiteral("includeHidden"), includeHidden},
                     {QStringLiteral("maxDepth"), 128},
                     {QStringLiteral("followSymlinks"), false},
                     {QStringLiteral("complete"), complete},
                     {QStringLiteral("truncated"), truncated},
                     {QStringLiteral("cancelled"), cancelled},
                     {QStringLiteral("issues"), traversalIssues(tree.value->status)}};
    addCoverage(data, tree.value->status, path.value->canonicalPath, includeHidden, true,
                kGrepGlobLimit);
    return {cancelled ? ToolExecutionStatus::Cancelled : ToolExecutionStatus::Succeeded,
            cancelled
                ? QStringLiteral(
                      "Search stopped before the full scope was inspected. %1 match(es) found.\n%2")
                      .arg(matches.size())
                      .arg(output.join(QLatin1Char('\n')))
            : output.isEmpty() ? QStringLiteral("glob: No files matching '%1' under '%2'.")
                                     .arg(pattern, path.value->canonicalPath)
                               : output.join(QLatin1Char('\n')),
            std::make_shared<StructuredObservation>(
                StructuredObservation{StructuredObservationKind::PathMatches, data})};
}

ToolExecutionResult RealToolExecutor::executeRunCommand(const PlannedToolInvocation& invocation,
                                                        QString& currentWorkingDirectory) const {
    Q_UNUSED(invocation);
    Q_UNUSED(currentWorkingDirectory);
    return {ToolExecutionStatus::Blocked,
            QStringLiteral(
                "run-command requires gateway authorization and asynchronous sandbox execution.")};
}

ToolExecutionResult RealToolExecutor::executeAppLaunch(const PlannedToolInvocation& invocation,
                                                       QString& currentWorkingDirectory) const {
    Q_UNUSED(currentWorkingDirectory);
    const auto app = getArgument(invocation, QStringLiteral("app")).trimmed();
    if (app.isEmpty())
        return {ToolExecutionStatus::InvalidArguments,
                QStringLiteral("app-launch: No app argument provided.")};
    if (looksLikeDomainName(app))
        return {ToolExecutionStatus::InvalidArguments,
                QStringLiteral("app-launch: Use open-url for websites.")};
    return {ToolExecutionStatus::Blocked,
            QStringLiteral("app-launch requires the gateway process sandbox.")};
}

ToolExecutionResult RealToolExecutor::executeAppQuit(const PlannedToolInvocation& invocation,
                                                     QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        const QString app = getArgument(invocation, QStringLiteral("app")).trimmed();
        if (app.isEmpty()) {
            logs.append(QStringLiteral("app-quit: No app argument provided."));
            continue;
        }

        QString error;
#if defined(Q_OS_MACOS)
        runSynchronousProcess(
            QStringLiteral("osascript"),
            QStringList{QStringLiteral("-e"),
                        QStringLiteral("quit app \"%1\"").arg(osascriptEscape(app))},
            QString(), 15000, &error);
#elif defined(Q_OS_WIN)
        runSynchronousProcess(QStringLiteral("taskkill"),
                              QStringList{QStringLiteral("/IM"),
                                          app.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)
                                              ? app
                                              : app + QStringLiteral(".exe"),
                                          QStringLiteral("/F")},
                              QString(), 15000, &error);
#else
        runSynchronousProcess(QStringLiteral("pkill"),
                              QStringList{QStringLiteral("-f"), QStringLiteral("-i"), app},
                              QString(), 15000, &error);
#endif
        logs.append(error.isEmpty() ? QStringLiteral("app-quit: Quit requested for '%1'.").arg(app)
                                    : QStringLiteral("app-quit: %1 (%2)").arg(error, app));

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult RealToolExecutor::executeOpenUrl(const PlannedToolInvocation& invocation,
                                                     QString& currentWorkingDirectory) const {
    Q_UNUSED(invocation);
    Q_UNUSED(currentWorkingDirectory);
    return {ToolExecutionStatus::Blocked,
            QStringLiteral("open-url requires the gateway process sandbox.")};
}

ToolExecutionResult RealToolExecutor::executeSpawnAgent(const PlannedToolInvocation& invocation,
                                                        QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        const QString task = getArgument(invocation, QStringLiteral("task")).trimmed();
        if (task.isEmpty()) {
            return {ToolExecutionStatus::InvalidArguments,
                    QStringLiteral("spawn-agent: No task argument provided.")};
        }
        if (!subagentRunner_) {
            return {
                ToolExecutionStatus::Blocked,
                QStringLiteral("spawn-agent: No subagent runner is configured in this session.")};
        }
        SubagentAssignment assignment;
        assignment.goal = task;
        assignment.purpose = getArgument(invocation, QStringLiteral("purpose")).trimmed();
        if (assignment.purpose != QLatin1String("independent_research") &&
            assignment.purpose != QLatin1String("parallel_verification") &&
            assignment.purpose != QLatin1String("specialist_review"))
            return {ToolExecutionStatus::InvalidArguments,
                    QStringLiteral("spawn-agent: invalid delegation purpose.")};
        assignment.role = getArgument(invocation, QStringLiteral("role")).trimmed().left(40);
        assignment.workspace = getArgument(invocation, QStringLiteral("workspace")).trimmed();
        assignment.modelId = getArgument(invocation, QStringLiteral("modelId")).trimmed();
        const QString requestedTools = getArgument(invocation, QStringLiteral("allowedTools"));
        if (!requestedTools.isEmpty()) {
            const auto document = QJsonDocument::fromJson(requestedTools.toUtf8());
            if (!document.isArray() || document.array().size() > 12) {
                return {ToolExecutionStatus::InvalidArguments,
                        QStringLiteral(
                            "spawn-agent: allowedTools must be an array of up to 12 tool ids.")};
            }
            for (const auto& value : document.array()) {
                if (!value.isString() || value.toString().trimmed().isEmpty())
                    return {ToolExecutionStatus::InvalidArguments,
                            QStringLiteral("spawn-agent: invalid allowed tool id.")};
                assignment.allowedToolIds.append(value.toString());
            }
            assignment.allowedToolIds.removeDuplicates();
        }
        if (assignment.allowedToolIds.isEmpty() && assignment.workspace.isEmpty())
            return {ToolExecutionStatus::Blocked,
                    QStringLiteral("spawn-agent: specify a bounded tool or workspace scope.")};
        auto result = subagentRunner_(assignment, invocation.runtimeToolCallId);
        if (result.status != ToolExecutionStatus::Succeeded)
            return result;
        logs.append(QStringLiteral("spawn-agent: subagent finished the task '%1'.\n\n%2")
                        .arg(task.left(200), result.summary));

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult
RealToolExecutor::executeBrowserScreenshot(const PlannedToolInvocation& invocation,
                                           QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        const QString url = getArgument(invocation, QStringLiteral("url")).trimmed();
        if (url.isEmpty()) {
            logs.append(QStringLiteral("browser-screenshot: No url argument provided."));
            continue;
        }
        QString path = getArgument(invocation, QStringLiteral("path")).trimmed();
        if (path.isEmpty()) {
            path = QDir(QDir::tempPath())
                       .filePath(QStringLiteral("sentinel_browser_%1.png")
                                     .arg(QDateTime::currentMSecsSinceEpoch()));
        } else {
            const QString scoped = resolveToolPath(currentWorkingDirectory, path);
            if (scoped.isEmpty()) {
                logs.append(
                    QStringLiteral("browser-screenshot: Path is outside the approved workspace."));
                continue;
            }
            path = scoped;
        }
        logs.append(
            runPlaywrightCli(QStringLiteral("screenshot"), url, path, QStringLiteral("2500")));

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult RealToolExecutor::executeBrowserPdf(const PlannedToolInvocation& invocation,
                                                        QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        const QString url = getArgument(invocation, QStringLiteral("url")).trimmed();
        if (url.isEmpty()) {
            logs.append(QStringLiteral("browser-pdf: No url argument provided."));
            continue;
        }
        QString path = getArgument(invocation, QStringLiteral("path")).trimmed();
        if (path.isEmpty()) {
            path = QDir(QDir::tempPath())
                       .filePath(QStringLiteral("sentinel_browser_%1.pdf")
                                     .arg(QDateTime::currentMSecsSinceEpoch()));
        } else {
            const QString scoped = resolveToolPath(currentWorkingDirectory, path);
            if (scoped.isEmpty()) {
                logs.append(QStringLiteral("browser-pdf: Path is outside the approved workspace."));
                continue;
            }
            path = scoped;
        }
        logs.append(runPlaywrightCli(QStringLiteral("pdf"), url, path, QString()));

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult RealToolExecutor::executeSystemNotify(const PlannedToolInvocation& invocation,
                                                          QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        const QString title = getArgument(invocation, QStringLiteral("title")).trimmed();
        const QString message = getArgument(invocation, QStringLiteral("message")).trimmed();
        if (message.isEmpty()) {
            logs.append(QStringLiteral("system-notify: No message argument provided."));
            continue;
        }

        QString error;
#if defined(Q_OS_MACOS)
        runSynchronousProcess(
            QStringLiteral("osascript"),
            QStringList{
                QStringLiteral("-e"),
                QStringLiteral("display notification \"%1\" with title \"%2\"")
                    .arg(osascriptEscape(message),
                         osascriptEscape(title.isEmpty() ? QStringLiteral("Sentinel") : title))},
            QString(), 15000, &error);
#elif defined(Q_OS_WIN)
        // PowerShell toast via the Windows Runtime projection; works on
        // Windows 10/11 without any extra dependencies.
        QString escapedTitle = title.isEmpty() ? QStringLiteral("Sentinel") : title;
        escapedTitle.replace(QLatin1Char('\''), QStringLiteral("''"));
        QString cleanMessage = message;
        cleanMessage.replace(QLatin1Char('\''), QStringLiteral("''"));
        const QString psScript = QStringLiteral(
            "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, "
            "ContentType = WindowsRuntime] | Out-Null;"
            "[Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, "
            "ContentType = WindowsRuntime] | Out-Null;"
            "$template = "
            "'<toast><visual><binding template=\"ToastGeneric\">"
            "<text id=\"1\">%1</text><text id=\"2\">%2</text></binding></visual></toast>';"
            "$xml = New-Object Windows.Data.Xml.Dom.XmlDocument;"
            "$xml.LoadXml($template);"
            "$toast = New-Object Windows.UI.Notifications.ToastNotification($xml);"
            "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier("
            "'Sentinel').Show($toast);");
        runSynchronousProcess(
            QStringLiteral("powershell"),
            QStringList{QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                        QStringLiteral("-Command"), psScript.arg(escapedTitle, cleanMessage)},
            QString(), 15000, &error);
#else
        runSynchronousProcess(
            QStringLiteral("notify-send"),
            QStringList{title.isEmpty() ? QStringLiteral("Sentinel") : title, message}, QString(),
            15000, &error);
#endif
        logs.append(error.isEmpty()
                        ? QStringLiteral("system-notify: Notification sent: %1").arg(message)
                        : QStringLiteral("system-notify: %1").arg(error));

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult RealToolExecutor::executeClipboardRead(const PlannedToolInvocation& invocation,
                                                           QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        QClipboard* clipboard = activeClipboard();
        if (!clipboard) {
            ToolExecutionResult failure{
                ToolExecutionStatus::Failed,
                QStringLiteral("clipboard-read: Clipboard is unavailable without a GUI session.")};
            failure.failureCategory = ToolFailureCategory::RuntimeUnavailable;
            return failure;
        }
        const QString text = clipboard->text();
        if (text.isEmpty()) {
            logs.append(QStringLiteral("clipboard-read: Clipboard is empty."));
        } else {
            QString preview = text;
            if (preview.size() > kWebFetchPreviewChars) {
                preview = preview.left(kWebFetchPreviewChars) +
                          QStringLiteral("\n... (content truncated at %1 characters)")
                              .arg(QString::number(kWebFetchPreviewChars));
            }
            logs.append(QStringLiteral("clipboard-read: (%1 characters)\n%2")
                            .arg(QString::number(text.size()), preview));
        }

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult
RealToolExecutor::executeClipboardWrite(const PlannedToolInvocation& invocation,
                                        QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        const QString text = getArgument(invocation, QStringLiteral("text"));
        if (text.isEmpty()) {
            logs.append(QStringLiteral("clipboard-write: No text argument provided."));
            continue;
        }
        QClipboard* clipboard = activeClipboard();
        if (!clipboard) {
            ToolExecutionResult failure{
                ToolExecutionStatus::Failed,
                QStringLiteral("clipboard-write: Clipboard is unavailable without a GUI session.")};
            failure.failureCategory = ToolFailureCategory::RuntimeUnavailable;
            return failure;
        }
        clipboard->setText(text);
        logs.append(QStringLiteral("clipboard-write: Copied %1 character(s) to the clipboard.")
                        .arg(QString::number(text.size())));

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult RealToolExecutor::executeSystemInfo(const PlannedToolInvocation& invocation,
                                                        QString& currentWorkingDirectory) const {
    const QString fullReport = systemInfoReport();
    const bool truncated = fullReport.size() > 4096;
    const QString report = fullReport.left(4096);
    const QJsonObject data{{QStringLiteral("report"), report},
                           {QStringLiteral("complete"), !truncated},
                           {QStringLiteral("truncated"), truncated},
                           {QStringLiteral("partialEvidence"), truncated}};
    return {ToolExecutionStatus::Succeeded, QStringLiteral("system-info:\n%1").arg(report),
            std::make_shared<StructuredObservation>(
                StructuredObservation{StructuredObservationKind::Generic, data})};
}

ToolExecutionResult RealToolExecutor::executeProcessList(const PlannedToolInvocation& invocation,
                                                         QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        logs.append(processListReport());

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult RealToolExecutor::executeCurrentTime(const PlannedToolInvocation& invocation,
                                                         QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        const QDateTime now = QDateTime::currentDateTime();
        logs.append(QStringLiteral("current-time: %1\nUTC: %2\nEpoch seconds: %3")
                        .arg(now.toString(QStringLiteral("dddd, yyyy-MM-dd HH:mm:ss t")),
                             now.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'")),
                             QString::number(now.toSecsSinceEpoch())));

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult RealToolExecutor::executeSetAlarm(const PlannedToolInvocation& invocation,
                                                      QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        if (!alarmStore_) {
            ToolExecutionResult failure{ToolExecutionStatus::Failed,
                                        QStringLiteral("set-alarm: No alarm store is configured.")};
            failure.failureCategory = ToolFailureCategory::RuntimeUnavailable;
            return failure;
        }
        const QString rawTime = getArgument(invocation, QStringLiteral("time"));
        const QString label = getArgument(invocation, QStringLiteral("label")).trimmed();
        const auto triggerAt = parseAlarmTime(rawTime);
        if (!triggerAt.isValid()) {
            ToolExecutionResult failure{
                ToolExecutionStatus::InvalidArguments,
                QStringLiteral("set-alarm: Could not parse time '%1'. Use HH:mm, HH:mm:ss, or an "
                               "ISO datetime (yyyy-MM-ddTHH:mm).")
                    .arg(rawTime)};
            failure.failureCategory = ToolFailureCategory::InvalidArguments;
            return failure;
        }
        const auto entry =
            alarmStore_->schedule(triggerAt, label.isEmpty() ? QStringLiteral("Alarm") : label);
        logs.append(QStringLiteral("set-alarm: Alarm scheduled at %1 (id: %2) - %3")
                        .arg(triggerAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), entry.id,
                             entry.label));

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult RealToolExecutor::executeListAlarms(const PlannedToolInvocation& invocation,
                                                        QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        if (!alarmStore_) {
            ToolExecutionResult failure{
                ToolExecutionStatus::Failed,
                QStringLiteral("list-alarms: No alarm store is configured.")};
            failure.failureCategory = ToolFailureCategory::RuntimeUnavailable;
            return failure;
        }
        const auto alarms = alarmStore_->active();
        if (alarms.isEmpty()) {
            logs.append(QStringLiteral("list-alarms: No active alarms."));
        } else {
            QStringList lines;
            for (const auto& alarm : alarms) {
                lines.append(
                    QStringLiteral("%1 - %2 - %3")
                        .arg(alarm.triggerAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                             alarm.label, alarm.id));
            }
            logs.append(QStringLiteral("list-alarms: %1 active alarm(s):\n%2")
                            .arg(QString::number(alarms.size()), lines.join(QLatin1Char('\n'))));
        }

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult RealToolExecutor::executeCancelAlarm(const PlannedToolInvocation& invocation,
                                                         QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        if (!alarmStore_) {
            ToolExecutionResult failure{
                ToolExecutionStatus::Failed,
                QStringLiteral("cancel-alarm: No alarm store is configured.")};
            failure.failureCategory = ToolFailureCategory::RuntimeUnavailable;
            return failure;
        }
        const QString id = getArgument(invocation, QStringLiteral("id")).trimmed();
        if (id.isEmpty()) {
            ToolExecutionResult failure{ToolExecutionStatus::InvalidArguments,
                                        QStringLiteral("cancel-alarm: No id argument provided.")};
            failure.failureCategory = ToolFailureCategory::InvalidArguments;
            return failure;
        }
        if (!alarmStore_->remove(id)) {
            ToolExecutionResult failure{
                ToolExecutionStatus::Failed,
                QStringLiteral("cancel-alarm: No active alarm with id %1.").arg(id)};
            failure.failureCategory = ToolFailureCategory::NotFound;
            return failure;
        }
        logs.append(QStringLiteral("cancel-alarm: Alarm %1 cancelled.").arg(id));

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult RealToolExecutor::executeTodoWrite(const PlannedToolInvocation& invocation,
                                                       QString&) const {
    if (invocation.runtimeSessionId.isEmpty())
        return {ToolExecutionStatus::Blocked,
                QStringLiteral("Checklist requires a runtime session.")};
    const auto document =
        QJsonDocument::fromJson(getArgument(invocation, QStringLiteral("todos")).toUtf8());
    if (!document.isArray())
        return {ToolExecutionStatus::InvalidArguments,
                QStringLiteral("Checklist must be an array.")};
    const auto todos = document.array();
    // Validate again at the handler boundary; never partially accept a malformed list.
    if (todos.size() > 100)
        return {ToolExecutionStatus::InvalidArguments,
                QStringLiteral("Checklist exceeds 100 items.")};
    for (const auto& value : todos) {
        const auto item = value.toObject();
        const auto content = item.value(QStringLiteral("content")).toString();
        const auto status = item.value(QStringLiteral("status")).toString();
        if (!value.isObject() || content.trimmed().isEmpty() || content.size() > 2000 ||
            (status != QLatin1String("pending") && status != QLatin1String("in_progress") &&
             status != QLatin1String("completed") && status != QLatin1String("cancelled")))
            return {ToolExecutionStatus::InvalidArguments,
                    QStringLiteral("Invalid checklist item.")};
    }
    {
        QMutexLocker lock(&todosMutex_);
        if (!sessionTodos_.contains(invocation.runtimeSessionId) && sessionTodos_.size() >= 256)
            return {ToolExecutionStatus::Failed,
                    QStringLiteral("Checklist session capacity reached.")};
        if (todos.isEmpty())
            sessionTodos_.remove(invocation.runtimeSessionId);
        else
            sessionTodos_.insert(invocation.runtimeSessionId, todos);
    }
    QString unusedDirectory;
    return executeTodoRead(invocation, unusedDirectory);
}

ToolExecutionResult RealToolExecutor::executeTodoRead(const PlannedToolInvocation& invocation,
                                                      QString&) const {
    if (invocation.runtimeSessionId.isEmpty())
        return {ToolExecutionStatus::Blocked,
                QStringLiteral("Checklist requires a runtime session.")};
    QJsonArray todos;
    {
        QMutexLocker lock(&todosMutex_);
        todos = sessionTodos_.value(invocation.runtimeSessionId);
    }
    const QJsonObject data{{QStringLiteral("todos"), todos},
                           {QStringLiteral("complete"), true},
                           {QStringLiteral("persistent"), false}};
    return {ToolExecutionStatus::Succeeded,
            QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)),
            std::make_shared<StructuredObservation>(
                StructuredObservation{StructuredObservationKind::Generic, data})};
}

ToolExecutionResult RealToolExecutor::executeMemorySearch(const PlannedToolInvocation& invocation,
                                                          QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        const QString query = getArgument(invocation, QStringLiteral("query")).trimmed();
        if (query.isEmpty()) {
            return {ToolExecutionStatus::InvalidArguments,
                    QStringLiteral("memory-search: No query argument provided.")};
        }
        if (!memoryStore_ || !memoryStore_->isAvailable()) {
            ToolExecutionResult failure{
                ToolExecutionStatus::Failed,
                QStringLiteral("memory-search: Memory store is unavailable.")};
            failure.failureCategory = ToolFailureCategory::RuntimeUnavailable;
            return failure;
        }
        const int limit = qBound(1, getIntArgument(invocation, QStringLiteral("limit"), 10), 50);
        auto context = memoryContextResolver_ ? memoryContextResolver_(invocation.runtimeSessionId)
                                              : memoryContext_;
        // Static standalone contexts cannot be borrowed by a different gateway session.
        if (!memoryContextResolver_ && !invocation.runtimeSessionId.isEmpty() &&
            invocation.runtimeSessionId != context.sessionId) {
            context = {};
        }
        const auto scoped = memoryStore_->retrieveMemory(query, context, limit);
        if (scoped.status != ScopedMemoryStatus::Ready) {
            ToolExecutionResult failure{
                ToolExecutionStatus::Failed,
                QStringLiteral("memory-search: Scoped memory access unavailable or unauthorized.")};
            failure.failureCategory = scoped.status == ScopedMemoryStatus::Unauthorized
                                          ? ToolFailureCategory::PermissionDenied
                                          : ToolFailureCategory::RuntimeUnavailable;
            return failure;
        }
        if (scoped.records.isEmpty()) {
            logs.append(QStringLiteral("memory-search: No memory entries match '%1'.").arg(query));
        } else {
            logs.append(QStringLiteral("memory-search: showing up to %1 match(es) for '%2':\n%3")
                            .arg(QString::number(limit), query, scopedMemoryContextText(scoped)));
        }

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult RealToolExecutor::executeHistorySearch(const PlannedToolInvocation& invocation,
                                                           QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        const QString query = getArgument(invocation, QStringLiteral("query")).trimmed();
        if (query.isEmpty()) {
            return {ToolExecutionStatus::InvalidArguments,
                    QStringLiteral("history-search: No query argument provided.")};
        }
        if (!chatHistoryStore_ || !chatHistoryStore_->isAvailable()) {
            ToolExecutionResult failure{
                ToolExecutionStatus::Failed,
                QStringLiteral("history-search: Chat history store is unavailable.")};
            failure.failureCategory = ToolFailureCategory::RuntimeUnavailable;
            return failure;
        }
        const int limit = qBound(1, getIntArgument(invocation, QStringLiteral("limit"), 10), 50);
        const auto messages = chatHistoryStore_->searchMessages(query, limit);
        QStringList matches;
        for (const auto& message : messages) {
            QString preview = QStringLiteral("history:%1 [%2] %3")
                                  .arg(message.id)
                                  .arg(chatRoleName(message.role), message.content.simplified());
            if (preview.size() > 400)
                preview = preview.left(400) + QStringLiteral("...");
            matches.append(preview);
        }

        if (matches.isEmpty()) {
            logs.append(
                QStringLiteral("history-search: No history entries match '%1'.").arg(query));
        } else {
            logs.append(QStringLiteral("history-search: showing up to %1 match(es) for '%2':\n%3")
                            .arg(QString::number(limit), query, matches.join(QLatin1Char('\n'))));
        }

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult RealToolExecutor::executeWebFetch(const PlannedToolInvocation& invocation,
                                                      QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        const QString url = getArgument(invocation, QStringLiteral("url")).trimmed();
        if (url.isEmpty()) {
            return {ToolExecutionStatus::InvalidArguments,
                    QStringLiteral("web-fetch: No url argument provided.")};
        }
        const QString format =
            getArgument(invocation, QStringLiteral("format")).trimmed().toLower();
        const auto fetchFormat =
            format == QStringLiteral("text")
                ? WebFetchFormat::Text
                : (format == QStringLiteral("html") ? WebFetchFormat::Html
                                                    : WebFetchFormat::Markdown);

        const auto response = webFetchTool_.fetch(url, fetchFormat);
        if (!response.success) {
            ToolExecutionResult failure{
                ToolExecutionStatus::Failed,
                QStringLiteral("web-fetch: Request failed: %1")
                    .arg(response.errorString.isEmpty()
                             ? QStringLiteral("HTTP %1").arg(response.statusCode)
                             : response.errorString.left(400))};
            failure.failureCategory = ToolFailureCategory::NetworkFailure;
            return failure;
        }
        QString content = response.content;
        const bool truncated = content.size() > kWebFetchPreviewChars;
        if (content.size() > kWebFetchPreviewChars) {
            content = content.left(kWebFetchPreviewChars) +
                      QStringLiteral("\n\n... [content truncated at %1 characters] ...")
                          .arg(QString::number(kWebFetchPreviewChars));
        }
        logs.append(QStringLiteral("web-fetch: %1\n\n%2").arg(url, content));
        const QJsonObject data{{QStringLiteral("url"), url},
                               {QStringLiteral("statusCode"), response.statusCode},
                               {QStringLiteral("contentLength"), response.contentLength},
                               {QStringLiteral("truncated"), truncated},
                               {QStringLiteral("partialEvidence"), truncated}};
        return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n")),
                std::make_shared<StructuredObservation>(
                    StructuredObservation{StructuredObservationKind::Generic, data})};

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

ToolExecutionResult
RealToolExecutor::executeVoiceTranscribe(const PlannedToolInvocation& invocation,
                                         QString& currentWorkingDirectory) const {
    const auto authorized =
        authorizedFilePath(fileSystemService_, invocation, QStringLiteral("path"), AccessMode::Read,
                           currentWorkingDirectory);
    if (!authorized.ok())
        return fileToolFailure(QStringLiteral("voice-transcribe"), authorized);
    const auto currentGrant = fileSystemService_.resolve(
        authorized.value->canonicalPath, currentWorkingDirectory, FileSystemAccess::Read);
    if (!currentGrant.ok() ||
        currentGrant.value->canonicalPath != authorized.value->canonicalPath ||
        currentGrant.value->deviceId != authorized.value->deviceId ||
        currentGrant.value->fileId != authorized.value->fileId)
        return {ToolExecutionStatus::Blocked, QStringLiteral("voice-transcribe: PermissionDenied")};
    if (!sttRuntime_)
        return {ToolExecutionStatus::Failed,
                QStringLiteral("voice-transcribe: RuntimeUnavailable")};
    const auto file = fileSystemService_.stat(*authorized.value);
    if (!file.ok() || !file.value->regularFile)
        return {ToolExecutionStatus::Failed, QStringLiteral("voice-transcribe: UnsupportedFormat")};
    const auto transcript = sttRuntime_->transcribeFile(authorized.value->canonicalPath, {}, {});
    return transcript.failure == AudioFailure::None
               ? ToolExecutionResult{ToolExecutionStatus::Succeeded,
                                     QStringLiteral("voice-transcribe: OK\n%1")
                                         .arg(transcript.finalText)}
               : ToolExecutionResult{
                     ToolExecutionStatus::Failed,
                     QStringLiteral("voice-transcribe: %1. %2")
                         .arg(audioFailureName(transcript.failure), transcript.detail.left(300))};
}

ToolExecutionResult RealToolExecutor::executeVoiceSpeak(const PlannedToolInvocation& invocation,
                                                        QString& currentWorkingDirectory) const {
    Q_UNUSED(currentWorkingDirectory);
    const auto text = getArgument(invocation, QStringLiteral("text"));
    if (text.trimmed().isEmpty())
        return {ToolExecutionStatus::InvalidArguments,
                QStringLiteral("voice-speak: Text is empty.")};
    if (!ttsRuntime_)
        return {ToolExecutionStatus::Failed, QStringLiteral("voice-speak: RuntimeUnavailable")};
    const auto audio = ttsRuntime_->synthesize(SpeechSynthesisRequest{text, {}, {}, 1.0}, {});
    return audio.failure == AudioFailure::None
               ? ToolExecutionResult{ToolExecutionStatus::Succeeded,
                                     QStringLiteral("voice-speak: audio generated at %1")
                                         .arg(audio.filePath)}
               : ToolExecutionResult{
                     ToolExecutionStatus::Failed,
                     QStringLiteral("voice-speak: %1. %2")
                         .arg(audioFailureName(audio.failure), audio.detail.left(300))};
}

ToolExecutionResult RealToolExecutor::executeWebSearch(const PlannedToolInvocation& invocation,
                                                       QString& currentWorkingDirectory) const {
    QStringList logs;
    do {
        const auto query = getArgument(invocation, QStringLiteral("query"));
        if (query.trimmed().isEmpty()) {
            return {ToolExecutionStatus::InvalidArguments,
                    QStringLiteral("web-search: No query argument provided.")};
        }

        const auto response = webSearchTool_.search(query.trimmed());
        if (!response.success) {
            ToolExecutionResult failure{ToolExecutionStatus::Failed,
                                        QStringLiteral("web-search: Request failed: %1")
                                            .arg(response.errorString.left(400))};
            failure.failureCategory = ToolFailureCategory::NetworkFailure;
            return failure;
        }
        for (int i = 0; i < response.results.size() && i < 20; ++i) {
            const auto& result = response.results.at(i);
            logs.append(QStringLiteral("%1. %2\n%3\n%4")
                            .arg(i + 1)
                            .arg(result.title.left(200))
                            .arg(result.url.left(500))
                            .arg(result.snippet.left(600)));
        }
        if (response.results.isEmpty()) {
            logs.append(QStringLiteral("web-search: No results found for '%1'.").arg(query));
        }
        const QJsonObject data{{QStringLiteral("query"), query.trimmed()},
                               {QStringLiteral("resultCount"), response.results.size()},
                               {QStringLiteral("complete"), false},
                               {QStringLiteral("truncated"), response.results.size() > 20},
                               {QStringLiteral("partialEvidence"), response.results.isEmpty()}};
        return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n")),
                std::make_shared<StructuredObservation>(
                    StructuredObservation{StructuredObservationKind::Generic, data})};

    } while (false);
    return {ToolExecutionStatus::Succeeded, logs.join(QStringLiteral("\n\n"))};
}

} // namespace sentinel::core
