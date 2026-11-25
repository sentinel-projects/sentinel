// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SENTINEL_DESKTOP_APPLICATIONBOOTSTRAPPER_H
#define SENTINEL_DESKTOP_APPLICATIONBOOTSTRAPPER_H

#include "SingleInstanceGuard.h"

#include "sentinel/core/app/AgentInspectorService.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/platform/StandardPathProvider.h"
#include "sentinel/core/platform/ProfileWriteLease.h"
#include "sentinel/desktop/DesktopRuntimeClient.h"
#include "sentinel/desktop/DesktopShellViewModel.h"
#include "sentinel/desktop/WindowStatePersistence.h"
#include "sentinel/desktop/viewmodels/AgentInspectorViewModel.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QQmlApplicationEngine>
#include <QTranslator>
#include <memory>

namespace sentinel::desktop {

class ApplicationBootstrapper final : public QObject {
    Q_OBJECT
public:
    ApplicationBootstrapper(int argc, char* argv[], QObject* parent = nullptr);
    ~ApplicationBootstrapper() override;

    bool ensureSingleInstance();
    void initializeLogging();
    void initializeGraphics();
    void initializePlatformIntegrations();

    bool setupQmlEngine(QApplication& app);

private:
    void ensureBackgroundDaemon();

    int m_argc;
    char** m_argv;
    QCommandLineParser m_parser;
    std::unique_ptr<QProcess> m_daemonProcess;

    bool m_verbose{false};
    bool m_quiet{false};
    bool m_safeMode{false};

    sentinel::core::StandardPathProvider m_pathProvider;
    sentinel::core::ProfileWriteLease m_profileWriteLease;
    SingleInstanceGuard m_singleInstanceGuard;
    std::unique_ptr<sentinel::core::AppSettings> m_settings;
    std::unique_ptr<DaemonClient> m_daemonClient;
    std::unique_ptr<DesktopRuntimeClient> m_runtimeClient;
    std::unique_ptr<sentinel::core::AgentInspectorService> m_inspectorService;
    std::unique_ptr<AgentInspectorViewModel> m_inspectorViewModel;
    std::unique_ptr<sentinel::core::ModeManager> m_modeManager;
    std::unique_ptr<sentinel::core::WinTaskbarIntegration> m_taskbarIntegration;
    std::unique_ptr<DesktopShellViewModel> m_shellViewModel;
    std::unique_ptr<QSettings> m_windowSettings;
    std::unique_ptr<WindowStateController> m_windowState;
    QTranslator m_translator;
    QQmlApplicationEngine m_engine;
};

} // namespace sentinel::desktop

#endif // SENTINEL_DESKTOP_APPLICATIONBOOTSTRAPPER_H
