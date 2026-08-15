#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QTimer>
#include <QNetworkAccessManager>
#include <memory>

#include "ChildProcess.h"
#include "ConfigModel.h"

#include <string>
#include <vector>

namespace cpapdash::supervisor {

/**
 * The status-area shell: a Windows tray icon, a macOS menu bar item, a Linux
 * StatusNotifier entry. QSystemTrayIcon is all three, which is the whole point
 * of doing this once in Qt rather than three times natively -- SDD-005
 * specified the macOS menu bar item a year ago and it was never built, because
 * it was a separate project in a separate language.
 *
 * The icon itself carries state so a glance answers "is it running?". On macOS
 * it is a TEMPLATE image, so the system tints it to match a light or dark menu
 * bar and keeps up when the user switches appearance; a colour icon there would
 * be a small navy tile that ignores the theme.
 */
class TrayShell : public QObject {
    Q_OBJECT

public:
    TrayShell(ChildProcess* child, QString service_exe, QObject* parent = nullptr);

    /// False when the desktop has no status area to put an icon in. On Linux
    /// that is a real possibility (GNOME needs an extension), and the honest
    /// response is to say so rather than run invisibly forever.
    bool available() const;

    /// A second launch happened. Make it obvious we are already here, since the
    /// user just asked for the application and needs to see something.
    void announcePresence();

    /// Public entry for the same dialog the menu opens, so another launch can
    /// ask for it.
    void showSettings() { openSettings(); }

private slots:
    void onChildState(ChildProcess::State state);
    void onChildExited(int exit_code, const QString& stderr_tail);
    void pollHealth();

private:
    void buildMenu();
    void updateActions();
    void applyIcon();
    void setStatusText(const QString& text);
    QString baseUrl() const;
    int  configuredPort() const;

    void openDashboard();
    void openSettings();
    void openInstallFolder();
    void saveSettings(const ConfigModel& model, bool via_api,
                      const std::vector<std::string>& restart_reasons);
    void startOver();
    void runWizard(bool wipe_first);
    void syncNow();

    ChildProcess* child_;
    QString       service_exe_;
    std::unique_ptr<QSystemTrayIcon> tray_;
    std::unique_ptr<QMenu>           menu_;
    QAction* status_action_    = nullptr;
    QAction* dashboard_action_ = nullptr;
    QAction* sync_action_      = nullptr;
    QAction* start_action_     = nullptr;
    QAction* stop_action_      = nullptr;
    QAction* restart_action_   = nullptr;
    QAction* retry_action_     = nullptr;
    QAction* autostart_action_ = nullptr;
    QTimer   health_timer_;
    std::unique_ptr<QNetworkAccessManager> net_;

    QString  version_;
    bool     healthy_ = false;
    bool     open_when_ready_ = false;   ///< dashboard asked for before it was up
    bool     ui_available_ = true;   ///< from /health; assumed until told otherwise

    /// Read from config.json rather than hardcoded. The C# tray assumed 8893,
    /// so anyone who changed web_port got a tray permanently stuck on
    /// "starting..." while the service ran perfectly well on another port.
    int cached_port_ = 0;
};

}  // namespace cpapdash::supervisor
