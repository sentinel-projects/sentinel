// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace sentinel::core {

class ISettingsStore {
public:
    Q_DISABLE_COPY(ISettingsStore)
    ISettingsStore() = default;
    virtual ~ISettingsStore() = default;

    virtual QString value(const QString& key, const QString& defaultValue = QString()) const = 0;
    virtual void setValue(QString key, QString value) = 0;
    virtual void remove(const QString& key) = 0;
    // All changes publish together or none do. Unsupported owners must not emulate
    // a transaction with successive setValue calls.
    virtual bool applyChanges(const QMap<QString, QString>&, const QStringList& = {}) {
        return false;
    }
    virtual QString errorCode() const { return {}; }
};

} // namespace sentinel::core

namespace sentinel::core::interfaces {
using ISettingsStore = ::sentinel::core::ISettingsStore;
}
