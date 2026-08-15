#include "Autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <QSettings>
#endif

namespace cpapdash::supervisor::autostart {

namespace {

#ifdef Q_OS_WIN
// The same key and value name the Inno installer writes, and the same the C#
// shell used. Sharing them is what makes the installer's "Start when I log in"
// checkbox and this menu item two views of one setting rather than two settings
// that disagree.
constexpr const char* kRunKey =
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const char* kValueName = "CpapDashDesktop";
#endif

#ifdef Q_OS_MACOS
QString agentPath() {
    return QDir::homePath() + "/Library/LaunchAgents/com.hms.cpapdash.supervisor.plist";
}

/// The .app, not the executable inside it.
///
/// applicationFilePath() points at Contents/MacOS/CpapDashDesktop. Launching
/// that directly works but starts a process macOS does not associate with the
/// bundle, so it gets no icon, no menu bar identity and no bundle Info.plist --
/// including LSUIElement, without which a Dock icon appears.
QString bundlePath() {
    QDir dir(QCoreApplication::applicationDirPath());   // Contents/MacOS
    dir.cdUp();                                         // Contents
    dir.cdUp();                                         // Foo.app
    const QString path = dir.absolutePath();
    return path.endsWith(".app") ? path : QCoreApplication::applicationFilePath();
}
#endif

#ifdef Q_OS_LINUX
QString desktopEntryPath() {
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    const QString base = xdg.isEmpty() ? QDir::homePath() + "/.config"
                                       : QString::fromLocal8Bit(xdg);
    return base + "/autostart/cpapdash.desktop";
}
#endif

}  // namespace

bool isSupported() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}

bool isEnabled() {
#if defined(Q_OS_WIN)
    QSettings run(kRunKey, QSettings::NativeFormat);
    return !run.value(kValueName).toString().isEmpty();
#elif defined(Q_OS_MACOS)
    return QFileInfo::exists(agentPath());
#elif defined(Q_OS_LINUX)
    return QFileInfo::exists(desktopEntryPath());
#else
    return false;
#endif
}

bool setEnabled(bool enabled, QString* error) {
    const auto fail = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };

#if defined(Q_OS_WIN)
    QSettings run(kRunKey, QSettings::NativeFormat);
    if (enabled) {
        // QUOTED. The install path lives under %LOCALAPPDATA%\Programs and
        // account names contain spaces, so an unquoted value is read as a
        // program name plus arguments and silently never starts.
        run.setValue(kValueName,
                     '"' + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) + '"');
    } else {
        run.remove(kValueName);
    }
    run.sync();
    if (run.status() != QSettings::NoError)
        return fail("Windows would not let CpapDash change your startup items.");
    return true;

#elif defined(Q_OS_MACOS)
    const QString path = agentPath();
    if (!enabled) {
        if (!QFileInfo::exists(path)) return true;
        return QFile::remove(path) ? true
             : fail("Could not remove " + path);
    }

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail("Could not write " + path);

    // `open -a` rather than the executable, so launchd starts the BUNDLE and
    // the app keeps its identity. RunAtLoad only -- no KeepAlive, because the
    // supervisor quitting is a decision the user made, and relaunching it would
    // make Quit do nothing.
    QTextStream out(&f);
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n"
           "<dict>\n"
           "  <key>Label</key>\n"
           "  <string>com.hms.cpapdash.supervisor</string>\n"
           "  <key>ProgramArguments</key>\n"
           "  <array>\n"
           "    <string>/usr/bin/open</string>\n"
           "    <string>-a</string>\n"
           "    <string>" << bundlePath() << "</string>\n"
           "  </array>\n"
           "  <key>RunAtLoad</key>\n"
           "  <true/>\n"
           "</dict>\n"
           "</plist>\n";
    f.close();
    return true;

#elif defined(Q_OS_LINUX)
    const QString path = desktopEntryPath();
    if (!enabled) {
        if (!QFileInfo::exists(path)) return true;
        return QFile::remove(path) ? true : fail("Could not remove " + path);
    }

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail("Could not write " + path);

    // The XDG autostart directory, which every desktop environment reads --
    // rather than a systemd user unit, which only some do.
    QTextStream out(&f);
    out << "[Desktop Entry]\n"
           "Type=Application\n"
           "Name=CpapDash\n"
           "Comment=Collects and stores your CPAP therapy data\n"
           "Exec=\"" << QCoreApplication::applicationFilePath() << "\"\n"
           "Icon=cpapdash\n"
           "Terminal=false\n"
           "X-GNOME-Autostart-enabled=true\n";
    f.close();
    return true;

#else
    Q_UNUSED(enabled);
    return fail("This platform cannot manage startup items.");
#endif
}

}  // namespace cpapdash::supervisor::autostart
