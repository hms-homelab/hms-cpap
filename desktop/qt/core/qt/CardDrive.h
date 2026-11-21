#pragma once

#include <QAbstractNativeEventFilter>
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <memory>

class QWinEventNotifier;

namespace cpapdash::supervisor {

/**
 * SDD-073 D1: a drive appearing or going, as the OS says it.
 *
 * macOS and Linux: the folder volumes are mounted in (/Volumes, /media/<user>,
 * /run/media/<user>) changes, which QFileSystemWatcher hears from the kernel.
 * macOS also: DiskArbitration's volume path being set or a disk going, since
 * the /Volumes change comes when the mount point is made, before the volume
 * is mounted on it.
 * Windows: WM_DEVICECHANGE (a volume arrived or was removed), broadcast to
 * every top-level window. No clock: nothing is looked at between events.
 */
class DriveWatch : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit DriveWatch(QObject* parent = nullptr);
    ~DriveWatch() override;

    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;

signals:
    void changed();

private:
    QFileSystemWatcher watcher_;
    void* da_session_ = nullptr;   ///< macOS: the DiskArbitration session (DASessionRef)
};

/// A mounted volume that holds a CPAP machine's card.
struct CardVolume {
    QString root;     ///< where it is mounted
    QString device;   ///< macOS /dev/diskNsM, Linux /dev/sdXN, Windows "E:"
    QString name;     ///< what the person sees (its label, or the root)
};

/// The first mounted volume that looks like a CPAP card (BridgeSetup.h), or
/// an empty root.
CardVolume findCardVolume();

/**
 * SDD-073 D1: `hms_cpap --read-card`, run with the computer's permission
 * (the one elevation the installer asks for): macOS the administrator prompt
 * (osascript ... with administrator privileges), Windows the UAC prompt
 * (ShellExecuteEx "runas"), Linux pkexec. Its progress is a small file the
 * reader rewrites, heard through QFileSystemWatcher; its end is the process
 * ending (QProcess::finished, or the process handle signalled on Windows).
 */
class CardReadJob : public QObject {
    Q_OBJECT
public:
    CardReadJob(QString service_exe, QString work_dir, QObject* parent = nullptr);
    ~CardReadJob() override;

    /// `prompt`: what the macOS administrator prompt says (the others show
    /// the OS's own words).
    void start(const CardVolume& card, const QString& prompt);
    bool running() const { return running_; }
    QString outPath() const;

signals:
    void progress(const QString& phase, qint64 done, qint64 total);
    /// ok: the .cpapcard is at outPath(). Not ok: `declined` when the
    /// permission was not given, else `error` (the reader's words, or empty).
    void finished(bool ok, bool declined, const QString& error, const QJsonObject& answer);

private:
    void readProgress();
    void ended(int exit_code);

    QString service_exe_;
    QString work_dir_;
    bool    running_ = false;
    QFileSystemWatcher watch_;
    std::unique_ptr<QProcess> proc_;
#ifdef Q_OS_WIN
    void*              handle_ = nullptr;
    QWinEventNotifier* notifier_ = nullptr;
#endif
};

}  // namespace cpapdash::supervisor
