// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "StatusCommand.h"

#include "sentinel/core/app/AppMetadata.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/platform/DpapiEncryptedSettingsStore.h"
#include "sentinel/core/platform/StandardPathProvider.h"
#include "sentinel/core/platform/ProfileWriteLease.h"

#include <QDebug>
#include <iostream>

namespace sentinel::cli {

int executeStatusCommand(const QStringList& /*args*/) {
    sentinel::core::StandardPathProvider pathProvider;
    sentinel::core::ProfileWriteLease profileWriteLease;
    if (!profileWriteLease.acquire(pathProvider)) {
        qCritical("Qt profile is reserved for a Rust cutover.");
        return 1;
    }
    sentinel::core::AppSettings settings(
        std::make_unique<sentinel::core::DpapiEncryptedSettingsStore>(
            std::make_unique<sentinel::core::JsonSettingsStore>(pathProvider.settingsFilePath())));

    std::cout << "=== Sentinel System Diagnostics ===" << std::endl;
    std::cout << "Application:    " << sentinel::core::AppMetadata::displayName().toStdString()
              << std::endl;
    std::cout << "Version:        " << sentinel::core::AppMetadata::version().toStdString()
              << std::endl;
    std::cout << "App ID:         " << sentinel::core::AppMetadata::appId().toStdString()
              << std::endl;
    std::cout << "Settings File:  " << pathProvider.settingsFilePath().toStdString() << std::endl;
    std::cout << "Memory Database:" << pathProvider.memoryDatabasePath().toStdString() << std::endl;
    std::cout << "Chat Database:  " << pathProvider.chatHistoryDatabasePath().toStdString()
              << std::endl;
    std::cout << "Log Directory:  " << pathProvider.logDirectoryPath().toStdString() << std::endl;
    std::cout << "Ollama Endpoint:" << settings.ollamaEndpoint().toStdString() << std::endl;
    std::cout << "Routing Mode:   " << settings.routingModeName().toStdString() << std::endl;
    std::cout << "Language:       " << settings.appLanguage().toStdString() << std::endl;
    std::cout << "===================================" << std::endl;
    return 0;
}

} // namespace sentinel::cli
