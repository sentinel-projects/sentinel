#pragma once
#include "sentinel/core/app/StorageAdmission.h"

#include "sentinel/core/runtime/ToolDescriptor.h"

#include <QDateTime>
#include <QList>
#include <QString>

namespace sentinel::core {

struct PersistentPermissionGrant {
    QString id;
    SecurityDomain domain = SecurityDomain::ExternalService;
    AccessMode access = AccessMode::Invoke;
    AuthorizationResourceKind resourceKind = AuthorizationResourceKind::None;
    QString scope;
    QDateTime createdAt;
    QString pluginOwnerId;
};

class IPermissionGrantStore {
public:
    virtual ~IPermissionGrantStore() = default;
    virtual bool coordinatedBy(const std::shared_ptr<StorageAdmission>&) const {
        return false;
    }
    virtual bool load(QList<PersistentPermissionGrant>& grants) = 0;
    virtual bool save(const QList<PersistentPermissionGrant>& grants) = 0;
    virtual bool remove(const QString& id) = 0;
    virtual bool clear() = 0;
    virtual QString lastError() const = 0;
};

} // namespace sentinel::core
