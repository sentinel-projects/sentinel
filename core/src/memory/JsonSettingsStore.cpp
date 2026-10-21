// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/memory/JsonSettingsStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

namespace sentinel::core {

JsonSettingsStore::JsonSettingsStore(QString filePath) : filePath_(std::move(filePath)) {
    load();
}

QString JsonSettingsStore::value(const QString& key, const QString& defaultValue) const {
    const auto jsonValue = values_.value(key);
    return jsonValue.isString() ? jsonValue.toString() : defaultValue;
}

void JsonSettingsStore::setValue(QString key, QString value) {
    if (!errorCode_.isEmpty()) return;
    const auto previous = values_;
    values_.insert(key, value);
    if (!save()) values_ = previous;
}

void JsonSettingsStore::remove(const QString& key) {
    if (!errorCode_.isEmpty() || !values_.contains(key))
        return;
    const auto previous = values_;
    values_.remove(key);
    if (!save()) values_ = previous;
}

bool JsonSettingsStore::applyChanges(const QMap<QString, QString>& values,
                                     const QStringList& removals) {
    if (!errorCode_.isEmpty()) {
        return false;
    }
    const auto previous = values_;
    for (const auto& key : removals) {
        values_.remove(key);
    }
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        values_.insert(it.key(), it.value());
    }
    if (save()) {
        return true;
    }
    values_ = previous;
    return false;
}

QString JsonSettingsStore::filePath() const {
    return filePath_;
}

QString JsonSettingsStore::errorCode() const { return errorCode_; }

void JsonSettingsStore::load() {
    QFile file(filePath_);
    if (!file.exists()) return;
    if (!file.open(QIODevice::ReadOnly)) {
        errorCode_ = QStringLiteral("CorruptState");
        return;
    }
    if (file.size() > 16 * 1024 * 1024) {
        errorCode_ = QStringLiteral("CorruptState");
        return;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject() || parseError.error != QJsonParseError::NoError) {
        errorCode_ = QStringLiteral("CorruptState");
        return;
    }
    const auto root = document.object();
    const auto version = root.value(QStringLiteral("sentinelSettingsSchemaVersion"));
    if (!version.isUndefined() && (!version.isDouble() || version.toInt() > 1 ||
                                    version.toInt() < 1)) {
        errorCode_ = QStringLiteral("UnsupportedSettingsVersion");
        return;
    }
    values_ = root;
}

bool JsonSettingsStore::save() const {
    const QFileInfo fileInfo(filePath_);
    const QDir parentDir = fileInfo.dir();
    const bool createdParent = !parentDir.exists();
    if (createdParent) {
        if (!QDir().mkpath(parentDir.absolutePath())) {
            errorCode_ = QStringLiteral("StoreUnavailable");
            return false;
        }
        QFile::setPermissions(parentDir.absolutePath(),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner);
    }

    QSaveFile file(filePath_);
    if (!file.open(QIODevice::WriteOnly)) {
        errorCode_ = QStringLiteral("StoreUnavailable");
        return false;
    }

    auto output = values_;
    output.insert(QStringLiteral("sentinelSettingsSchemaVersion"), 1);
    const auto bytes = QJsonDocument(output).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        errorCode_ = QStringLiteral("StoreUnavailable");
        return false;
    }

    QFile::setPermissions(filePath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

} // namespace sentinel::core
