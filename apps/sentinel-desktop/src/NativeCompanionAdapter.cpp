// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/desktop/NativeCompanionAdapter.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/desktop/DesktopShellViewModel.h"
#include "sentinel/desktop/GlobalShortcutBackend.h"
#include "sentinel/desktop/QuickPanelController.h"
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileOpenEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QWindow>
#ifdef Q_OS_LINUX
#include "sentinel/desktop/PortalShortcutBackend.h"
#endif
#ifdef Q_OS_MACOS
#include "sentinel/desktop/MacNativeNotifications.h"
#include <Carbon/Carbon.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif
namespace sentinel::desktop {
#ifdef Q_OS_MACOS
void activateMacQuickPanelApplication();
#endif
namespace {
bool headless() {
    return QGuiApplication::platformName() == "offscreen" ||
           QGuiApplication::platformName() == "minimal";
}
} // namespace
NativeCompanionAdapter::NativeCompanionAdapter(
    DesktopShellViewModel& vm, core::AppSettings& settings, QObject* root, QObject* parent,
    std::unique_ptr<GlobalShortcutBackend> shortcutBackend)
    : QObject(parent), shortcutBackend_(std::move(shortcutBackend)), viewModel_(vm),
      settings_(settings) {
    startupStatus_ = settings_.quickPanelStartAtLogin()
                         ? "Enabled preference; OS registration has not been checked"
                         : "Disabled";
    setWindow(root);
    qApp->installEventFilter(this);
    connect(qApp, &QGuiApplication::focusWindowChanged, this, [this](QWindow* focused) {
        if (!focused || !observedPanel_ || !observedPanel_->isVisible()) return;
        for (auto* current = focused; current; current = current->transientParent())
            if (current == observedPanel_) { panelReceivedFocus_ = true; return; }
        if (panelReceivedFocus_) observedPanel_->hide();
    });
#ifdef Q_OS_WIN
    qApp->installNativeEventFilter(this);
#endif
#ifdef Q_OS_LINUX
    if (!shortcutBackend_ && !headless())
        shortcutBackend_ = std::make_unique<PortalShortcutBackend>(QDBusConnection::sessionBus(),
                                                                   QGuiApplication::platformName());
#endif
    if (shortcutBackend_) {
        connect(shortcutBackend_.get(), &GlobalShortcutBackend::statusChanged, this, [this] {
            shortcutStatus_ = shortcutBackend_->status();
            emit shortcutChanged();
        });
        connect(shortcutBackend_.get(), &GlobalShortcutBackend::activated, this,
                [this](const QString& token) {
                    if (!shortcutBackend_->registered() || !settings_.companionEnabled())
                        return;
                    const bool applyToken =
                        QGuiApplication::platformName().startsWith("wayland") && !token.isEmpty();
                    const auto previous = qgetenv("XDG_ACTIVATION_TOKEN");
                    const bool wasSet = qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN");
                    if (applyToken)
                        qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
                    togglePanel();
                    if (!applyToken)
                        return;
                    if (wasSet)
                        qputenv("XDG_ACTIVATION_TOKEN", previous);
                    else
                        qunsetenv("XDG_ACTIVATION_TOKEN");
                });
        connect(qApp, &QCoreApplication::aboutToQuit, this,
                [this] { shortcutBackend_->shutdown(); });
        connect(&settings_, &core::AppSettings::quickPanelShortcutChanged, this,
                &NativeCompanionAdapter::registerShortcut);
        connect(&settings_, &core::AppSettings::companionEnabledChanged, this,
                &NativeCompanionAdapter::registerShortcut);
        registerShortcut();
    }
    if (headless()) {
        viewModel_.setCompanionNativeAvailable(false);
        if (!shortcutBackend_)
            shortcutStatus_ = "Unavailable on headless platform";
        return;
    }
#ifdef Q_OS_MACOS
    notifications_ = std::make_unique<MacNativeNotifications>();
    connect(notifications_.get(), &MacNativeNotifications::statusChanged, this,
            [this](const QString& status) {
                notificationStatus_ = status;
                emit notificationStatusChanged();
                qDebug() << "macOS notification authorization:" << status;
            });
    connect(notifications_.get(), &MacNativeNotifications::clicked, this,
            [this](const QString& session, const QString& page) {
                activateMacQuickPanelApplication();
                if (quickPanel_ && !session.isEmpty())
                    quickPanel_->openLink("sentinel://session/" + session);
                else {
                    openSentinel();
                    emit viewModel_.requestWindowActive(page);
                }
            });
#endif
    menu_ = std::make_unique<QMenu>();
    connect(menu_->addAction(tr("Quick Panel")), &QAction::triggered, this,
            &NativeCompanionAdapter::togglePanel);
    connect(menu_->addAction(tr("Open Sentinel")), &QAction::triggered, this,
            &NativeCompanionAdapter::openSentinel);
    connect(menu_->addAction(tr("Settings")), &QAction::triggered, this,
            &NativeCompanionAdapter::openSettings);
    menu_->addSeparator();
    connect(menu_->addAction(tr("Quit Sentinel")), &QAction::triggered, qApp, &QApplication::quit);
    QIcon icon(QStringLiteral(":/branding/tray.png"));
#ifdef Q_OS_MACOS
    icon.setIsMask(true);
#endif
    trayIcon_ = std::make_unique<QSystemTrayIcon>(icon);
#ifndef Q_OS_MACOS
    trayIcon_->setContextMenu(menu_.get());
#endif
    connect(trayIcon_.get(), &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger)
                    togglePanel();
#ifdef Q_OS_MACOS
                else if (reason == QSystemTrayIcon::Context)
                    menu_->popup(QCursor::pos());
#endif
            });
    connect(trayIcon_.get(), &QSystemTrayIcon::messageClicked, this, [this] {
        if (quickPanel_ && !notificationSession_.isEmpty())
            quickPanel_->openLink("sentinel://session/" + notificationSession_);
        else {
            openSentinel();
            emit viewModel_.requestWindowActive(notificationPage_);
        }
    });
    connect(&settings_, &core::AppSettings::companionEnabledChanged, this,
            &NativeCompanionAdapter::refreshVisibility);
    if (!shortcutBackend_)
        connect(&settings_, &core::AppSettings::quickPanelShortcutChanged, this,
                &NativeCompanionAdapter::registerShortcut);
    refreshVisibility();
}
NativeCompanionAdapter::~NativeCompanionAdapter() {
    if (shortcutBackend_)
        shortcutBackend_->shutdown();
    unregisterShortcut();
#ifdef Q_OS_MACOS
    notifications_.reset();
#endif
    if (trayIcon_)
        trayIcon_->hide();
#ifdef Q_OS_WIN
    qApp->removeNativeEventFilter(this);
#endif
}
void NativeCompanionAdapter::setWindow(QObject* root) {
    window_ = qobject_cast<QWindow*>(root);
    if (window_)
        window_->installEventFilter(this);
}
void NativeCompanionAdapter::bindQuickPanel(QuickPanelController* controller) {
    quickPanel_ = controller;
    connect(controller, &QuickPanelController::openRequested, this,
            [this](const QString& page, const QString&) {
                openSentinel();
                emit viewModel_.requestWindowActive(page);
            });
    connect(controller, &QuickPanelController::changed, this, [this] {
        if (trayIcon_)
            trayIcon_->setToolTip("Sentinel — " +
                                  quickPanel_->state().value("connection").toString());
    });
    connect(&viewModel_, &DesktopShellViewModel::nativeNotificationRequested, this,
            [this](const QString& title, const QString& body, const QString& category) {
                if (!trayIcon_ || !trayIcon_->isVisible() || viewModel_.dndEnabled() || viewModel_.isChannelMuted(category))
                    return;
#ifdef Q_OS_MACOS
                // Agent terminal notifications are owned by QuickPanelController.
                if (category == "Agent")
                    return;
#endif
                notificationSession_.clear();
                notificationPage_ = category == "Models"    ? "Models"
                                    : category == "Updates" ? "Settings"
                                                            : "Dashboard";
#ifdef Q_OS_MACOS
                notifications_->deliver(title, tr("Open Sentinel to review this notification."), {},
                                        notificationPage_);
#else
                trayIcon_->showMessage(title, body, QSystemTrayIcon::NoIcon, 8000);
#endif
            });
    connect(controller, &QuickPanelController::notificationRequested, this,
            [this](const QString& title, const QString& body, const QString& session, const QString& category, const QString& priority) {
                if (!trayIcon_ || !trayIcon_->isVisible() ||
                    !viewModel_.shouldShowNotification({{"category", category}, {"priority", priority}, {"title", title}}))
                    return;
                notificationSession_ = session;
                notificationPage_ = "Dashboard";
#ifdef Q_OS_MACOS
                notifications_->deliver(title, tr("Open Sentinel to review this run or approval."),
                                        session, notificationPage_);
#else
                trayIcon_->showMessage(title, body, QSystemTrayIcon::NoIcon, 8000);
#endif
            });
}
void NativeCompanionAdapter::refreshVisibility() {
    const bool available = !headless() && QSystemTrayIcon::isSystemTrayAvailable();
    viewModel_.setCompanionNativeAvailable(available);
    const bool background = settings_.companionEnabled() && available;
    qApp->setQuitOnLastWindowClosed(!background);
    if (trayIcon_)
        trayIcon_->setVisible(background);
    registerShortcut();
    if (!background && window_ && !window_->isVisible())
        openSentinel();
}
void NativeCompanionAdapter::togglePanel() {
    if (!settings_.companionEnabled())
        return;
    QWindow* panel = nullptr;
    for (auto* candidate : QGuiApplication::allWindows())
        if (candidate->objectName() == "sentinelQuickPanel") {
            panel = candidate;
            break;
        }
    if (!panel) {
        openSentinel();
        return;
    }
    panel->installEventFilter(this);
    if (panel->isVisible()) {
        panelOpenTimer_.invalidate();
        panel->hide();
        return;
    }
    if (observedPanel_ != panel) {
        disconnect(frameConnection_);
        observedPanel_ = panel;
        if (auto* quick = qobject_cast<QQuickWindow*>(panel))
            frameConnection_ = connect(
                quick, &QQuickWindow::frameSwapped, this,
                [this] {
                    if (panelOpenTimer_.isValid()) {
                        qDebug() << "Quick Panel open to first frame (ms):"
                                 << panelOpenTimer_.nsecsElapsed() / 1000000.0;
                        panelOpenTimer_.invalidate();
                    }
                },
                Qt::QueuedConnection);
    }
    panelReceivedFocus_ = false;
    panelOpenTimer_.start();
    QRect anchor = trayIcon_ ? trayIcon_->geometry() : QRect{};
    QScreen* screen =
        QGuiApplication::screenAt(anchor.isEmpty() ? QCursor::pos() : anchor.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect area = screen->availableGeometry();
    panel->setScreen(screen);
    panel->resize(qMin(420, area.width() - 24), qMin(panel->height(), area.height() - 24));
    int x = anchor.isEmpty() ? area.right() - panel->width() - 12
                             : anchor.center().x() - panel->width() / 2;
    int y = anchor.isEmpty() || anchor.center().y() < area.center().y()
                ? qMax(area.top() + 8, anchor.bottom() + 8)
                : anchor.top() - panel->height() - 8;
    if (!QGuiApplication::platformName().startsWith("wayland"))
        panel->setPosition(qBound(area.left() + 8, x, area.right() - panel->width() - 8),
                           qBound(area.top() + 8, y, area.bottom() - panel->height() - 8));
    panel->show();
    if (!QGuiApplication::platformName().startsWith("wayland"))
        panel->raise();
#ifdef Q_OS_MACOS
    if (!headless())
        activateMacQuickPanelApplication();
#endif
    panel->requestActivate();
    QMetaObject::invokeMethod(panel, "focusPrompt", Qt::QueuedConnection);
}
void NativeCompanionAdapter::openSentinel() {
    if (window_) {
        window_->setWindowStates(window_->windowStates() & ~Qt::WindowMinimized);
        window_->setVisible(true);
        if (!QGuiApplication::platformName().startsWith("wayland"))
            window_->raise();
        window_->requestActivate();
    }
}
void NativeCompanionAdapter::openSettings() {
#ifdef Q_OS_MACOS
    if (notifications_)
        notifications_->refreshAuthorization();
#endif
    openSentinel();
    emit viewModel_.requestWindowActive("Settings");
}
bool NativeCompanionAdapter::eventFilter(QObject* object, QEvent* event) {
    if (observedPanel_ && observedPanel_->isVisible()) {
        if (event->type() == QEvent::ApplicationDeactivate ||
            (event->type() == QEvent::ApplicationStateChange && QGuiApplication::applicationState() != Qt::ApplicationActive)) observedPanel_->hide();
        if (event->type() == QEvent::MouseButtonPress) {
            auto* target = qobject_cast<QWindow*>(object);
            if (auto* item = qobject_cast<QQuickItem*>(object)) target = item->window();
            bool ownedPopup = false;
            for (auto* current = target; current; current = current->transientParent())
                if (current == observedPanel_) { ownedPopup = true; break; }
            const auto position = static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
            if (!ownedPopup && !observedPanel_->geometry().contains(position)) observedPanel_->hide();
        }
    }

    if (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride) {
        auto* panel = qobject_cast<QWindow*>(object);
        if (!panel || panel->objectName() != "sentinelQuickPanel")
            panel = QGuiApplication::focusWindow();
        if (panel && panel->objectName() == "sentinelQuickPanel" && panel->isVisible() &&
            static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            event->accept();
            if (event->type() == QEvent::KeyPress) {
                if (panel->property("detailPopupVisible").toBool())
                    QMetaObject::invokeMethod(panel, "dismissDetails");
                else
                    panel->hide();
            }
            return true;
        }
    }
    if (object == window_ && event->type() == QEvent::Close && settings_.companionEnabled() &&
        trayIcon_ && trayIcon_->isVisible()) {
        static_cast<QCloseEvent*>(event)->ignore();
        window_->hide();
        return true;
    }
    if (event->type() == QEvent::FileOpen && quickPanel_) {
        auto* file = static_cast<QFileOpenEvent*>(event);
        if (file->url().scheme() == "sentinel")
            return quickPanel_->openLink(file->url().toString());
        // File attachment execution is deliberately unavailable; reveal the main surface.
        openSentinel();
        return true;
    }
    return QObject::eventFilter(object, event);
}
bool NativeCompanionAdapter::startAtLogin() const {
    return settings_.quickPanelStartAtLogin();
}
void NativeCompanionAdapter::setStartAtLogin(bool enabled) {
    if (headless()) {
        startupStatus_ = "Unavailable on headless platform";
        emit startupChanged();
        return;
    }
    bool success = false;
    const auto executable = QCoreApplication::applicationFilePath();
#ifdef Q_OS_WIN
    QSettings registry(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QSettings::NativeFormat);
    if (enabled)
        registry.setValue("Sentinel",
                          QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(executable)));
    else
        registry.remove("Sentinel");
    registry.sync();
    success = registry.status() == QSettings::NoError;
#else
#ifdef Q_OS_MACOS
    const QString directory = QDir::homePath() + "/Library/LaunchAgents";
    const QString path = directory + "/dev.sentinel.desktop.plist";
    const QByteArray content =
        QStringLiteral("<?xml version=\"1.0\"?><plist "
                       "version=\"1.0\"><dict><key>Label</key><string>dev.sentinel.desktop</"
                       "string><key>ProgramArguments</key><array><string>%1</string></"
                       "array><key>RunAtLoad</key><true/></dict></plist>")
            .arg(executable.toHtmlEscaped())
            .toUtf8();
#else
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/autostart";
    const QString path = directory + "/dev.sentinel.Sentinel.desktop";
    QString escaped = executable;
    escaped.replace("\\", "\\\\").replace("\"", "\\\"").replace("`", "\\`").replace("$", "\\$");
    const QByteArray content =
        QStringLiteral(
            "[Desktop Entry]\nType=Application\nName=Sentinel\nExec=\"%1\"\nTerminal=false\n")
            .arg(escaped)
            .toUtf8();
#endif
    if (!enabled)
        success = !QFile::exists(path) || QFile::remove(path);
    else if (QDir{}.mkpath(directory)) {
        QSaveFile file(path);
        success = file.open(QIODevice::WriteOnly) && file.write(content) == content.size() &&
                  file.commit();
    }
#endif
    if (success)
        settings_.setQuickPanelStartAtLogin(enabled);
    startupStatus_ = success ? (enabled ? "Enabled for next login" : "Disabled")
                             : "Login startup registration failed";
    qInfo().noquote() << startupStatus_;
    emit startupChanged();
}
QString NativeCompanionAdapter::shortcut() const {
    return settings_.quickPanelShortcut();
}
void NativeCompanionAdapter::setShortcut(const QString& sequence) {
    settings_.setQuickPanelShortcut(sequence);
}
void NativeCompanionAdapter::unregisterShortcut() {
    if (shortcutBackend_)
        shortcutBackend_->setShortcut({});
#ifdef Q_OS_MACOS
    if (hotKey_)
        UnregisterEventHotKey(static_cast<EventHotKeyRef>(hotKey_));
    if (hotKeyHandler_)
        RemoveEventHandler(static_cast<EventHandlerRef>(hotKeyHandler_));
#elif defined(Q_OS_WIN)
    if (hotKey_)
        UnregisterHotKey(nullptr, 0x534e);
#endif
    hotKey_ = hotKeyHandler_ = nullptr;
}
void NativeCompanionAdapter::registerShortcut() {
    if (shortcutBackend_) {
        shortcutBackend_->setShortcut(settings_.companionEnabled() ? shortcut() : QString{});
        shortcutStatus_ = shortcutBackend_->status();
        emit shortcutChanged();
        return;
    }
    unregisterShortcut();
    shortcutStatus_ = "Disabled";
    if (settings_.companionEnabled() && !headless() && !shortcut().isEmpty() &&
        shortcut() != "Disabled") {
#ifdef Q_OS_MACOS
        UInt32 key = 0, modifiers = 0;
        const QString sequence = shortcut();
        if (sequence == "Ctrl+Alt+Space") {
            key = kVK_Space;
            modifiers = controlKey | optionKey;
        } else if (sequence == "Ctrl+Alt+S") {
            key = kVK_ANSI_S;
            modifiers = controlKey | optionKey;
        }
        if (!modifiers)
            shortcutStatus_ = "Unsupported shortcut; use Ctrl+Alt+Space or Ctrl+Alt+S";
        else {
            EventTypeSpec eventType{kEventClassKeyboard, kEventHotKeyPressed};
            EventHandlerRef handler = nullptr;
            auto callback = [](EventHandlerCallRef, EventRef, void* user) -> OSStatus {
                QMetaObject::invokeMethod(static_cast<NativeCompanionAdapter*>(user), "togglePanel",
                                          Qt::QueuedConnection);
                return noErr;
            };
            OSStatus status =
                InstallApplicationEventHandler(callback, 1, &eventType, this, &handler);
            EventHotKeyRef hotkey = nullptr;
            if (status == noErr)
                status = RegisterEventHotKey(key, modifiers, EventHotKeyID{0x534e544c, 1},
                                             GetApplicationEventTarget(), kEventHotKeyExclusive,
                                             &hotkey);
            hotKeyHandler_ = handler;
            hotKey_ = hotkey;
            shortcutStatus_ =
                status == noErr ? "Registered"
                                : QStringLiteral("Registration failed (%1); shortcut may be in use")
                                      .arg(status);
        }
#elif defined(Q_OS_WIN)
        const auto sequence = shortcut();
        const UINT key = sequence == "Ctrl+Alt+Space" ? VK_SPACE
                         : sequence == "Ctrl+Alt+S"   ? 'S'
                                                      : 0;
        if (!key)
            shortcutStatus_ = "Unsupported shortcut";
        else if (RegisterHotKey(nullptr, 0x534e, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, key)) {
            hotKey_ = this;
            shortcutStatus_ = "Registered";
        } else
            shortcutStatus_ = QStringLiteral("Registration failed (%1); shortcut may be in use")
                                  .arg(GetLastError());
#else
        shortcutStatus_ = "Global shortcut unavailable on this platform";
#endif
    }
    qInfo().noquote() << "Quick Panel shortcut:" << shortcutStatus_;
    emit shortcutChanged();
}
#ifdef Q_OS_WIN
bool NativeCompanionAdapter::nativeEventFilter(const QByteArray&, void* message, qintptr* result) {
    const auto* event = static_cast<MSG*>(message);
    if (event->message == WM_HOTKEY && event->wParam == 0x534e) {
        togglePanel();
        *result = 0;
        return true;
    }
    return false;
}
#endif
} // namespace sentinel::desktop
