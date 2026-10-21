// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/interfaces/ISettingsStore.h"

#include <QMap>

namespace sentinel::core {

class InMemorySettingsStore final : public ISettingsStore {
public:
    QString value(const QString& key, const QString& defaultValue = QString()) const override;
    void setValue(QString key, QString value) override;
    void remove(const QString& key) override;
    bool applyChanges(const QMap<QString, QString>& values,
                      const QStringList& removals = {}) override;

private:
    QMap<QString, QString> values_;
};

} // namespace sentinel::core
