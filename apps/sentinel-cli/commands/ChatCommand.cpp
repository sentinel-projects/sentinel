// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ChatCommand.h"

#include "ExitCodes.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/ApplicationControllerBuilder.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/platform/DpapiEncryptedSettingsStore.h"
#include "sentinel/core/platform/StandardPathProvider.h"
#include "sentinel/core/platform/ProfileWriteLease.h"

#include <QDebug>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTextStream>
#include <QTimer>
#include <iostream>

namespace sentinel::cli {

namespace {

// Chat response deadline. SENTINEL_CLI_CHAT_TIMEOUT_MS exists only so
// deterministic tests can bound the wait without changing command-line
// arguments; when it is unset or invalid the documented 30-second deadline
// applies (docs/reference/CLI_REFERENCE.md).
int chatDeadlineMs() {
    bool ok = false;
    const int envMs = qEnvironmentVariableIntValue("SENTINEL_CLI_CHAT_TIMEOUT_MS", &ok);
    if (ok && envMs >= 100) {
        return envMs;
    }
    return 30000;
}

} // namespace

int executeChatCommand(const QStringList& args) {
    const QString prompt = args.join(QLatin1Char(' '));
    if (prompt.trimmed().isEmpty()) {
        std::cerr << "Usage: sentinel-cli chat <prompt>" << std::endl;
        return kExitUsage;
    }

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

    const int deadlineMs = chatDeadlineMs();

    enum class WaitPhase { Discovery, Response };
    auto phase = WaitPhase::Discovery;
    auto responded = false;
    auto discoveryDeadlineExpired = false;
    auto responseTimedOut = false;

    QEventLoop loop;
    QTimer deadlineTimer;
    deadlineTimer.setSingleShot(true);

    QObject::connect(&deadlineTimer, &QTimer::timeout, &loop, [&]() {
        if (phase == WaitPhase::Discovery) {
            discoveryDeadlineExpired = true;
            loop.quit();
            return;
        }
        responseTimedOut = true;
        // Preserve the historical marker on stdout for existing scripts; the
        // failure detail goes to stderr as required by the exit contract.
        std::cout << "[Timeout waiting for response]" << std::endl;
        std::cerr << "sentinel-cli: chat timed out after " << deadlineMs << " ms without a response"
                  << std::endl;
        loop.quit();
    });

    QObject::connect(controller.get(), &sentinel::core::ApplicationController::agentResponseChanged,
                     &loop, [&controller, &loop, &responded, &responseTimedOut]() {
                         // A reply that settles after the deadline decision must
                         // not re-enter the reporting path.
                         if (responseTimedOut) {
                             return;
                         }
                         responded = true;
                         const QString response = controller->lastAgentResponse();
                         std::cout << "\nSentinel: " << response.toStdString() << std::endl;
                         loop.quit();
                     });

    // The chat request is evaluated against the model discovery snapshot, and
    // a fresh process starts Pending, so wait (bounded) for the first
    // discovery round to settle before sending. A failed discovery still
    // settles this wait; the send that follows decides the exit code.
    if (!controller->ollamaDiscovery().succeeded()) {
        auto discoveryConn = QObject::connect(
            controller.get(), &sentinel::core::ApplicationController::ollamaStatusChanged, &loop,
            [&loop]() { loop.quit(); });
        phase = WaitPhase::Discovery;
        deadlineTimer.start(deadlineMs);
        controller->refreshOllamaStatus();
        loop.exec();
        deadlineTimer.stop();
        QObject::disconnect(discoveryConn);
        if (discoveryDeadlineExpired) {
            std::cerr << "sentinel-cli: model discovery did not settle within " << deadlineMs
                      << " ms; continuing with the current snapshot" << std::endl;
        }
    }

    // Classify the settled outcome exactly once, from the state the controller
    // reports after the wait. No later asynchronous callback can change it.
    auto classify = [&controller]() -> int {
        const QString lifecycle = controller->chatSendLifecycleState();
        const QString category = controller->chatErrorCategory();
        if (lifecycle == QLatin1String("completed") && category == QLatin1String("None")) {
            return kExitSuccess;
        }
        std::cerr << "sentinel-cli: chat failed (state '" << lifecycle.toStdString()
                  << "', category '" << category.toStdString()
                  << "'): " << controller->chatSendLifecycleSummary().toStdString() << std::endl;
        return kExitFailure;
    };

    std::cout << "User: " << prompt.toStdString() << std::endl;
    const bool accepted = controller->sendMessage(prompt);

    // Synchronous providers emit the response before the event loop starts.
    if (responded) {
        return classify();
    }
    if (!accepted) {
        std::cerr << "sentinel-cli: chat request refused: "
                  << controller->chatSendLifecycleSummary().toStdString() << std::endl;
        return kExitFailure;
    }

    // Async providers (streaming / local inference) respond through the event loop.
    phase = WaitPhase::Response;
    deadlineTimer.start(deadlineMs);
    loop.exec();
    deadlineTimer.stop();

    if (responseTimedOut) {
        // The deadline decision is authoritative: a late reply must not turn
        // this run into success or failure afterwards. Release a provider
        // worker still holding the request so teardown stays bounded instead
        // of joining a stalled worker indefinitely.
        controller->stopChatGeneration();
        return kExitTimeout;
    }
    if (responded) {
        return classify();
    }
    std::cerr << "sentinel-cli: chat ended without a response" << std::endl;
    return kExitFailure;
}

} // namespace sentinel::cli
