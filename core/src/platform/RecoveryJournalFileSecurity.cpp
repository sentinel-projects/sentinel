// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/platform/RecoveryJournalFileSecurity.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cerrno>
#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(Q_OS_DARWIN)
#include <sys/acl.h>
#endif
namespace sentinel::core::recoveryfs {
namespace {
bool noExtendedAcl(const QString& path) {
#if defined(Q_OS_DARWIN)
    const auto acl = ::acl_get_file(QFile::encodeName(path).constData(), ACL_TYPE_EXTENDED);
    if (!acl) {
        // Darwin reports ENOENT for an existing vnode with no extended ACL.
        return errno == ENOENT;
    }
    acl_entry_t entry = nullptr;
    const bool empty = ::acl_valid(acl) == 0 && ::acl_get_entry(acl, ACL_FIRST_ENTRY, &entry) != 0;
    ::acl_free(acl);
    return empty;
#else
    Q_UNUSED(path)
    return true;
#endif
}
} // namespace
bool createPrivateDirectory(const QString& path) {
#if defined(Q_OS_UNIX)
    return ::mkdir(QFile::encodeName(path).constData(), 0700) == 0;
#else
    Q_UNUSED(path)
    return false;
#endif
}
bool createPrivateFile(const QString& path) {
#if defined(Q_OS_UNIX)
    const int fd = ::open(QFile::encodeName(path).constData(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    const bool ok = ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    return ok && closed;
#else
    Q_UNUSED(path)
    return false;
#endif
}
bool trustedProfileDirectory(const QString& path) {
#if defined(Q_OS_UNIX)
    auto current = QDir::cleanPath(path);
    bool leaf = true;
    while (true) {
        struct stat st {};
        if (::lstat(QFile::encodeName(current).constData(), &st) != 0 || !S_ISDIR(st.st_mode) ||
            (st.st_uid != ::geteuid() && st.st_uid != 0) || (leaf && st.st_uid != ::geteuid()) ||
            ((static_cast<unsigned int>(st.st_mode) & 0022U) != 0 &&
             (st.st_uid != 0 ||
              (static_cast<unsigned int>(st.st_mode) & static_cast<unsigned int>(S_ISVTX)) == 0)) ||
            !noExtendedAcl(current)) {
            return false;
        }
        if (current == "/") {
            return true;
        }
        current = QFileInfo(current).absolutePath();
        leaf = false;
    }
#else
    Q_UNUSED(path)
    return false;
#endif
}
bool privatePath(const QString& path, bool directory, quint64* device, quint64* inode) {
#if defined(Q_OS_UNIX)
    struct stat st {};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0 || st.st_uid != ::geteuid() ||
        (static_cast<unsigned int>(st.st_mode) & 0077U) != 0 ||
        (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) ||
        (!directory && st.st_nlink != 1) || !noExtendedAcl(path)) {
        return false;
    }
    if (device) {
        *device = static_cast<quint64>(st.st_dev);
    }
    if (inode) {
        *inode = static_cast<quint64>(st.st_ino);
    }
    return true;
#else
    Q_UNUSED(path)
    Q_UNUSED(directory)
    Q_UNUSED(device)
    Q_UNUSED(inode)
    return false;
#endif
}
bool syncDirectory(const QString& path) {
#if defined(Q_OS_UNIX)
    const int fd = ::open(QFile::encodeName(path).constData(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#else
    Q_UNUSED(path)
    return false;
#endif
}
} // namespace sentinel::core::recoveryfs
