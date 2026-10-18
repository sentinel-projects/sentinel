// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only credential-isolated daemon/application composition; not DaemonService main.
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/ApplicationControllerBuilder.h"
#include "sentinel/core/chat/SQLiteConversationStore.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include "sentinel/core/resource/ResourceMonitor.h"
#include "service/DaemonIpcServer.h"
#include <QCoreApplication>
#include <QDir>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <csignal>
namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) {
    stopping = 1;
}
class EmptyCatalog final : public sentinel::core::IOllamaRuntimeClient {
public:
    sentinel::core::OllamaConfig config() const override {
        return {};
    }
    sentinel::core::OllamaHealthCheckResult healthCheck() const override {
        return {};
    }
    QList<sentinel::core::OllamaModelSummary> installedModels() const override {
        return {};
    }
};
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3 || QString::fromLocal8Bit(argv[2]) != "--isolated-benchmark") {
        return 2;
    }
#ifndef Q_OS_UNIX
    return 77;
#endif
    const auto identity = "sentinel-b01-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    app.setApplicationName(identity);
    QStandardPaths::setTestModeEnabled(true);
    QStringList ownedPaths;
    for (auto location : {QStandardPaths::AppDataLocation, QStandardPaths::AppConfigLocation}) {
        const auto path = QStandardPaths::writableLocation(location);
        if (!path.endsWith(identity) || QDir(path).exists()) {
            return 3;
        }
        ownedPaths.append(path);
    }
    const auto cleanup = qScopeGuard([&] {
        for (const auto& path : ownedPaths) {
            if (QDir(path).exists()) {
                QDir(path).removeRecursively();
            }
        }
    });
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        return 3;
    }
    const QDir profile(temporary.path());
    sentinel::core::AppSettings settings(
        std::make_unique<sentinel::core::JsonSettingsStore>(profile.filePath("settings.json")),
        sentinel::core::inMemoryTestCredentialStore());
    settings.setNetworkMode("Offline");
    sentinel::core::ApplicationControllerBuilder builder;
    builder.withModelService(std::make_unique<sentinel::core::ModelService>(&settings))
        .withOllamaRuntimeClient(std::make_unique<EmptyCatalog>())
        .withMemoryStore(
            std::make_unique<sentinel::core::SQLiteMemoryStore>(profile.filePath("memory.sqlite3")))
        .withConversationStore(std::make_unique<sentinel::core::SQLiteConversationStore>(
            profile.filePath("chat.sqlite3")));
    auto controller = builder.build();
    auto server = std::make_unique<sentinel::daemon::DaemonIpcServer>(controller.get());
    server->setSettings(&settings);
    if (!server->openSessionProjectionStore(profile.filePath("projection.sqlite3")) ||
        !server->startServer(QString::fromLocal8Bit(argv[1]))) {
        return 4;
    }
    std::unique_ptr<sentinel::core::ResourceMonitor> monitor;
    if (qEnvironmentVariable("SENTINEL_RESOURCE_TELEMETRY") == "1") {
        monitor = std::make_unique<sentinel::core::ResourceMonitor>(
            sentinel::core::makeNativeResourceMonitorAdapter(), nullptr);
        if (!monitor->start()) {
            return 5;
        }
    }
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (stopping) {
            server->requestShutdown();
        }
    });
    QObject::connect(server.get(), &sentinel::daemon::DaemonIpcServer::shutdownDrained, &app,
                     &QCoreApplication::quit);
    timer.start(50);
    QTimer::singleShot(3660000, &app, &QCoreApplication::quit);
    qInfo("B01_ISOLATION in-memory-credentials offline empty-catalog no-selected-model");
    const int code = app.exec();
    if (monitor) {
        monitor->stop();
    }
    server.reset();
    controller.reset();
    for (auto location : {QStandardPaths::AppDataLocation, QStandardPaths::AppConfigLocation}) {
        const auto path = QStandardPaths::writableLocation(location);
        if (!path.endsWith(identity)) {
            return 6;
        }
        if (QDir(path).exists() && !QDir(path).removeRecursively()) {
            return 7;
        }
    }
    qInfo("B01_CLEANUP complete");
    return code;
}
