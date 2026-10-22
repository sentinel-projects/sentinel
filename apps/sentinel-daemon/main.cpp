// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "service/DaemonService.h"

#include "sentinel/core/app/AppMetadata.h"
#include "sentinel/core/app/StorageMigration.h"

#include <QCommandLineParser>
#include <QTimer>
#ifdef Q_OS_MACOS
#include <QSslSocket>
#endif
#include <csignal>

namespace {
volatile std::sig_atomic_t shutdownRequested = 0;
void requestShutdown(int) {
    shutdownRequested = 1;
}
} // namespace
#include <QCoreApplication>
#include <QDebug>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
#ifdef Q_OS_MACOS
    // Select the native TLS implementation before workers open any sockets.
    // Homebrew's OpenSSL backend can load a different libcrypto major at runtime.
    // Certificate and hostname verification remain enabled by Qt.
    if (QSslSocket::availableBackends().contains(QStringLiteral("securetransport")) &&
        !QSslSocket::setActiveBackend(QStringLiteral("securetransport"))) {
        qCritical() << "Could not initialize the macOS native TLS backend.";
        return 1;
    }
#endif
    QCoreApplication::setApplicationName(sentinel::core::AppMetadata::displayName() +
                                         QStringLiteral(" Daemon"));
    QCoreApplication::setOrganizationName(sentinel::core::AppMetadata::organizationName());
    QCoreApplication::setApplicationVersion(sentinel::core::AppMetadata::version());

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(
        {"portable", "Store state beside the daemon executable (disposable profiles)."});
    parser.addOption({"socket", "Owner-only local socket path.", "path"});
    parser.addOption({"profile-name", "Isolated QStandardPaths identity for testing.", "name"});
    parser.process(app);
    if (parser.isSet("profile-name")) {
        QCoreApplication::setApplicationName(parser.value("profile-name"));
    } else {
        // The daemon inherits its storage roots from the renamed display name;
        // move data written under the pre-rename daemon identity first.
        sentinel::core::StorageMigration::migrateLegacyApplicationStorage(
            sentinel::core::AppMetadata::legacyDisplayName() + QStringLiteral(" Daemon"),
            QCoreApplication::applicationName());
    }
    sentinel::daemon::DaemonService service;
    if (!service.initialize(parser.value("socket"))) {
        qCritical().noquote() << "Failed to start Sentinel Daemon Service.";
        return 1;
    }

    std::signal(SIGINT, requestShutdown);
    std::signal(SIGTERM, requestShutdown);
    QTimer signalTimer;
    QObject::connect(&signalTimer, &QTimer::timeout, &app, [&service] {
        if (shutdownRequested) {
            service.requestShutdown();
        }
    });
    signalTimer.start(50);
    qInfo().noquote() << "Sentinel Daemon running in background.";
    return QCoreApplication::exec();
}
