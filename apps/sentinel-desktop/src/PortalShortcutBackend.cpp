#include "sentinel/desktop/PortalShortcutBackend.h"
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QUuid>

namespace sentinel::desktop {
namespace {
constexpr QLatin1StringView service("org.freedesktop.portal.Desktop");
constexpr QLatin1StringView desktopPath("/org/freedesktop/portal/desktop");
constexpr QLatin1StringView shortcutsInterface("org.freedesktop.portal.GlobalShortcuts");
constexpr QLatin1StringView requestInterface("org.freedesktop.portal.Request");
constexpr QLatin1StringView sessionInterface("org.freedesktop.portal.Session");
constexpr QLatin1StringView shortcutId("quick-panel");
QString token() {
    return QStringLiteral("sentinel_") + QUuid::createUuid().toString(QUuid::Id128);
}
QString senderPath(const QDBusConnection& bus) {
    auto sender = bus.baseService().mid(1);
    return sender.replace('.', '_');
}
} // namespace
QDBusArgument& operator<<(QDBusArgument& argument, const PortalShortcut& shortcut) {
    argument.beginStructure();
    argument << shortcut.id << shortcut.properties;
    argument.endStructure();
    return argument;
}
const QDBusArgument& operator>>(const QDBusArgument& argument, PortalShortcut& shortcut) {
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.properties;
    argument.endStructure();
    return argument;
}
PortalShortcutBackend::PortalShortcutBackend(const QDBusConnection& bus, const QString& platform,
                                             QObject* parent, int requestTimeoutMs)
    : GlobalShortcutBackend(parent), bus_(bus), platform_(platform) {
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<PortalShortcuts>();
    timeout_.setSingleShot(true);
    timeout_.setInterval(requestTimeoutMs);
    connect(&timeout_, &QTimer::timeout, this, [this] { fail("Portal registration timed out"); });
    auto* watcher =
        new QDBusServiceWatcher(service, bus_, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString&, const QString&, const QString& owner) {
                if (stopped_ || sequence_.isEmpty()) {
                    return;
                }
                cancel();
                updateStatus("Global shortcuts portal unavailable");
                if (!owner.isEmpty()) {
                    start();
                }
            });
    bus_.connect(service, desktopPath, shortcutsInterface, "Activated", this,
                 SLOT(shortcutActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    bus_.connect(service, desktopPath, shortcutsInterface, "ShortcutsChanged", this,
                 SLOT(shortcutsChanged(QDBusObjectPath, sentinel::desktop::PortalShortcuts)));
}
PortalShortcutBackend::~PortalShortcutBackend() {
    shutdown();
}
QString PortalShortcutBackend::status() const {
    return status_;
}
bool PortalShortcutBackend::registered() const {
    return stage_ == Stage::Registered;
}
void PortalShortcutBackend::updateStatus(const QString& status) {
    if (status_ == status) {
        return;
    }
    status_ = status;
    emit statusChanged();
}
void PortalShortcutBackend::setShortcut(const QString& sequence) {
    const auto desired = sequence == "Disabled" ? QString{} : sequence;
    if (stopped_ || desired == sequence_) {
        return;
    }
    cancel();
    sequence_ = desired;
    if (sequence_.isEmpty()) {
        updateStatus("Disabled");
    } else {
        start();
    }
}
void PortalShortcutBackend::start() {
    if (platform_ != "wayland" && platform_ != "wayland-egl" && platform_ != "xcb") {
        updateStatus("Global shortcut unavailable on this platform");
        return;
    }
    if (sequence_ != "Ctrl+Alt+Space" && sequence_ != "Ctrl+Alt+S") {
        updateStatus("Unsupported shortcut; use Ctrl+Alt+Space or Ctrl+Alt+S");
        return;
    }
    if (!bus_.isConnected()) {
        updateStatus("Global shortcuts portal unavailable: no session bus");
        return;
    }
    stage_ = Stage::Probing;
    updateStatus("Checking global shortcuts portal");
    const auto generation = generation_;
    auto message = QDBusMessage::createMethodCall(service, desktopPath,
                                                  "org.freedesktop.DBus.Properties", "Get");
    message.setArguments({QString(shortcutsInterface), QStringLiteral("version")});
    auto* pending = new QDBusPendingCallWatcher(bus_.asyncCall(message), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this, generation](auto* call) {
        QDBusPendingReply<QDBusVariant> reply = *call;
        call->deleteLater();
        if (generation != generation_) {
            return;
        }
        if (reply.isError() || reply.value().variant().toUInt() < 1) {
            fail("Global shortcuts portal unavailable or unsupported");
            return;
        }
        stage_ = Stage::Creating;
        const auto sessionToken = token();
        sessionPath_ =
            desktopPath + QStringLiteral("/session/") + senderPath(bus_) + "/" + sessionToken;
        bus_.connect(service, sessionPath_, sessionInterface, "Closed", this,
                     SLOT(sessionClosed(QVariantMap, QDBusMessage)));
        request("CreateSession", {}, {{"session_handle_token", sessionToken}});
    });
}
void PortalShortcutBackend::request(const QString& method, QVariantList arguments,
                                    QVariantMap options) {
    const auto requestToken = token();
    requestPath_ =
        desktopPath + QStringLiteral("/request/") + senderPath(bus_) + "/" + requestToken;
    options.insert("handle_token", requestToken);
    if (!bus_.connect(service, requestPath_, requestInterface, "Response", this,
                      SLOT(response(uint, QVariantMap, QDBusMessage)))) {
        fail("Portal response subscription failed");
        return;
    }
    arguments.append(options);
    auto message = QDBusMessage::createMethodCall(service, desktopPath, shortcutsInterface, method);
    message.setArguments(arguments);
    const auto generation = generation_;
    const auto expectedPath = requestPath_;
    auto* pending = new QDBusPendingCallWatcher(bus_.asyncCall(message), this);
    timeout_.start();
    connect(pending, &QDBusPendingCallWatcher::finished, this,
            [this, generation, expectedPath](auto* call) {
                QDBusPendingReply<QDBusObjectPath> reply = *call;
                call->deleteLater();
                if (generation != generation_ || requestPath_ != expectedPath) {
                    return;
                }
                if (reply.isError() || reply.value().path() != expectedPath) {
                    fail("Portal registration failed: invalid request reply");
                }
            });
}
void PortalShortcutBackend::response(uint code, const QVariantMap& results,
                                     const QDBusMessage& message) {
    if (requestPath_.isEmpty() || message.path() != requestPath_) {
        return;
    }
    bus_.disconnect(service, requestPath_, requestInterface, "Response", this,
                    SLOT(response(uint, QVariantMap, QDBusMessage)));
    requestPath_.clear();
    timeout_.stop();
    if (code != 0) {
        fail(code == 1 ? "Shortcut registration cancelled or refused"
                       : "Shortcut registration failed");
        return;
    }
    if (stage_ == Stage::Creating) {
        if (results.value("session_handle").toString() != sessionPath_) {
            fail("Portal registration failed: invalid session handle");
            return;
        }
        stage_ = Stage::Binding;
        updateStatus("Waiting for portal shortcut consent");
        const auto trigger = sequence_ == "Ctrl+Alt+Space" ? "CTRL+ALT+space" : "CTRL+ALT+s";
        PortalShortcuts shortcuts{
            {shortcutId,
             {{"description", tr("Toggle Sentinel Quick Panel")}, {"preferred_trigger", trigger}}}};
        request("BindShortcuts",
                {QVariant::fromValue(QDBusObjectPath(sessionPath_)), QVariant::fromValue(shortcuts),
                 QString{}},
                {});
    } else if (stage_ == Stage::Binding) {
        updateBindings(qdbus_cast<PortalShortcuts>(results.value("shortcuts")));
    }
}
void PortalShortcutBackend::updateBindings(const PortalShortcuts& shortcuts) {
    for (const auto& shortcut : shortcuts) {
        const auto trigger = shortcut.properties.value("trigger_description").toString();
        if (shortcut.id == shortcutId && !trigger.trimmed().isEmpty()) {
            stage_ = Stage::Registered;
            updateStatus("Registered via portal: " + trigger);
            return;
        }
    }
    fail("Shortcut not bound; unavailable, refused or in use");
}
void PortalShortcutBackend::shortcutsChanged(const QDBusObjectPath& session,
                                             const PortalShortcuts& shortcuts) {
    if (registered() && session.path() == sessionPath_) {
        updateBindings(shortcuts);
    }
}
void PortalShortcutBackend::shortcutActivated(const QDBusObjectPath& session, const QString& id,
                                              qulonglong, const QVariantMap& options) {
    if (registered() && session.path() == sessionPath_ && id == shortcutId) {
        emit activated(options.value("activation_token").toString());
    }
}
void PortalShortcutBackend::sessionClosed(const QVariantMap&, const QDBusMessage& message) {
    if (message.path() == sessionPath_) {
        fail("Global shortcuts portal session closed; disable and re-enable to retry");
    }
}
void PortalShortcutBackend::cancel() {
    ++generation_;
    timeout_.stop();
    stage_ = Stage::Idle;
    if (!requestPath_.isEmpty()) {
        bus_.disconnect(service, requestPath_, requestInterface, "Response", this,
                        SLOT(response(uint, QVariantMap, QDBusMessage)));
        bus_.asyncCall(
            QDBusMessage::createMethodCall(service, requestPath_, requestInterface, "Close"));
        requestPath_.clear();
    }
    if (!sessionPath_.isEmpty()) {
        bus_.disconnect(service, sessionPath_, sessionInterface, "Closed", this,
                        SLOT(sessionClosed(QVariantMap, QDBusMessage)));
        bus_.asyncCall(
            QDBusMessage::createMethodCall(service, sessionPath_, sessionInterface, "Close"));
        sessionPath_.clear();
    }
}
void PortalShortcutBackend::fail(const QString& status) {
    cancel();
    updateStatus(status);
}
void PortalShortcutBackend::shutdown() {
    stopped_ = true;
    sequence_.clear();
    cancel();
    updateStatus("Disabled");
}
} // namespace sentinel::desktop
