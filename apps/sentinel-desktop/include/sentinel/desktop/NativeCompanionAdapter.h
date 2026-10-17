// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sentinel/desktop/GlobalShortcutBackend.h"
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
#endif
#include <memory>
class QMenu;
class QSystemTrayIcon;
class QWindow;
namespace sentinel::core {
class AppSettings;
}
namespace sentinel::desktop {
class DesktopShellViewModel;
class QuickPanelController;
#ifdef Q_OS_MACOS
class MacNativeNotifications;
#endif
class NativeCompanionAdapter final : public QObject
#ifdef Q_OS_WIN
    ,
                                     public QAbstractNativeEventFilter
#endif
{
    Q_OBJECT
    Q_PROPERTY(bool startAtLogin READ startAtLogin WRITE setStartAtLogin NOTIFY startupChanged)
    Q_PROPERTY(QString startupStatus READ startupStatus NOTIFY startupChanged)
    Q_PROPERTY(QString shortcut READ shortcut WRITE setShortcut NOTIFY shortcutChanged)
    Q_PROPERTY(QString shortcutStatus READ shortcutStatus NOTIFY shortcutChanged)
    Q_PROPERTY(QString notificationStatus READ notificationStatus NOTIFY notificationStatusChanged)
public:
    QString notificationStatus() const {
        return notificationStatus_;
    }
    NativeCompanionAdapter(DesktopShellViewModel&, core::AppSettings&, QObject* rootWindow,
                           QObject* parent = nullptr,
                           std::unique_ptr<GlobalShortcutBackend> shortcutBackend = {});
    ~NativeCompanionAdapter() override;
    void bindQuickPanel(QuickPanelController*);
    void setWindow(QObject* window);
    bool startAtLogin() const;
    void setStartAtLogin(bool enabled);
    QString startupStatus() const {
        return startupStatus_;
    }
    QString shortcut() const;
    QString shortcutStatus() const {
        return shortcutStatus_;
    }
    void setShortcut(const QString& sequence);
    Q_INVOKABLE void togglePanel();
    Q_INVOKABLE void openSentinel();
    Q_INVOKABLE void openSettings();
signals:
    void notificationStatusChanged();
    void startupChanged();
    void shortcutChanged();

protected:
#ifdef Q_OS_WIN
    bool nativeEventFilter(const QByteArray&, void*, qintptr*) override;
#endif
    bool eventFilter(QObject*, QEvent*) override;

private:
    std::unique_ptr<GlobalShortcutBackend> shortcutBackend_;
    QString notificationStatus_;
#ifdef Q_OS_MACOS
    std::unique_ptr<MacNativeNotifications> notifications_;
#endif
    void refreshVisibility();
    void registerShortcut();
    void unregisterShortcut();
    DesktopShellViewModel& viewModel_;
    core::AppSettings& settings_;
    QWindow* window_ = nullptr;
    QPointer<QWindow> observedPanel_;
    bool panelReceivedFocus_ = false;
    QMetaObject::Connection frameConnection_;
    QElapsedTimer panelOpenTimer_;
    QuickPanelController* quickPanel_ = nullptr;
    std::unique_ptr<QSystemTrayIcon> trayIcon_;
    std::unique_ptr<QMenu> menu_;
    QString shortcutStatus_, notificationSession_;
    QString notificationPage_ = QStringLiteral("Dashboard");
    QString startupStatus_ = QStringLiteral("Login startup is disabled unless enabled in Settings");
    void* hotKey_ = nullptr;
    void* hotKeyHandler_ = nullptr;
};
} // namespace sentinel::desktop
