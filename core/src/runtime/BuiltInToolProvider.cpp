// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/runtime/BuiltInToolProvider.h"
#include "sentinel/core/security/AuthorizationResolver.h"
#include <QDir>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
namespace sentinel::core {
namespace {
QJsonObject builtInSchema(const ToolDescriptor& tool) {
    QJsonObject properties;
    QJsonArray required;
    for (const auto& parameter : tool.parameters) {
        QJsonObject field{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("description"), parameter.description}};
        if (parameter.required) {
            required.append(parameter.id);
            if (parameter.id != QLatin1String("content") &&
                parameter.id != QLatin1String("oldString") &&
                parameter.id != QLatin1String("newString"))
                field.insert(QStringLiteral("minLength"), 1);
        }
        if (parameter.id == QLatin1String("staged") ||
            parameter.id == QLatin1String("includeHidden") ||
            parameter.id == QLatin1String("replaceAll")) {
            field.insert(QStringLiteral("type"), QStringLiteral("boolean"));
            field.insert(QStringLiteral("default"), false);
            field.remove(QStringLiteral("minLength"));
        } else if (parameter.id == QLatin1String("timeout") ||
                   parameter.id == QLatin1String("offset") ||
                   parameter.id == QLatin1String("limit")) {
            field.insert(QStringLiteral("type"), QStringLiteral("integer"));
            field.insert(QStringLiteral("minimum"), 1);
            field.remove(QStringLiteral("minLength"));
            if (parameter.id == QLatin1String("timeout")) {
                field.insert(QStringLiteral("minimum"), 1000);
                field.insert(QStringLiteral("maximum"), 600000);
                field.insert(QStringLiteral("default"), 60000);
            } else if (parameter.id == QLatin1String("offset"))
                field.insert(QStringLiteral("default"), 1);
            else if (tool.id == QLatin1String("memory-search") ||
                     tool.id == QLatin1String("history-search")) {
                field.insert(QStringLiteral("maximum"), 50);
                field.insert(QStringLiteral("default"), 10);
            } else
                field.insert(QStringLiteral("default"), 2000);
        } else if (tool.id == QLatin1String("todo-write") &&
                   parameter.id == QLatin1String("todos")) {
            field = QJsonObject{
                {QStringLiteral("type"), QStringLiteral("array")},
                {QStringLiteral("maxItems"), 100},
                {QStringLiteral("items"),
                 QJsonObject{
                     {QStringLiteral("type"), QStringLiteral("object")},
                     {QStringLiteral("properties"),
                      QJsonObject{
                          {QStringLiteral("content"),
                           QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                       {QStringLiteral("minLength"), 1},
                                       {QStringLiteral("maxLength"), 2000}}},
                          {QStringLiteral("status"),
                           QJsonObject{
                               {QStringLiteral("type"), QStringLiteral("string")},
                               {QStringLiteral("enum"),
                                QJsonArray{QStringLiteral("pending"), QStringLiteral("in_progress"),
                                           QStringLiteral("completed"),
                                           QStringLiteral("cancelled")}}}},
                          {QStringLiteral("priority"),
                           QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                       {QStringLiteral("enum"),
                                        QJsonArray{QStringLiteral("low"), QStringLiteral("medium"),
                                                   QStringLiteral("high")}},
                                       {QStringLiteral("default"), QStringLiteral("medium")}}}}},
                     {QStringLiteral("required"),
                      QJsonArray{QStringLiteral("content"), QStringLiteral("status")}},
                     {QStringLiteral("additionalProperties"), false}}}};
        } else if (tool.id == QLatin1String("spawn-agent") &&
                   parameter.id == QLatin1String("purpose")) {
            field.insert(QStringLiteral("enum"), QJsonArray{QStringLiteral("independent_research"),
                                                            QStringLiteral("parallel_verification"),
                                                            QStringLiteral("specialist_review")});
        } else if (tool.id == QLatin1String("spawn-agent") &&
                   parameter.id == QLatin1String("allowedTools")) {
            field = QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                {QStringLiteral("items"),
                                 QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
                                {QStringLiteral("maxItems"), 12}};
        } else if (tool.id == QLatin1String("web-fetch") &&
                   parameter.id == QLatin1String("format")) {
            field.insert(QStringLiteral("enum"),
                         QJsonArray{QStringLiteral("markdown"), QStringLiteral("text"),
                                    QStringLiteral("html")});
            field.insert(QStringLiteral("default"), QStringLiteral("markdown"));
        } else if (tool.id == QLatin1String("run-command") &&
                   parameter.id == QLatin1String("sandbox")) {
            field.insert(QStringLiteral("enum"),
                         QJsonArray{QStringLiteral(""), QStringLiteral("docker")});
            field.insert(QStringLiteral("default"), QStringLiteral(""));
        }
        properties.insert(parameter.id, field);
    }
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), required},
                       {QStringLiteral("additionalProperties"), false}};
}
QList<ToolEvidenceDescriptor> builtInEvidence(const QString& id) {
    using D = ObservationDomain;
    using F = EvidenceFreshness;
    using S = EvidenceScope;
    auto one = [](D domain, F freshness, S scope, QString argument = {}, QString qualifier = {}) {
        return QList<ToolEvidenceDescriptor>{
            {domain, freshness, scope, std::move(argument), std::move(qualifier)}};
    };
    if (id == QLatin1String("git-status") || id == QLatin1String("git-diff"))
        return one(D::Workspace, F::Live, S::ExactResource, QStringLiteral("path"));
    if (id == QLatin1String("stat-path"))
        return one(D::FileSystem, F::Live, S::ExactResource, QStringLiteral("path"));
    if (id == QLatin1String("list-directory"))
        return one(D::FileSystem, F::Live, S::DirectoryEntries, QStringLiteral("path"));
    if (id == QLatin1String("read-file") || id == QLatin1String("list-code-definitions"))
        return one(D::FileSystem, F::TurnScoped, S::ExactResource, QStringLiteral("path"));
    if (id == QLatin1String("grep") || id == QLatin1String("glob"))
        return one(D::FileSystem, F::Live, S::SearchScope, QStringLiteral("path"),
                   QStringLiteral("pattern"));
    if (id == QLatin1String("write-file") || id == QLatin1String("edit-file") ||
        id == QLatin1String("delete-file"))
        return one(D::FileSystem, F::TurnScoped, S::Operation, QStringLiteral("path"));
    if (id == QLatin1String("move-file"))
        return {{D::FileSystem, F::TurnScoped, S::Operation, QStringLiteral("source"), {}},
                {D::FileSystem, F::TurnScoped, S::Operation, QStringLiteral("destination"), {}}};
    if (id == QLatin1String("apply-patch"))
        return one(D::Workspace, F::TurnScoped, S::Operation);
    if (id == QLatin1String("process-list"))
        return one(D::Process, F::Live, S::ExactResource);
    if (id == QLatin1String("system-info") || id == QLatin1String("current-time"))
        return one(D::System, F::Live, S::ExactResource);
    if (id == QLatin1String("clipboard-read"))
        return one(D::Clipboard, F::Live, S::ExactResource);
    if (id == QLatin1String("web-search"))
        return one(D::Network, F::Live, S::SearchScope, QStringLiteral("query"));
    if (id == QLatin1String("web-fetch"))
        return one(D::Network, F::Live, S::ExactResource, QStringLiteral("url"));
    if (id == QLatin1String("browser-screenshot") || id == QLatin1String("browser-pdf"))
        return one(D::Browser, F::Live, S::ExactResource, QStringLiteral("url"));
    if (id == QLatin1String("memory-search"))
        return one(D::Memory, F::SessionStable, S::SearchScope, QStringLiteral("query"));
    if (id == QLatin1String("history-search"))
        return one(D::ConversationHistory, F::SessionStable, S::SearchScope,
                   QStringLiteral("query"));
    if (id == QLatin1String("run-command"))
        return one(D::ProcessExecution, F::TurnScoped, S::Operation, QStringLiteral("command"));
    if (id == QLatin1String("app-launch") || id == QLatin1String("app-quit"))
        return one(D::Application, F::TurnScoped, S::Operation, QStringLiteral("app"));
    if (id == QLatin1String("open-url"))
        return one(D::Browser, F::TurnScoped, S::Operation, QStringLiteral("url"));
    return {};
}
} // namespace
QList<ToolDescriptor> BuiltInToolProvider::descriptors() {
    QList<ToolDescriptor> tools = {
        ToolDescriptor{
            QStringLiteral("git-status"),
            QStringLiteral("Git Status"),
            QStringLiteral("Inspect repository status using fixed Git arguments. "
                           "Read-only, optional index locking and fsmonitor disabled."),
            ToolRiskLevel::Medium,
            ToolExecutionMode::Local,
            {ToolParameterDescriptor{QStringLiteral("path"),
                                     QStringLiteral("Authorized repository directory."), true}}},
        ToolDescriptor{
            QStringLiteral("git-diff"),
            QStringLiteral("Git Diff"),
            QStringLiteral("Inspect unstaged or staged repository changes. "
                           "External diff and text conversion commands are disabled."),
            ToolRiskLevel::Medium,
            ToolExecutionMode::Local,
            {ToolParameterDescriptor{QStringLiteral("path"),
                                     QStringLiteral("Authorized repository directory."), true},
             ToolParameterDescriptor{QStringLiteral("staged"),
                                     QStringLiteral("Inspect staged changes (default false)."),
                                     false}}},
        ToolDescriptor{
            QStringLiteral("stat-path"),
            QStringLiteral("Stat Path"),
            QStringLiteral(
                "Inspect one authorized file or directory: path, type and byte size. "
                "A missing path returns a typed failure; it is not a successful observation."),
            ToolRiskLevel::Low,
            ToolExecutionMode::Local,
            {ToolParameterDescriptor{
                QStringLiteral("path"),
                QStringLiteral("Workspace-relative or authorized absolute path."), true}}},
        ToolDescriptor{QStringLiteral("local-plan-summary"),
                       QStringLiteral("Local Plan Summary"),
                       QStringLiteral("Local planning summary tool."),
                       ToolRiskLevel::Low,
                       ToolExecutionMode::MetadataOnly,
                       {
                           ToolParameterDescriptor{
                               QStringLiteral("topic"),
                               QStringLiteral("Short user topic for local summary."), true},
                       }},
        ToolDescriptor{
            QStringLiteral("list-directory"),
            QStringLiteral("List Directory"),
            QStringLiteral("Lists root entries only (non-recursive, max 500). Hidden entries "
                           "excluded by default; includeHidden:true includes them. "
                           "Completeness/truncation/permission metadata describes only this root. "
                           "Use glob with includeHidden:true for recursive scope."),
            ToolRiskLevel::Low,
            ToolExecutionMode::Local,
            {ToolParameterDescriptor{
                 QStringLiteral("path"),
                 QStringLiteral(
                     "Directory path: '.' for the active workspace root, or a workspace-relative "
                     "directory. External paths require separate authorization."),
                 true},
             ToolParameterDescriptor{QStringLiteral("includeHidden"),
                                     QStringLiteral("Include hidden entries (true/false)."),
                                     false}}},
        ToolDescriptor{
            QStringLiteral("read-file"),
            QStringLiteral("Read File"),
            QStringLiteral("Reads a text file inside the workspace. Output has line numbers. "
                           "Use offset/limit to page through long files instead of re-reading "
                           "the whole file. Use list-directory for directories."),
            ToolRiskLevel::Low,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{
                    QStringLiteral("path"),
                    QStringLiteral("Absolute or workspace-relative text file path. "
                                   "Directories require list-directory; use actual "
                                   "workspace paths, not invented /workspace paths."),
                    true},
                ToolParameterDescriptor{QStringLiteral("offset"),
                                        QStringLiteral("1-based line to start from (default 1)."),
                                        false},
                ToolParameterDescriptor{QStringLiteral("limit"),
                                        QStringLiteral("Max lines to return (default 2000)."),
                                        false},
            }},
        ToolDescriptor{
            QStringLiteral("write-file"),
            QStringLiteral("Write File"),
            QStringLiteral("Writes a full file. Prefer edit-file for small changes to existing "
                           "files."),
            ToolRiskLevel::High,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("path"),
                                        QStringLiteral("Absolute path to write."), true},
                ToolParameterDescriptor{QStringLiteral("content"), QStringLiteral("File contents."),
                                        true},
            }},
        ToolDescriptor{
            QStringLiteral("edit-file"),
            QStringLiteral("Edit File"),
            QStringLiteral("Edits a file by replacing oldString with newString. Fuzzy matching "
                           "handles minor whitespace/indentation differences, but provide the "
                           "exact text including surrounding context when possible. oldString "
                           "must be unique in the file unless replaceAll is true. Use an empty "
                           "oldString to create a new file."),
            ToolRiskLevel::High,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("path"), QStringLiteral("File to edit."),
                                        true},
                ToolParameterDescriptor{
                    QStringLiteral("oldString"),
                    QStringLiteral("Text to replace (empty to create a new file)."), true},
                ToolParameterDescriptor{QStringLiteral("newString"),
                                        QStringLiteral("Replacement text."), true},
                ToolParameterDescriptor{QStringLiteral("replaceAll"),
                                        QStringLiteral("true to replace every occurrence."), false},
            }},
        ToolDescriptor{
            QStringLiteral("grep"),
            QStringLiteral("Grep"),
            QStringLiteral("Searches file contents with a regular expression under a directory "
                           "(default: workspace). Returns up to 100 matches as 'Line N: text'. "
                           "Use this instead of grep/find in run-command."),
            ToolRiskLevel::Low,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("pattern"),
                                        QStringLiteral("Regular expression."), true},
                ToolParameterDescriptor{QStringLiteral("path"),
                                        QStringLiteral("Directory to search (default: workspace)."),
                                        false},
                ToolParameterDescriptor{QStringLiteral("include"),
                                        QStringLiteral("Glob filter like *.cpp or *.{ts,tsx}."),
                                        false},
                ToolParameterDescriptor{
                    QStringLiteral("includeHidden"),
                    QStringLiteral("Request hidden inclusion (default false); does not by itself "
                                   "guarantee complete traversal."),
                    false},
            }},
        ToolDescriptor{
            QStringLiteral("glob"),
            QStringLiteral("Glob"),
            QStringLiteral("Finds files by glob pattern (e.g. *.cpp, src/**/*.h) under a "
                           "directory recursively, depth max 128, no symlink targets; traversal "
                           "max 5000 files and result max 100 paths. Hidden entries excluded by "
                           "default. includeHidden:true includes hidden entries; typed coverage "
                           "controls absence claims."),
            ToolRiskLevel::Low,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("pattern"), QStringLiteral("Glob pattern."),
                                        true},
                ToolParameterDescriptor{QStringLiteral("path"),
                                        QStringLiteral("Directory to search (default: workspace)."),
                                        false},
                ToolParameterDescriptor{
                    QStringLiteral("includeHidden"),
                    QStringLiteral("Request hidden inclusion (default false); does not by itself "
                                   "guarantee complete traversal."),
                    false},
            }},
        ToolDescriptor{
            QStringLiteral("delete-file"),
            QStringLiteral("Delete File"),
            QStringLiteral("Deletes a single file inside the workspace. Directories are "
                           "refused. Deletion is permanent; prefer move-file when unsure."),
            ToolRiskLevel::High,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("path"), QStringLiteral("File to delete."),
                                        true},
            }},
        ToolDescriptor{QStringLiteral("move-file"),
                       QStringLiteral("Move File"),
                       QStringLiteral("Moves or renames a file inside the workspace. Existing "
                                      "destination files are never overwritten."),
                       ToolRiskLevel::High,
                       ToolExecutionMode::Local,
                       {
                           ToolParameterDescriptor{QStringLiteral("source"),
                                                   QStringLiteral("File to move."), true},
                           ToolParameterDescriptor{
                               QStringLiteral("destination"),
                               QStringLiteral("Target path (may use a new file name)."), true},
                       }},
        ToolDescriptor{
            QStringLiteral("apply-patch"),
            QStringLiteral("Apply Patch"),
            QStringLiteral("Applies a unified diff patch to workspace files. Supports update "
                           "(--- a/file + +++ b/file), add (--- /dev/null), and delete "
                           "(+++ /dev/null) with @@ hunk headers. Prefer this over many "
                           "edit-file calls for multi-file or multi-hunk changes."),
            ToolRiskLevel::High,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("patch"),
                                        QStringLiteral("The complete unified diff patch text."),
                                        true},
            }},
        ToolDescriptor{
            QStringLiteral("list-code-definitions"),
            QStringLiteral("List Code Definitions"),
            QStringLiteral("Lists classes, functions, and other top-level symbols with line "
                           "numbers for a source file (C/C++, Python, JavaScript/"
                           "TypeScript, Rust). Use it to understand file structure before "
                           "reading or editing."),
            ToolRiskLevel::Low,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("path"), QStringLiteral("Source file path."),
                                        true},
            }},
        ToolDescriptor{
            QStringLiteral("run-command"),
            QStringLiteral("Run Command"),
            QStringLiteral(
                "Runs a shell command and returns stdout/stderr. Prefer dedicated tools "
                "(read-file, edit-file, grep, glob) over cat/grep/find. Use the timeout "
                "parameter (milliseconds) for long-running commands; default 60000, max "
                "600000. Use workdir instead of 'cd x && ...'. Set sandbox=docker to run the "
                "command inside an isolated, network-disabled Docker container with the "
                "workspace mounted at /workspace (requires Docker)."),
            ToolRiskLevel::High,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("command"),
                                        QStringLiteral("The shell command to run."), true},
                ToolParameterDescriptor{
                    QStringLiteral("timeout"),
                    QStringLiteral("Timeout in milliseconds (default 60000, max 600000)."), false},
                ToolParameterDescriptor{QStringLiteral("workdir"),
                                        QStringLiteral("Working directory for the command."),
                                        false},
                ToolParameterDescriptor{
                    QStringLiteral("sandbox"),
                    QStringLiteral("empty for local shell, or 'docker' for container sandbox."),
                    false},
            }},
        ToolDescriptor{
            QStringLiteral("app-launch"),
            QStringLiteral("Launch App"),
            QStringLiteral("Launches a desktop application by name (e.g. 'Spotify', 'Firefox'). "
                           "Use this for opening apps; not for scripts."),
            ToolRiskLevel::High,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("app"),
                                        QStringLiteral("Application name (e.g. Spotify)."), true},
                ToolParameterDescriptor{QStringLiteral("args"),
                                        QStringLiteral("Optional argument to pass."), false},
            }},
        ToolDescriptor{
            QStringLiteral("app-quit"),
            QStringLiteral("Quit App"),
            QStringLiteral("Quits a running desktop application by name (e.g. 'Spotify')."),
            ToolRiskLevel::High,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("app"),
                                        QStringLiteral("Application name (e.g. Spotify)."), true},
            }},
        ToolDescriptor{
            QStringLiteral("open-url"),
            QStringLiteral("Open URL"),
            QStringLiteral("Opens a URL in the user's default web browser (http/https only). "
                           "Use for website requests like 'sahibinden.com aç' — domains are "
                           "websites, not applications. Use web-fetch instead when the page "
                           "content itself is needed."),
            ToolRiskLevel::Medium,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("url"), QStringLiteral("The URL to open."),
                                        true},
            }},
        ToolDescriptor{QStringLiteral("system-notify"),
                       QStringLiteral("System Notify"),
                       QStringLiteral("Shows an immediate desktop notification. For reminders at a "
                                      "future time use set-alarm instead."),
                       ToolRiskLevel::Medium,
                       ToolExecutionMode::Local,
                       {
                           ToolParameterDescriptor{QStringLiteral("title"),
                                                   QStringLiteral("Notification title."), false},
                           ToolParameterDescriptor{QStringLiteral("message"),
                                                   QStringLiteral("Notification body."), true},
                       }},
        ToolDescriptor{QStringLiteral("clipboard-read"),
                       QStringLiteral("Clipboard Read"),
                       QStringLiteral("Reads the current system clipboard text."),
                       ToolRiskLevel::Medium,
                       ToolExecutionMode::Local,
                       {}},
        ToolDescriptor{QStringLiteral("clipboard-write"),
                       QStringLiteral("Clipboard Write"),
                       QStringLiteral("Replaces the system clipboard text."),
                       ToolRiskLevel::Medium,
                       ToolExecutionMode::Local,
                       {
                           ToolParameterDescriptor{QStringLiteral("text"),
                                                   QStringLiteral("Text to copy."), true},
                       }},
        ToolDescriptor{
            QStringLiteral("system-info"),
            QStringLiteral("System Info"),
            QStringLiteral(
                "Reports the operating system, kernel, CPU architecture, hostname, "
                "user, total RAM, root volume usage, and knownDirectories (home, desktop, "
                "documents, downloads) resolved by the operating system. Directory metadata "
                "does not grant filesystem access. Read-only."),
            ToolRiskLevel::Low,
            ToolExecutionMode::Local,
            {}},
        ToolDescriptor{
            QStringLiteral("process-list"),
            QStringLiteral("Process List"),
            QStringLiteral("Lists currently running processes (pid, CPU, name). Read-only; use "
                           "app-quit to close an application."),
            ToolRiskLevel::Medium,
            ToolExecutionMode::Local,
            {}},
        ToolDescriptor{
            QStringLiteral("current-time"),
            QStringLiteral("Current Time"),
            QStringLiteral("Returns the current local date and time with the timezone, UTC "
                           "time, and epoch seconds."),
            ToolRiskLevel::Low,
            ToolExecutionMode::Local,
            {}},
        ToolDescriptor{
            QStringLiteral("set-alarm"),
            QStringLiteral("Set Alarm"),
            QStringLiteral("Schedules a future alarm/reminder. The user is notified in chat "
                           "with a desktop notification when it fires. Time formats: 'HH:mm' "
                           "(next occurrence), 'HH:mm:ss', or ISO 'yyyy-MM-ddTHH:mm'."),
            ToolRiskLevel::Medium,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("time"),
                                        QStringLiteral("When the alarm fires."), true},
                ToolParameterDescriptor{QStringLiteral("label"),
                                        QStringLiteral("What the alarm is about."), true},
            }},
        ToolDescriptor{QStringLiteral("list-alarms"),
                       QStringLiteral("List Alarms"),
                       QStringLiteral("Lists all active scheduled alarms."),
                       ToolRiskLevel::Low,
                       ToolExecutionMode::Local,
                       {}},
        ToolDescriptor{
            QStringLiteral("cancel-alarm"),
            QStringLiteral("Cancel Alarm"),
            QStringLiteral("Cancels an active alarm by its id (see list-alarms for ids)."),
            ToolRiskLevel::Low,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("id"),
                                        QStringLiteral("The alarm id returned by set-alarm."),
                                        true},
            }},
        ToolDescriptor{
            QStringLiteral("todo-write"),
            QStringLiteral("Todo Write"),
            QStringLiteral("Records the task checklist for this goal as a JSON array of "
                           "{content, status, priority} objects. status: pending | in_progress "
                           "| completed | cancelled. Use for goals with 3+ steps: create the "
                           "list first, mark exactly one item in_progress at a time, and mark "
                           "completed only after verifying. Always pass the full list (it "
                           "replaces the previous list)."),
            ToolRiskLevel::Low,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{
                    QStringLiteral("todos"),
                    QStringLiteral("JSON array, e.g. [{\"content\":\"step\",\"status\":"
                                   "\"pending\",\"priority\":\"high\"}]"),
                    true},
            }},
        ToolDescriptor{QStringLiteral("todo-read"),
                       QStringLiteral("Todo Read"),
                       QStringLiteral("Reads the current task checklist for this goal."),
                       ToolRiskLevel::Low,
                       ToolExecutionMode::Local,
                       {}},
        ToolDescriptor{
            QStringLiteral("memory-search"),
            QStringLiteral("Memory Search"),
            QStringLiteral("Searches Sentinel's long-term memory entries (keys and values) for "
                           "the query text. Use this before asking the user for information "
                           "that may already be remembered."),
            ToolRiskLevel::Low,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("query"),
                                        QStringLiteral("Text to search for."), true},
                ToolParameterDescriptor{
                    QStringLiteral("limit"),
                    QStringLiteral("Max matches to return (default 10, max 50)."), false},
            }},
        ToolDescriptor{
            QStringLiteral("history-search"),
            QStringLiteral("History Search"),
            QStringLiteral("Searches earlier messages of this chat history (all roles) for "
                           "the query text. Use it to recall what the user said before."),
            ToolRiskLevel::Low,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("query"),
                                        QStringLiteral("Text to search for."), true},
                ToolParameterDescriptor{
                    QStringLiteral("limit"),
                    QStringLiteral("Max matches to return (default 10, max 50)."), false},
            }},
        ToolDescriptor{
            QStringLiteral("spawn-agent"),
            QStringLiteral("Spawn Agent"),
            QStringLiteral("Delegates a self-contained subtask to a bounded read-only "
                           "subagent and returns its final answer. Delegate only independent "
                           "research, parallel verification, or a bounded specialist review. "
                           "Do not delegate a single-step or one-tool task, a trivial answer, "
                           "or a retry of failed work. Subagents cannot spawn further "
                           "subagents or edit files."),
            ToolRiskLevel::Medium,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{
                    QStringLiteral("task"),
                    QStringLiteral("The complete, self-contained task description."), true},
                ToolParameterDescriptor{
                    QStringLiteral("purpose"),
                    QStringLiteral(
                        "independent_research, parallel_verification, or specialist_review."),
                    true},
                ToolParameterDescriptor{QStringLiteral("role"),
                                        QStringLiteral("Optional short role label."), false},
                ToolParameterDescriptor{QStringLiteral("allowedTools"),
                                        QStringLiteral("Optional subset of read-only tool ids."),
                                        false},
                ToolParameterDescriptor{QStringLiteral("workspace"),
                                        QStringLiteral("Optional workspace subdirectory scope."),
                                        false},
                ToolParameterDescriptor{
                    QStringLiteral("modelId"),
                    QStringLiteral("Optional current model id; other models are refused."), false},
            }},
        ToolDescriptor{
            QStringLiteral("browser-screenshot"),
            QStringLiteral("Browser Screenshot"),
            QStringLiteral("Takes a full-page screenshot of a URL in a headless Chromium "
                           "browser via Playwright and saves it as PNG. Requires Node.js "
                           "(npx) and 'npx playwright install chromium'. Use open-url when "
                           "the user just wants to see the page."),
            ToolRiskLevel::Medium,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("url"), QStringLiteral("The page URL."),
                                        true},
            }},
        ToolDescriptor{
            QStringLiteral("browser-pdf"),
            QStringLiteral("Browser PDF"),
            QStringLiteral("Renders a URL to a PDF document in a headless Chromium browser "
                           "via Playwright. Requires Node.js (npx) and 'npx playwright "
                           "install chromium'."),
            ToolRiskLevel::Medium,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("url"), QStringLiteral("The page URL."),
                                        true},
            }},
        ToolDescriptor{
            QStringLiteral("web-fetch"),
            QStringLiteral("Web Fetch"),
            QStringLiteral("Fetches a single http(s) URL and returns page content (markdown by "
                           "default). Use after web-search to open a result."),
            ToolRiskLevel::High,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("url"), QStringLiteral("The URL to fetch."),
                                        true},
                ToolParameterDescriptor{
                    QStringLiteral("format"),
                    QStringLiteral("markdown | text | html (default markdown)."), false},
            }},
        ToolDescriptor{
            QStringLiteral("web-search"),
            QStringLiteral("Web Search"),
            QStringLiteral("Web lookup querying the network via local client. Use for current "
                           "news, weather, or anything requiring fresh information."),
            ToolRiskLevel::High,
            ToolExecutionMode::Local,
            {
                ToolParameterDescriptor{QStringLiteral("query"),
                                        QStringLiteral("The search query."), true},
            }},
        ToolDescriptor{QStringLiteral("voice-transcribe"),
                       QStringLiteral("Voice Transcribe"),
                       QStringLiteral("Transcribes an existing local audio file with Whisper."),
                       ToolRiskLevel::High,
                       ToolExecutionMode::Local,
                       {
                           ToolParameterDescriptor{QStringLiteral("path"),
                                                   QStringLiteral("Path to the audio file."), true},
                       }},
        ToolDescriptor{QStringLiteral("voice-speak"),
                       QStringLiteral("Voice Speak"),
                       QStringLiteral("Synthesizes speech audio from text with Piper."),
                       ToolRiskLevel::High,
                       ToolExecutionMode::Local,
                       {
                           ToolParameterDescriptor{QStringLiteral("text"),
                                                   QStringLiteral("The text to speak."), true},
                       }},
    };
    const QSet<QString> filesystem{QStringLiteral("stat-path"),
                                   QStringLiteral("list-directory"),
                                   QStringLiteral("read-file"),
                                   QStringLiteral("write-file"),
                                   QStringLiteral("edit-file"),
                                   QStringLiteral("delete-file"),
                                   QStringLiteral("move-file"),
                                   QStringLiteral("apply-patch"),
                                   QStringLiteral("grep"),
                                   QStringLiteral("glob"),
                                   QStringLiteral("list-code-definitions")};
    const QSet<QString> network{QStringLiteral("web-fetch"), QStringLiteral("web-search")};
    const QSet<QString> voice{QStringLiteral("voice-transcribe"), QStringLiteral("voice-speak")};
    const QSet<QString> system{QStringLiteral("run-command"),  QStringLiteral("app-launch"),
                               QStringLiteral("app-quit"),     QStringLiteral("system-info"),
                               QStringLiteral("process-list"), QStringLiteral("current-time")};
    const QSet<QString> portableReady{
        QStringLiteral("stat-path"),    QStringLiteral("list-directory"),
        QStringLiteral("read-file"),    QStringLiteral("write-file"),
        QStringLiteral("edit-file"),    QStringLiteral("delete-file"),
        QStringLiteral("move-file"),    QStringLiteral("grep"),
        QStringLiteral("glob"),         QStringLiteral("list-code-definitions"),
        QStringLiteral("current-time"), QStringLiteral("apply-patch"),
        QStringLiteral("system-info")};
    const QHash<QString, QString> experimentalReasons{
        {QStringLiteral("git-status"),
         QStringLiteral("Requires Git and an enforceable process sandbox; output is bounded.")},
        {QStringLiteral("git-diff"),
         QStringLiteral("Requires Git and an enforceable process sandbox; output is bounded.")},
        {QStringLiteral("run-command"),
         QStringLiteral(
             "Sandbox backend availability and command effects are platform-dependent.")},
        {QStringLiteral("app-launch"),
         QStringLiteral("Detached desktop launch cannot be sandboxed on Linux; launch "
                        "acknowledgement does not prove the app opened.")},
        {QStringLiteral("app-quit"),
         QStringLiteral("Process-name matching and graceful quit semantics differ by platform.")},
        {QStringLiteral("open-url"),
         QStringLiteral("Desktop opener acknowledgement does not prove browser navigation.")},
        {QStringLiteral("system-notify"),
         QStringLiteral("Desktop notification delivery is not observable across platforms.")},
        {QStringLiteral("clipboard-read"),
         QStringLiteral("GUI clipboard availability and observation completeness are not "
                        "represented structurally.")},
        {QStringLiteral("clipboard-write"),
         QStringLiteral("GUI clipboard availability and write confirmation are not represented "
                        "structurally.")},
        {QStringLiteral("process-list"),
         QStringLiteral(
             "Platform process listings are truncated and lack structured completeness metadata.")},
        {QStringLiteral("set-alarm"),
         QStringLiteral(
             "Alarm scheduling acknowledgement has no structured persistence/delivery receipt.")},
        {QStringLiteral("list-alarms"),
         QStringLiteral("Alarm listing has no bounded structured completeness observation.")},
        {QStringLiteral("cancel-alarm"),
         QStringLiteral("Alarm removal has no structured persistence confirmation.")},
        {QStringLiteral("todo-write"),
         QStringLiteral("Checklist state is executor-wide rather than session-scoped.")},
        {QStringLiteral("todo-read"),
         QStringLiteral("Checklist state is executor-wide rather than session-scoped.")},
        {QStringLiteral("memory-search"),
         QStringLiteral(
             "Search has no structured completeness contract; absence is not authoritative.")},
        {QStringLiteral("history-search"),
         QStringLiteral(
             "Search has no structured completeness contract; absence is not authoritative.")},
        {QStringLiteral("spawn-agent"),
         QStringLiteral("Child completion and cancellation depend on parent runtime wiring.")},
        {QStringLiteral("browser-screenshot"),
         QStringLiteral(
             "Requires externally installed Playwright/Chromium; output is a private artifact.")},
        {QStringLiteral("browser-pdf"),
         QStringLiteral(
             "Requires externally installed Playwright/Chromium; output is a private artifact.")},
        {QStringLiteral("web-fetch"),
         QStringLiteral("Synchronous network client cannot cancel an in-flight request.")},
        {QStringLiteral("web-search"),
         QStringLiteral("Synchronous network client cannot cancel an in-flight request or prove "
                        "search completeness.")},
        {QStringLiteral("voice-transcribe"),
         QStringLiteral("Requires configured Whisper binary and audio input support.")},
        {QStringLiteral("voice-speak"),
         QStringLiteral("Requires Piper binary and model; synthesis does not imply playback.")}};
    const QSet<QString> cancellable{QStringLiteral("git-status"),
                                    QStringLiteral("git-diff"),
                                    QStringLiteral("list-directory"),
                                    QStringLiteral("glob"),
                                    QStringLiteral("grep"),
                                    QStringLiteral("list-code-definitions"),
                                    QStringLiteral("run-command"),
                                    QStringLiteral("process-list"),
                                    QStringLiteral("browser-screenshot"),
                                    QStringLiteral("browser-pdf"),
                                    QStringLiteral("voice-transcribe"),
                                    QStringLiteral("voice-speak"),
                                    QStringLiteral("spawn-agent")};
    for (auto& tool : tools) {
        tool.maturity = portableReady.contains(tool.id) ? ToolMaturity::ProductionReady
                                                        : ToolMaturity::Experimental;
        tool.maturityReason = experimentalReasons.value(tool.id);
        if (portableReady.contains(tool.id))
            tool.platforms = {ToolPlatformSupport::Supported, ToolPlatformSupport::Supported,
                              ToolPlatformSupport::Supported};
        if (tool.id == QLatin1String("app-launch"))
            tool.platforms.linux = ToolPlatformSupport::Unsupported;
        if (tool.id == QLatin1String("app-quit"))
            tool.platforms.linux = ToolPlatformSupport::PartiallySupported;
        tool.cancellationSupport =
            cancellable.contains(tool.id)             ? ToolCancellationSupport::Cancellable
            : tool.id == QLatin1String("apply-patch") ? ToolCancellationSupport::SafeBoundary
                                                      : ToolCancellationSupport::Atomic;
        if (filesystem.contains(tool.id))
            tool.errorSemantics = ToolErrorSemantics::Typed;
        if (tool.id == QLatin1String("run-command") || tool.id == QLatin1String("web-fetch") ||
            tool.id == QLatin1String("web-search") ||
            tool.id == QLatin1String("voice-transcribe") || tool.id == QLatin1String("voice-speak"))
            tool.errorSemantics = ToolErrorSemantics::Typed;
        if (tool.id == QLatin1String("local-plan-summary")) {
            tool.exposedToModel = false;
            tool.enabled = false;
            tool.maturity = ToolMaturity::Internal;
            tool.maturityReason =
                QStringLiteral("Internal planning metadata; never model-visible.");
        }
        if (tool.id == QLatin1String("todo-write") || tool.id == QLatin1String("todo-read")) {
            tool.structuredObservationKind = StructuredObservationKind::Generic;
            tool.maturityReason = QStringLiteral(
                "Session-isolated bounded checklist; volatile until daemon restart.");
        }
        static const QSet<QString> parallelBuiltIns{
            QStringLiteral("stat-path"),   QStringLiteral("list-directory"),
            QStringLiteral("read-file"),   QStringLiteral("grep"),
            QStringLiteral("glob"),        QStringLiteral("list-code-definitions"),
            QStringLiteral("write-file"),  QStringLiteral("edit-file"),
            QStringLiteral("delete-file"), QStringLiteral("move-file"),
            QStringLiteral("web-search"),  QStringLiteral("web-fetch")};
        tool.parallelSafe = parallelBuiltIns.contains(tool.id);
        tool.inputSchema = builtInSchema(tool);
        tool.evidenceProduced = builtInEvidence(tool.id);
        if (tool.id == QLatin1String("stat-path"))
            tool.structuredObservationKind = StructuredObservationKind::FileSystemFact;
        else if (tool.id == QLatin1String("list-directory"))
            tool.structuredObservationKind = StructuredObservationKind::DirectoryListing;
        else if (tool.id == QLatin1String("glob"))
            tool.structuredObservationKind = StructuredObservationKind::PathMatches;
        else if (tool.id == QLatin1String("read-file"))
            tool.structuredObservationKind = StructuredObservationKind::FileContent;
        else if (tool.id == QLatin1String("grep"))
            tool.structuredObservationKind = StructuredObservationKind::TextSearch;
        else if (tool.id == QLatin1String("list-code-definitions"))
            tool.structuredObservationKind = StructuredObservationKind::CodeDefinitions;
        else if (tool.id == QLatin1String("git-status") || tool.id == QLatin1String("git-diff"))
            tool.structuredObservationKind = StructuredObservationKind::Generic;
        else if (tool.id == QLatin1String("system-info") || tool.id == QLatin1String("run-command"))
            tool.structuredObservationKind = StructuredObservationKind::Generic;
        else if (tool.id == QLatin1String("web-fetch") || tool.id == QLatin1String("web-search"))
            tool.structuredObservationKind = StructuredObservationKind::Generic;
        tool.source = ToolSource::BuiltIn;
        tool.providerId = QStringLiteral("builtin");
        if (tool.id == QLatin1String("git-status") || tool.id == QLatin1String("git-diff")) {
            tool.category = QStringLiteral("Git");
            tool.requiredPermissionDomain = QStringLiteral("subprocess-execution");
            tool.errorSemantics = ToolErrorSemantics::Typed;
        } else if (filesystem.contains(tool.id)) {
            tool.category = QStringLiteral("Filesystem");
            tool.requiredPermissionDomain = tool.riskLevel == ToolRiskLevel::Low
                                                ? QStringLiteral("workspace-access")
                                                : QStringLiteral("filesystem-write");
        } else if (network.contains(tool.id)) {
            tool.category = QStringLiteral("Network");
            tool.scope = ToolScope::Cloud;
            tool.requiredPermissionDomain = QStringLiteral("cloud-provider-access");
        } else if (voice.contains(tool.id)) {
            tool.category = QStringLiteral("Voice");
            tool.requiredPermissionDomain = tool.id == QLatin1String("voice-transcribe")
                                                ? QStringLiteral("voice-capture")
                                                : QStringLiteral("voice-playback");
        } else if (system.contains(tool.id)) {
            tool.category = QStringLiteral("System");
            tool.requiredPermissionDomain = QStringLiteral("subprocess-execution");
        } else {
            tool.category = QStringLiteral("Agent");
            tool.requiredPermissionDomain = QStringLiteral("context-injection");
        }

        using D = SecurityDomain;
        using A = AccessMode;
        using R = AuthorizationResourceKind;
        const auto add = [&tool](D domain, A access, R kind = R::None, QString argument = {},
                                 QString fixed = {}) {
            tool.authorizationRequirements.append(
                {domain, access, kind, std::move(argument), std::move(fixed)});
        };
        if (tool.id == QLatin1String("git-status") || tool.id == QLatin1String("git-diff")) {
            add(D::FileSystem, A::Read, R::FileSystemPath, QStringLiteral("path"));
            add(D::Process, A::Execute, R::ArgumentDigest, QStringLiteral("path"));
        } else if (tool.id == QLatin1String("stat-path") ||
                   tool.id == QLatin1String("list-directory") ||
                   tool.id == QLatin1String("read-file") || tool.id == QLatin1String("grep") ||
                   tool.id == QLatin1String("glob") ||
                   tool.id == QLatin1String("list-code-definitions")) {
            add(D::FileSystem, A::Read, R::FileSystemPath, QStringLiteral("path"));
        } else if (tool.id == QLatin1String("write-file")) {
            add(D::FileSystem, A::Write, R::FileSystemPath, QStringLiteral("path"));
        } else if (tool.id == QLatin1String("edit-file")) {
            add(D::FileSystem, A::Read, R::FileSystemPath, QStringLiteral("path"));
            add(D::FileSystem, A::Write, R::FileSystemPath, QStringLiteral("path"));
        } else if (tool.id == QLatin1String("delete-file")) {
            add(D::FileSystem, A::Delete, R::FileSystemPath, QStringLiteral("path"));
        } else if (tool.id == QLatin1String("move-file")) {
            add(D::FileSystem, A::Delete, R::FileSystemPath, QStringLiteral("source"));
            add(D::FileSystem, A::Write, R::FileSystemPath, QStringLiteral("destination"));
        } else if (tool.id == QLatin1String("apply-patch")) {
            add(D::FileSystem, A::Write);
        } else if (tool.id == QLatin1String("run-command")) {
            add(D::Process, A::Execute, R::ArgumentDigest, QStringLiteral("command"));
        } else if (tool.id == QLatin1String("web-fetch")) {
            add(D::Network, A::Read, R::Host, QStringLiteral("url"));
        } else if (tool.id == QLatin1String("web-search")) {
            add(D::Network, A::Invoke, R::None);
        } else if (tool.id == QLatin1String("browser-screenshot") ||
                   tool.id == QLatin1String("browser-pdf") ||
                   tool.id == QLatin1String("open-url")) {
            add(D::Browser, A::Invoke, R::Host, QStringLiteral("url"));
        } else if (tool.id == QLatin1String("clipboard-read")) {
            add(D::Clipboard, A::Read);
        } else if (tool.id == QLatin1String("clipboard-write")) {
            add(D::Clipboard, A::Write);
        } else if (tool.id == QLatin1String("memory-search")) {
            add(D::Memory, A::Read);
        } else if (tool.id == QLatin1String("history-search")) {
            add(D::Conversation, A::Read);
        } else if (tool.id == QLatin1String("voice-transcribe")) {
            add(D::Audio, A::Read);
            add(D::FileSystem, A::Read, R::FileSystemPath, QStringLiteral("path"));
        } else if (tool.id == QLatin1String("voice-speak")) {
            add(D::Audio, A::Invoke);
        } else if (tool.id == QLatin1String("app-launch")) {
            add(D::Application, A::Execute);
        } else if (tool.id == QLatin1String("app-quit")) {
            add(D::Application, A::Control);
        } else if (tool.id == QLatin1String("spawn-agent")) {
            add(D::Agent, A::Invoke);
        } else if (tool.id == QLatin1String("system-info") ||
                   tool.id == QLatin1String("current-time") ||
                   tool.id == QLatin1String("process-list")) {
            add(D::System, A::Read);
        } else if (tool.id == QLatin1String("system-notify")) {
            add(D::System, A::Control);
        } else if (tool.id == QLatin1String("set-alarm") ||
                   tool.id == QLatin1String("cancel-alarm")) {
            add(D::Application, A::Control);
        } else if (tool.id == QLatin1String("list-alarms")) {
            add(D::Application, A::Read);
        } else if (tool.id == QLatin1String("todo-write")) {
            add(D::Conversation, A::Write);
        } else if (tool.id == QLatin1String("todo-read")) {
            add(D::Conversation, A::Read);
        } else if (tool.id == QLatin1String("local-plan-summary")) {
            add(D::Agent, A::Invoke);
        }
    }
    return tools;
}

