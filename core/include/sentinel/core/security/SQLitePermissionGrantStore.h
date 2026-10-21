#pragma once

#include "sentinel/core/app/StorageAdmission.h"

#include "sentinel/core/security/IPermissionGrantStore.h"

namespace sentinel::core {

class SQLitePermissionGrantStore final : public IPermissionGrantStore {
public:
    explicit SQLitePermissionGrantStore(QString databasePath,
                                        std::shared_ptr<StorageAdmission> admission = {},
                                        std::uint64_t generation = 0);
    bool load(QList<PersistentPermissionGrant>& grants) override;
    bool save(const QList<PersistentPermissionGrant>& grants) override;
    bool remove(const QString& id) override;
    bool clear() override;
    bool coordinatedBy(const std::shared_ptr<StorageAdmission>& authority) const override {
        return authority && admission_ == authority;
    }
    QString lastError() const override { return lastError_; }

private:
    bool initialize();
    StorageAdmission::Writer admitWrite();
    std::shared_ptr<StorageAdmission> admission_;
    const std::uint64_t generation_;
    QString databasePath_;
    QString lastError_;
    bool ready_ = false;
};

} // namespace sentinel::core
