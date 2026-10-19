// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
namespace sentinel::core::recoveryfs {
bool createPrivateDirectory(const QString&);
bool createPrivateFile(const QString&);
bool trustedProfileDirectory(const QString&);
bool privatePath(const QString&, bool directory, quint64* device = nullptr,
                 quint64* inode = nullptr);
bool syncDirectory(const QString&);
} // namespace sentinel::core::recoveryfs
