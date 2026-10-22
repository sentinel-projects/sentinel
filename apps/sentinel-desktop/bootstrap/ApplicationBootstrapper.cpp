void qml_register_types_Sentinel_Warden();
#include "sentinel/desktop/RemoteAgentInspectorService.h"
// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ApplicationBootstrapper.h"
#include "sentinel/desktop/CoreWardenItem.h"
#include <qqml.h>

#include "GraphicsInitializer.h"
#include "LoggingInitializer.h"
#include "PlatformInitializer.h"

#include "sentinel/core/app/AppMetadata.h"

#include "sentinel/core/app/StorageMigration.h"

#include "sentinel/core/app/ModeManager.h"
#include "sentinel/core/app/RecoveryService.h"
#include "sentinel/core/app/SettingsService.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/platform/DpapiEncryptedSettingsStore.h"
#include "sentinel/core/platform/WinProtocolHandler.h"
#include "sentinel/core/platform/WinTaskbarIntegration.h"
#include "sentinel/core/runtime/LocalInference.h"
#include "sentinel/desktop/DaemonClient.h"
#include "sentinel/desktop/DesktopModelHelper.h"
#include "sentinel/desktop/DesktopSettingsStore.h"
#include "sentinel/desktop/NativeCompanionAdapter.h"
#include "sentinel/desktop/QuickPanelController.h"

#include <QCommandLineOption>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QNetworkProxy>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>

namespace sentinel::desktop {

namespace {

QString discoverDaemonBinary() {
    // Prefer a sibling binary next to the running desktop app (dev/build layout),
    // then fall back to PATH lookup for packaged installs.
    const QString appDir = QCoreApplication::applicationDirPath();
    QString sibling = appDir + QStringLiteral("/sentinel-daemon");
    if (QFileInfo::exists(sibling)) {
        return sibling;
    }
#ifdef Q_OS_MACOS
    // Release bundles install the daemon under Contents/Helpers so a
    // drag-to-Applications install keeps the helper with the app.
    const QString helper =
        QDir(appDir).absoluteFilePath(QStringLiteral("../Helpers/sentinel-daemon"));
    if (QFileInfo(helper).isExecutable()) {
        return helper;
    }
#endif
#ifdef Q_OS_WIN
    QString siblingExe = appDir + QStringLiteral("/sentinel-daemon.exe");
    if (QFileInfo::exists(siblingExe)) {
        return siblingExe;
    }
#endif
    QString buildSibling = QDir(appDir).absoluteFilePath(
#ifdef Q_OS_MACOS
        QStringLiteral("../../../../sentinel-daemon/sentinel-daemon")
#else
        QStringLiteral("../sentinel-daemon/sentinel-daemon")
#endif
    );
    if (QFileInfo(buildSibling).isExecutable()) {
        return buildSibling;
    }
    QString byPath = QStandardPaths::findExecutable(QStringLiteral("sentinel-daemon"));
    if (!byPath.isEmpty()) {
        return byPath;
    }
    return {};
}

} // namespace

void ApplicationBootstrapper::ensureBackgroundDaemon() {
    if (m_safeMode || m_daemonProcess) {
        qInfo().noquote() << "Background daemon launch already attempted or disabled.";
        return;
    }

    const QString daemonBinary = discoverDaemonBinary();
    if (daemonBinary.isEmpty()) {
        qInfo().noquote() << "sentinel-daemon binary not found; skipping background launch.";
        return;
    }

    m_daemonProcess = std::make_unique<QProcess>(this);
    QStringList arguments;
    const auto endpoint = m_parser.value(QStringLiteral("daemon-socket"));
    if (!endpoint.isEmpty()) {
        arguments << QStringLiteral("--socket") << endpoint;
    }
#ifdef Q_OS_MACOS
    // The helper lives in Contents/Helpers, so Qt's bundle-relative plugin
    // search does not find Contents/PlugIns on its own.
    const QString plugins =
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../PlugIns"));
    if (QFileInfo(plugins).isDir()) {
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("QT_PLUGIN_PATH"), plugins);
        m_daemonProcess->setProcessEnvironment(environment);
    }
#endif
    m_daemonProcess->setProgram(daemonBinary);
    m_daemonProcess->setArguments(arguments);
    // The background service outlives the UI and is protected by its own socket lock.
    if (!m_daemonProcess->startDetached()) {
        qWarning() << "Cannot start sentinel-daemon";
    }
}

