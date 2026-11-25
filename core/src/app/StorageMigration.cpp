// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/app/StorageMigration.h"
#include "sentinel/core/platform/ProfileWriteLease.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QPair>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>

namespace sentinel::core {

namespace {

// Bounds for the one-time rewrite of absolute paths stored in moved JSON files.
constexpr int kMaxDirectoryDepth = 4;
constexpr int kMaxScannedFiles = 2000;
constexpr qint64 kMaxFileSize = 4 * 1024 * 1024;

struct StorageRoots {
    QString config;
    QString data;
    QString localData;
};

StorageRoots rootsForApplicationName(const QString& applicationName) {
    const QString previousApplicationName = QCoreApplication::applicationName();
    QCoreApplication::setApplicationName(applicationName);
    const StorageRoots roots{
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation),
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),
    };
    QCoreApplication::setApplicationName(previousApplicationName);
    return roots;
}

void addPathVariants(QList<QPair<QByteArray, QByteArray>>& variants, const QString& legacyRoot,
                     const QString& currentRoot) {
    const QList<QString> rootForms{legacyRoot, QDir::toNativeSeparators(legacyRoot)};
    const QList<QString> currentForms{currentRoot, QDir::toNativeSeparators(currentRoot)};
    for (qsizetype i = 0; i < rootForms.size(); ++i) {
        const QByteArray legacyVariant = rootForms.at(i).toUtf8();
        const QByteArray currentVariant = currentForms.at(i).toUtf8();
        const QByteArray escapedLegacy = QByteArray(legacyVariant).replace('\\', "\\\\");
        const QByteArray escapedCurrent = QByteArray(currentVariant).replace('\\', "\\\\");

        const auto append = [&variants](const QByteArray& from, const QByteArray& to) {
            for (const auto& existing : variants) {
                if (existing.first == from) {
                    return;
                }
            }
            variants.append({from, to});
        };
        // Doubled-backslash (JSON-escaped) forms first: they are the longest match.
        if (escapedLegacy != legacyVariant) {
            append(escapedLegacy, escapedCurrent);
        }
        append(legacyVariant, currentVariant);
    }
}

// Settings such as model-storage.json and model-library-local.json keep absolute
// paths. Rewriting them keeps those references valid after the directory move.
int rewriteStoredRootPaths(const QString& legacyRoot, const QString& currentRoot) {
    QList<QPair<QByteArray, QByteArray>> variants;
    addPathVariants(variants, legacyRoot, currentRoot);
    if (variants.isEmpty()) {
        return 0;
    }

    int updatedFiles = 0;
    int scannedFiles = 0;
    QList<QPair<QString, int>> pending{{currentRoot, 0}};
    while (!pending.isEmpty()) {
        const auto [directoryPath, depth] = pending.takeFirst();
        if (depth > kMaxDirectoryDepth) {
            continue;
        }
        const QDir directory(directoryPath);
        const auto subdirectories =
            directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QString& subdirectory : subdirectories) {
            pending.append({directory.filePath(subdirectory), depth + 1});
        }

        const auto files = directory.entryList(QDir::Files | QDir::Hidden | QDir::System);
        for (const QString& file : files) {
            if (++scannedFiles > kMaxScannedFiles) {
                return updatedFiles;
            }
            if (!file.contains(QLatin1String(".json"), Qt::CaseInsensitive)) {
                continue;
            }
            const QString path = directory.filePath(file);
            QFileInfo info(path);
            if (info.size() <= 0 || info.size() > kMaxFileSize) {
                continue;
            }

            QFile stored(path);
            if (!stored.open(QIODevice::ReadOnly)) {
                continue;
            }
            QByteArray contents = stored.readAll();
            stored.close();

            bool changed = false;
            for (const auto& [legacyVariant, currentVariant] : variants) {
                if (contents.contains(legacyVariant)) {
                    contents.replace(legacyVariant, currentVariant);
                    changed = true;
                }
            }
            if (!changed) {
                continue;
            }
            if (stored.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                stored.write(contents) == contents.size()) {
                ++updatedFiles;
                qInfo().noquote() << "Storage migration repointed stored paths in" << path;
            }
        }
    }
    if (updatedFiles > 0) {
        qInfo().noquote() << "Storage migration updated stored paths in" << updatedFiles
                          << "files under" << currentRoot;
    }
    return updatedFiles;
}

