#pragma once
#include "sentinel/desktop/GlobalShortcutBackend.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

namespace sentinel::desktop {
struct PortalShortcut {
    QString id;
    QVariantMap properties;
};
using PortalShortcuts = QList<PortalShortcut>;
QDBusArgument& operator<<(QDBusArgument& argument, const PortalShortcut& shortcut);
const QDBusArgument& operator>>(const QDBusArgument& argument, PortalShortcut& shortcut);

class PortalShortcutBackend final : public GlobalShortcutBackend {
    Q_OBJECT
public:
    explicit PortalShortcutBackend(const QDBusConnection& bus, const QString& platform,
                                   QObject* parent = nullptr, int requestTimeoutMs = 120000);
    ~PortalShortcutBackend() override;
    void setShortcut(const QString& sequence) override;
    void shutdown() override;
    QString status() const override;
    bool registered() const override;
private slots:
    void response(uint code, const QVariantMap& results, const QDBusMessage& message);
    void sessionClosed(const QVariantMap& details, const QDBusMessage& message);
    void shortcutActivated(const QDBusObjectPath& session, const QString& id, qulonglong timestamp,
                           const QVariantMap& options);
    void shortcutsChanged(const QDBusObjectPath& session,
                          const sentinel::desktop::PortalShortcuts& shortcuts);

private:
    enum class Stage { Idle, Probing, Creating, Binding, Registered };
    void start();
    void cancel();
    void fail(const QString& status);
    void updateStatus(const QString& status);
    void request(const QString& method, QVariantList arguments, QVariantMap options);
    void updateBindings(const PortalShortcuts& shortcuts);
    QDBusConnection bus_;
    QString platform_, sequence_, status_ = QStringLiteral("Disabled");
    QString requestPath_, sessionPath_;
    Stage stage_ = Stage::Idle;
    quint64 generation_ = 0;
    QTimer timeout_;
    bool stopped_ = false;
};
} // namespace sentinel::desktop
Q_DECLARE_METATYPE(sentinel::desktop::PortalShortcut)
Q_DECLARE_METATYPE(sentinel::desktop::PortalShortcuts)
