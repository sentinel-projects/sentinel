void qml_register_types_Sentinel_Warden();
#include "sentinel/desktop/CoreWardenItem.h"
// SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in UI harness: production QML and viewmodel, disposable disk stores,
// explicitly injected in-memory credential backend. No provider success is faked.
#include "DesktopDaemonFixture.h"
#include "DesktopDaemonInteraction.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/ApplicationControllerBuilder.h"
#include "sentinel/core/app/ModeManager.h"
#include "sentinel/core/chat/SQLiteConversationStore.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include "sentinel/desktop/DesktopRuntimeClient.h"
#include "sentinel/desktop/DesktopShellViewModel.h"
#include "sentinel/desktop/NativeCompanionAdapter.h"
#include "sentinel/desktop/QuickPanelController.h"
#include "sentinel/desktop/RemoteAgentInspectorService.h"
#include "sentinel/desktop/viewmodels/AgentInspectorViewModel.h"
#include <QAccessible>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTranslator>
#include <QUuid>
#include <cmath>
#include <cstdio>

// Deterministic catalog data only; the harness does not simulate provider success.
class CatalogUiFixture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList models MEMBER models NOTIFY changed)
    Q_PROPERTY(QVariantList catalog MEMBER catalog NOTIFY changed)
    Q_PROPERTY(QVariantList tags MEMBER tags NOTIFY changed)
    Q_PROPERTY(bool fetching MEMBER fetching NOTIFY changed)
    Q_PROPERTY(bool pulling MEMBER pulling NOTIFY changed)
    Q_PROPERTY(bool daemonReachable MEMBER daemonReachable NOTIFY changed)
    Q_PROPERTY(QString errorText MEMBER errorText NOTIFY changed)
    Q_PROPERTY(QString statusText MEMBER statusText NOTIFY changed)
    Q_PROPERTY(QString activeModel MEMBER activeModel NOTIFY changed)
    Q_PROPERTY(QString readme MEMBER readme NOTIFY changed)
    Q_PROPERTY(QString query MEMBER query NOTIFY changed)
    Q_PROPERTY(int installedCount MEMBER installedCount NOTIFY changed)
    Q_PROPERTY(bool setupBusy MEMBER setupBusy NOTIFY changed)
    Q_PROPERTY(bool runtimeInstalled MEMBER runtimeInstalled NOTIFY changed)
    Q_PROPERTY(QString setupStatus MEMBER setupStatus NOTIFY changed)
    Q_PROPERTY(QString catalogStatus MEMBER catalogStatus NOTIFY changed)
    Q_PROPERTY(bool hasMore MEMBER hasMore NOTIFY changed)
    Q_PROPERTY(bool hasPrevious MEMBER hasPrevious NOTIFY changed)
    Q_PROPERTY(double progress MEMBER progress NOTIFY changed)
public:
    QVariantList models, catalog, tags;
    bool fetching = false, pulling = false, daemonReachable = false, hasMore = false, hasPrevious = false;
    QString errorText, statusText, activeModel, readme, catalogStatus, query, setupStatus;
    int installedCount = 0;
    bool setupBusy = false, runtimeInstalled = true;
    double progress = 0;
    Q_INVOKABLE void fetch(const QString& = {}) {}
    Q_INVOKABLE void refreshCatalog(const QString&) {}
    Q_INVOKABLE void fetchDetails(const QString&) {}
    Q_INVOKABLE void cancel() {}
    void notify() { emit changed(); }
signals:
    void changed();
};

QVariant qmlVariant(const QVariant& value) {
    return value.metaType() == QMetaType::fromType<QJSValue>() ? value.value<QJSValue>().toVariant() : value;
}

