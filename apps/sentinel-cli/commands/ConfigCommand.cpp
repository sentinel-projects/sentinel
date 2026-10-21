// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ConfigCommand.h"

#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/platform/DpapiEncryptedSettingsStore.h"
#include "sentinel/core/platform/StandardPathProvider.h"

#include <iostream>

namespace sentinel::cli {

int executeConfigCommand(const QStringList& args) {
    const QString sub = args.value(0, QStringLiteral("get"));

    sentinel::core::StandardPathProvider pathProvider;
    sentinel::core::AppSettings settings(
        std::make_unique<sentinel::core::DpapiEncryptedSettingsStore>(
            std::make_unique<sentinel::core::JsonSettingsStore>(pathProvider.settingsFilePath())));

    if (sub == QStringLiteral("get")) {
        const QString key = args.value(1);
        if (key.isEmpty()) {
            std::cout << "ollamaEndpoint = " << settings.ollamaEndpoint().toStdString()
                      << std::endl;
            std::cout << "routingMode    = " << settings.routingModeName().toStdString()
                      << std::endl;
            std::cout << "appLanguage    = " << settings.appLanguage().toStdString() << std::endl;
            return 0;
        }
        if (key == QStringLiteral("ollamaEndpoint")) {
            std::cout << settings.ollamaEndpoint().toStdString() << std::endl;
        } else if (key == QStringLiteral("routingMode")) {
            std::cout << settings.routingModeName().toStdString() << std::endl;
        } else if (key == QStringLiteral("appLanguage")) {
            std::cout << settings.appLanguage().toStdString() << std::endl;
        } else {
            std::cerr << "Unknown configuration key: " << key.toStdString() << std::endl;
            return 1;
        }
        return 0;
    } else if (sub == QStringLiteral("set")) {
        const QString key = args.value(1);
        const QString val = args.value(2);
        if (key.isEmpty() || val.isEmpty()) {
            std::cerr << "Usage: sentinel-cli config set <key> <value>" << std::endl;
            return 1;
        }
        if (key == QStringLiteral("ollamaEndpoint")) {
            settings.setOllamaEndpoint(val);
        } else if (key == QStringLiteral("routingMode")) {
            settings.setRoutingModeName(val);
        } else if (key == QStringLiteral("appLanguage")) {
            settings.setAppLanguage(val);
        } else {
            std::cerr << "Unknown configuration key: " << key.toStdString() << std::endl;
            return 1;
        }
        if (!settings.storageErrorCode().isEmpty()) {
            std::cerr << "Configuration write failed: " << settings.storageErrorCode().toStdString()
                      << '\n';
            return 1;
        }
        std::cout << "Updated configuration: " << key.toStdString() << " = " << val.toStdString()
                  << std::endl;
        return 0;
    }

    std::cerr << "Unknown config subcommand: " << sub.toStdString() << std::endl;
    std::cerr << "Available: get, set" << std::endl;
    return 1;
}

} // namespace sentinel::cli
