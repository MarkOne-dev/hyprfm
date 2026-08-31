#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QProcess>

class RcloneService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool rcloneAvailable READ rcloneAvailable NOTIFY rcloneAvailableChanged)
    Q_PROPERTY(QStringList activeMounts READ activeMounts NOTIFY activeMountsChanged)

public:
    explicit RcloneService(QObject *parent = nullptr);
    ~RcloneService();

    bool rcloneAvailable() const;
    QStringList activeMounts() const;

    Q_INVOKABLE bool isRclonePath(const QString &path) const;
    Q_INVOKABLE bool isMounted(const QString &remoteName) const;
    Q_INVOKABLE bool isMounting(const QString &remoteName) const;
    Q_INVOKABLE bool isMountedForPath(const QString &path) const;
    Q_INVOKABLE void mountRemote(const QString &remoteName);
    Q_INVOKABLE void unmountRemote(const QString &remoteName);
    Q_INVOKABLE QString getMountPath(const QString &remoteName) const;
    Q_INVOKABLE QString getRemoteNameFromPath(const QString &path) const;

signals:
    void rcloneAvailableChanged();
    void activeMountsChanged();
    void mountFinished(const QString &remoteName, bool success, const QString &errorString);
    void unmountFinished(const QString &remoteName, bool success);

private:
    bool checkRcloneAvailable() const;
    void ensureMountsBaseDirExists() const;
    void startRcloneMountProcess(const QString &remoteName, const QString &mountPath);

    bool m_rcloneAvailable;
    QString m_mountsBaseDir;
    QHash<QString, QProcess*> m_processes;
};
