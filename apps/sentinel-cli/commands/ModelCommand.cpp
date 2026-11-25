// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ModelCommand.h"

#include "ExitCodes.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/ApplicationControllerBuilder.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/platform/DpapiEncryptedSettingsStore.h"
#include "sentinel/core/platform/StandardPathProvider.h"
#include "sentinel/core/platform/ProfileWriteLease.h"
#include "sentinel/core/runtime/OllamaRuntime.h"

#include <QDebug>
#include <QEventLoop>
#include <QTimer>

#include <chrono>
#include <iomanip>
#include <iostream>

namespace sentinel::cli {

namespace {

// Model pull inactivity deadline. SENTINEL_CLI_PULL_IDLE_TIMEOUT_MS exists
// only so deterministic tests can bound the wait without changing command-line
// arguments; unset production behavior keeps the documented 60-second
// inactivity deadline (docs/reference/CLI_REFERENCE.md). The deadline measures
// time without progress or status movement, so a healthy long download that
// keeps streaming is never cut off.
int pullIdleDeadlineMs() {
    bool ok = false;
    const int envMs = qEnvironmentVariableIntValue("SENTINEL_CLI_PULL_IDLE_TIMEOUT_MS", &ok);
    if (ok && envMs >= 100) {
        return envMs;
    }
    return 60000;
}

} // namespace

int executeModelCommand(const QStringList& args) {
    const QString sub = args.value(0, QStringLiteral("list"));

    sentinel::core::StandardPathProvider pathProvider;
    sentinel::core::ProfileWriteLease profileWriteLease;
    if (!profileWriteLease.acquire(pathProvider)) {
        qCritical("Qt profile is reserved for a Rust cutover.");
        return 1;
    }
    sentinel::core::AppSettings settings(
        std::make_unique<sentinel::core::DpapiEncryptedSettingsStore>(
            std::make_unique<sentinel::core::JsonSettingsStore>(pathProvider.settingsFilePath())));

    sentinel::core::ApplicationControllerBuilder builder;
    auto controller = builder.withStandardDefaults(pathProvider, settings).build();

    if (sub == QStringLiteral("list")) {
        std::cout << "Ollama Endpoint: " << settings.ollamaEndpoint().toStdString() << std::endl;

        QEventLoop loop;
        auto statusRefreshed = false;
        QObject::connect(controller.get(),
                         &sentinel::core::ApplicationController::ollamaStatusChanged, &loop,
                         [&loop, &statusRefreshed]() {
                             statusRefreshed = true;
                             loop.quit();
                         });

        controller->refreshOllamaStatus();
        QTimer listDeadline;
        listDeadline.setSingleShot(true);
        QObject::connect(&listDeadline, &QTimer::timeout, &loop, [&loop]() { loop.quit(); });
        listDeadline.start(5000);
        loop.exec();
        listDeadline.stop();

        std::cout << "Ollama Health:    " << controller->ollamaHealthStatus().toStdString() << " ("
                  << controller->ollamaConnectionStatus().toStdString() << ")" << std::endl;

        const auto discovery = controller->ollamaDiscovery();
        if (!statusRefreshed) {
            std::cerr << "Model discovery did not complete: discovery deadline expired after 5 "
                         "seconds."
                      << std::endl;
            return kExitTimeout;
        }
        if (discovery.lifecycle == sentinel::core::ChatRequestLifecycle::Pending ||
            discovery.lifecycle == sentinel::core::ChatRequestLifecycle::Cancelled) {
            std::cerr << "Model discovery did not complete: " << discovery.safeDetail.toStdString()
                      << std::endl;
            return kExitFailure;
        }
        if (!discovery.succeeded()) {
            std::cerr << "Model discovery failed: " << discovery.safeDetail.toStdString()
                      << std::endl;
            return kExitFailure;
        }
        const auto& models = discovery.models;
        if (models.isEmpty()) {
            std::cout << "Installed models: none" << std::endl;
        } else {
            std::cout << "Installed models:" << std::endl;
            for (const auto& model : models) {
                const double sizeMb = static_cast<double>(model.sizeBytes) / (1024.0 * 1024.0);
                std::cout << "  - " << model.name.toStdString();
                if (model.sizeBytes > 0) {
                    std::cout << " (" << std::fixed << std::setprecision(1) << sizeMb << " MB)";
                }
                std::cout << std::endl;
            }
        }
        return kExitSuccess;
    }

    if (sub == QStringLiteral("pull")) {
        const QString modelId = args.value(1).trimmed();
        if (modelId.isEmpty()) {
            // OllamaModelPuller::pull() silently ignores a blank model and
            // emits nothing, which previously left this command waiting in its
            // event loop forever.
            std::cerr << "Usage: sentinel-cli model pull <model_name>" << std::endl;
            return kExitUsage;
        }

        OllamaModelPuller puller;
        puller.setEndpoint(settings.ollamaEndpoint());

        QEventLoop loop;
        auto finished = false;
        auto pullSucceeded = false;
        auto pullTimedOut = false;
        QObject::connect(&puller, &OllamaModelPuller::pullFinished, &loop,
                         [&loop, &finished, &pullSucceeded](const QString& model, bool success) {
                             finished = true;
                             pullSucceeded = success;
                             std::cout << std::endl
                                       << (success ? "Pull completed: " : "Pull failed: ")
                                       << model.toStdString() << std::endl;
                             loop.quit();
                         });

        const int idleDeadlineMs = pullIdleDeadlineMs();
        auto lastProgress = puller.progress();
        auto lastStatus = puller.statusText();
        auto lastActivity = std::chrono::steady_clock::now();

        QTimer progressTimer;
        QObject::connect(&progressTimer, &QTimer::timeout, &loop,
                         [&loop, &puller, &finished, &pullTimedOut, &lastProgress, &lastStatus,
                          &lastActivity, idleDeadlineMs]() {
                             if (finished) {
                                 return;
                             }
                             std::cout
                                 << "\r  Progress: " << static_cast<int>(puller.progress() * 100)
                                 << "% - " << puller.statusText().toStdString() << "          ";
                             std::cout.flush();

                             const auto progress = puller.progress();
                             const auto status = puller.statusText();
                             if (progress != lastProgress || status != lastStatus) {
                                 lastProgress = progress;
                                 lastStatus = status;
                                 lastActivity = std::chrono::steady_clock::now();
                                 return;
                             }
                             if (std::chrono::steady_clock::now() - lastActivity >=
                                 std::chrono::milliseconds(idleDeadlineMs)) {
                                 pullTimedOut = true;
                                 std::cerr
                                     << std::endl
                                     << "sentinel-cli: model pull stalled: no progress within "
                                     << idleDeadlineMs << " ms" << std::endl;
                                 // Stop the in-flight request so a late success
                                 // callback cannot reclassify this run.
                                 puller.cancel();
                                 loop.quit();
                             }
                         });
        progressTimer.start(250);

        std::cout << "Pulling model: " << modelId.toStdString() << " ..." << std::endl;
        puller.pull(modelId);

        if (!finished) {
            loop.exec();
        }
        progressTimer.stop();

        // The deadline decision wins over any callback that arrives afterwards.
        if (pullTimedOut) {
            return kExitTimeout;
        }
        if (!finished) {
            std::cerr << "sentinel-cli: model pull failed: no result was reported for "
                      << modelId.toStdString() << std::endl;
            return kExitFailure;
        }
        if (!pullSucceeded) {
            std::cerr << "sentinel-cli: model pull failed: " << modelId.toStdString();
            const auto errorText = puller.errorText();
            if (!errorText.trimmed().isEmpty()) {
                std::cerr << " (" << errorText.toStdString() << ")";
            }
            std::cerr << std::endl;
            return kExitFailure;
        }
        return kExitSuccess;
    }

    std::cerr << "Unknown model subcommand: " << sub.toStdString() << std::endl;
    std::cerr << "Available: list, pull" << std::endl;
    return kExitUsage;
}

} // namespace sentinel::cli
