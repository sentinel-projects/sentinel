// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/runtime/ToolDescriptor.h"
#include "sentinel/core/security/IPermissionService.h"
#include "sentinel/core/security/IPermissionGrantStore.h"

#include <QList>
#include <QString>
#include <memory>
#include <mutex>

namespace sentinel::core {

// Owns runtime user grants. Defaults are evaluated by PermissionPolicyService;
// this class stores only explicit decisions.
class PermissionService final {
public:
    explicit PermissionService(std::shared_ptr<IPermissionGrantStore> store = {});
    PermissionEffect evaluateAuthorization(const AuthorizationRequest& request,
                                           const QString& sessionId) const;
    bool setAuthorization(const AuthorizationRequest& request, PermissionEffect effect,
                          const QString& sessionId, bool persistent);
    bool grantAuthorization(const AuthorizationRequest& request, const QString& sessionId,
                            bool persistent);
    bool grantAuthorizations(const QList<AuthorizationRequest>& requests, const QString& sessionId,
                             bool persistent);
    QList<PersistentPermissionGrant> persistentGrants() const {
        std::lock_guard lock(mutex_);
        return persistentGrants_;
    }
    bool removePersistentGrant(const QString& id);
    bool clearPersistentGrants();
    QString lastError() const { return store_ ? store_->lastError() : QStringLiteral("Permission store unavailable"); }
    void revokeAuthorization(const AuthorizationRequest& request, const QString& sessionId);
    void clearSessionGrants(const QString& sessionId);

private:
    struct Grant {
        AuthorizationRequest request;
        QString sessionId;
        PermissionEffect effect = PermissionEffect::Allow;
    };
    std::shared_ptr<IPermissionGrantStore> store_;
    mutable std::mutex mutex_;
    QList<PersistentPermissionGrant> persistentGrants_;
    QList<Grant> grants_;
    bool persistenceRestricted_ = false;
};

} // namespace sentinel::core
