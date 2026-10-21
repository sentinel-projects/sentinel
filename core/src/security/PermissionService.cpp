// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/security/PermissionService.h"

#include "sentinel/core/security/PathGuard.h"
#include <QUuid>

namespace sentinel::core {

namespace {
QString pluginOwner(const AuthorizationRequest& request) {
    return request.providerId.startsWith(QLatin1String("plugin:")) ? request.providerId : QString{};
}
AuthorizationResourceKind effectiveKind(const AuthorizationRequest& request) {
    if (request.resourceKind != AuthorizationResourceKind::None)
        return request.resourceKind;
    if (request.domain == SecurityDomain::FileSystem)
        return AuthorizationResourceKind::FileSystemPath;
    if (request.domain == SecurityDomain::Network || request.domain == SecurityDomain::Browser)
        return AuthorizationResourceKind::Host;
    if (request.domain == SecurityDomain::ExternalService)
        return AuthorizationResourceKind::Provider;
    return AuthorizationResourceKind::None;
}

QString scopeFor(const AuthorizationRequest& request) {
    const auto kind = effectiveKind(request);
    if (kind == AuthorizationResourceKind::FileSystemPath)
        return PathGuard::canonicalPath(request.resource);
    if (kind == AuthorizationResourceKind::Host)
        return request.resource.trimmed().toLower();
    return request.resource.trimmed();
}

bool matches(const AuthorizationRequest& request, const AuthorizationRequest& granted) {
    if (pluginOwner(request) != pluginOwner(granted) || request.domain != granted.domain ||
        request.access != granted.access || effectiveKind(request) != effectiveKind(granted))
        return false;
    const QString scope = scopeFor(granted);
    const QString resource = scopeFor(request);
    if (scope == resource)
        return true;
    if (effectiveKind(request) == AuthorizationResourceKind::FileSystemPath)
        return PathGuard::contains(scope, resource);
    if (effectiveKind(request) == AuthorizationResourceKind::Host)
        return resource.endsWith(QLatin1Char('.') + scope);
    return false;
}

bool persistable(const AuthorizationRequest& request) {
    const auto kind = effectiveKind(request);
    const QString scope = scopeFor(request);
    if (kind == AuthorizationResourceKind::Argument || scope.size() > 2048)
        return false;
    if (kind == AuthorizationResourceKind::FileSystemPath)
        return !scope.isEmpty() && scope != QStringLiteral("/") &&
               PathGuard::contains(scope, scope);
    if (kind == AuthorizationResourceKind::Host)
        return !scope.isEmpty() && !scope.contains(QLatin1Char('/')) &&
               !scope.contains(QLatin1Char('*')) && !scope.contains(QLatin1Char(' '));
    if (kind == AuthorizationResourceKind::Provider || kind == AuthorizationResourceKind::ArgumentDigest)
        return !scope.isEmpty();
    return false;
}
} // namespace

PermissionService::PermissionService(std::shared_ptr<IPermissionGrantStore> store)
    : store_(std::move(store)) {
    if (store_ && !store_->load(persistentGrants_))
        persistentGrants_.clear();
}

PermissionEffect PermissionService::evaluateAuthorization(const AuthorizationRequest& request,
                                                           const QString& sessionId) const {
    std::lock_guard lock(mutex_);
    if (persistenceRestricted_) {
        return PermissionEffect::Deny;
    }
    bool allowed = false;
    bool denied = false;
    for (const auto& grant : grants_) {
        if (grant.sessionId != sessionId || !matches(request, grant.request))
            continue;
        allowed |= grant.effect == PermissionEffect::Allow;
        denied |= grant.effect == PermissionEffect::Deny;
    }
    if (denied)
        return PermissionEffect::Deny;
    for (const auto& grant : persistentGrants_) {
        AuthorizationRequest stored{grant.domain, grant.access, grant.scope};
        stored.resourceKind = grant.resourceKind;
        stored.providerId = grant.pluginOwnerId;
        allowed |= matches(request, stored);
    }
    return allowed ? PermissionEffect::Allow : PermissionEffect::Ask;
}

bool PermissionService::setAuthorization(const AuthorizationRequest& request,
                                         PermissionEffect effect, const QString& sessionId,
                                         bool persistent) {
    if (persistent) {
        if (effect != PermissionEffect::Allow)
            return false;
        return grantAuthorizations({request}, sessionId, true);
    }
    if (request.resource.isEmpty() &&
        (request.domain == SecurityDomain::FileSystem || request.domain == SecurityDomain::Network ||
         request.domain == SecurityDomain::Browser ||
         request.domain == SecurityDomain::ExternalService))
        return false;

    std::lock_guard lock(mutex_);
    grants_.removeIf([&](const Grant& grant) {
        return grant.request.domain == request.domain && grant.request.access == request.access &&
               grant.request.resource == request.resource &&
               pluginOwner(grant.request) == pluginOwner(request) && grant.sessionId == sessionId;
    });
    if (effect != PermissionEffect::Ask)
        grants_.append({request, sessionId, effect});
    return true;
}

bool PermissionService::grantAuthorization(const AuthorizationRequest& request,
                                           const QString& sessionId, bool persistent) {
    return setAuthorization(request, PermissionEffect::Allow, sessionId, persistent);
}

bool PermissionService::grantAuthorizations(const QList<AuthorizationRequest>& requests,
                                            const QString& sessionId, bool persistent) {
    if (!persistent) {
        for (const auto& request : requests)
            if (!grantAuthorization(request, sessionId, false))
                return false;
        return true;
    }
    if (!store_)
        return false;
    std::lock_guard lock(mutex_);
    QList<PersistentPermissionGrant> additions;
    for (const auto& request : requests) {
        if (!persistable(request))
            return false;
        const QString scope = scopeFor(request);
        const auto kind = effectiveKind(request);
        bool existing = false;
        for (const auto& grant : persistentGrants_)
            existing |= grant.domain == request.domain && grant.access == request.access &&
                        grant.resourceKind == kind && grant.scope == scope &&
                        grant.pluginOwnerId == pluginOwner(request);
        for (const auto& grant : additions)
            existing |= grant.domain == request.domain && grant.access == request.access &&
                        grant.resourceKind == kind && grant.scope == scope &&
                        grant.pluginOwnerId == pluginOwner(request);
        if (!existing)
            additions.append({QUuid::createUuid().toString(QUuid::WithoutBraces), request.domain,
                              request.access, kind, scope, QDateTime::currentDateTimeUtc(),
                              pluginOwner(request)});
    }
    if (!store_->save(additions))
        return false;
    persistentGrants_.append(additions);
    return true;
}

bool PermissionService::removePersistentGrant(const QString& id) {
    std::lock_guard lock(mutex_);
    if (!store_ || !store_->remove(id)) {
        persistenceRestricted_ = true;
        return false;
    }
    persistentGrants_.removeIf([&](const auto& grant) { return grant.id == id; });
    return true;
}

bool PermissionService::clearPersistentGrants() {
    std::lock_guard lock(mutex_);
    if (!store_ || !store_->clear()) {
        persistenceRestricted_ = true;
        return false;
    }
    persistentGrants_.clear();
    return true;
}

void PermissionService::revokeAuthorization(const AuthorizationRequest& request,
                                            const QString& sessionId) {
    std::lock_guard lock(mutex_);
    grants_.removeIf([&](const Grant& grant) {
        return grant.sessionId == sessionId && grant.request.domain == request.domain &&
               grant.request.access == request.access &&
               pluginOwner(grant.request) == pluginOwner(request) &&
               grant.request.resource == request.resource;
    });
}

void PermissionService::clearSessionGrants(const QString& sessionId) {
    std::lock_guard lock(mutex_);
    grants_.removeIf([&](const Grant& grant) {
        return grant.sessionId == sessionId;
    });
}

} // namespace sentinel::core