ApplicationBootstrapper::ApplicationBootstrapper(int argc, char* argv[], QObject* parent)
    : QObject(parent), m_argc(argc), m_argv(argv) {
    QGuiApplication::setApplicationName(sentinel::core::AppMetadata::displayName());
    QGuiApplication::setOrganizationName(sentinel::core::AppMetadata::organizationName());
    QGuiApplication::setApplicationVersion(sentinel::core::AppMetadata::version());
    QGuiApplication::setDesktopFileName(sentinel::core::AppMetadata::appId());
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/dev.sentinel.Sentinel.png")));
    QGuiApplication::setDesktopFileName(QStringLiteral("dev.sentinel.Sentinel"));

    // The display name doubles as the Qt application name, so renaming the
    // product also renames every per-application storage root. Move pre-rename
    // data across before anything reads a standard path.
    sentinel::core::StorageMigration::migrateLegacyApplicationStorage(
        sentinel::core::AppMetadata::legacyDisplayName(),
        sentinel::core::AppMetadata::displayName());

    m_parser.setApplicationDescription(QStringLiteral("Sentinel AI Desktop Assistant"));
    m_parser.addHelpOption();
    m_parser.addVersionOption();

    QCommandLineOption verboseOption(
        {QStringLiteral("verbose")},
        QCoreApplication::translate("main", "Enable verbose diagnostic logging in terminal."));
    m_parser.addOption(verboseOption);

    QCommandLineOption quietOption(
        {QStringLiteral("quiet")},
        QCoreApplication::translate("main", "Suppress diagnostic terminal output."));
    m_parser.addOption(quietOption);

    QCommandLineOption safeModeOption(
        {QStringLiteral("safe-mode")},
        QCoreApplication::translate("main",
                                    "Start with all extensions disabled and factory defaults."));
    m_parser.addOption(safeModeOption);

    m_parser.addOption(QCommandLineOption(
        QStringLiteral("preferences-directory"),
        QStringLiteral("Store UI preferences in an explicit directory."), QStringLiteral("path")));
    m_parser.addOption(QCommandLineOption(QStringLiteral("daemon-socket"),
                                          QStringLiteral("Connect to a specific daemon socket."),
                                          QStringLiteral("path")));
    m_parser.addOption(
        QCommandLineOption(QStringLiteral("no-daemon-start"),
                           QStringLiteral("Connect without launching a background daemon.")));
    if (QCoreApplication::instance()) {
        m_parser.process(*QCoreApplication::instance());
        m_verbose = m_parser.isSet(verboseOption);
        m_quiet = m_parser.isSet(quietOption);
        m_safeMode = m_parser.isSet(safeModeOption);
        if (m_parser.isSet(QStringLiteral("preferences-directory"))) {
            m_pathProvider.setProfileDirectory(
                m_parser.value(QStringLiteral("preferences-directory")));
        }
    }
}

ApplicationBootstrapper::~ApplicationBootstrapper() = default;

bool ApplicationBootstrapper::ensureSingleInstance() {
    return m_singleInstanceGuard.tryLockAndSetupIpc();
}

void ApplicationBootstrapper::initializeLogging() {
    configureLogging(m_verbose, m_quiet, m_pathProvider.logDirectoryPath());
}

void ApplicationBootstrapper::initializeGraphics() {
    configureGraphicsBackend();
}

void ApplicationBootstrapper::initializePlatformIntegrations() {
    sentinel::desktop::initializePlatformIntegrations(m_pathProvider.crashDumpDirectoryPath());

    if (m_safeMode) {
        qInfo().noquote() << "Safe mode enabled: using factory defaults.";
    }

    configureDefaultUiFont();
}

