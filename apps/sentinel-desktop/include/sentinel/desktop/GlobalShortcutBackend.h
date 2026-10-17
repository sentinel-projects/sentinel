#pragma once
#include <QObject>
#include <QString>

namespace sentinel::desktop {
class GlobalShortcutBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void setShortcut(const QString& sequence) = 0;
    virtual void shutdown() = 0;
    virtual QString status() const = 0;
    virtual bool registered() const = 0;
signals:
    void statusChanged();
    void activated(const QString& token);
};
} // namespace sentinel::desktop
