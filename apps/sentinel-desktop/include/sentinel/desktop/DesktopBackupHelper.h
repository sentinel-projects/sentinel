// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include <memory>
class QFile;
class QTemporaryFile;
class QCryptographicHash;
namespace sentinel::desktop {
class DesktopRuntimeClient;
class DesktopBackupHelper final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool memoryReady READ memoryReady NOTIFY changed)
    Q_PROPERTY(bool memoryTransferActive READ memoryTransferActive NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool cancellable READ cancellable NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QStringList importDomains READ importDomains NOTIFY changed)
    Q_PROPERTY(QString preview READ preview NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
public:
    explicit DesktopBackupHelper(DesktopRuntimeClient* runtime, QObject* parent = nullptr);
    ~DesktopBackupHelper() override;
    bool memoryReady() const {
        return memoryReady_;
    }
    bool memoryTransferActive() const {
        return memoryActive_;
    }
    Q_INVOKABLE bool exportMemoryFile(const QUrl&, const QVariantMap&);
    Q_INVOKABLE bool inspectMemoryFile(const QUrl&, const QVariantMap&);
    Q_INVOKABLE bool restoreMemoryFile(bool skipIdentical);
    bool busy() const { return busy_; }
    bool cancellable() const {
        return memoryActive_ ? (busy_ || memoryReady_) && memoryCanCancel_
                             : busy_ && action_ != QStringLiteral("commit");
    }
    QString status() const { return status_; }
    QStringList importDomains() const { return importDomains_; }
    QString preview() const { return preview_; }
    double progress() const { return expected_ > 0 ? double(offset_) / expected_ : 0; }
    Q_INVOKABLE QVariantMap loadMemoryArchive(const QUrl& path);
    Q_INVOKABLE bool saveMemoryArchive(const QUrl& path, const QString& archive);
    Q_INVOKABLE bool exportFile(const QUrl& path, const QStringList& domains);
    Q_INVOKABLE bool inspectFile(const QUrl& path);
    Q_INVOKABLE bool importFile(const QStringList& domains, bool replace);
    Q_INVOKABLE void cancel();
signals:
    void changed();
private:
    void sendMemory(const QString&, QJsonObject = {});
    void receiveMemory(const QVariantMap&);
    void finishMemory(const QString&);
    void pollMemory();
    bool memoryActive_ = false, memoryReady_ = false, memoryCanCancel_ = true,
         memoryCancelPending_ = false;
    QString memoryJob_, memoryTag_, memoryMode_;
    QVariantMap memoryRequest_;
    std::unique_ptr<QFile> memorySource_;
    std::unique_ptr<QTemporaryFile> memoryTarget_;
    std::unique_ptr<QCryptographicHash> memoryHash_;
    void send(const QString& action, QJsonObject value = {});
    void receive(const QJsonObject& result);
    void finish(const QString& status);
    DesktopRuntimeClient* runtime_;
    bool busy_ = false, importing_ = false;
    QString status_, preview_, request_, action_, transfer_;
    QStringList importDomains_;
    QByteArray bytes_, importBytes_, digest_;
    QUrl destination_;
    int offset_ = 0, expected_ = 0;
};
}
