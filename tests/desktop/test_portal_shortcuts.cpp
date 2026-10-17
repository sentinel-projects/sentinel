#include "sentinel/desktop/PortalShortcutBackend.h"
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <memory>

using namespace sentinel::desktop;
namespace {
const QString service = QStringLiteral("org.freedesktop.portal.Desktop");
const QString root = QStringLiteral("/org/freedesktop/portal/desktop");
const QString shortcutInterface = QStringLiteral("org.freedesktop.portal.GlobalShortcuts");

class FakePortal final : public QDBusVirtualObject {
public:
    uint version = 1;
    uint refusal = 0;
    bool collision = false;
    bool hold = false;
    bool methodError = false;
    int creates = 0, binds = 0, closes = 0;
    QString session, lastRequest, preferred;
    QString introspect(const QString&) const override {
        return {};
    }
    bool handleMessage(const QDBusMessage& message, const QDBusConnection& bus) override {
        if (message.member() == "Get") {
            bus.send(message.createReply(QVariant::fromValue(QDBusVariant(version))));
            return true;
        }
        if (message.member() == "Close") {
            ++closes;
            bus.send(message.createReply());
            return true;
        }
        if (methodError) {
            bus.send(
                message.createErrorReply("org.freedesktop.portal.Error.Failed", "Fixture failure"));
            return true;
        }
        const auto options = qdbus_cast<QVariantMap>(message.arguments().last());
        auto sender = message.service().mid(1);
        sender.replace('.', '_');
        lastRequest = root + "/request/" + sender + "/" + options.value("handle_token").toString();
        QVariantMap results;
        uint code = 0;
        if (message.member() == "CreateSession") {
            ++creates;
            session = root + "/session/" + sender + "/" +
                      options.value("session_handle_token").toString();
            results.insert("session_handle", session);
        } else if (message.member() == "BindShortcuts") {
            ++binds;
            const auto shortcuts = qdbus_cast<PortalShortcuts>(message.arguments().at(1));
            if (shortcuts.size() == 1)
                preferred = shortcuts.first().properties.value("preferred_trigger").toString();
            code = refusal;
            PortalShortcuts bound;
            if (!collision)
                bound.append({"quick-panel", {{"trigger_description", "Ctrl+Alt+Space"}}});
            results.insert("shortcuts", QVariant::fromValue(bound));
        } else {
            return false;
        }
        bus.send(message.createReply(QVariant::fromValue(QDBusObjectPath(lastRequest))));
        if (!hold) {
            auto signal = QDBusMessage::createSignal(lastRequest, "org.freedesktop.portal.Request",
                                                     "Response");
            signal.setArguments({code, results});
            bus.send(signal);
        }
        return true;
    }
    void activate(const QDBusConnection& bus, const QString& path = {}) {
        auto signal = QDBusMessage::createSignal(root, shortcutInterface, "Activated");
        signal.setArguments({QVariant::fromValue(QDBusObjectPath(path.isEmpty() ? session : path)),
                             QStringLiteral("quick-panel"), QVariant::fromValue(qulonglong(42)),
                             QVariantMap{{"activation_token", "test-token"}}});
        bus.send(signal);
    }
};
} // namespace
class PortalShortcutsTest final : public QObject {
    Q_OBJECT
    QProcess daemon_;
    QString address_;
    std::unique_ptr<QDBusConnection> server_, client_;
    std::unique_ptr<FakePortal> portal_;
private slots:
    void initTestCase() {
        const auto daemon = QStandardPaths::findExecutable("dbus-daemon");
        QVERIFY2(!daemon.isEmpty(), "dbus-daemon is required for isolated portal contract tests");
        daemon_.start(daemon,
                      {"--session", "--nofork", "--address=unix:tmpdir=/tmp", "--print-address=1"});
        QVERIFY(daemon_.waitForStarted());
        QVERIFY2(daemon_.waitForReadyRead(),
                 qPrintable(QString::fromUtf8(daemon_.readAllStandardError())));
        address_ = QString::fromUtf8(daemon_.readLine()).trimmed();
        QVERIFY(!address_.isEmpty());
        server_ = std::make_unique<QDBusConnection>(
            QDBusConnection::connectToBus(address_, "portal-server"));
        client_ = std::make_unique<QDBusConnection>(
            QDBusConnection::connectToBus(address_, "portal-client"));
        QVERIFY(server_->isConnected());
        QVERIFY(client_->isConnected());
    }
    void init() {
        portal_ = std::make_unique<FakePortal>();
        QVERIFY(server_->registerVirtualObject(root, portal_.get(), QDBusConnection::SubPath));
        QVERIFY(server_->registerService(service));
    }
    void cleanup() {
        server_->unregisterService(service);
        server_->unregisterObject(root, QDBusConnection::UnregisterTree);
        portal_.reset();
        QCoreApplication::processEvents();
    }
    void cleanupTestCase() {
        server_.reset();
        client_.reset();
        QDBusConnection::disconnectFromBus("portal-server");
        QDBusConnection::disconnectFromBus("portal-client");
        daemon_.terminate();
        if (!daemon_.waitForFinished(3000)) {
            daemon_.kill();
            daemon_.waitForFinished();
        }
    }
    void registrationActivationAndCleanup() {
        PortalShortcutBackend backend(*client_, "wayland");
        QSignalSpy activation(&backend, &PortalShortcutBackend::activated);
        backend.setShortcut("Ctrl+Alt+Space");
        QVERIFY(!backend.registered());
        QTRY_VERIFY(backend.registered());
        QCOMPARE(portal_->preferred, QString("CTRL+ALT+space"));
        QCOMPARE(portal_->creates, 1);
        backend.setShortcut("Ctrl+Alt+Space");
        QCOMPARE(portal_->creates, 1);
        portal_->activate(*server_, root + "/session/foreign");
        portal_->activate(*server_);
        QTRY_COMPARE(activation.count(), 1);
        QCOMPARE(activation.first().first().toString(), QString("test-token"));
        backend.shutdown();
        QVERIFY(!backend.registered());
        QTRY_VERIFY(portal_->closes > 0);
        portal_->activate(*server_);
        QTest::qWait(20);
        QCOMPARE(activation.count(), 1);
    }
    void refusal() {
        portal_->refusal = 1;
        PortalShortcutBackend backend(*client_, "wayland");
        backend.setShortcut("Ctrl+Alt+S");
        QTRY_VERIFY(backend.status().contains("refused"));
        QVERIFY(!backend.registered());
        backend.setShortcut("Ctrl+Alt+S");
        QCOMPARE(portal_->binds, 1);
    }
    void methodFailure() {
        portal_->methodError = true;
        PortalShortcutBackend backend(*client_, "wayland");
        backend.setShortcut("Ctrl+Alt+S");
        QTRY_VERIFY(backend.status().contains("failed"));
        QVERIFY(!backend.registered());
    }
    void pendingRegistrationTimesOut() {
        portal_->hold = true;
        PortalShortcutBackend backend(*client_, "wayland", nullptr, 100);
        backend.setShortcut("Ctrl+Alt+S");
        QTRY_VERIFY(backend.status().contains("timed out"));
        QVERIFY(!backend.registered());
        auto signal = QDBusMessage::createSignal(portal_->lastRequest,
                                                 "org.freedesktop.portal.Request", "Response");
        signal.setArguments({uint(0), QVariantMap{{"session_handle", portal_->session}}});
        server_->send(signal);
        QTest::qWait(20);
        QVERIFY(!backend.registered());
        QCOMPARE(portal_->binds, 0);
    }
    void bindingRevoked() {
        PortalShortcutBackend backend(*client_, "wayland");
        backend.setShortcut("Ctrl+Alt+S");
        QTRY_VERIFY(backend.registered());
        auto signal = QDBusMessage::createSignal(root, shortcutInterface, "ShortcutsChanged");
        signal.setArguments({QVariant::fromValue(QDBusObjectPath(portal_->session)),
                             QVariant::fromValue(PortalShortcuts{})});
        server_->send(signal);
        QTRY_VERIFY(!backend.registered());
        QVERIFY(backend.status().contains("not bound"));
    }
    void collision() {
        portal_->collision = true;
        PortalShortcutBackend backend(*client_, "wayland");
        backend.setShortcut("Ctrl+Alt+S");
        QTRY_VERIFY(backend.status().contains("not bound"));
        QVERIFY(!backend.registered());
    }
    void unsupportedPortal() {
        portal_->version = 0;
        PortalShortcutBackend backend(*client_, "wayland");
        backend.setShortcut("Ctrl+Alt+S");
        QTRY_VERIFY(backend.status().contains("unsupported"));
        QCOMPARE(portal_->creates, 0);
    }
    void unavailablePortal() {
        server_->unregisterService(service);
        PortalShortcutBackend backend(*client_, "wayland");
        backend.setShortcut("Ctrl+Alt+S");
        QTRY_VERIFY(backend.status().contains("unavailable"));
        QVERIFY(!backend.registered());
    }
    void unsupportedPlatform() {
        PortalShortcutBackend backend(*client_, "minimal");
        backend.setShortcut("Ctrl+Alt+S");
        QVERIFY(backend.status().contains("unavailable"));
        QCOMPARE(portal_->creates, 0);
    }
    void changeAndReregister() {
        PortalShortcutBackend backend(*client_, "xcb");
        backend.setShortcut("Ctrl+Alt+S");
        QTRY_VERIFY(backend.registered());
        const auto previous = portal_->session;
        backend.setShortcut("Ctrl+Alt+Space");
        QTRY_COMPARE(portal_->binds, 2);
        QTRY_VERIFY(backend.registered());
        QVERIFY(previous != portal_->session);
        backend.setShortcut("Disabled");
        QVERIFY(!backend.registered());
        backend.setShortcut("Ctrl+Alt+S");
        QTRY_COMPARE(portal_->binds, 3);
    }
    void desktopPortalRestart() {
        PortalShortcutBackend backend(*client_, "wayland");
        backend.setShortcut("Ctrl+Alt+S");
        QTRY_VERIFY(backend.registered());
        server_->unregisterService(service);
        QTRY_VERIFY(!backend.registered());
        QVERIFY(server_->registerService(service));
        QTRY_VERIFY(backend.registered());
        QCOMPARE(portal_->binds, 2);
    }
    void sessionClosure() {
        PortalShortcutBackend backend(*client_, "wayland");
        backend.setShortcut("Ctrl+Alt+S");
        QTRY_VERIFY(backend.registered());
        auto signal = QDBusMessage::createSignal(portal_->session, "org.freedesktop.portal.Session",
                                                 "Closed");
        signal.setArguments({QVariantMap{}});
        server_->send(signal);
        QTRY_VERIFY(!backend.registered());
        QVERIFY(backend.status().contains("session closed"));
    }
    void pendingShutdown() {
        portal_->hold = true;
        PortalShortcutBackend backend(*client_, "wayland");
        backend.setShortcut("Ctrl+Alt+S");
        QTRY_COMPARE(portal_->creates, 1);
        backend.shutdown();
        QTRY_VERIFY(portal_->closes >= 2);
        QVERIFY(!backend.registered());
    }
};
QTEST_GUILESS_MAIN(PortalShortcutsTest)
#include "test_portal_shortcuts.moc"