namespace {
// Test-only stage output is flushed immediately. CTest's process-level
// timeout can report the last operation even if Qt's event loop blocks.
class Phase1Diagnostic {
public:
    explicit Phase1Diagnostic(bool enabled) : enabled_(enabled) {}
    void stage(const char* name) const {
        if (!enabled_) {
            return;
        }
        std::fprintf(stderr, "PHASE1 STAGE: %s\n", name);
        std::fflush(stderr);
    }

private:
    bool enabled_;
};
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("Sentinel Settings Certification");
    QStandardPaths::setTestModeEnabled(true);
    const bool daemonE2e = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--daemon-e2e";
#ifndef Q_OS_UNIX
    if (daemonE2e) {
        qInfo("P606_BLOCKED: production daemon peer authentication requires Unix");
        return 77;
    }
#endif
    if (daemonE2e)
        QCoreApplication::setApplicationName("sentinel-p606-" +
                                             QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (daemonE2e) {
        const auto fonts = QFontDatabase::families();
        for (const auto& family : {QString("DejaVu Sans"), QString("Arial"), QString("Noto Sans"),
                                   QString("Liberation Sans")}) {
            if (fonts.contains(family)) {
                QApplication::setFont(QFont(family, 14));
                break;
            }
        }
    }
    const bool shellSmoke = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--shell-smoke";
    const bool extensionsSmoke = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--extensions-smoke";
    const bool phase1FailureSmoke =
        argc == 2 && QString::fromLocal8Bit(argv[1]) == "--phase1-failure-smoke";
    const bool phase1Smoke =
        phase1FailureSmoke || (argc == 2 && QString::fromLocal8Bit(argv[1]) == "--phase1-smoke");
    const bool traySmoke = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--tray-smoke";
    const bool focusSmoke = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--focus-smoke";
    const bool navigationSmoke = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--navigation-smoke";
    const bool chatSmoke = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--chat-smoke";
    const bool settingsSmoke = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--settings-smoke";
    const bool modelsSmoke = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--models-smoke";
    const bool accessibilitySmoke =
        argc == 2 && QString::fromLocal8Bit(argv[1]) == "--accessibility-smoke";
    Phase1Diagnostic diagnostic(phase1Smoke);
    QTemporaryDir temporaryProfile;
    if (argc != 2 ||
        (!daemonE2e && !shellSmoke && !extensionsSmoke && !phase1Smoke && !focusSmoke &&
         !navigationSmoke && !chatSmoke && !modelsSmoke && !settingsSmoke && !traySmoke &&
         !accessibilitySmoke && !QDir(QString::fromLocal8Bit(argv[1])).exists()))
        return 2;
    QDir profile(daemonE2e || shellSmoke || extensionsSmoke || phase1Smoke || focusSmoke ||
                         navigationSmoke || chatSmoke || modelsSmoke || settingsSmoke ||
                         traySmoke || accessibilitySmoke
                     ? temporaryProfile.path()
                     : QString::fromLocal8Bit(argv[1]));
    sentinel::core::AppSettings settings(
        std::make_unique<sentinel::core::JsonSettingsStore>(profile.filePath("settings.json")),
        sentinel::core::inMemoryTestCredentialStore());
    sentinel::core::ApplicationControllerBuilder builder;
    builder.withModelService(std::make_unique<sentinel::core::ModelService>(&settings))
        .withMemoryStore(
            std::make_unique<sentinel::core::SQLiteMemoryStore>(profile.filePath("memory.sqlite3")))
        .withConversationStore(std::make_unique<sentinel::core::SQLiteConversationStore>(
            profile.filePath("chat.sqlite3")));
    std::shared_ptr<sentinel::test::DesktopScript> daemonScript;
    if (daemonE2e) {
        daemonScript = sentinel::test::configureDesktopDaemon(builder, settings, profile);
        if (!daemonScript)
            return 247;
    }
    diagnostic.stage("controller-build");
    auto controller = builder.build();
    diagnostic.stage("controller-ready");
    std::unique_ptr<sentinel::daemon::DaemonIpcServer> fixtureServer;
    if (daemonE2e) {
        controller->attachControlledTaskSettings(settings);
        controller->setAgentAutonomousMode(false);
        fixtureServer = std::make_unique<sentinel::daemon::DaemonIpcServer>(controller.get());
        fixtureServer->setSettings(&settings);
        if (!fixtureServer->openSessionProjectionStore(profile.filePath("projection.sqlite3")))
            return 248;
    }
    sentinel::core::ModeManager modes;
    sentinel::core::JsonSettingsStore presentationStore(profile.filePath("warden-harness.json"));
    sentinel::desktop::CoreWardenPreferences wardenPreferences(presentationStore);
    qml_register_types_Sentinel_Warden();
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("coreWardenPreferences", &wardenPreferences);
    engine.rootContext()->setContextProperty("navigationSmoke", navigationSmoke);
    QObject::connect(&engine, &QQmlApplicationEngine::exit, &app, &QApplication::exit);
    if (shellSmoke || phase1Smoke || navigationSmoke || chatSmoke || modelsSmoke || settingsSmoke ||
        traySmoke)
        settings.setOnboardingComplete(true);
    engine.addImportPath(QStringLiteral(CERTIFICATION_QML_IMPORT));
    QTranslator translator;
    auto translate = [&] {
        app.removeTranslator(&translator);
        if (translator.load(QStringLiteral(CERTIFICATION_TRANSLATIONS) + "/sentinel_" +
                            settings.appLanguage() + ".qm"))
            app.installTranslator(&translator);
        engine.retranslate();
    };
    translate();
    QObject::connect(&settings, &sentinel::core::AppSettings::appLanguageChanged, &app, translate);
    // Match production bootstrap: translator changes precede viewmodel language signals.
    sentinel::desktop::DaemonClient transport(profile.filePath("absent.sock"), 500, 10000);
    sentinel::desktop::DesktopRuntimeClient remoteRuntime(transport);
    std::unique_ptr<sentinel::desktop::DesktopShellViewModel> shell =
        (shellSmoke || daemonE2e)
            ? std::make_unique<sentinel::desktop::DesktopShellViewModel>(remoteRuntime, modes,
                                                                         settings)
            : std::make_unique<sentinel::desktop::DesktopShellViewModel>(*controller, modes,
                                                                         settings);
    auto& viewModel = *shell;
    sentinel::desktop::QuickPanelController quickPanel(remoteRuntime);
    sentinel::desktop::RemoteAgentInspectorService inspectorService(transport);
    sentinel::desktop::AgentInspectorViewModel inspector(inspectorService);
    engine.rootContext()->setContextProperty("agentInspectorViewModel", &inspector);
    engine.rootContext()->setContextProperty("quickPanelController", &quickPanel);
    if (shellSmoke || daemonE2e)
        viewModel.setReducedMotionEnabled(true);
    sentinel::desktop::NativeCompanionAdapter native(viewModel, settings, nullptr);
    engine.rootContext()->setContextProperty("nativeDesktop", &native);
    engine.rootContext()->setContextProperty("shellViewModel", &viewModel);
    if (qEnvironmentVariableIsSet("SENTINEL_UI_NOTIFICATION_FIXTURE")) {
        viewModel.setDndEnabled(true); // Synthetic history only; do not request an OS banner.
        viewModel.addNotificationWithPriority("Security", "Disposable test notice", "Synthetic content for notification UI verification.", "High");
    }
    CatalogUiFixture library, emptyLibrary;
    library.models = {
        QVariantMap{{"id", "qwen-small"}, {"name", "Qwen 2.5 7B"}, {"ollamaId", "qwen2.5:7b"}, {"category", "LLM"}, {"provider", "Ollama"}, {"description", "Small variant"}, {"badge", "Open"}, {"badgeColor", "#4f8ef7"}},
        QVariantMap{{"id", "qwen-large"}, {"name", "Qwen 2.5 14B"}, {"ollamaId", "qwen2.5:14b"}, {"category", "LLM"}, {"provider", "Ollama"}, {"description", "Large variant"}, {"badge", "Open"}, {"badgeColor", "#4f8ef7"}}
    };
    engine.rootContext()->setContextProperty("ollamaLibraryFetcher", &library);
    for (const auto* name : {"ggufLibraryFetcher", "lmStudioLibraryFetcher", "ollamaModelDetailFetcher", "ollamaPuller", "daemonClient"})
        engine.rootContext()->setContextProperty(name, &emptyLibrary);
    diagnostic.stage("qml-engine-load");
    if (shellSmoke || daemonE2e) {
        engine.rootContext()->setContextProperty("daemonClient", &transport);
        engine.loadFromModule("Sentinel.Desktop", "Main");
    } else
        engine.load(QUrl::fromLocalFile(
            extensionsSmoke ? QFileInfo(QStringLiteral(CERTIFICATION_QML_WRAPPER))
                                  .dir()
                                  .filePath("extensions_hub_certification.qml")
            : accessibilitySmoke ? QFileInfo(QStringLiteral(CERTIFICATION_QML_WRAPPER))
                                       .dir()
                                       .filePath("accessibility_certification.qml")
                                 : QStringLiteral(CERTIFICATION_QML_WRAPPER)));
    diagnostic.stage("qml-engine-loaded");
    if (engine.rootObjects().isEmpty())
        return 3;
    if (daemonE2e) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QTimer::singleShot(250, &app, [&, window] {
            const auto result = sentinel::test::runDesktopDaemonInteraction(
                window, viewModel, remoteRuntime, transport, inspector, *fixtureServer, *controller,
                daemonScript, profile);
            app.exit(result);
        });
    }
    if (shellSmoke) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QTimer::singleShot(250, &app, [&, window] {
            auto find = [](auto&& self, QQuickItem* item, const QString& name) -> QQuickItem* {
                if (item->objectName() == name)
                    return item;
                for (auto* child : item->childItems())
                    if (auto* result = self(self, child, name))
                        return result;
                return nullptr;
            };
            if (!window || window->property("currentShellPage").toString() != "Dashboard" ||
                viewModel.daemonConnected()) {
                app.exit(240);
                return;
            }
            window->requestActivate();
            for (const auto& page :
                 {QString("Models"), QString("Inspector"), QString("Dashboard")}) {
                auto* button = find(find, window->contentItem(), page + "NavigationButton");
                if (!button) {
                    app.exit(241);
                    return;
                }
                button->forceActiveFocus();
                QTest::keyClick(window, Qt::Key_Space);
                if (!QTest::qWaitFor(
                        [&] { return window->property("currentShellPage").toString() == page; },
                        2000)) {
                    app.exit(242);
                    return;
                }
                if (page == "Inspector" &&
                    (!inspector.active() || inspector.runs()->rowCount() != 0)) {
                    app.exit(243);
                    return;
                }
            }
            // Public shell shortcuts at compact and wide supported window sizes.
            for (const auto width : {780, 1320}) {
                window->setWidth(width);
                window->setHeight(720);
                QTest::keyClick(window, Qt::Key_2, Qt::ControlModifier);
                if (!QTest::qWaitFor(
                        [&] { return window->property("currentShellPage").toString() == "Models"; },
                        2000)) {
                    app.exit(244);
                    return;
                }
                QTest::keyClick(window, Qt::Key_1, Qt::ControlModifier);
                if (!QTest::qWaitFor(
                        [&] {
                            return window->property("currentShellPage").toString() == "Dashboard";
                        },
                        2000)) {
                    app.exit(245);
                    return;
                }
                if (viewModel.daemonConnected() ||
                    remoteRuntime.quickPanelSnapshot().value("ready").toBool()) {
                    app.exit(246);
                    return;
                }
            }
            qInfo("P6-05 production Main launch/navigation/Inspector/disconnected readiness "
                  "passed; no daemon started");
            app.exit(0);
        });
    }
    if (extensionsSmoke) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QTimer::singleShot(200, &app, [&, window] {
            auto find = [](auto&& self, QQuickItem* item, const QString& name) -> QQuickItem* {
                if (item->objectName() == name) return item;
                for (auto* child : item->childItems()) if (auto* result = self(self, child, name)) return result;
                return nullptr;
            };
            auto check = [](bool ok, const char* message) { if (!ok) qFatal("Extensions acceptance: %s", message); };
            auto* hub = find(find, window->contentItem(), "extensionsHubPage");
            auto* fixture = window->findChild<QObject*>("extensionsFixture");
            auto* search = find(find, hub, "extensionsSearch");
            auto* refresh = find(find, hub, "extensionsRefresh");
            check(hub && fixture && search && refresh, "mounted page");
            check(!search->isEnabled() && !refresh->isEnabled(), "disconnected actions disabled");
            auto* tools = find(find, window->contentItem(), "ToolsNavigationButton");
            auto* extensions = find(find, window->contentItem(), "ExtensionsNavigationButton");
            auto* settingsNav = find(find, window->contentItem(), "settingsNavigationButton");
            check(tools && extensions && settingsNav, "rail destinations");
            check(tools->y() < extensions->y() && extensions->mapToScene(QPointF()).y() < settingsNav->mapToScene(QPointF()).y(), "Tools Extensions Settings order");
            tools->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_Space);
            QTest::qWait(30);
            check(hub->property("section").toString() == "tools" && window->property("currentPage").toString() == "Tools", "keyboard Tools route");
            extensions->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_Space);
            QTest::qWait(30);
            check(hub->property("section").toString() == "plugins", "keyboard Extensions route");
            QVariantList entries{QVariantMap{{"id", "plugin:fixture"}, {"name", "Synthetic plugin metadata"}, {"type", "plugins"}, {"enabledKnown", true}, {"enabled", false}},
                QVariantMap{{"id", "mcp:fixture"}, {"name", "Synthetic configured server"}, {"type", "mcp"}, {"enabledKnown", true}, {"enabled", true}},
                QVariantMap{{"id", "skill:fixture"}, {"name", "Synthetic discovered skill"}, {"type", "skills"}, {"enabledKnown", false}}};
            QVariantMap data{{"phase", "ready"}, {"message", "Synthetic UI fixture — no plugin, MCP server, skill or tool executed."}, {"entries", entries}, {"tools", QStringList{"fixture.read", "fixture.refused"}}, {"policy", QStringList{"Synthetic policy: execution permission not granted"}}};
            fixture->setProperty("daemonConnected", true);
            fixture->setProperty("data", data);
            QTest::qWait(40);
            check(search->isEnabled() && refresh->isEnabled(), "metadata actions available");
            for (const auto& key : {"plugins", "mcp", "skills", "tools"}) {
                auto* button = find(find, hub, "extensionsSection_" + QString(key));
                check(button, "section button");
                button->forceActiveFocus();
                QTest::keyClick(window, Qt::Key_Space);
                QTest::qWait(30);
                check(hub->property("section").toString() == key && window->activeFocusItem()->isVisible(), "section keyboard/focus");
                const auto evidence = qEnvironmentVariable("SENTINEL_EXTENSIONS_EVIDENCE_DIR");
                if (!evidence.isEmpty()) {
                    QDir().mkpath(evidence);
                    for (const auto& theme : {QStringLiteral("Paper"), QStringLiteral("Liquid Glass Dark")}) {
                        viewModel.setThemeName(theme);
                        QTest::qWait(80);
                        window->grabWindow().save(evidence + "/" + key + (theme == "Paper" ? "-light.png" : "-dark.png"));
                    }
                }
            }
            // Synthetic lifecycle management: no process or trust mutation is performed.
            auto pluginRow = entries.first().toMap();
            pluginRow["enabled"] = true;
            pluginRow["health"] = "Failed";
            pluginRow["maturity"] = "Experimental";
            pluginRow["hostRunning"] = false;
            pluginRow["actions"] = QStringList{"disable"};
            pluginRow["actionScope"] = "daemon-lifetime";
            entries[0] = pluginRow;
            data["entries"] = entries;
            fixture->setProperty("data", data);
            hub->setProperty("section", "plugins");
            QMetaObject::invokeMethod(hub, "reload");
            QCoreApplication::processEvents();
            auto* disable = find(find, hub, "extensionAction_disable");
            check(disable && disable->isEnabled(), "backend-supported deactivation exposed");
            disable->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_Space);
            QCoreApplication::processEvents();
            check(fixture->property("actionCalls").toInt() == 1, "one management request");
            check(!disable->isEnabled() && !refresh->isEnabled(), "pending action prevents duplicate and refresh");
            auto* actionStatus = find(find, hub, "extensionsActionStatus");
            check(actionStatus && actionStatus->isVisible(), "pending status visible");
            const auto managementEvidence = qEnvironmentVariable("SENTINEL_EXTENSIONS_EVIDENCE_DIR");
            if (!managementEvidence.isEmpty()) {
                window->grabWindow().save(managementEvidence + "/management-pending.png");
            }
            fixture->setProperty("actionData", QVariantMap{{"phase", "unconfirmed"}});
            QCoreApplication::processEvents();
            check(actionStatus->property("text").toString().contains("could not be confirmed"), "timeout is not success");
            if (!managementEvidence.isEmpty()) {
                window->grabWindow().save(managementEvidence + "/management-unconfirmed.png");
            }
            fixture->setProperty("actionData", QVariantMap{{"phase", "accepted"}});
            QMetaObject::invokeMethod(hub, "openSection", Q_ARG(QVariant, QVariant("tools")));
            QCoreApplication::processEvents();
            hub->setProperty("query", "no-match");
            QTest::qWait(20);
            auto* empty = find(find, hub, "extensionsEmpty");
            check(empty && empty->isVisible(), "search empty state");
            hub->setProperty("query", "fixture.read");
            QTest::qWait(20);
            check(!empty->isVisible(), "local tool identity filter");
            for (const auto& phase : {"loading", "error", "unavailable", "disconnected"}) {
                data["phase"] = phase;
                data["entries"] = QVariantList{};
                data["tools"] = QStringList{};
                fixture->setProperty("data", data);
                QTest::qWait(20);
                check(!search->isEnabled(), "non-ready search disabled");
                const auto evidence = qEnvironmentVariable("SENTINEL_EXTENSIONS_EVIDENCE_DIR");
                if (!evidence.isEmpty())
                    window->grabWindow().save(evidence + "/state-" + phase + ".png");
            }
            fixture->setProperty("daemonConnected", false);
            QTest::qWait(20);
            check(!refresh->isEnabled(), "disconnect disables refresh");
            window->resize(780, 640);
            QTest::qWait(40);
            check(search->width() > 100 && refresh->mapToScene(QPointF()).x() + refresh->width() <= window->width(), "compact layout fits");
            const auto evidence = qEnvironmentVariable("SENTINEL_EXTENSIONS_EVIDENCE_DIR");
            if (!evidence.isEmpty())
                window->grabWindow().save(evidence + "/compact-disconnected.png");
            QTest::keyClick(window, Qt::Key_Tab);
            check(window->activeFocusItem() && window->activeFocusItem()->isVisible(),
                  "Tab remains visible");
            qInfo("Extensions routing, rail order, four sections, filter, loading/error/disconnected/unsupported and keyboard/compact checks passed; synthetic data only");
            app.exit(0);
        });
    }
    if (accessibilitySmoke) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QTimer::singleShot(250, &app, [&, window] {
            auto itemOf = [&](const char* name) {
                return window->findChild<QQuickItem*>(QString::fromLatin1(name));
            };
            auto ifaceOf = [&](const char* name) -> QAccessibleInterface* {
                auto* item = itemOf(name);
                return item ? QAccessible::queryAccessibleInterface(item) : nullptr;
            };
            auto nameOf = [&](const char* name) {
                auto* iface = ifaceOf(name);
                return iface ? iface->text(QAccessible::Name) : QString();
            };
            auto roleOf = [&](const char* name) {
                auto* iface = ifaceOf(name);
                return iface ? int(iface->role()) : -1;
            };
            auto fail = [&](int code, const char* stage) {
                qWarning() << "Accessibility acceptance failed:" << stage << "exit" << code;
                app.exit(code);
            };
            // Popups finish closing only once the exit transition has run; poll
            // instead of guessing a sleep so the check stays deterministic.
            auto waitHidden = [&](QObject* popup) {
                for (int i = 0; i < 100 && popup->property("visible").toBool(); ++i)
                    QTest::qWait(20);
                return !popup->property("visible").toBool();
            };

            // Part A — base control accessibility contracts (roles/names/descriptions).
            if (roleOf("a11yButton") != int(QAccessible::Button) ||
                nameOf("a11yButton") != QStringLiteral("Save changes")) {
                fail(201, "button role/name");
                return;
            }
            if (!ifaceOf("a11yButton") || ifaceOf("a11yButton")->text(QAccessible::Description) !=
                                              QStringLiteral("Persist the current settings")) {
                fail(202, "button description");
                return;
            }
            if (roleOf("a11yIconButton") != int(QAccessible::Button) ||
                nameOf("a11yIconButton") != QStringLiteral("Confirm action")) {
                fail(203, "icon-only button name");
                return;
            }
            auto* disabledButton = itemOf("a11yDisabledButton");
            if (!disabledButton || disabledButton->isEnabled() ||
                roleOf("a11yDisabledButton") != int(QAccessible::Button)) {
                fail(204, "disabled semantics");
                return;
            }
            if (roleOf("a11yTextField") != int(QAccessible::EditableText) ||
                nameOf("a11yTextField") != QStringLiteral("Search models")) {
                fail(205, "text field role/name");
                return;
            }
            if (!ifaceOf("a11yTextField") ||
                ifaceOf("a11yTextField")->text(QAccessible::Description) !=
                    QStringLiteral("Filters the locally known model catalog")) {
                fail(206, "text field description");
                return;
            }
            if (roleOf("a11yComboBox") != int(QAccessible::ComboBox) ||
                nameOf("a11yComboBox") != QStringLiteral("Provider")) {
                fail(207, "combo role/name");
                return;
            }
            if (roleOf("a11ySwitch") != int(QAccessible::Switch) ||
                nameOf("a11ySwitch") != QStringLiteral("Enable notifications")) {
                fail(208, "switch role/name");
                return;
            }
            if (roleOf("a11ySlider") != int(QAccessible::Slider) ||
                nameOf("a11ySlider") != QStringLiteral("Volume")) {
                fail(209, "slider role/name");
                return;
            }
            if (roleOf("a11ySpinBox") != int(QAccessible::SpinBox) ||
                nameOf("a11ySpinBox") != QStringLiteral("Retries")) {
                fail(210, "spin box role/name");
                return;
            }
            if (itemOf("a11yButton")->focusPolicy() != Qt::StrongFocus ||
                itemOf("a11ySwitch")->focusPolicy() != Qt::StrongFocus) {
                fail(211, "focus policy");
                return;
            }

            // Keyboard activation and focus indicators.
            auto* button = itemOf("a11yButton");
            button->forceActiveFocus(Qt::TabFocusReason);
            if (!button->hasActiveFocus()) {
                fail(212, "button focus");
                return;
            }
            QTest::keyClick(window, Qt::Key_Space);
            QTest::qWait(20);
            if (window->property("buttonClicks").toInt() != 1) {
                fail(213, "button space activation");
                return;
            }
            QTest::keyClick(window, Qt::Key_Return);
            QTest::qWait(20);
            if (window->property("buttonClicks").toInt() != 2) {
                fail(213, "button enter activation");
                return;
            }

            // Disabled controls cannot take focus and cannot be activated.
            disabledButton->forceActiveFocus(Qt::TabFocusReason);
            if (disabledButton->hasActiveFocus()) {
                fail(214, "disabled focus");
                return;
            }
            QTest::keyClick(window, Qt::Key_Space);
            QTest::qWait(20);
            if (window->property("disabledClicks").toInt() != 0) {
                fail(215, "disabled activation");
                return;
            }

            // Text entry.
            auto* textField = itemOf("a11yTextField");
            textField->forceActiveFocus(Qt::TabFocusReason);
            for (const char c : {'q', 'w', 'e', 'n'})
                QTest::keyClick(window, c);
            QTest::qWait(20);
            if (textField->property("text").toString() != QStringLiteral("qwen")) {
                fail(216, "text field input");
                return;
            }

            // Switch: keyboard toggle updates value and exposed checked state.
            auto* switchItem = itemOf("a11ySwitch");
            switchItem->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(window, Qt::Key_Space);
            QTest::qWait(20);
            if (!switchItem->property("checked").toBool()) {
                fail(217, "switch toggle");
                return;
            }
            if (!QAccessible::queryAccessibleInterface(switchItem)->state().checked) {
                fail(218, "switch checked state");
                return;
            }

            // Slider increases on the Right arrow.
            auto* slider = itemOf("a11ySlider");
            slider->forceActiveFocus(Qt::TabFocusReason);
            const double beforeSlider = slider->property("value").toDouble();
            QTest::keyClick(window, Qt::Key_Right);
            QTest::qWait(20);
            if (slider->property("value").toDouble() <= beforeSlider) {
                fail(219, "slider arrow increase");
                return;
            }

            // Spin box increases on the Up arrow.
            auto* spinBox = itemOf("a11ySpinBox");
            spinBox->forceActiveFocus(Qt::TabFocusReason);
            const int beforeSpin = spinBox->property("value").toInt();
            QTest::keyClick(window, Qt::Key_Up);
            QTest::qWait(20);
            if (spinBox->property("value").toInt() <= beforeSpin) {
                fail(220, "spin box arrow increase");
                return;
            }

            // Combo box expands and reports the expanded accessible state.
            auto* combo = itemOf("a11yComboBox");
            combo->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(window, Qt::Key_Space);
            QTest::qWait(60);
            if (!combo->property("popup").value<QObject*>() ||
                !combo->property("popup").value<QObject*>()->property("visible").toBool()) {
                fail(221, "combo popup opens");
                return;
            }
            if (!QAccessible::queryAccessibleInterface(combo)->state().expanded) {
                fail(222, "combo expanded state");
                return;
            }
            QTest::keyClick(window, Qt::Key_Escape);
            QTest::qWait(60);

            // Tab navigation reaches the next visible focusable control.
            button->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(window, Qt::Key_Tab);
            QTest::qWait(20);
            auto* nextFocus = window->activeFocusItem();
            if (!nextFocus || !nextFocus->isVisible() || nextFocus == button) {
                fail(223, "tab navigation");
                return;
            }

            // Part B — approval dialog semantics and safety.
            auto* approvalModal = window->findChild<QObject*>("a11yApprovalModal");
            if (!approvalModal || !QMetaObject::invokeMethod(approvalModal, "open")) {
                fail(224, "approval open");
                return;
            }
            QTest::qWait(80);
            auto* approvalIface = ifaceOf("a11yApprovalContent");
            if (!approvalIface || approvalIface->role() != QAccessible::Dialog ||
                approvalIface->text(QAccessible::Name) !=
                    QStringLiteral("Tool approval required")) {
                fail(225, "approval dialog semantics");
                return;
            }
            auto* deny = itemOf("a11yApprovalDeny");
            if (window->activeFocusItem() != deny) {
                auto* focused = window->activeFocusItem();
                qWarning() << "approval focus debug: focus"
                           << (focused ? focused->objectName() : QStringLiteral("<null>"))
                           << "expected" << QStringLiteral("a11yApprovalDeny");
                fail(226, "approval initial focus");
                return;
            }
            QTest::keyClick(window, Qt::Key_Escape);
            QTest::qWait(40);
            if (!approvalModal->property("visible").toBool()) {
                fail(227, "approval NoAutoClose");
                return;
            }
            QTest::keyClick(window, Qt::Key_Return);
            QTest::qWait(40);
            if (window->property("approvals").toInt() != 0 ||
                window->property("denials").toInt() != 1) {
                fail(228, "approval enter denies");
                return;
            }
            QTest::keyClick(window, Qt::Key_Tab);
            QTest::qWait(20);
            if (window->activeFocusItem() != itemOf("a11yApprovalAllow")) {
                fail(229, "approval tab order");
                return;
            }
            if (!QMetaObject::invokeMethod(approvalModal, "close")) {
                fail(230, "approval close");
                return;
            }
            if (!waitHidden(approvalModal)) {
                fail(230, "approval close");
                return;
            }
            if (window->property("approvals").toInt() != 0) {
                fail(231, "no accidental approval");
                return;
            }

            // Recovery dialog focus + safe focus restoration. This one is shown by
            // setting `visible` (as Main.qml does for its overlays) rather than by
            // calling open(), so the focus contract is proven for that path too.
            button->forceActiveFocus(Qt::TabFocusReason);
            auto* recoveryModal = window->findChild<QObject*>("a11yRecoveryModal");
            if (!recoveryModal) {
                fail(232, "recovery open");
                return;
            }
            recoveryModal->setProperty("visible", true);
            QTest::qWait(80);
            if (window->activeFocusItem() != itemOf("a11yRecoveryRestore")) {
                fail(233, "recovery initial focus");
                return;
            }
            auto* recoveryIface = ifaceOf("a11yRecoveryContent");
            if (!recoveryIface || recoveryIface->role() != QAccessible::Dialog ||
                recoveryIface->text(QAccessible::Name) !=
                    QStringLiteral("Restore previous session")) {
                fail(234, "recovery dialog semantics");
                return;
            }
            QTest::keyClick(window, Qt::Key_Escape);
            if (!waitHidden(recoveryModal)) {
                fail(235, "recovery escape close");
                return;
            }
            if (!button->hasActiveFocus()) {
                fail(236, "focus restoration");
                return;
            }

            qInfo("Base control roles/names/descriptions, keyboard activation, disabled semantics, "
                  "tab order, "
                  "dialog roles, default focus, no-accidental-approval and focus restoration "
                  "passed (offscreen)");
            app.exit(0);
        });
    }
    if (phase1Smoke) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        struct AcceptanceFailure {};
        const auto exercise = [&, window] {
            auto find = [](auto&& self, QQuickItem* item, const QString& name) -> QQuickItem* {
                if (item->objectName() == name) return item;
                for (auto* child : item->childItems()) if (auto* found = self(self, child, name)) return found;
                return nullptr;
            };
            diagnostic.stage("phase1-callback-enter");
            auto check = [&](bool ok, const char* detail) {
                diagnostic.stage(detail);
                std::fprintf(stderr, "PHASE1 CHECK: %s result=%s\n", detail, ok ? "pass" : "FAIL");
                std::fflush(stderr);
                if (!ok) {
                    std::fprintf(stderr, "Phase 1 acceptance failed: %s\n", detail);
                    std::fflush(stderr);
                    throw AcceptanceFailure{};
                }
            };
            check(!phase1FailureSmoke, "injected failure exit contract");
            auto* page = find(find, window->contentItem(), "settingsPage");
            auto* popup = window->findChild<QObject*>("notificationCenterPopup");
            auto* palette = window->findChild<QObject*>("certificationPalette");
            check(page && popup && palette, "mounted shared surfaces");
            page->setProperty("activeCategory", "Notifications");
            QApplication::processEvents();
            auto* history = find(find, page, "notificationHistoryButton");
            check(history, "Settings history entry");
            history->forceActiveFocus();
            diagnostic.stage("history-click");
            QMetaObject::invokeMethod(history, "clicked");
            QTest::qWait(100);
            check(popup->property("visible").toBool(), "history opens");
            auto* empty = find(find, window->contentItem(), "notificationHistoryEmpty");
            check(empty && empty->isVisible(), "empty history");
            QTest::keyClick(window, Qt::Key_Escape);
            QTest::qWait(100);
            check(!viewModel.notificationCenterVisible() && !popup->property("visible").toBool(), "Escape synchronizes visibility");
            check(history->hasActiveFocus(), "focus returns to history entry");
            diagnostic.stage("notification-create");
            viewModel.addNotificationWithPriority("Security", "Disposable phase 1 notice", "Synthetic test content", "High");
            viewModel.setNotificationCenterVisible(true);
            QTest::qWait(100);
            auto* list = find(find, window->contentItem(), "notificationHistoryList");
            check(list && list->property("count").toInt() == 2 && !empty->isVisible(), "group and populated history");
            viewModel.setDndEnabled(true);
            QApplication::processEvents();
            check(list->property("count").toInt() == 2, "DND retains history");
            auto* markRead = find(find, window->contentItem(), "notificationMarkReadButton");
            auto* archive = find(find, window->contentItem(), "notificationArchiveButton");
            check(markRead && archive, "notification row actions");
            diagnostic.stage("notification-mark-read");
            QMetaObject::invokeMethod(markRead, "clicked");
            QApplication::processEvents();
            const auto readRow = QJsonDocument::fromJson(viewModel.notificationFilteredSummaries().first().toUtf8()).object();
            check(readRow.value("read").toBool(), "row marks notification read");
            archive = find(find, window->contentItem(), "notificationArchiveButton");
            check(archive, "archive action survives refresh");
            diagnostic.stage("notification-archive");
            QMetaObject::invokeMethod(archive, "clicked");
            QApplication::processEvents();
            check(QJsonDocument::fromJson(viewModel.notificationFilteredSummaries().first().toUtf8()).object().value("archived").toBool(), "row archives notification");
            check(viewModel.notificationFilteredSummaries().join(" ").contains("Disposable phase 1 notice"), "archive remains in history");
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
            QTest::qWait(100);
            check(!popup->property("visible").toBool() && !viewModel.notificationCenterVisible(), "outside dismissal");
            check(viewModel.clearArchivedNotifications(), "clear archived");
            viewModel.setDndEnabled(false);
            const auto actions = qmlVariant(palette->property("actions")).toList();
            for (const auto& value : actions) {
                const auto action = value.toMap();
                const auto name = action.value("action").toString();
                if (name == "settings" || name == "notifications" || name == "search-chats" || name == "universal-search" || name == "theme") {
                    const auto oldTheme = viewModel.themeName();
                    diagnostic.stage("palette-action");
                    QMetaObject::invokeMethod(palette, "runAction", Q_ARG(QVariant, value));
                    QApplication::processEvents();
                    if (name == "settings")
                        check(page->property("activeCategory").toString() ==
                                  action.value("category", "Interface").toString(),
                              "category route");
                    if (name == "notifications") {
                        check(viewModel.notificationCenterVisible(), "palette history route");
                        viewModel.setNotificationCenterVisible(false);
                    }
                    if (name.startsWith("search") || name == "universal-search")
                        check(!action.value("enabled").toBool(), "unsupported search disabled");
                    if (name == "theme")
                        check(viewModel.themeName() != oldTheme, "theme cycle");
                }
            }
            // Exercise a large real font at the original 1100px breakpoint, not a wider window.
            window->setWidth(1100);
            auto* settingsSearch = find(find, page, "settingsSearch");
            check(settingsSearch, "sidebar search present");
            const QFont originalSearchFont = settingsSearch->property("font").value<QFont>();
            QFont largeSearchFont = originalSearchFont;
            largeSearchFont.setPixelSize(26);
            settingsSearch->setProperty("font", largeSearchFont);
            check(QTest::qWaitFor([&] {
                      const QFontMetricsF metrics(largeSearchFont);
                      return settingsSearch->height() -
                                     settingsSearch->property("topPadding").toReal() -
                                     settingsSearch->property("bottomPadding").toReal() >=
                                 metrics.height() &&
                             settingsSearch->width() -
                                     settingsSearch->property("leftPadding").toReal() -
                                     settingsSearch->property("rightPadding").toReal() >=
                                 metrics.horizontalAdvance(
                                     settingsSearch->property("placeholderText").toString());
                  }),
                  "large-font sidebar search content fits");
            auto placeholderFits = [](auto&& self, QQuickItem* item) -> bool {
                if (item->isVisible() && item->property("truncated").toBool())
                    return false;
                for (auto* child : item->childItems())
                    if (!self(self, child))
                        return false;
                return true;
            };
            check(placeholderFits(placeholderFits, settingsSearch),
                  "large-font placeholder not truncated");
            settingsSearch->setProperty("font", originalSearchFont);
            for (int width : {1100, 900, 780}) {
                window->setWidth(width);
                window->setHeight(width == 780 ? 640 : 780);
                QTest::qWait(60);
                auto* sidebar = find(find, page, "settingsCategorySidebar");
                check(sidebar && sidebar->isVisible() == !page->property("compact").toBool(), "responsive breakpoint");
                auto inspect = [&](auto&& self, QQuickItem* item) -> void {
                    // Constant class names and geometry only: no text, paths or user data.
                    std::fprintf(
                        stderr,
                        "PHASE1 SIDEBAR: width=%d class=%s geometry=%.1fx%.1f inspect-begin\n",
                        width, item->metaObject()->className(), item->width(), item->height());
                    std::fflush(stderr);
                    const bool visible = item->isVisible();
                    diagnostic.stage("sidebar-visibility-read");
                    if (visible) {
                        const QVariant truncated = item->property("truncated");
                        std::fprintf(stderr, "PHASE1 SIDEBAR: truncated-valid=%d truncated=%d\n",
                                     truncated.isValid(), truncated.toBool());
                        std::fflush(stderr);
                        check(!truncated.toBool(), "complete sidebar labels");
                    }
                    diagnostic.stage("sidebar-children-read-begin");
                    const auto children = item->childItems();
                    diagnostic.stage("sidebar-children-read-end");
                    for (auto* child : children)
                        self(self, child);
                    diagnostic.stage("sidebar-item-complete");
                };
                inspect(inspect, sidebar);
            }
            auto* status = find(find, window->contentItem(), "daemonStatusButton");
            check(status, "focusable daemon indicator");
            status->forceActiveFocus();
            diagnostic.stage("accessible-interface-query");
            const auto* accessible = QAccessible::queryAccessibleInterface(status);
            check(accessible && !accessible->text(QAccessible::Name).isEmpty(), "status accessible name");
            QTest::keyClick(window, Qt::Key_Space);
            QTest::qWait(60);
            auto* statusPopup = window->findChild<QObject*>("daemonStatusPopup");
            check(statusPopup && statusPopup->property("visible").toBool(), "keyboard opens daemon details");
            check(window->activeFocusItem() && window->activeFocusItem() != status, "status popup receives focus");
            QTest::keyClick(window, Qt::Key_Escape);
            QTest::qWait(40);
            check(status->hasActiveFocus(), "status popup restores focus");
            diagnostic.stage("voice-capture-start");
            window->setProperty("surface", 2);
            viewModel.startVoiceCapture(true);
            QTest::qWait(60);
            check(!viewModel.voiceInputStatus().isEmpty() && !viewModel.voiceInputMessage().contains("ModelUnavailable"), "voice prerequisite guidance");
            auto* voiceSetup = find(find, window->contentItem(), "chatVoiceSetupButton");
            check(voiceSetup && voiceSetup->isVisible(), "voice setup action");
            diagnostic.stage("voice-setup-click");
            QMetaObject::invokeMethod(voiceSetup, "clicked");
            QApplication::processEvents();
            check(page->property("activeCategory").toString() == "Voice" && window->property("surface").toInt() == 0, "voice settings route");
            QQmlComponent finishComponent(&engine);
            finishComponent.setData(R"(
                import QtQuick
                import Sentinel.Desktop
                FinishStep {
                    width: 900; height: 680
                    viewModel: QtObject {
                        property bool daemonConnected: false
                        property string activeRuntimeReadinessState: "ready"
                        property string currentModeName: "Calm"
                        property string onboardingProcessingMode: "local"
                        property var onboardingSnapshot: ({providerId: "fixture", modelId: "fixture"})
                        property string appLanguage: "en"
                        property string themeName: "Liquid Glass Light"
                        function languageDisplayName(code) { return code }
                    }
                }
            )", QUrl("qrc:/phase1-finish-test.qml"));
            diagnostic.stage("finish-component-create");
            std::unique_ptr<QObject> finish(finishComponent.create());
            check(bool(finish), "finish component fixture");
            auto* readiness = qvariant_cast<QObject*>(finish->property("viewModel"));
            check(readiness && !finish->property("inferenceReady").toBool(), "offline readiness is false");
            readiness->setProperty("daemonConnected", true);
            check(finish->property("inferenceReady").toBool() && finish->property("modelRunnable").toBool(), "authoritative ready presentation");
            readiness->setProperty("activeRuntimeReadinessState", "busy");
            check(!finish->property("inferenceReady").toBool() && finish->property("modelRunnable").toBool(), "busy is not ready to send");
            for (const char* state : {"missingModel", "unavailable", "unknown"}) {
                readiness->setProperty("activeRuntimeReadinessState", state);
                check(!finish->property("inferenceReady").toBool() && !finish->property("modelRunnable").toBool(), "unconfirmed model is not runnable");
            }
            window->setProperty("surface", 0);
            auto luminance = [](const QColor& color) {
                auto linear = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
                return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
            };
            for (const char* theme : {"Liquid Glass Light", "Liquid Glass Dark"}) {
                viewModel.setThemeName(theme);
                for (bool contrast : {false, true}) {
                    viewModel.setHighContrastEnabled(contrast);
                    QTest::qWait(100);
                    diagnostic.stage("contrast-window-grab");
                    const auto screenshot = window->grabWindow();
                    check(!screenshot.isNull(), "renderer returns icon pixels");
                    const auto background = window->property("settingsSurface").value<QColor>();
                    for (const char* buttonName : {"DashboardNavigationButton", "ModelsNavigationButton", "InspectorNavigationButton"}) {
                        auto* button = find(find, window->contentItem(), buttonName);
                        check(button, "navigation icon button");
                        const auto origin = button->mapToScene(QPointF(button->width() / 2 - 10, button->height() / 2 - 10));
                        const double scale = screenshot.devicePixelRatio();
                        int contrastingPixels = 0;
                        for (int y = int(origin.y() * scale); y < int((origin.y() + 20) * scale); ++y)
                            for (int x = int(origin.x() * scale); x < int((origin.x() + 20) * scale); ++x) {
                                const double a = luminance(screenshot.pixelColor(x, y)), b = luminance(background);
                                if ((qMax(a, b) + 0.05) / (qMin(a, b) + 0.05) >= 3.0)
                                    ++contrastingPixels;
                            }
                        check(contrastingPixels >= 10, "icon strokes visible at >= 3:1 against rail surface");
                        qInfo() << "Icon contrast pixels" << theme << contrast << buttonName << contrastingPixels;
                    }
                }
            }
            qInfo("Onboarding readiness state matrix and rendered rail stroke samples passed; not full WCAG certification");
            qInfo("Phase 1 notification lifecycle/dismissal/focus, palette routes, labels, status and missing-STT flow passed (isolated harness)");
            diagnostic.stage("phase1-assertions-complete");
            app.exit(0);
        };
        QTimer::singleShot(250, &app, [&, exercise] {
            try {
                exercise();
            } catch (const AcceptanceFailure&) {
                // Catch inside the Qt callback and retain main's normal QML teardown.
                diagnostic.stage("phase1-failure-exit");
                app.exit(97);
            }
        });
    }
    if (traySmoke) {
        auto* panel = engine.rootObjects().first()->findChild<QQuickWindow*>("sentinelQuickPanel");
        auto* fixture = engine.rootObjects().first()->findChild<QObject*>("quickPanelFixture");
        if (!panel || !fixture) {
            qWarning("Tray acceptance: panel/fixture missing (70)");
            return 70;
        }
        panel->show();
        QTimer::singleShot(200, &app, [&, panel, fixture] {
            auto fail = [&app](int code, const char* stage) {
                qWarning() << "Tray acceptance failed:" << stage << "exit" << code;
                app.exit(code);
            };
            auto find = [](auto&& self, QQuickItem* parent, const QString& name) -> QQuickItem* {
                if (parent->objectName() == name) return parent;
                for (auto* child : parent->childItems()) if (auto* result = self(self, child, name)) return result;
                return nullptr;
            };
            auto* prompt = find(find, panel->contentItem(), "quickPanelPrompt");
            auto* send = find(find, panel->contentItem(), "quickPanelSend");
            auto* provider = find(find, panel->contentItem(), "quickPanelProvider");
            if (!prompt || !send || !provider || prompt->isEnabled() || send->isEnabled()) {
                fail(71, "initial disconnected controls");
                return;
            }
            QVariantMap state{{"ready", true}, {"busy", false}, {"connection", "Connected"}, {"state", "idle"}, {"voiceState", "idle"}, {"voiceAvailable", false}, {"voiceTranscript", ""}, {"approvalCount", 0}, {"approval", QVariantMap{}}, {"preview", ""}};
            fixture->setProperty("state", state);
            QApplication::processEvents();
            prompt->setProperty("text", "Quick question");
            prompt->forceActiveFocus();
            QTest::keyClick(panel, Qt::Key_Return);
            if (fixture->property("submissions").toInt() != 1 ||
                !prompt->property("text").toString().isEmpty()) {
                fail(72, "Chat keyboard submission");
                return;
            }
            prompt->setProperty("text", "line one");
            prompt->forceActiveFocus();
            QTest::keyClick(panel, Qt::Key_Return, Qt::ShiftModifier);
            if (fixture->property("submissions").toInt() != 1 ||
                !prompt->property("text").toString().contains('\n')) {
                fail(78, "Shift-Enter newline");
                return;
            }
            auto* statusButton = find(find, panel->contentItem(), "quickPanelStatus");
            auto* statusPopup = panel->findChild<QObject*>("quickPanelStatusDetails");
            if (!statusButton || !statusPopup) {
                fail(79, "status popup objects");
                return;
            }
            QMetaObject::invokeMethod(statusButton, "clicked");
            QTest::qWait(40);
            if (!statusPopup->property("visible").toBool()) {
                fail(80, "status popup opens");
                return;
            }
            QTest::keyClick(panel, Qt::Key_Escape);
            QTest::qWait(80);
            if (statusPopup->property("visible").toBool() || !panel->isVisible()) {
                fail(81, "Escape dismisses popup only");
                return;
            }
            panel->setProperty("agentMode", true);
            prompt->setProperty("text", "Agent question");
            QTest::keyClick(panel, Qt::Key_Return);
            if (fixture->property("submissions").toInt() != 2 ||
                !fixture->property("lastAgent").toBool()) {
                fail(73, "Agent keyboard submission");
                return;
            }
            prompt->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(panel, Qt::Key_Tab);
            if (!panel->activeFocusItem() || !panel->activeFocusItem()->isVisible()) {
                fail(74, "keyboard focus");
                return;
            }
            state["preview"] = "Fixture response preview — no inference performed.";
            fixture->setProperty("state", state);
            QTest::qWait(80);
            panel->grabWindow().save(QDir::tempPath() + "/sentinel-tray-panel.png");
            for (int width : {420, 320}) {
                panel->setWidth(width);
                QApplication::processEvents();
                if (provider->width() <= 0 ||
                    send->mapToScene(QPointF()).x() + send->width() > panel->width()) {
                    std::fprintf(stderr,
                                 "TRAY LAYOUT: window=%d provider-width=%.1f send-right=%.1f\n",
                                 panel->width(), provider->width(),
                                 send->mapToScene(QPointF()).x() + send->width());
                    std::fflush(stderr);
                    fail(75, "compact layout");
                    return;
                }
            }
            state["busy"] = true;
            fixture->setProperty("state", state);
            QApplication::processEvents();
            if (prompt->isEnabled() || provider->isEnabled() || !send->isEnabled()) {
                fail(76, "busy controls");
                return;
            }
            auto* warden = qobject_cast<sentinel::desktop::CoreWardenItem*>(find(find, panel->contentItem(), "coreWardenPlayer"));
            if (!warden || !warden->isVisible()) {
                fail(83, "Warden visible");
                return;
            }
            // The fixture changes only panel UI state; no provider/runtime event is injected.
            warden->setAnimationState("thinking");
            warden->setAnimate(true);
            QElapsedTimer loadDeadline;
            loadDeadline.start();
            while (!warden->playing() && loadDeadline.elapsed() < 3000) {
                QTest::qWait(10);
            }
            if (!warden->playing()) {
                fail(84, "Warden animation assets/load");
                return;
            }
            panel->hide();
            QTest::qWait(300);
            if (warden->playing() || warden->cachedFrames() != 0) {
                fail(85, "hidden Warden releases frames");
                return;
            }
            panel->show();
            loadDeadline.restart();
            while (!warden->playing() && loadDeadline.elapsed() < 3000) {
                QTest::qWait(10);
            }
            if (!warden->playing()) {
                fail(86, "shown Warden resumes");
                return;
            }
            wardenPreferences.setMode("off");
            QTest::qWait(100);
            if (warden->isVisible() || warden->cachedFrames() != 0) {
                fail(87, "off mode releases frames");
                return;
            }
            wardenPreferences.setMode("auto");
            warden->setAnimationState("idle");
            warden->setAnimate(false);
            fixture->setProperty("error", "Fixture connection error");
            QApplication::processEvents();
            auto* error = find(find, panel->contentItem(), "quickPanelError");
            if (!error || !error->isVisible() ||
                error->property("text").toString() != "Fixture connection error") {
                fail(77, "error presentation");
                return;
            }
            fixture->setProperty("error", "request-timeout");
            QApplication::processEvents();
            if (!error->property("text").toString().contains("daemon did not reply")) {
                fail(82, "timeout error summary");
                return;
            }
            const auto evidence = qEnvironmentVariable("SENTINEL_TRAY_EVIDENCE_DIR");
            if (!evidence.isEmpty()) {
                QDir().mkpath(evidence);
                panel->setWidth(420);
                for (const auto& theme : {QStringLiteral("Paper"), QStringLiteral("Liquid Glass Dark")}) {
                    viewModel.setThemeName(theme);
                    QTest::qWait(100);
                    panel->grabWindow().save(evidence + (theme == "Paper" ? "/quick-error-light.png" : "/quick-error-dark.png"));
                }
                fixture->setProperty("error", "");
                state["busy"] = false;
                state["preview"] = "";
                fixture->setProperty("state", state);
                prompt->setProperty("text", "");
                QTest::qWait(100);
                panel->grabWindow().save(evidence + "/quick-idle-dark.png");
                state["busy"] = true;
                state["approvalCount"] = 1;
                state["approval"] = QVariantMap{{"tool", "Fixture read-only operation"}, {"risk", 0}, {"detail", "Synthetic test approval; no task executed."}};
                fixture->setProperty("state", state);
                QTest::qWait(100);
                panel->grabWindow().save(evidence + "/quick-approval-fixture-dark.png");
            }
            qInfo("Tray loading/empty/response/error, Chat/Agent keyboard submission, focus and compact layout passed");
            app.exit(0);
        });
    }
    if (modelsSmoke) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        window->setProperty("surface", 3);
        QTimer::singleShot(350, &app, [&, window] {
            auto find = [](auto&& self, QQuickItem* parent, const QString& name) -> QQuickItem* {
                if (parent->objectName() == name) return parent;
                for (auto* child : parent->childItems()) if (auto* found = self(self, child, name)) return found;
                return nullptr;
            };
            auto* page = find(find, window->contentItem(), "modelsPage");
            auto* grid = find(find, window->contentItem(), "modelGrid");
            auto* sidebar = find(find, window->contentItem(), "modelSidebar");
            auto* selector = find(find, window->contentItem(), "modelVersionSelector");
            if (!page || !grid || !sidebar || !selector || grid->property("count").toInt() != 1) { app.exit(40); return; }
            selector->setProperty("currentIndex", 1);
            QMetaObject::invokeMethod(selector, "activated", Q_ARG(int, 1));
            auto* delegate = selector->parentItem()->parentItem()->parentItem();
            if (qmlVariant(delegate->property("selectedModel")).toMap().value("ollamaId") != "qwen2.5:14b") { qWarning() << "Variant selection" << delegate << delegate->property("selectedModel") << selector->property("currentIndex"); app.exit(41); return; }
            auto* detailsButton = find(find, window->contentItem(), "modelDetailsButton");
            auto* popup = page->findChild<QObject*>("modelDetailsPopup");
            if (!detailsButton || !popup || !QMetaObject::invokeMethod(detailsButton, "clicked")) { app.exit(49); return; }
            if (qmlVariant(popup->property("modelInfo")).toMap().value("ollamaId") != "qwen2.5:14b") { app.exit(50); return; }
            QMetaObject::invokeMethod(popup, "close");
            page->setProperty("sidebarCollapsed", true);
            window->setWidth(780); window->setHeight(640);
            QTimer::singleShot(250, &app, [&, page, sidebar, window, find] {
                if (sidebar->width() > 65 || sidebar->property("labelsVisible").toBool()) { app.exit(42); return; }
                page->setProperty("sidebarCollapsed", false);
                QTimer::singleShot(70, &app, [&, sidebar] {
                    if (sidebar->property("labelsVisible").toBool() && sidebar->width() < sidebar->property("expandedWidth").toReal() - 1) app.exit(43);
                });
                QTimer::singleShot(250, &app, [&, page, sidebar, window, find] {
                    if (!sidebar->property("labelsVisible").toBool()) { app.exit(44); return; }
                    auto* label = find(find, sidebar, "modelSidebarLabel");
                    auto* icon = find(find, sidebar, "modelSidebarIcon");
                    if (!label || !icon || label->x() < icon->x() + icon->width()) { app.exit(45); return; }
                    page->setProperty("searchQuery", "no-match-fixture");
                    QApplication::processEvents();
                    if (!qmlVariant(page->property("currentModels")).toList().isEmpty()) { app.exit(46); return; }
                    library.fetching = true; library.notify();
                    QApplication::processEvents();
                    if (!page->property("catalogFetching").toBool()) { app.exit(47); return; }
                    library.fetching = false; library.errorText = "Fixture catalog unavailable"; library.notify();
                    page->setProperty("searchQuery", "");
                    QApplication::processEvents();
                    sidebar->forceActiveFocus(Qt::TabFocusReason);
                    QTest::keyClick(window, Qt::Key_Tab);
                    if (!window->activeFocusItem()) { app.exit(48); return; }
                    for (int i = 0; i < 90; ++i)
                        library.models.append(QVariantMap{{"id", QString("fixture/page%1").arg(i)}, {"name", QString("Page model %1").arg(i)}, {"category", "LLM"}});
                    library.notify();
                    QApplication::processEvents();
                    if (qmlVariant(page->property("currentModels")).toList().size() != 40 || page->property("pageCount").toInt() != 3) { app.exit(51); return; }
                    auto* next = find(find, window->contentItem(), "modelsNextPage");
                    auto* previous = find(find, window->contentItem(), "modelsPreviousPage");
                    if (!next || !previous || !QMetaObject::invokeMethod(next, "clicked")) { app.exit(52); return; }
                    QApplication::processEvents();
                    if (page->property("currentPage").toInt() != 1 || qmlVariant(page->property("currentModels")).toList().size() != 40) { app.exit(53); return; }
                    QMetaObject::invokeMethod(next, "clicked"); QApplication::processEvents();
                    if (qmlVariant(page->property("currentModels")).toList().size() != 11 || next->isEnabled()) { app.exit(54); return; }
                    QMetaObject::invokeMethod(previous, "clicked"); QApplication::processEvents();
                    if (page->property("currentPage").toInt() != 1) { app.exit(55); return; }
                    page->setProperty("searchQuery", "Page model 89"); QApplication::processEvents();
                    if (page->property("currentPage").toInt() != 0 || qmlVariant(page->property("currentModels")).toList().size() != 1) { app.exit(56); return; }
                    qInfo("Model pagination, grouping, variants, responsive sidebar, empty/loading/error and keyboard flow passed");
                    app.exit(0);
                });
            });
        });
    }
    if (settingsSmoke) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QTimer::singleShot(200, &app, [&, window] {
            auto findItem = [](auto&& self, QQuickItem* parent, const QString& name) -> QQuickItem* {
                if (parent->objectName() == name) return parent;
                for (auto* child : parent->childItems())
                    if (auto* found = self(self, child, name)) return found;
                return nullptr;
            };
            auto* page = findItem(findItem, window->contentItem(), "settingsPage");
            if (!page) { app.exit(50); return; }
            const QStringList categories{"Interface", "Appearance", "AI", "Voice", "Memory", "Security", "Notifications", "System"};
            for (int width : {1320, 780, 560}) {
                window->setWidth(width);
                for (const auto& category : categories) {
                    page->setProperty("activeCategory", category);
                    QApplication::processEvents();
                    auto* scroll = findItem(findItem, page, "settingsContentScroll");
                    if (!scroll || scroll->width() < 250 || scroll->height() < 300) { app.exit(51); return; }
                }
            }
            QJsonArray layoutReport;
            for (const auto& language : settings.availableLanguages()) {
                settings.setAppLanguage(language);
                for (int width : {1320, 780, 560}) {
                    window->setWidth(width);
                    for (const auto& category : categories) {
                        page->setProperty("activeCategory", category);
                        QApplication::processEvents();
                        auto* scroll = findItem(findItem, page, "settingsContentScroll");
                        if (!scroll || scroll->width() < 250 || scroll->height() < 300) { app.exit(61); return; }
                        QJsonArray elisions;
                        auto inspectText = [&](auto&& self, QQuickItem* item) -> void {
                            if (!item->isVisible()) return;
                            if (item->property("truncated").toBool() && elisions.size() < 40)
                                elisions.append(QJsonObject{{"name", item->objectName()}, {"text", item->property("text").toString().left(240)}, {"width", item->width()}});
                            for (auto* child : item->childItems()) self(self, child);
                        };
                        inspectText(inspectText, page);
                        layoutReport.append(QJsonObject{{"language", language}, {"width", width}, {"category", category}, {"contentWidth", scroll->width()}, {"contentHeight", scroll->height()}, {"elisionsForReview", elisions}});
                    }
                }
            }
            window->setWidth(1320);
            settings.setAppLanguage("ar");
            QApplication::processEvents();
            auto* sidebar = findItem(findItem, page, "settingsCategorySidebar");
            auto* rtlContent = findItem(findItem, page, "settingsContentScroll");
            if (!sidebar || !rtlContent || sidebar->mapToScene(QPointF()).x() <= rtlContent->mapToScene(QPointF()).x()) { app.exit(62); return; }
            QTest::qWait(40);
            window->grabWindow().save(QDir::tempPath() + "/sentinel-settings-rtl.png");
            QFile layoutFile(QDir::tempPath() + "/sentinel-settings-language-layout.json");
            if (layoutFile.open(QIODevice::WriteOnly)) layoutFile.write(QJsonDocument(layoutReport).toJson());
            settings.setAppLanguage("en");
            window->setWidth(1320);
            page->setProperty("activeCategory", "Notifications");
            QApplication::processEvents();
            auto* dnd = findItem(findItem, page, "settingsDndSwitch");
            if (!dnd || !dnd->isVisible()) { app.exit(52); return; }
            const auto* accessibleDnd = QAccessible::queryAccessibleInterface(dnd);
            if (!accessibleDnd || accessibleDnd->text(QAccessible::Name).isEmpty()) { app.exit(60); return; }
            dnd->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(window, Qt::Key_Space);
            if (!viewModel.dndEnabled()) { app.exit(53); return; }
            QTest::keyClick(window, Qt::Key_Space);
            if (viewModel.dndEnabled()) { app.exit(54); return; }
            page->setProperty("activeCategory", "Appearance");
            for (const auto& theme : QStringList{"Liquid Glass Light", "Paper", "Linen", "Sage Light", "Sky Light", "Solarized Light"}) {
                viewModel.setThemeName(theme);
                QApplication::processEvents();
                if (viewModel.themeName() != theme) { app.exit(57); return; }
                const auto background = window->property("settingsBackground").value<QColor>();
                const auto text = window->property("settingsText").value<QColor>();
                if (background.lightnessF() < 0.8 || text.lightnessF() > 0.4) { app.exit(55); return; }
            }
            QJsonArray contrastReport;
            auto luminance = [](const QColor& color) {
                auto linear = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
                return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
            };
            for (const auto& theme : viewModel.availableThemes()) {
                viewModel.setThemeName(theme);
                for (bool highContrast : {false, true}) {
                    viewModel.setHighContrastEnabled(highContrast);
                    QApplication::processEvents();
                    for (const auto& backgroundKey : {"settingsBackground", "settingsSurface"}) {
                        const auto background = window->property(backgroundKey).value<QColor>();
                        for (const auto& textKey : {"settingsText", "settingsMuted"}) {
                            const auto text = window->property(textKey).value<QColor>();
                            const double a = luminance(background), b = luminance(text);
                            const double ratio = (qMax(a, b) + 0.05) / (qMin(a, b) + 0.05);
                            contrastReport.append(QJsonObject{{"theme", theme}, {"highContrast", highContrast}, {"background", backgroundKey}, {"text", textKey}, {"ratio", ratio}, {"AA_normal_text", ratio >= 4.5}});
                        }
                    }
                }
            }
            QFile contrastFile(QDir::tempPath() + "/sentinel-settings-contrast.json");
            if (!contrastFile.open(QIODevice::WriteOnly)) { app.exit(58); return; }
            contrastFile.write(QJsonDocument(contrastReport).toJson());
            contrastFile.close();
            for (const auto& pair : contrastReport) {
                if (!pair.toObject().value("AA_normal_text").toBool()) {
                    qWarning() << "Contrast below AA" << pair; app.exit(59); return;
                }
            }
            viewModel.setReducedMotionEnabled(true);
            viewModel.setReducedTransparencyEnabled(true);
            viewModel.setHighContrastEnabled(true);
            QApplication::processEvents();
            if (window->property("settingsMotionDuration").toInt() != 0 ||
                window->property("settingsPanel").value<QColor>().alpha() != 255) { app.exit(56); return; }
            viewModel.setHighContrastEnabled(false);
            viewModel.setThemeName("Paper");
            // Leaf scope is an explicit test fixture; production live preparation stays
            // Unsupported.
            auto admission = std::make_shared<sentinel::core::StorageAdmission>();
            if (!settings.attachStorageAdmission(admission) ||
                admission->prepareLocal(std::chrono::milliseconds(0)) !=
                    sentinel::core::StorageAdmission::Result::Ready) {
                app.exit(63);
                return;
            }
            page->setProperty("activeCategory", "Notifications");
            QApplication::processEvents();
            auto* blockedSwitch = findItem(findItem, page, "settingsDndSwitch");
            if (!blockedSwitch || !blockedSwitch->isVisible()) {
                app.exit(64);
                return;
            }
            blockedSwitch->forceActiveFocus(Qt::TabFocusReason);
            const bool previousDnd = viewModel.dndEnabled();
            QTest::keyClick(window, Qt::Key_Space);
            QApplication::processEvents();
            if (viewModel.dndEnabled() != previousDnd ||
                blockedSwitch->property("checked").toBool() != previousDnd ||
                window->activeFocusItem() != blockedSwitch ||
                settings.storageErrorCode() != "StorageBusy") {
                qWarning() << "P806A1 blocked UI" << previousDnd << viewModel.dndEnabled()
                           << blockedSwitch->property("checked") << window->activeFocusItem()
                           << blockedSwitch << settings.storageErrorCode();
                app.exit(65);
                return;
            }
            window->grabWindow().save(QDir::tempPath() + "/sentinel-settings-storage-blocked.png");
            if (admission->resume() != sentinel::core::StorageAdmission::Result::Ready) {
                app.exit(66);
                return;
            }
            QTest::keyClick(window, Qt::Key_Space);
            if (viewModel.dndEnabled() == previousDnd) {
                app.exit(67);
                return;
            }
            QTest::keyClick(window, Qt::Key_Space);
            page->setProperty("activeCategory", "Appearance");
            qInfo("P806A1: blocked keyboard settings write preserved visible value and focus; "
                  "explicit retry after resume succeeded");
            QTimer::singleShot(200, &app, [&, window] {
                const auto screenshotPath = QDir::tempPath() + "/sentinel-settings-appearance.png";
                const bool saved = window->grabWindow().save(screenshotPath);
                auto* page = window->findChild<QQuickItem*>("settingsPage");
                if (page) {
                    page->setProperty("activeCategory", "System");
                    QTest::qWait(80);
                    window->grabWindow().save(QDir::tempPath() + "/sentinel-settings-backup.png");
                    page->setProperty("activeCategory", "Memory");
                    QTest::qWait(80);
                    window->grabWindow().save(QDir::tempPath() + "/sentinel-settings-profile.png");
                }
                qInfo() << "Settings screenshot" << screenshotPath << saved;
                qInfo("Settings categories, responsive layout, keyboard DND, light palettes, motion and opacity smoke passed");
                app.exit(0);
            });
        });
    }
    if (chatSmoke) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        window->setProperty("surface", 2);
        QTimer::singleShot(300, &app, [&, window] {
            auto findItem = [](auto&& self, QQuickItem* parent, const QString& name) -> QQuickItem* {
                if (parent->objectName() == name) return parent;
                for (auto* child : parent->childItems()) if (auto* found = self(self, child, name)) return found;
                return nullptr;
            };
            auto* greeting = findItem(findItem, window->contentItem(), "chatGreeting");
            auto* composer = findItem(findItem, window->contentItem(), "chatComposer");
            auto* input = findItem(findItem, window->contentItem(), "chatPromptInput");
            if (!greeting || !composer || !input || !greeting->isVisible() || composer->height() < 100) { app.exit(30); return; }
            const auto evidence = qEnvironmentVariable("SENTINEL_WARDEN_UI_EVIDENCE");
            if (!evidence.isEmpty()) {
                QDir().mkpath(evidence);
                for (const auto size : {QSize(1100, 780), QSize(780, 640), QSize(560, 640), QSize(1600, 960)}) {
                    window->resize(size);
                    QTest::qWait(150);
                    auto* mascot = findItem(findItem, greeting, "coreWardenPlayer");
                    const auto greetingBottom = greeting->mapToScene(QPointF(0, greeting->height())).y();
                    const auto composerTop = composer->mapToScene(QPointF()).y();
                    if (!mascot || mascot->property("cachedFrames").toInt() != 1 || greetingBottom > composerTop + 1) {
                        qWarning() << "Warden layout failure" << size << greetingBottom << composerTop;
                        app.exit(36); return;
                    }
                    window->grabWindow().save(evidence + QString("/chat-%1x%2.png").arg(size.width()).arg(size.height()));
                }
                window->resize(QSize(1100, 780));
                QTest::qWait(100);
            }
            const auto first = viewModel.createConversation("Empty one");
            const auto second = viewModel.createConversation("Empty two");
            viewModel.switchConversation(first);
            QApplication::processEvents();
            if (!greeting->isVisible()) { app.exit(31); return; }
            viewModel.switchConversation(second);
            QApplication::processEvents();
            if (!greeting->isVisible()) { app.exit(32); return; }
            // No model is configured in this disposable profile: exercise the real failed-response state.
            viewModel.sendMessage("Test message for alignment");
            QApplication::processEvents();
            QTimer::singleShot(200, &app, [&, window, findItem, greeting, composer, input] {
                const bool rowsReady = QTest::qWaitFor([&] {
                    return findItem(findItem, window->contentItem(), "userMessageBubble")
                        && findItem(findItem, window->contentItem(), "assistantMessageBubble");
                }, 2000);
                auto* user = findItem(findItem, window->contentItem(), "userMessageBubble");
                auto* assistant = findItem(findItem, window->contentItem(), "assistantMessageBubble");
                if (!rowsReady || !user || !assistant || greeting->isVisible() || user->x() <= assistant->x()) { qWarning() << "Alignment failure" << user << assistant << greeting->isVisible() << viewModel.conversationHistoryMessageCount() << (user ? user->x() : -1) << (assistant ? assistant->x() : -1); app.exit(33); return; }
                window->setWidth(780); window->setHeight(640);
                QApplication::processEvents();
                auto* attach = findItem(findItem, window->contentItem(), "chatAttachButton");
                auto* send = findItem(findItem, window->contentItem(), "chatSendButton");
                if (!attach || !send || attach->mapToScene(QPointF()).x() >= send->mapToScene(QPointF()).x() || composer->height() < 100) { app.exit(34); return; }
                input->forceActiveFocus(Qt::TabFocusReason);
                QTest::keyClick(window, Qt::Key_Tab);
                if (!window->activeFocusItem() || !window->activeFocusItem()->isVisible()) { app.exit(35); return; }
                qInfo("Empty conversation, alignment, failed response, responsive composer and keyboard focus smoke passed");
                app.exit(0);
            });
        });
    }
    if (navigationSmoke) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QTimer::singleShot(100, &app, [&, window] {
            auto findItem = [](auto&& self, QQuickItem* parent, const QString& name) -> QQuickItem* {
                if (parent->objectName() == name) return parent;
                for (auto* child : parent->childItems())
                    if (auto* found = self(self, child, name)) return found;
                return nullptr;
            };
            auto* home = findItem(findItem, window->contentItem(), "DashboardNavigationButton");
            auto* preferences = findItem(findItem, window->contentItem(), "settingsNavigationButton");
            if (!home || !preferences) { app.exit(23); return; }
            home->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(window, Qt::Key_Space);
            QApplication::processEvents();
            if (window->property("surface").toInt() != 2) { app.exit(24); return; }
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                             preferences->mapToScene(QPointF(preferences->width() / 2,
                                                            preferences->height() / 2)).toPoint());
            QApplication::processEvents();
            if (window->property("surface").toInt() != 0) { app.exit(25); return; }
        });
    }
    if (focusSmoke) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        if (qEnvironmentVariableIsSet("SENTINEL_ONBOARDING_COMPACT")) window->resize(780, 640);
        auto* onboarding = window->findChild<QQuickItem*>("sentinelOnboarding");
        auto* next = window->findChild<QQuickItem*>("onboardingNext");
        auto* back = window->findChild<QQuickItem*>("onboardingBack");
        QTimer::singleShot(200, &app, [&, window, onboarding, next, back] {
            auto contained = [&] {
                auto* item = window->activeFocusItem();
                if (!item || !item->isVisible() || !item->isEnabled())
                    return false;
                while (item && item != onboarding)
                    item = item->parentItem();
                return item == onboarding;
            };
            auto key = [&](int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
                QTest::keyClick(window, Qt::Key(code), modifiers);
                QApplication::processEvents();
            };
            if (!onboarding || !next || !back || !contained()) {
                app.exit(10);
                return;
            }
            for (int step = 0; step < 7; ++step) {
                if (step > 0 && !viewModel.advanceOnboarding(true)) {
                    app.exit(11);
                    return;
                }
                QApplication::processEvents();
                window->requestActivate();
                QApplication::processEvents();
                next->forceActiveFocus(Qt::TabFocusReason);
                const auto evidence = qEnvironmentVariable("SENTINEL_ONBOARDING_EVIDENCE_DIR");
                if (!evidence.isEmpty()) {
                    QDir().mkpath(evidence);
                    for (const auto& theme : {QStringLiteral("Paper"), QStringLiteral("Liquid Glass Dark")}) {
                        viewModel.setThemeName(theme);
                        QTest::qWait(100);
                        window->grabWindow().save(evidence + QString("/step-%1-%2.png").arg(step).arg(theme == "Paper" ? "light" : "dark"));
                    }
                }
                QSet<QQuickItem*> visited;
                for (int i = 0; i < 24; ++i) {
                    key(Qt::Key_Tab);
                    if (!contained()) {
                        qWarning() << "Tab escaped" << step << i << window->activeFocusItem();
                        app.exit(12);
                        return;
                    }
                    visited.insert(window->activeFocusItem());
                    key(Qt::Key_Backtab, Qt::ShiftModifier);
                    if (!contained()) {
                        qWarning() << "Backtab escaped" << step << i << window->activeFocusItem();
                        app.exit(13);
                        return;
                    }
                    key(Qt::Key_Tab);
                }
                if (step > 0 && !visited.contains(back)) {
                    app.exit(14);
                    return;
                }
            }
            viewModel.backOnboarding();
            QApplication::processEvents();
            next->forceActiveFocus();
            key(Qt::Key_Space);
            if (viewModel.onboardingStepIndex() != 6 || !contained()) {
                app.exit(15);
                return;
            }
            qInfo("Onboarding initial/forward/reverse/Back/Space focus smoke passed across seven "
                  "steps");
            app.exit(0);
        });
    }
    QTimer::singleShot(15 * 60 * 1000, &app, [&app] { app.exit(98); });
    diagnostic.stage("event-loop-enter");
    const auto code = app.exec();
    diagnostic.stage("event-loop-exit");
    // Tear down production QML before its stack-owned viewmodel disappears.
    diagnostic.stage("qml-root-teardown");
    for (auto* root : engine.rootObjects())
        delete root;
    diagnostic.stage("qml-root-teardown-complete");
    if (daemonE2e) {
        daemonScript->release.release();
        fixtureServer.reset();
        controller.reset();
        for (auto location : {QStandardPaths::AppDataLocation, QStandardPaths::AppConfigLocation}) {
            const auto path = QStandardPaths::writableLocation(location);
            if (QDir(path).exists() && !QDir(path).removeRecursively())
                return 249;
        }
    }
    return code;
}

#include "settings_ui_certification_probe.moc"