bool ApplicationBootstrapper::setupQmlEngine(QApplication& app) {
    app.setQuitOnLastWindowClosed(false);
    m_daemonClient =
        std::make_unique<DaemonClient>(m_parser.value(QStringLiteral("daemon-socket")).isEmpty()
                                           ? DaemonClient::defaultSocketPath()
                                           : m_parser.value(QStringLiteral("daemon-socket")),
                                       10000, 1000, this);
    m_runtimeClient = std::make_unique<DesktopRuntimeClient>(*m_daemonClient, this);
    auto desktopStore = std::make_unique<DesktopSettingsStore>(*m_runtimeClient,
        m_pathProvider.settingsFilePath() + QStringLiteral(".desktop"));
    auto* wardenPreferences = new CoreWardenPreferences(*desktopStore, this);
    qml_register_types_Sentinel_Warden();
    m_engine.rootContext()->setContextProperty("coreWardenPreferences", wardenPreferences);
    m_settings = std::make_unique<sentinel::core::AppSettings>(
        std::move(desktopStore),
        sentinel::core::CredentialStore(std::shared_ptr<sentinel::core::ICredentialBackend>{}));
    const auto settingsError = m_settings->storageErrorCode();
    const bool settingsRecovered = settingsError == QLatin1String("CorruptState") ||
                                   settingsError == QLatin1String("UnsupportedSettingsVersion") ||
                                   settingsError == QLatin1String("StoreUnavailable");
    if (settingsRecovered) {
        sentinel::core::RecoveryService::recordCondition(QStringLiteral("settings"),
                                                         QStringLiteral("settings"), settingsError,
                                                         QStringLiteral("repair-settings"));
    } else if (settingsError.isEmpty()) {
        sentinel::core::RecoveryService::clearCondition(QStringLiteral("settings"),
                                                        QStringLiteral("settings"));
    }

    installStartupTranslator(app, *m_settings, m_translator);

    QObject::connect(m_settings.get(), &sentinel::core::AppSettings::appLanguageChanged, &app,
                     [this, &app]() {
                         const auto lang = effectiveLanguageCode(*m_settings);
                         installTranslator(app, m_translator, lang);
                         m_engine.retranslate();
                     });

    // Runtime authority is exclusively provided by the daemon.
    m_runtimeClient->setPreferredSession(m_settings->activeConversationId());
    QObject::connect(m_runtimeClient.get(), &DesktopRuntimeClient::sessionChanged, m_settings.get(),
                     [this](const QString& id) { m_settings->setActiveConversationId(id); });
    auto inspector = std::make_unique<RemoteAgentInspectorService>(*m_daemonClient);
    auto* remoteInspector = inspector.get();
    m_inspectorService = std::move(inspector);
    m_inspectorViewModel = std::make_unique<AgentInspectorViewModel>(*m_inspectorService);
    connect(remoteInspector, &RemoteAgentInspectorService::updated, m_inspectorViewModel.get(),
            [this, remoteInspector] {
                m_inspectorViewModel->refreshFromRemote(remoteInspector->recentRuns(500),
                                                        remoteInspector->hasMore());
            });
    m_modeManager = std::make_unique<sentinel::core::ModeManager>();
    m_taskbarIntegration = std::make_unique<sentinel::core::WinTaskbarIntegration>();
    m_shellViewModel = std::make_unique<DesktopShellViewModel>(
        *m_runtimeClient, *m_modeManager, *m_settings, m_taskbarIntegration.get());
    if (settingsRecovered)
        m_shellViewModel->reportStartupRecovery(settingsError);

    m_singleInstanceGuard.bindShellViewModel(m_shellViewModel.get());

    const QString ownUrl = sentinel::core::extractSentinelUrl(QCoreApplication::arguments());
    if (!ownUrl.isEmpty()) {
        qInfo().noquote() << "Deep link from command line:" << ownUrl;
    }

    auto* ollamaPuller =
        new DesktopModelHelper(*m_daemonClient, QStringLiteral("ollamaPuller"), this);
    auto* ollamaLibraryFetcher =
        new DesktopModelHelper(*m_daemonClient, QStringLiteral("ollamaLibraryFetcher"), this);
    auto* ollamaModelDetailFetcher =
        new DesktopModelHelper(*m_daemonClient, QStringLiteral("ollamaModelDetailFetcher"), this);
    auto* ggufLibraryFetcher =
        new DesktopModelHelper(*m_daemonClient, QStringLiteral("ggufLibraryFetcher"), this);
    auto* lmStudioLibraryFetcher =
        new DesktopModelHelper(*m_daemonClient, QStringLiteral("lmStudioLibraryFetcher"), this);

    QObject::connect(m_daemonClient.get(), &DaemonClient::connectionStateChanged, this, [this] {
        if (m_daemonClient->connectionState() == DaemonClient::ConnectionState::Unavailable &&
            !m_parser.isSet(QStringLiteral("no-daemon-start"))) {
            ensureBackgroundDaemon();
        }
    });
    auto* daemonClient = m_daemonClient.get();

    auto* quickPanel = new QuickPanelController(*m_runtimeClient, this);
    auto* native = new NativeCompanionAdapter(*m_shellViewModel, *m_settings, nullptr, this);
    native->bindQuickPanel(quickPanel);
    connect(&m_singleInstanceGuard, &SingleInstanceGuard::deepLinkReceived, quickPanel,
            &QuickPanelController::openLink);
    m_engine.rootContext()->setContextProperty(QStringLiteral("nativeDesktop"), native);
    m_engine.rootContext()->setContextProperty(QStringLiteral("quickPanelController"), quickPanel);
    m_engine.rootContext()->setContextProperty(QStringLiteral("shellViewModel"),
                                               m_shellViewModel.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("agentInspectorViewModel"),
                                               m_inspectorViewModel.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("ollamaPuller"), ollamaPuller);
    m_engine.rootContext()->setContextProperty(QStringLiteral("ollamaLibraryFetcher"),
                                               ollamaLibraryFetcher);
    m_engine.rootContext()->setContextProperty(QStringLiteral("ollamaModelDetailFetcher"),
                                               ollamaModelDetailFetcher);
    m_engine.rootContext()->setContextProperty(QStringLiteral("ggufLibraryFetcher"),
                                               ggufLibraryFetcher);
    m_engine.rootContext()->setContextProperty(QStringLiteral("lmStudioLibraryFetcher"),
                                               lmStudioLibraryFetcher);
    m_engine.rootContext()->setContextProperty(QStringLiteral("daemonClient"), daemonClient);

    QObject::connect(
        &m_engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    m_engine.loadFromModule(QStringLiteral("Sentinel.Desktop"), QStringLiteral("Main"));

    QObject* rootWindow =
        m_engine.rootObjects().isEmpty() ? nullptr : m_engine.rootObjects().first();
    if (auto* quickWindow = qobject_cast<QQuickWindow*>(rootWindow)) {
        installGraphicsDiagnostics(*quickWindow);
    } else {
        qWarning()
            << "Sentinel graphics diagnostics unavailable: root object is not a QQuickWindow";
    }

    native->setWindow(rootWindow);
    if (auto* window = qobject_cast<QQuickWindow*>(rootWindow)) {
        m_shellViewModel->registerMainWindow(window->winId());
    }

    // Local, non-sensitive window state, isolated per profile so tests and portable installs
    // do not touch a developer's real preferences.
    m_windowSettings = std::make_unique<QSettings>(
        m_pathProvider.settingsFilePath() + QStringLiteral(".window"), QSettings::IniFormat);
    m_windowState = std::make_unique<WindowStateController>(*m_windowSettings, this);
    if (auto* window = qobject_cast<QWindow*>(rootWindow))
        m_windowState->attach(window);
    if (!ownUrl.isEmpty()) {
        quickPanel->openLink(ownUrl);
    }

    return true;
}

} // namespace sentinel::desktop
