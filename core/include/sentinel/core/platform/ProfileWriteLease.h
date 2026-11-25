// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sentinel/core/platform/StandardPathProvider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#endif

namespace sentinel::core {

// Keep the lease alive until all profile writers have been destroyed. The
// Rust cutover tool acquires exclusive locks on these same directory inodes.
class ProfileWriteLease final {
public:
    ProfileWriteLease() = default;
    ProfileWriteLease(const ProfileWriteLease&) = delete;
    ProfileWriteLease& operator=(const ProfileWriteLease&) = delete;
    ~ProfileWriteLease() {
        release();
    }

    bool acquire(const StandardPathProvider& paths) {
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
        QStringList directories;
        for (const auto& path : {paths.settingsFilePath(), paths.conversationDatabasePath(),
                                 paths.memoryDatabasePath()}) {
            const QString directory = QFileInfo(path).absolutePath();
            if (!QDir{}.mkpath(directory))
                return false;
            directories.append(directory);
        }
        return acquireDirectories(directories, false);
#else
        Q_UNUSED(paths);
        return true;
#endif
    }

    bool acquireDirectories(const QStringList& paths, bool exclusive) {
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
        if (!m_descriptors.empty())
            return false;
        QStringList directories;
        for (const auto& path : paths) {
            const QString canonical = QFileInfo(path).canonicalFilePath();
            if (canonical.isEmpty())
                return false;
            directories.append(canonical);
        }
        directories.removeDuplicates();
        directories.sort();
        for (const auto& directory : directories) {
            const auto encoded = QFile::encodeName(directory);
            const int descriptor =
                ::open(encoded.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (descriptor < 0) {
                release();
                return false;
            }
            m_descriptors.push_back(descriptor);
            if (::flock(descriptor, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
                release();
                return false;
            }
            struct stat entry{};
            if (::fstatat(descriptor, ".sentinel-rust-cutover", &entry, AT_SYMLINK_NOFOLLOW) == 0 ||
                errno != ENOENT) {
                release();
                return false;
            }
        }
#else
        Q_UNUSED(paths);
        Q_UNUSED(exclusive);
#endif
        return true;
    }

private:
    void release() {
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
        for (const int descriptor : m_descriptors)
            ::close(descriptor);
        m_descriptors.clear();
#endif
    }

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    std::vector<int> m_descriptors;
#endif
};

} // namespace sentinel::core
