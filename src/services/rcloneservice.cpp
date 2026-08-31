#include "services/rcloneservice.h"

#include <QDir>
#include <QStandardPaths>
#include <QTimer>
#include <QDebug>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
    QHash<QString, bool> m_mountSuccessEmitted;
}

RcloneService::RcloneService(QObject *parent)
    : QObject(parent)
{
    m_rcloneAvailable = checkRcloneAvailable();
    m_mountsBaseDir = QDir::homePath() + QStringLiteral("/.local/share/hyprfm/mounts");
    ensureMountsBaseDirExists();
}

RcloneService::~RcloneService()
{
    // Clean up all active mounts on app exit
    const QStringList remotes = m_processes.keys();
    for (const QString &remote : remotes) {
        unmountRemote(remote);
    }
}

bool RcloneService::rcloneAvailable() const
{
    return m_rcloneAvailable;
}

QStringList RcloneService::activeMounts() const
{
    return m_processes.keys();
}

bool RcloneService::checkRcloneAvailable() const
{
    return !QStandardPaths::findExecutable(QStringLiteral("rclone")).isEmpty();
}

void RcloneService::ensureMountsBaseDirExists() const
{
    QDir().mkpath(m_mountsBaseDir);
}

bool RcloneService::isRclonePath(const QString &path) const
{
    const QString prefix = m_mountsBaseDir + QStringLiteral("/");
    return path.startsWith(prefix) && path.length() > prefix.length();
}

bool RcloneService::isMounted(const QString &remoteName) const
{
    return m_processes.contains(remoteName) && m_mountSuccessEmitted.value(remoteName);
}

bool RcloneService::isMounting(const QString &remoteName) const
{
    return m_processes.contains(remoteName) && !m_mountSuccessEmitted.value(remoteName);
}

bool RcloneService::isMountedForPath(const QString &path) const
{
    const QString remote = getRemoteNameFromPath(path);
    if (remote.isEmpty())
        return false;
    return isMounted(remote);
}

QString RcloneService::getRemoteNameFromPath(const QString &path) const
{
    if (!isRclonePath(path))
        return {};

    QString sub = path.mid(m_mountsBaseDir.length());
    if (sub.startsWith(QLatin1Char('/'))) {
        sub = sub.mid(1);
    }
    int slashIdx = sub.indexOf(QLatin1Char('/'));
    if (slashIdx != -1) {
        return sub.left(slashIdx);
    }
    return sub;
}

QString RcloneService::getMountPath(const QString &remoteName) const
{
    return m_mountsBaseDir + QStringLiteral("/") + remoteName;
}

void RcloneService::mountRemote(const QString &remoteName)
{
    if (!m_rcloneAvailable) {
        emit mountFinished(remoteName, false, QStringLiteral("rclone executable not found"));
        return;
    }

    if (isMounted(remoteName)) {
        emit mountFinished(remoteName, true, QString());
        return;
    }

    if (isMounting(remoteName)) {
        return;
    }

    const QString mountPath = getMountPath(remoteName);
    QDir().mkpath(mountPath);

    // Clean up any stale or lingering FUSE mounts asynchronously before starting
    QProcess *unmountProc = new QProcess(this);
    connect(unmountProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, remoteName, mountPath, unmountProc](int exitCode) {
        unmountProc->deleteLater();
        if (exitCode != 0) {
            QProcess *unmountProc3 = new QProcess(this);
            connect(unmountProc3, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                    [this, remoteName, mountPath, unmountProc3](int) {
                unmountProc3->deleteLater();
                startRcloneMountProcess(remoteName, mountPath);
            });
            unmountProc3->start(QStringLiteral("fusermount3"), {QStringLiteral("-u"), mountPath});
        } else {
            startRcloneMountProcess(remoteName, mountPath);
        }
    });
    unmountProc->start(QStringLiteral("fusermount"), {QStringLiteral("-u"), mountPath});
}

