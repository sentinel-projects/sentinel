// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sentinel/core/memory/MemoryManagement.h"
#include <QIODevice>
#include <functional>
namespace sentinel::core {
struct MemoryArchiveLimits {
    qint64 maximumBytes = 64LL * 1024 * 1024;
    int maximumRecords = 10000;
    bool valid() const {
        return maximumBytes >= 98304 && maximumBytes <= 256LL * 1024 * 1024 && maximumRecords > 0 &&
               maximumRecords <= 100000;
    }
};
struct MemoryArchiveControl {
    std::function<bool()> cancelled;
    std::function<void(qint64, qint64)> progress;
    std::function<bool()> beforeCommit;
};
struct MemoryArchiveResult {
    QString status = "InvalidInput";
    int version = 0;
    qint64 records = 0;
    qint64 bytes = 0;
    int chunks = 0;
    QString digest;
    bool ok() const {
        return status == "Ready";
    }
};
using MemoryArchiveConsumer = std::function<QString(const ScopedMemoryRecord&)>;
using MemoryArchiveProducer = std::function<QString(const MemoryArchiveConsumer&)>;
MemoryArchiveResult writeMemoryArchive(QIODevice&, MemoryScope, const MemoryManagementContext&,
                                       const MemoryArchiveLimits&, const MemoryArchiveControl&,
                                       const MemoryArchiveProducer&);
MemoryArchiveResult readMemoryArchive(QIODevice&, MemoryScope, const MemoryManagementContext&,
                                      const MemoryArchiveLimits&, const MemoryArchiveControl&,
                                      const MemoryArchiveConsumer& = {});
inline constexpr int memoryArchiveChunkBytes = 1024 * 1024;
inline constexpr int memoryHistoryLimit = 256;
} // namespace sentinel::core
