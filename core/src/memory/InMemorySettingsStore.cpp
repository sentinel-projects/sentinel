// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/memory/InMemorySettingsStore.h"

namespace sentinel::core {

QString InMemorySettingsStore::value(const QString& key, const QString& defaultValue) const {
    return values_.value(key, defaultValue);
}

void InMemorySettingsStore::setValue(QString key, QString value) {
    values_.insert(key, value);
}

void InMemorySettingsStore::remove(const QString& key) {
    values_.remove(key);
}

bool InMemorySettingsStore::applyChanges(const QMap<QString, QString>& values,
                                         const QStringList& removals) {
    for (const auto& key : removals) {
        values_.remove(key);
    }
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        values_.insert(it.key(), it.value());
    }
    return true;
}

} // namespace sentinel::core
