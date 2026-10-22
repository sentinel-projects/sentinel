// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDir>
#include <QString>

namespace sentinel::test {

// Local socket fixtures must fit in sockaddr_un::sun_path (104 bytes on macOS,
// 108 on Linux). QDir::tempPath() is deep under /var/folders on macOS and the
// test application name can add another 50+ bytes, so socket fixtures use the
// short /tmp root on Unix. Windows named pipes are not filesystem paths.
inline QString shortSocketDirTemplate(const QString& prefix) {
#ifdef Q_OS_UNIX
    return QStringLiteral("/tmp/") + prefix + QStringLiteral("-XXXXXX");
#else
    return QDir::tempPath() + QLatin1Char('/') + prefix + QStringLiteral("-XXXXXX");
#endif
}

} // namespace sentinel::test