namespace {
struct BuiltInOperation {
    RealToolExecutor::BuiltInMethod method;
    bool processBacked;
};
const QHash<QString, BuiltInOperation> kMethods = {
    {QStringLiteral("git-status"), {&RealToolExecutor::executeGitInspection, true}},
    {QStringLiteral("git-diff"), {&RealToolExecutor::executeGitInspection, true}},
    {QStringLiteral("stat-path"), {&RealToolExecutor::executeStatPath, false}},
    {QStringLiteral("local-plan-summary"), {&RealToolExecutor::executeLocalPlanSummary, false}},
    {QStringLiteral("list-directory"), {&RealToolExecutor::executeListDirectory, false}},
    {QStringLiteral("read-file"), {&RealToolExecutor::executeReadFile, false}},
    {QStringLiteral("write-file"), {&RealToolExecutor::executeWriteFile, false}},
    {QStringLiteral("edit-file"), {&RealToolExecutor::executeEditFile, false}},
    {QStringLiteral("grep"), {&RealToolExecutor::executeGrep, false}},
    {QStringLiteral("glob"), {&RealToolExecutor::executeGlob, false}},
    {QStringLiteral("delete-file"), {&RealToolExecutor::executeDeleteFile, false}},
    {QStringLiteral("move-file"), {&RealToolExecutor::executeMoveFile, false}},
    {QStringLiteral("apply-patch"), {&RealToolExecutor::executeApplyPatch, false}},
    {QStringLiteral("list-code-definitions"),
     {&RealToolExecutor::executeListCodeDefinitions, false}},
    {QStringLiteral("run-command"), {&RealToolExecutor::executeRunCommand, true}},
    {QStringLiteral("app-launch"), {&RealToolExecutor::executeAppLaunch, true}},
    {QStringLiteral("app-quit"), {&RealToolExecutor::executeAppQuit, true}},
    {QStringLiteral("open-url"), {&RealToolExecutor::executeOpenUrl, true}},
    {QStringLiteral("system-notify"), {&RealToolExecutor::executeSystemNotify, true}},
    {QStringLiteral("clipboard-read"), {&RealToolExecutor::executeClipboardRead, false}},
    {QStringLiteral("clipboard-write"), {&RealToolExecutor::executeClipboardWrite, false}},
    {QStringLiteral("system-info"), {&RealToolExecutor::executeSystemInfo, false}},
    {QStringLiteral("process-list"), {&RealToolExecutor::executeProcessList, true}},
    {QStringLiteral("current-time"), {&RealToolExecutor::executeCurrentTime, false}},
    {QStringLiteral("set-alarm"), {&RealToolExecutor::executeSetAlarm, false}},
    {QStringLiteral("list-alarms"), {&RealToolExecutor::executeListAlarms, false}},
    {QStringLiteral("cancel-alarm"), {&RealToolExecutor::executeCancelAlarm, false}},
    {QStringLiteral("todo-write"), {&RealToolExecutor::executeTodoWrite, false}},
    {QStringLiteral("todo-read"), {&RealToolExecutor::executeTodoRead, false}},
    {QStringLiteral("memory-search"), {&RealToolExecutor::executeMemorySearch, false}},
    {QStringLiteral("history-search"), {&RealToolExecutor::executeHistorySearch, false}},
    {QStringLiteral("spawn-agent"), {&RealToolExecutor::executeSpawnAgent, false}},
    {QStringLiteral("browser-screenshot"), {&RealToolExecutor::executeBrowserScreenshot, true}},
    {QStringLiteral("browser-pdf"), {&RealToolExecutor::executeBrowserPdf, true}},
    {QStringLiteral("web-fetch"), {&RealToolExecutor::executeWebFetch, false}},
    {QStringLiteral("web-search"), {&RealToolExecutor::executeWebSearch, false}},
    {QStringLiteral("voice-transcribe"), {&RealToolExecutor::executeVoiceTranscribe, true}},
    {QStringLiteral("voice-speak"), {&RealToolExecutor::executeVoiceSpeak, true}},
};
class BuiltInHandler final : public IToolHandler,
                             public std::enable_shared_from_this<BuiltInHandler> {
public:
    BuiltInHandler(RealToolExecutor& executor, RealToolExecutor::BuiltInMethod method,
                   bool processBacked)
        : executor_(executor), method_(method), processBacked_(processBacked) {}

    QString unavailableReason() const override {
        if (method_ == &RealToolExecutor::executeGitInspection &&
            QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
            return QStringLiteral("Git executable is not installed.");
        if ((method_ == &RealToolExecutor::executeClipboardRead ||
             method_ == &RealToolExecutor::executeClipboardWrite) &&
            !qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
            return QStringLiteral("Clipboard requires a GUI session; this runtime is headless.");
        return {};
    }

    IToolExecutor::Cancel execute(const ToolExecutionRequest& request, const QString& sessionId,
                                  const QString& toolCallId, IToolExecutor::Output output,
                                  IToolExecutor::Completion completion) override {
        if (processBacked_)
            return executor_.executeAsync(request, sessionId, toolCallId, std::move(output),
                                          std::move(completion));
        const auto& id = request.plan.invocations.first().toolId;
        // The gateway has validated this snapshot against the approved resources.
        // Never replace its frozen workspace with the application's process cwd.
        const auto& resources = request.plan.invocations.first().resourceSnapshot;
        const QString executionDirectory =
            resources ? resources->workingDirectory : QDir::currentPath();
        const bool cancellable = id == QLatin1String("list-directory") ||
                                 id == QLatin1String("glob") || id == QLatin1String("grep") ||
                                 id == QLatin1String("list-code-definitions") ||
                                 id == QLatin1String("apply-patch");
        if (request.callbackContext && request.plan.invocations.first().descriptorSnapshot &&
            request.plan.invocations.first().descriptorSnapshot->parallelSafe) {
            auto invocation = request.plan.invocations.first();
            auto token = std::make_shared<std::atomic_bool>(false);
            invocation.toolCancellation = token;
            auto self = shared_from_this();
            QPointer<QObject> context(request.callbackContext);
            const QString cwd = executionDirectory;
            pool_.start([self, invocation = std::move(invocation), cwd, context, token,
                         completion = std::move(completion)]() mutable {
                ToolExecutionResult result;
                if (token->load() || (invocation.cancellation && invocation.cancellation->load()))
                    result = {ToolExecutionStatus::Cancelled,
                              QStringLiteral("Tool cancelled before execution.")};
                else {
                    QString workingDirectory = cwd;
                    result = (self->executor_.*self->method_)(invocation, workingDirectory);
                }
                if (context)
                    QMetaObject::invokeMethod(
                        context.data(),
                        [completion = std::move(completion), result = std::move(result)]() mutable {
                            completion(std::move(result));
                        },
                        Qt::QueuedConnection);
            });
            return [token] { token->store(true); };
        }
        const bool inlineInspection =
            request.synchronousReadOnly &&
            (id == QLatin1String("glob") || id == QLatin1String("read-file"));
        if (cancellable && !sessionId.isEmpty() && !inlineInspection) {
            auto invocation = request.plan.invocations.first();
            auto token = std::make_shared<std::atomic_bool>(false);
            invocation.toolCancellation = token;
            auto self = shared_from_this();
            QTimer::singleShot(0, [self, invocation = std::move(invocation), cancellable,
                                   executionDirectory, completion = std::move(completion),
                                   token]() mutable {
                if (token->load() || (invocation.cancellation && invocation.cancellation->load())) {
                    completion(
                        {ToolExecutionStatus::Cancelled,
                         QStringLiteral("Filesystem operation cancelled before inspection.")});
                    return;
                }
                QString cwd = executionDirectory;
                auto result = (self->executor_.*self->method_)(invocation, cwd);
                if (cancellable && invocation.toolId != QLatin1String("apply-patch") &&
                    (token->load() ||
                     (invocation.cancellation && invocation.cancellation->load())) &&
                    result.status == ToolExecutionStatus::Succeeded) {
                    result.status = ToolExecutionStatus::Cancelled;
                    if (result.structuredObservation) {
                        auto partial =
                            std::make_shared<StructuredObservation>(*result.structuredObservation);
                        partial->data.insert(QStringLiteral("complete"), false);
                        partial->data.insert(QStringLiteral("cancelled"), true);
                        result.structuredObservation = partial;
                    }
                }
                completion(std::move(result));
            });
            return [token] { token->store(true); };
        }
        QString currentWorkingDirectory = executionDirectory;
        completion((executor_.*method_)(request.plan.invocations.first(), currentWorkingDirectory));
        return {};
    }

private:
    RealToolExecutor& executor_;
    RealToolExecutor::BuiltInMethod method_;
    bool processBacked_;
    QThreadPool pool_;
};
} // namespace
bool BuiltInToolProvider::registerTools(IToolRegistry& registry, RealToolExecutor& executor) {
    const auto tools = descriptors();
    if (tools.size() != static_cast<int>(kMethods.size()))
        return false;
    QList<IToolRegistry::Registration> registrations;
    for (int i = 0; i < tools.size(); ++i) {
        auto descriptor = tools.at(i);
        descriptor.source = ToolSource::BuiltIn;
        descriptor.providerId = QStringLiteral("builtin");
        const auto operation = kMethods.value(descriptor.id);
        if (!operation.method)
            return false;
        auto handler =
            std::make_shared<BuiltInHandler>(executor, operation.method, operation.processBacked);
        registrations.append({std::move(descriptor), std::move(handler)});
    }
    return registry.replaceProvider(ToolSource::BuiltIn, QStringLiteral("builtin"),
                                    std::move(registrations));
}
} // namespace sentinel::core
