// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SENTINEL_DAEMON_DAEMONSERVICE_H
#define SENTINEL_DAEMON_DAEMONSERVICE_H

#include "DaemonIpcServer.h"
#include "LocalModelRuntime.h"

#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/ApplicationController.h"
#include "sentinel/core/platform/StandardPathProvider.h"
#include "sentinel/core/platform/ProfileWriteLease.h"
#include "sentinel/core/resource/ResourceMonitor.h"

#include <QObject>
#include <QTimer>
#include <memory>

namespace sentinel::daemon {

class DaemonService final : public QObject {
    Q_OBJECT
public:
    explicit DaemonService(QObject* parent = nullptr);
    ~DaemonService() override;

    bool initialize(const QString& socketPath = {});
    void requestShutdown() {
        m_healthTimer.stop();
        if (m_resourceMonitor) {
            m_resourceMonitor->stop();
        }
        m_ipcServer.requestShutdown();
    }

private slots:
    void performHealthCheck();

private:
    void startResourceTelemetryIfEnabled();
    sentinel::core::StandardPathProvider m_pathProvider;
    sentinel::core::ProfileWriteLease m_profileWriteLease;
    std::unique_ptr<sentinel::core::AppSettings> m_settings;
    std::unique_ptr<sentinel::core::ApplicationController> m_controller;
    DaemonIpcServer m_ipcServer;
    std::unique_ptr<LocalModelRuntime> m_localModelRuntime;
    // P4-02 opt-in telemetry. Off unless SENTINEL_RESOURCE_TELEMETRY is set, so
    // the daemon has no unintended startup dependency on monitoring.
    std::unique_ptr<sentinel::core::ResourceMonitor> m_resourceMonitor;
    QTimer m_healthTimer;
};

} // namespace sentinel::daemon

#endif // SENTINEL_DAEMON_DAEMONSERVICE_H
