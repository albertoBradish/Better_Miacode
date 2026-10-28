#pragma once

#include <QObject>
#include <QString>

namespace miacode {
class CliIntegration final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(QString commandPath READ commandPath NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    // An explicit directory is for isolated tests: no user PATH/profile writes.
    explicit CliIntegration(QObject* parent = nullptr, QString testDirectory = {},
                            QString testProfileDirectory = {});
    bool enabled() const;
    QString commandPath() const;
    QString status() const { return status_; }
    Q_INVOKABLE bool setEnabled(bool enabled);
    static bool tasksEnabled();
    static QString commandName();
signals:
    void changed();
private:
    QString testDirectory_;
    QString testProfileDirectory_;
    QString status_;
};
}
