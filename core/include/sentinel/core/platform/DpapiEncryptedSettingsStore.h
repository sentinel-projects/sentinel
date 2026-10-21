// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/interfaces/ISettingsStore.h"

#include <QString>
#include <functional>
#include <memory>

namespace sentinel::core {

class DpapiEncryptedSettingsStore final : public ISettingsStore {
public:
    // Injectable macOS key source permits hermetic encryption tests. Empty uses OS Keychain.
    using KeySource = std::function<QByteArray()>;
    explicit DpapiEncryptedSettingsStore(std::unique_ptr<ISettingsStore> inner,
                                         KeySource keySource = {});

    QString value(const QString& key, const QString& defaultValue = QString()) const override;
    void setValue(QString key, QString value) override;
    void remove(const QString& key) override;
    bool applyChanges(const QMap<QString, QString>& values,
                      const QStringList& removals = {}) override;
    QString errorCode() const override;

private:
    static bool isSecretKey(const QString& key);
    QByteArray encrypt(const QString& plainText);
    QString decrypt(const QByteArray& cipherData) const;

    std::unique_ptr<ISettingsStore> inner_;
    KeySource keySource_;
};

} // namespace sentinel::core