void RcloneService::startRcloneMountProcess(const QString &remoteName, const QString &mountPath)
{
    QProcess *proc = new QProcess(this);
    m_processes.insert(remoteName, proc);
    m_mountSuccessEmitted[remoteName] = false;
    emit activeMountsChanged();

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, remoteName, proc](int exitCode, QProcess::ExitStatus) {
        const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        m_processes.remove(remoteName);
        emit activeMountsChanged();

        if (!m_mountSuccessEmitted.value(remoteName)) {
            emit mountFinished(remoteName, false, err.isEmpty() ? QStringLiteral("rclone mount exited unexpectedly.") : err);
        }
        m_mountSuccessEmitted.remove(remoteName);
        proc->deleteLater();
    });

    connect(proc, &QProcess::errorOccurred, this, [this, remoteName, proc](QProcess::ProcessError) {
        const QString err = proc->errorString();
        m_processes.remove(remoteName);
        emit activeMountsChanged();

        if (!m_mountSuccessEmitted.value(remoteName)) {
            emit mountFinished(remoteName, false, err);
        }
        m_mountSuccessEmitted.remove(remoteName);
        proc->deleteLater();
    });

    proc->start(QStringLiteral("rclone"), {
        QStringLiteral("mount"),
        remoteName + QStringLiteral(":"),
        mountPath,
        QStringLiteral("--vfs-cache-mode"),
        QStringLiteral("writes"),
        QStringLiteral("--vfs-cache-max-age"),
        QStringLiteral("72h"),
        QStringLiteral("--dir-cache-time"),
        QStringLiteral("72h"),
        QStringLiteral("--attr-timeout"),
        QStringLiteral("72h"),
        QStringLiteral("--no-checksum"),
        QStringLiteral("--vfs-read-chunk-size"),
        QStringLiteral("1M"),
        QStringLiteral("--vfs-read-chunk-size-limit"),
        QStringLiteral("off"),
        QStringLiteral("--buffer-size"),
        QStringLiteral("32M"),
        QStringLiteral("--poll-interval"),
        QStringLiteral("15s"),
        QStringLiteral("-o"),
        QStringLiteral("big_writes")
    });

    // Poll mountpoint checks every 100ms to verify FUSE mount is active
    QTimer *mountTimer = new QTimer(this);
    connect(mountTimer, &QTimer::timeout, this, [this, remoteName, mountPath, mountTimer]() {
        if (!m_processes.contains(remoteName) || m_processes.value(remoteName)->state() != QProcess::Running) {
            mountTimer->stop();
            mountTimer->deleteLater();
            return;
        }

        struct stat st_dir, st_parent;
        QString parentPath = QDir(mountPath).filePath(QStringLiteral(".."));
        if (stat(mountPath.toLocal8Bit().constData(), &st_dir) == 0 &&
            stat(parentPath.toLocal8Bit().constData(), &st_parent) == 0) {

            if (st_dir.st_dev != st_parent.st_dev) {
                mountTimer->stop();
                mountTimer->deleteLater();

                m_mountSuccessEmitted[remoteName] = true;
                emit mountFinished(remoteName, true, QString());
            }
        }
    });

    // Timeout safety: if it doesn't mount in 10 seconds, abort
    QTimer::singleShot(10000, this, [this, remoteName, mountTimer]() {
        if (m_processes.contains(remoteName) && !m_mountSuccessEmitted.value(remoteName)) {
            mountTimer->stop();
            mountTimer->deleteLater();

            unmountRemote(remoteName);
            emit mountFinished(remoteName, false, QStringLiteral("Mount operation timed out. Verify your rclone remote or network connection."));
        }
    });

    mountTimer->start(100);
}

void RcloneService::unmountRemote(const QString &remoteName)
{
    if (!m_processes.contains(remoteName)) {
        emit unmountFinished(remoteName, true);
        return;
    }

    QProcess *proc = m_processes.value(remoteName);
    if (proc) {
        disconnect(proc, nullptr, this, nullptr);
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), proc, &QObject::deleteLater);
        proc->terminate();
        QTimer::singleShot(2000, proc, [proc]() {
            if (proc->state() != QProcess::NotRunning) {
                proc->kill();
            }
        });
    }

    m_processes.remove(remoteName);
    m_mountSuccessEmitted.remove(remoteName);
    emit activeMountsChanged();

    // Call fusermount asynchronously to release FUSE mount points
    const QString mountPath = getMountPath(remoteName);
    QProcess *unmountProc = new QProcess(this);
    connect(unmountProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, remoteName, mountPath, unmountProc](int exitCode) {
        unmountProc->deleteLater();
        if (exitCode != 0) {
            QProcess *unmountProc3 = new QProcess(this);
            connect(unmountProc3, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                    [this, remoteName, unmountProc3](int) {
                unmountProc3->deleteLater();
                emit unmountFinished(remoteName, true);
            });
            unmountProc3->start(QStringLiteral("fusermount3"), {QStringLiteral("-u"), mountPath});
        } else {
            emit unmountFinished(remoteName, true);
        }
    });
    unmountProc->start(QStringLiteral("fusermount"), {QStringLiteral("-u"), mountPath});
}
