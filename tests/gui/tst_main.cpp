// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QtQuickTest>

class DesktopTestSetup final : public QObject {
    Q_OBJECT
public slots:
    void applicationAvailable() {
        QStandardPaths::setTestModeEnabled(true);
        const auto families = QFontDatabase::families();
        for (const auto& family :
             {QStringLiteral("DejaVu Sans"), QStringLiteral("Arial"), QStringLiteral("Noto Sans"),
              QStringLiteral("Liberation Sans")}) {
            if (families.contains(family)) {
                QGuiApplication::setFont(QFont(family, 12));
                return;
            }
        }
        qFatal("UI verification requires an installed supported font");
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        engine->addImportPath(QStringLiteral(DESKTOP_QML_IMPORT));
        const auto evidence = qEnvironmentVariable("SENTINEL_UI_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            const auto root = QDir(QStringLiteral(UI_EVIDENCE_ROOT)).canonicalPath();
            const auto target = QDir(evidence).canonicalPath();
            if (root.isEmpty() || target.isEmpty() || !target.startsWith(root + '/')) {
                qFatal(
                    "UI evidence must use a disposable directory under the existing build output");
            }
        }
        engine->rootContext()->setContextProperty(QStringLiteral("uiEvidenceDirectory"), evidence);
        engine->rootContext()->setContextProperty(QStringLiteral("uiTestFontFamily"),
                                                  QGuiApplication::font().family());
    }
};

QUICK_TEST_MAIN_WITH_SETUP(SentinelGuiTests, DesktopTestSetup)
#include "tst_main.moc"
