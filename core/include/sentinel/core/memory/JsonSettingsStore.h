// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/interfaces/ISettingsStore.h"

#include <QJsonObject>
#include <QString>

namespace sentinel::core {

class JsonSettingsStore final : public ISettingsStore {
public:
    explicit JsonSettingsStore(QString filePath);

    QString value(const QString& key, const QString& defaultValue = QString()) const override;
    void setValue(QString key, QString value) override;
    void remove(const QString& key) override;
    bool applyChanges(const QMap<QString, QString>& values,
                      const QStringList& removals = {}) override;

    QString filePath() const;
    QString errorCode() const override;

private:
    void load();
    bool save() const;

    QString filePath_;
    QJsonObject values_;
    mutable QString errorCode_;
};

} // namespace sentinel::core
