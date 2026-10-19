// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sentinel/core/memory/ScopedMemory.h"
#include <QJsonObject>
#include <cstdint>
namespace sentinel::core {
enum class MemoryManagementStatus : std::uint8_t {
    Ready,
    Unavailable,
    Unauthorized,
    InvalidInput,
    Conflict,
    NotFound,
    LimitExceeded
};
struct MemoryManagementContext {
    MemoryAccessContext access;
    bool allowLegacy = false; // Trusted profile-management caller only; never recall authority.
};
struct MemoryPageQuery {
    MemoryScope scope = MemoryScope::Session;
    QString search;
    QString afterId;
    int limit = 10;
};
struct MemoryManagementResult {
    MemoryManagementStatus status = MemoryManagementStatus::Unavailable;
    QList<ScopedMemoryRecord> records;
    QString nextCursor;
};
struct MemoryMutation {
    enum class Kind : std::uint8_t { Create, Correct, Delete, Move, Import };
    Kind kind = Kind::Create;
    ScopedMemoryRecord record;
    QString expectedRevision;
    MemoryScope destination = MemoryScope::Session;
    bool skipIdentical = false;
};
MemoryManagementStatus prepareManagedMutation(const MemoryMutation&, const ScopedMemoryRecord*,
                                              const MemoryManagementContext&, ScopedMemoryRecord*,
                                              bool* skip);
bool manageableMemory(const ScopedMemoryRecord&, const MemoryManagementContext&);
bool authorizedMemoryScope(MemoryScope, const MemoryManagementContext&);
QString memoryManagementStatusName(MemoryManagementStatus);
QJsonObject managedMemoryJson(const ScopedMemoryRecord&, bool preview = false);
bool parseManagedMemory(const QJsonObject&, ScopedMemoryRecord*);
QByteArray encodeMemoryArchive(const QList<ScopedMemoryRecord>&);
MemoryManagementResult decodeMemoryArchive(const QByteArray&);
} // namespace sentinel::core