int moveMissingEntries(const QString& legacyPath, const QString& currentPath) {
    QDir legacyDirectory(legacyPath);
    const QDir currentDirectory(currentPath);
    int moved = 0;
    const auto entries = legacyDirectory.entryList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                                   QDir::Hidden | QDir::System);
    for (const QString& entry : entries) {
        const QString destination = currentDirectory.filePath(entry);
        if (QFileInfo::exists(destination)) {
            continue;
        }
        if (QDir().rename(legacyDirectory.filePath(entry), destination)) {
            ++moved;
        } else {
            qWarning().noquote() << "Storage migration kept" << legacyDirectory.filePath(entry)
                                 << "in place; it could not be moved to" << destination;
        }
    }

    const auto remaining = legacyDirectory.entryList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                                     QDir::Hidden | QDir::System);
    if (remaining.isEmpty()) {
        QDir().rmdir(legacyPath);
    } else {
        qInfo().noquote() << "Storage migration left" << remaining.size() << "legacy entries in"
                          << legacyPath << "because the current storage already contains them.";
    }
    return moved;
}

int migrateRoot(const QString& legacyPath, const QString& currentPath) {
    if (legacyPath.isEmpty() || currentPath.isEmpty() || legacyPath == currentPath) {
        return 0;
    }
    if (!QFileInfo::exists(legacyPath)) {
        return 0;
    }

    const bool targetExists = QFileInfo::exists(currentPath);
    ProfileWriteLease migrationLease;
    const QStringList roots = targetExists ? QStringList{legacyPath, currentPath}
                                           : QStringList{legacyPath};
    if (!migrationLease.acquireDirectories(roots, true)) {
        qWarning() << "Storage migration deferred because a profile is in use or reserved.";
        return 0;
    }

    if (!targetExists) {
        QDir().mkpath(QFileInfo(currentPath).absolutePath());
        if (QDir().rename(legacyPath, currentPath)) {
            qInfo().noquote() << "Storage migration moved" << legacyPath << "to" << currentPath;
            rewriteStoredRootPaths(legacyPath, currentPath);
            return 1;
        }
        qWarning().noquote() << "Storage migration could not move" << legacyPath << "to"
                             << currentPath << "; retrying requires leases on both directories.";
        return 0;
    }

    const int movedEntries = moveMissingEntries(legacyPath, currentPath);
    if (movedEntries > 0) {
        qInfo().noquote() << "Storage migration merged" << movedEntries << "entries from"
                          << legacyPath << "into" << currentPath;
        rewriteStoredRootPaths(legacyPath, currentPath);
        return 1;
    }
    return 0;
}

} // namespace

int StorageMigration::migrateLegacyApplicationStorage(const QString& legacyApplicationName,
                                                      const QString& applicationName) {
    if (legacyApplicationName.isEmpty() || applicationName.isEmpty() ||
        legacyApplicationName == applicationName) {
        return 0;
    }

    const StorageRoots legacyRoots = rootsForApplicationName(legacyApplicationName);
    const StorageRoots currentRoots = rootsForApplicationName(applicationName);

    const QList<QPair<QString, QString>> pairs{
        {legacyRoots.config, currentRoots.config},
        {legacyRoots.data, currentRoots.data},
        {legacyRoots.localData, currentRoots.localData},
    };

    int migratedRoots = 0;
    QSet<QString> handledLegacyRoots;
    for (const auto& [legacyRoot, currentRoot] : pairs) {
        if (legacyRoot.isEmpty() || handledLegacyRoots.contains(legacyRoot)) {
            continue;
        }
        handledLegacyRoots.insert(legacyRoot);
        migratedRoots += migrateRoot(legacyRoot, currentRoot);
    }
    return migratedRoots;
}

} // namespace sentinel::core
