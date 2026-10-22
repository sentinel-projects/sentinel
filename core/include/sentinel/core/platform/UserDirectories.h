// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDir>
#include <QJsonObject>
#include <QStandardPaths>

namespace sentinel::core {
// Location metadata only: these paths never grant filesystem authority.
inline QJsonObject standardUserDirectories() {
    return {{QStringLiteral("home"), QDir::homePath()},
            {QStringLiteral("desktop"),
             QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)},
            {QStringLiteral("documents"),
             QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)},
            {QStringLiteral("downloads"),
             QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)}};
}
} // namespace sentinel::core
