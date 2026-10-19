// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private test composition only. Never included by application executables.
#include "sentinel/core/agent/AgentRuntime.h"
#include "sentinel/core/agent/NullAgentRuntime.h"
#include "sentinel/core/app/ApplicationControllerBuilder.h"
#include "sentinel/core/app/WorkspaceService.h"
#include "sentinel/core/model/ModelManagement.h"
#include "sentinel/core/runtime/RealToolExecutor.h"
#include "sentinel/core/security/StaticSandboxPolicy.h"
#include "service/DaemonIpcServer.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSemaphore>
#include <QThread>
#include <atomic>
namespace sentinel::test {
struct DesktopScript {
    QString path;
    std::atomic_int decisions{0};
    std::atomic_bool sawObservation{false}, timedOut{false};
    QSemaphore entered, release;
    ~DesktopScript() {
        release.release();
    }
};
class DesktopCatalog final : public core::IOllamaRuntimeClient {
public:
    core::OllamaConfig config() const override {
        return {};
    }
    core::OllamaHealthCheckResult healthCheck() const override {
        core::OllamaHealthCheckResult result;
        result.connectionStatus = core::OllamaConnectionStatus::Connected;
        result.healthStatus = core::OllamaHealthStatus::Healthy;
        return result;
    }
    QList<core::OllamaModelSummary> installedModels() const override {
        return {{"sentinel-test-model", {}, 0}};
    }
};
class DesktopScriptedProvider final : public core::IChatProvider {
public:
    explicit DesktopScriptedProvider(std::shared_ptr<DesktopScript> script)
        : script_(std::move(script)) {}
    QString name() const override {
        return "P606-scripted";
    }
    core::ChatProviderStatus status() const override {
        return core::ChatProviderStatus::Ready;
    }
    bool supportsStreaming() const override {
        return true;
    }
    bool supportsBoundedStreaming() const override {
        return true;
    }
    core::ChatProviderReply sendMessageStreaming(const QString& text,
                                                 const std::function<void(const QString&)>& delta,
                                                 const std::shared_ptr<std::atomic_bool>& cancel,
                                                 const core::ChatRequestOptions& options) override {
        Q_ASSERT(options.maxOutputTokens > 0 && options.deadlineMs > 0);
        return sendMessageStreaming(text, delta, cancel);
    }
    core::ChatProviderReply sendMessage(const QString& text) override {
        return sendRequest(text, {});
    }
    core::ChatProviderReply sendRequest(const QString& text,
                                        const core::ChatRequestOptions&) override {
        QJsonObject body;
        if (text.startsWith("Classify the USER GOAL")) {
            body = {{"requirements", QJsonArray{QJsonObject{{"domain", "filesystem"},
                                                            {"resource", script_->path},
                                                            {"purpose", "operate"}},
                                                QJsonObject{{"domain", "filesystem"},
                                                            {"resource", script_->path},
                                                            {"purpose", "inspect"},
                                                            {"tool", "read-file"}}}}};
        } else {
            const auto step = script_->decisions++;
            if (step == 0) {
                body = {{"action", "tool"},
                        {"tool", "write-file"},
                        {"args", QJsonObject{{"path", script_->path},
                                             {"content", "p606 authorized bytes\n"}}}};
            } else if (step == 1) {
                body = {{"action", "tool"},
                        {"tool", "read-file"},
                        {"args", QJsonObject{{"path", script_->path}}}};
            } else {
                script_->sawObservation = text.contains("p606 authorized bytes");
                body = {{"action", "final"},
                        {"grounding", "verified"},
                        {"answer", "Fixture file changed and read back."}};
            }
        }
        core::ChatProviderReply reply;
        reply.success = true;
        reply.lifecycle = core::ChatRequestLifecycle::Completed;
        reply.message = QString::fromUtf8(QJsonDocument(body).toJson(QJsonDocument::Compact));
        return reply;
    }
    core::ChatProviderReply
    sendMessageStreaming(const QString&, const std::function<void(const QString&)>& delta,
                         const std::shared_ptr<std::atomic_bool>& cancel) override {
        delta("p606_delta ");
        script_->entered.release();
        int waited = 0;
        while (!script_->release.tryAcquire(1, 10) && !(cancel && cancel->load())) {
            waited += 10;
            if (waited >= 5000) {
                script_->timedOut = true;
                break;
            }
        }
        core::ChatProviderReply reply;
        if (cancel && cancel->load()) {
            reply.lifecycle = core::ChatRequestLifecycle::Cancelled;
            reply.category = core::ChatProviderErrorCategory::Cancelled;
            reply.errorMessage = "Cancelled";
            return reply;
        }
        if (script_->timedOut.load()) {
            reply.lifecycle = core::ChatRequestLifecycle::Failed;
            reply.category = core::ChatProviderErrorCategory::Timeout;
            return reply;
        }
        delta("completed");
        reply.success = true;
        reply.message = "p606_delta completed";
        reply.lifecycle = core::ChatRequestLifecycle::Completed;
        return reply;
    }

private:
    std::shared_ptr<DesktopScript> script_;
};
inline std::shared_ptr<DesktopScript>
configureDesktopDaemon(core::ApplicationControllerBuilder& builder, core::AppSettings& settings,
                       const QDir& profile) {
    auto script = std::make_shared<DesktopScript>();
    const auto root = profile.filePath("workspace");
    QDir().mkpath(root);
    script->path = QDir(root).filePath("allowed.txt");
    QFile file(script->path);
    if (!file.open(QIODevice::WriteOnly) || file.write("p606 original bytes\n") != 20) {
        return {};
    }
    file.close();
    settings.setNetworkMode("LocalOnly");
    settings.setOnboardingComplete(true);
    settings.setThemeName("Paper");
    settings.setReducedMotionEnabled(true);
    auto models = std::make_unique<core::ModelService>(&settings);
    models->registerProvider("ollama", [script](const core::ModelBinding&) {
        return std::make_shared<DesktopScriptedProvider>(script);
    });
    core::OllamaModelDiscoveryResult catalog;
    catalog.models = {{"sentinel-test-model", {}, 0}};
    catalog.lifecycle = core::ChatRequestLifecycle::Completed;
    catalog.errorCategory = core::ChatProviderErrorCategory::None;
    models->acceptOllamaDiscovery(catalog, models->beginProviderHealthObservation());
    models->setSelectedModel({"ollama", "sentinel-test-model"});
    builder.withModelService(std::move(models))
        .withOllamaRuntimeClient(std::make_unique<DesktopCatalog>())
        .withAgentRuntime(
            std::make_unique<core::NullAgentRuntime>(core::NullAgentRuntime::standardTools()))
        .withToolExecutor(std::make_unique<core::RealToolExecutor>())
        .withSandboxPolicy(std::make_unique<core::StaticSandboxPolicy>(
            QSet<QString>{"tool.metadata.read", "tool.risk.medium", "tool.risk.high"}));
    core::WorkspaceService workspaces;
    const auto created = workspaces.createWorkspace({}, "P606 fixture", "Coding");
    const auto rooted = workspaces.setWorkspaceRoot(
        created.catalogJson, created.selectedWorkspaceId, QFileInfo(root).canonicalFilePath());
    if (!rooted.success) {
        return {};
    }
    settings.setWorkspaceCatalogJson(rooted.catalogJson);
    settings.setSelectedWorkspaceId(created.selectedWorkspaceId);
    settings.setWorkspaceProfilesJson(
        workspaces.updateProfile({}, created.selectedWorkspaceId, {},
                                 {{"providerId", "ollama"}, {"modelId", "sentinel-test-model"}}));
    return script;
}
} // namespace sentinel::test
