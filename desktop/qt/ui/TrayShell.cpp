#include "TrayShell.h"

#include <QApplication>
#include <QStyle>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>
#include <QMessageBox>
#include <QEventLoop>
#include <QCoreApplication>
#include <QProcess>
#include <QAbstractButton>
#include <QPushButton>

#include "SettingsDialog.h"
#include "ConfiguratorWindow.h"
#include "StartupWindow.h"
#include "ConfigGate.h"
#include "Autostart.h"

#include <nlohmann/json.hpp>
#include <optional>

namespace cpapdash::supervisor {

namespace {

constexpr int kDefaultPort   = 8893;
constexpr int kHealthPollMs  = 5000;

/// The user's data directory, matching AppConfig::dataDir().
QString dataDir() {
    const QByteArray override_dir = qgetenv("HMS_CPAP_DATA_DIR");
    if (!override_dir.isEmpty()) return QString::fromLocal8Bit(override_dir);
    return QDir::homePath() + "/.hms-cpap";
}

/// Icon family. macOS gets the monochrome template; everyone else gets colour.
///
/// Falls back rather than returning nothing. A missing resource makes
/// QSystemTrayIcon show NO icon at all -- it logs "No Icon set" and the user
/// gets an invisible menu bar item they cannot click, which reads as "the app
/// did not start". A wrong-looking icon is a bug report; an absent one is a
/// support ticket about something that is actually running fine.
QIcon stateIcon(const char* state) {
#ifdef Q_OS_MACOS
    {
        QIcon mono(QStringLiteral(":/icons/mono-%1.png").arg(state));
        mono.addFile(QStringLiteral(":/icons/mono-%1@2x.png").arg(state));
        if (!mono.isNull()) {
            // Tells macOS this is a template: it applies the menu bar's own
            // colour, inverts it for dark mode, and highlights it correctly
            // when the menu is open. Without this the icon is a fixed-colour
            // blob that looks wrong in one appearance and unreadable in the
            // other.
            mono.setIsMask(true);
            return mono;
        }
        qWarning("CpapDash: the macOS template icons are missing from the "
                 "resource bundle; falling back to the colour set");
    }
#endif
    QIcon icon(QStringLiteral(":/icons/tray-%1.png").arg(state));
    icon.addFile(QStringLiteral(":/icons/tray-%1@2x.png").arg(state));
    if (!icon.isNull()) return icon;

    qWarning("CpapDash: no tray icons in the resource bundle at all");
    return QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
}

}  // namespace

TrayShell::TrayShell(ChildProcess* child, QString service_exe, QObject* parent)
    : QObject(parent), child_(child), service_exe_(std::move(service_exe)) {
    net_ = std::make_unique<QNetworkAccessManager>(this);

    tray_ = std::make_unique<QSystemTrayIcon>(this);
    buildMenu();
    tray_->setContextMenu(menu_.get());
    tray_->setToolTip("CpapDash");
    applyIcon();
    tray_->show();

    connect(child_, &ChildProcess::stateChanged, this, &TrayShell::onChildState);
    connect(child_, &ChildProcess::exited,       this, &TrayShell::onChildExited);
    connect(child_, &ChildProcess::restarting,   this, [this] {
        setStatusText("Status: restarting to apply settings");
    });

    connect(tray_.get(), &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                // Double click opens the dashboard on Windows and Linux. On
                // macOS a click opens the menu and never reaches here, which is
                // the platform's convention rather than a gap.
                if (reason == QSystemTrayIcon::DoubleClick) openDashboard();
            });

    connect(&health_timer_, &QTimer::timeout, this, &TrayShell::pollHealth);
    health_timer_.start(kHealthPollMs);
}

void TrayShell::announcePresence() {
    // There is no window to raise -- this lives in the status area -- so the
    // honest answer is to point at where it already is.
    tray_->showMessage("CpapDash is already running",
                       "Look for the CpapDash icon in the menu bar.",
                       QSystemTrayIcon::Information, 4000);
}

bool TrayShell::available() const {
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void TrayShell::buildMenu() {
    menu_ = std::make_unique<QMenu>();

    auto* header = menu_->addAction("CpapDash");
    header->setEnabled(false);

    menu_->addSeparator();

    status_action_ = menu_->addAction("Status: starting...");
    status_action_->setEnabled(false);

    // Disabled until the service actually answers /health. Offering a link that
    // lands on a connection error -- or on a 404 from a service that is up but
    // not yet serving the UI -- teaches people the app is broken when it is
    // merely still starting.
    // Both stay enabled at all times, deliberately. Greying them out as the
    // service starts and stops means the menu changes under the user between
    // one glance and the next, and an item that is disabled at the moment they
    // reach for it looks broken. They are always clickable, and each says what
    // is wrong at the point it is asked -- which is when the user actually
    // wants to know.
    dashboard_action_ = menu_->addAction("Open Dashboard", this, &TrayShell::openDashboard);
    sync_action_ = menu_->addAction("Sync Now", this, &TrayShell::syncNow);

    menu_->addSeparator();

    // Start and Stop are the point of a supervisor: it owns the process, so it
    // is the one thing that can honestly offer to control it. Exactly one of
    // them is ever visible, because a greyed-out pair asks the user to work out
    // which half applies.
    start_action_ = menu_->addAction("Start", this, [this] { child_->start(); });
    stop_action_  = menu_->addAction("Stop",  this, [this] { child_->stop();  });

    restart_action_ = menu_->addAction("Restart", this, [this] {
        setStatusText("Status: restarting...");
        child_->restart();
    });

    retry_action_ = menu_->addAction("Retry", this, [this] {
        retry_action_->setVisible(false);
        setStatusText("Status: starting...");
        child_->restart();
    });
    retry_action_->setVisible(false);

    menu_->addSeparator();

    // Settings, NOT "open config folder". Handing someone a folder full of JSON
    // is telling them to edit the file themselves, which is the thing this
    // application exists to stop.
    menu_->addAction("Settings...", this, &TrayShell::openSettings);

    // A visible menu item. Buried inside the settings dialog it was effectively
    // hidden -- Qt moves ResetRole buttons around per platform, and on macOS it
    // landed somewhere nobody would look.
    menu_->addAction("Start Over...", this, &TrayShell::startOver);

    // Checkable, reflecting what is actually on disk rather than what we last
    // set -- the installer writes the same Windows Run key, so the two must
    // agree or the tick lies about the state of the machine.
    if (autostart::isSupported()) {
        autostart_action_ = menu_->addAction("Start at Login");
        autostart_action_->setCheckable(true);
        autostart_action_->setChecked(autostart::isEnabled());
        connect(autostart_action_, &QAction::toggled, this, [this](bool on) {
            QString err;
            if (!autostart::setEnabled(on, &err)) {
                // Put the tick back: it must show what IS, not what was asked
                // for. A checkbox that stays ticked after a failed write is a
                // user believing CpapDash starts at login when it does not.
                QSignalBlocker block(autostart_action_);
                autostart_action_->setChecked(!on);
                QMessageBox::warning(nullptr, "CpapDash",
                    err.isEmpty() ? QStringLiteral("Could not change your startup items.") : err);
            }
        });
    }

    menu_->addSeparator();
    menu_->addAction("Open Installation Folder", this, &TrayShell::openInstallFolder);

    menu_->addSeparator();
    menu_->addAction("Quit", this, [] { QApplication::quit(); });

    updateActions();
}

/// One place decides what the menu offers, driven by what is actually true.
void TrayShell::updateActions() {
    const bool running = child_->isRunning();

    if (start_action_)   start_action_->setVisible(!running);
    if (stop_action_)    stop_action_->setVisible(running);
    if (restart_action_) restart_action_->setVisible(running);

    // Both of these talk HTTP to the service, so both need it answering --
    // not merely spawned.
    // Open Dashboard and Sync Now are never disabled -- see buildMenu(). What
    // they cannot do, they explain when clicked.
}

void TrayShell::applyIcon() {
    const char* state = "stopped";
    switch (child_->state()) {
        case ChildProcess::State::Running:  state = healthy_ ? "running" : "starting"; break;
        case ChildProcess::State::Starting: state = "starting"; break;
        case ChildProcess::State::Failed:   state = "problem";  break;
        case ChildProcess::State::Stopped:  state = "stopped";  break;
    }
    tray_->setIcon(stateIcon(state));
}

void TrayShell::setStatusText(const QString& text) {
    if (status_action_) status_action_->setText(text);
    tray_->setToolTip("CpapDash - " + text.mid(text.indexOf(' ') + 1));
}

int TrayShell::configuredPort() const {
    // Re-read every time rather than caching for the process lifetime: the port
    // can change while we are running, and a stale value points the dashboard
    // menu item at a dead URL.
    QFile f(dataDir() + "/config.json");
    if (!f.open(QIODevice::ReadOnly)) return kDefaultPort;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return kDefaultPort;
    const int port = doc.object().value("web_port").toInt(kDefaultPort);
    return (port > 0 && port < 65536) ? port : kDefaultPort;
}

QString TrayShell::baseUrl() const {
    return QStringLiteral("http://localhost:%1").arg(configuredPort());
}

void TrayShell::openDashboard() {
    // Not running: the user asked for the dashboard, so offer the thing that
    // would produce one rather than refusing.
    if (!child_->isRunning()) {
        if (QMessageBox::question(nullptr, "CpapDash",
                "CpapDash is not running, so there is no dashboard to open.\n\n"
                "Start it now?",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
            == QMessageBox::Yes) {
            child_->start();
            // Opened once it answers, rather than immediately into a connection
            // error -- the service needs a moment to bind and serve.
            open_when_ready_ = true;
        }
        return;
    }

    if (!healthy_) {
        tray_->showMessage("CpapDash",
                           "CpapDash is still starting. The dashboard will be "
                           "ready in a moment.",
                           QSystemTrayIcon::Information, 4000);
        open_when_ready_ = true;
        return;
    }

    if (!ui_available_) {
        QMessageBox::warning(nullptr, "CpapDash",
            "CpapDash is running, but the dashboard files are missing from this "
            "installation.\n\n"
            "Reinstalling should restore them. Your therapy data is not affected.");
        return;
    }

    QDesktopServices::openUrl(QUrl(baseUrl()));
}

/// Reveal where CpapDash is actually installed.
///
/// Not the data folder -- that holds config.json and the therapy database and
/// has its own life. This is the program itself: the place a support request
/// asks about, and where hms_cpap sits next to the supervisor.
void TrayShell::openInstallFolder() {
#ifdef Q_OS_MACOS
    // applicationDirPath() is CpapDashDesktop.app/Contents/MacOS, which is the
    // inside of the bundle and not somewhere a person thinks of as a folder.
    // Reveal the .app itself, the way Finder presents it.
    QDir dir(QCoreApplication::applicationDirPath());
    dir.cdUp(); dir.cdUp(); dir.cdUp();          // MacOS -> Contents -> .app -> parent
    const QString bundle = QDir(QCoreApplication::applicationDirPath())
                               .absoluteFilePath("../../..");
    QProcess::startDetached("open", {"-R", QDir::cleanPath(bundle)});
#else
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QCoreApplication::applicationDirPath()));
#endif
}

void TrayShell::openSettings() {
    // Load from the API while the service is up, so we see what it actually has
    // -- including secrets as masks. When it is down, read the file, which is
    // the only source there is.
    ConfigModel model;
    bool from_api = false;

    if (healthy_) {
        QNetworkRequest req{QUrl(baseUrl() + "/api/config")};
        req.setTransferTimeout(5000);
        auto* reply = net_->get(req);
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        if (reply->error() == QNetworkReply::NoError) {
            try {
                model.loadFromApi(nlohmann::json::parse(reply->readAll().toStdString()));
                from_api = true;
            } catch (const std::exception&) { /* fall through to the file */ }
        }
        reply->deleteLater();
    }

    if (!from_api) {
        QFile f(dataDir() + "/config.json");
        if (f.open(QIODevice::ReadOnly)) {
            try {
                model.loadFromDisk(nlohmann::json::parse(f.readAll().toStdString()));
            } catch (const std::exception&) {
                // An unparseable config is the ONE case where writing over it
                // would be wrong: the user hand-edited it and still wants what
                // is in there. Say so and stop.
                QMessageBox::critical(nullptr, "CpapDash",
                    "config.json could not be read, so settings cannot be shown "
                    "without overwriting it.\n\nFix the file, or use Start Over "
                    "to begin again.");
                return;
            }
        }
    }

    SettingsDialog dialog(model, healthy_);
    connect(&dialog, &SettingsDialog::startOverRequested, &dialog, [this, &dialog] {
        dialog.reject();
        startOver();
    });

    if (dialog.exec() != QDialog::Accepted) return;
    saveSettings(dialog.model(), from_api, dialog.restartReasons());
}

void TrayShell::saveSettings(const ConfigModel& model, bool via_api,
                             const std::vector<std::string>& restart_reasons) {
    if (via_api) {
        // The service is up, so IT stays the only writer of config.json and we
        // are a client of the same endpoint the web UI uses. No window exists
        // in which two processes could disagree about what is on disk.
        const auto patch = model.toPatch();
        if (patch.empty() && restart_reasons.empty()) return;

        QNetworkRequest req{QUrl(baseUrl() + "/api/config")};
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setTransferTimeout(8000);
        auto* reply = net_->sendCustomRequest(
            req, "PUT", QByteArray::fromStdString(patch.dump()));

        connect(reply, &QNetworkReply::finished, this, [this, reply, restart_reasons] {
            const bool ok = reply->error() == QNetworkReply::NoError;
            reply->deleteLater();
            if (!ok) {
                QMessageBox::warning(nullptr, "CpapDash", "The settings could not be saved.");
                return;
            }
            if (restart_reasons.empty()) {
                tray_->showMessage("CpapDash", "Settings saved.",
                                   QSystemTrayIcon::Information, 3000);
                return;
            }
            QString what;
            for (const auto& r : restart_reasons) what += "\n  - " + QString::fromStdString(r);
            if (QMessageBox::question(nullptr, "Restart to apply?",
                    "Saved. These only take effect after a restart:" + what +
                    "\n\nRestart now?") == QMessageBox::Yes) {
                child_->restart();
            }
        });
        return;
    }

    // The service is down and cannot be started, so there is no API to call and
    // no web UI to reach -- and nothing else can be writing the file either.
    bool safe = true;
    const auto doc = model.toDiskJson(&safe);
    if (!safe) {
        // A secret we only ever saw as a mask would be written verbatim as the
        // literal "********". Silent credential loss; refuse instead.
        QMessageBox::critical(nullptr, "CpapDash",
            "These settings came from the running service and include hidden "
            "passwords, so they cannot be written to disk directly.\n\n"
            "Start the service and try again.");
        return;
    }

    QDir().mkpath(dataDir());
    QFile f(dataDir() + "/config.json");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(nullptr, "CpapDash",
            "Could not write to " + f.fileName());
        return;
    }
    f.write(QByteArray::fromStdString(doc.dump(2)));
    f.close();

    tray_->showMessage("CpapDash", "Settings saved. Starting the service...",
                       QSystemTrayIcon::Information, 3000);
    child_->start();
}

void TrayShell::startOver() {
    // Two very different things get called "start over", and conflating them
    // destroys therapy history. The uninstaller already refuses to touch
    // ~/.hms-cpap for exactly this reason, so this will not be the thing that
    // does it by accident.
    QMessageBox box;
    box.setWindowTitle("Start Over");
    box.setIcon(QMessageBox::Warning);
    box.setText("What should be cleared?");
    box.setInformativeText(
        "Settings only keeps every night already recorded, and just runs setup "
        "again.\n\n"
        "Everything also deletes your therapy database. Nights that have not "
        "been uploaded anywhere cannot be recovered.");

    QPushButton* settings_only = box.addButton("Settings only", QMessageBox::AcceptRole);
    QPushButton* everything    = box.addButton("Everything",    QMessageBox::DestructiveRole);
    box.addButton("Cancel", QMessageBox::RejectRole);
    box.setDefaultButton(settings_only);
    box.exec();

    QAbstractButton* clicked = box.clickedButton();
    if (clicked != settings_only && clicked != everything) return;
    const bool wipe_data = (clicked == everything);

    if (wipe_data) {
        // A second, explicit confirmation. One misclick should not be able to
        // delete a year of someone's medical record.
        if (QMessageBox::warning(nullptr, "Delete therapy history?",
                "This permanently deletes every night CpapDash has recorded on "
                "this computer.\n\nThis cannot be undone. Continue?",
                QMessageBox::Cancel | QMessageBox::Yes,
                QMessageBox::Cancel) != QMessageBox::Yes) {
            return;
        }
    }

    child_->stop();

    const QString dir = dataDir();
    QFile::remove(dir + "/config.json");

    if (wipe_data) {
        // Only the files CpapDash owns. Removing the whole directory would take
        // anything else the user put there with it.
        for (const QString& name : {"cpap.db", "cpap.db-wal", "cpap.db-shm"})
            QFile::remove(dir + "/" + name);
    }

    // Straight into setup. Clearing the configuration and then leaving someone
    // staring at a stopped service would be a worse place than they started;
    // "start over" means start over, so the wizard opens.
    runWizard(/*wipe_first=*/false);
}

/// Opens the configurator and, if it is completed, writes the result and starts
/// the service. Shared by Start Over and by a first run.
void TrayShell::runWizard(bool wipe_first) {
    if (wipe_first) {
        child_->stop();
        QFile::remove(dataDir() + "/config.json");
    }

    bool parse_failed = false;
    std::optional<nlohmann::json> config;
    QFile f(dataDir() + "/config.json");
    if (f.open(QIODevice::ReadOnly)) {
        try { config = nlohmann::json::parse(f.readAll().toStdString()); }
        catch (const std::exception&) { parse_failed = true; }
    }

    const auto gate = evaluateConfig(config, parse_failed);

    ConfigModel model;
    if (config.has_value()) model.loadFromDisk(*config);

    ConfiguratorWindow wizard(model, gate, service_exe_);
    if (wizard.exec() != QDialog::Accepted) {
        updateActions();
        applyIcon();
        return;
    }

    QDir().mkpath(dataDir());
    QFile out(dataDir() + "/config.json");
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(nullptr, "CpapDash",
            "Could not save your settings to " + out.fileName());
        return;
    }
    out.write(QByteArray::fromStdString(wizard.model().current().dump(2)));
    out.close();

    // Finishing setup used to hand straight back to the menu bar while the
    // service spent several seconds checking its config and building its
    // schema. The tray showed a stale menu and nothing said what was going on,
    // so "did that work?" had no answer. Watch it come up instead.
    child_->stop();
    StartupWindow startup(child_, service_exe_, configuredPort());
    connect(&startup, &StartupWindow::settingsRequested, this, [this] {
        QTimer::singleShot(0, this, &TrayShell::openSettings);
    });
    startup.exec();

    updateActions();
    applyIcon();
}

void TrayShell::syncNow() {
    if (!healthy_) {
        tray_->showMessage("CpapDash",
                           "The service is not answering yet, so there is nothing to sync.",
                           QSystemTrayIcon::Warning, 4000);
        return;
    }
    QNetworkRequest req{QUrl(baseUrl() + "/api/sync/now")};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto* reply = net_->post(req, QByteArray("{}"));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QString body = QString::fromUtf8(reply->readAll());
        reply->deleteLater();
        tray_->showMessage("CpapDash",
                           body.contains("already") ? "A sync is already running."
                                                    : "Sync requested.",
                           QSystemTrayIcon::Information, 3000);
    });
}

void TrayShell::pollHealth() {
    if (!child_->isRunning()) {
        healthy_ = false;
        return;
    }
    QNetworkRequest req{QUrl(baseUrl() + "/health")};
    req.setTransferTimeout(3000);
    auto* reply = net_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const bool ok = reply->error() == QNetworkReply::NoError;
        const auto body = reply->readAll();
        reply->deleteLater();

        if (ok) {
            const auto doc = QJsonDocument::fromJson(body);
            if (doc.isObject()) {
                const auto o = doc.object();
                version_ = o.value("version").toString();
                // /health reports whether the dashboard is servable, so the
                // supervisor does not have to make a second request and guess
                // from a status code. An older service that predates the field
                // has no opinion; assume the bundle is there rather than
                // disabling a menu item that probably works.
                ui_available_ = o.value("ui").toBool(true);
            }
        }

        // Liveness and health are different questions and are kept apart on
        // purpose: the process being up does not mean it is serving, and
        // reporting "running" for that is how someone ends up staring at a
        // browser error wondering what they broke.
        if (healthy_ != ok) {
            healthy_ = ok;
            child_->setHealthy(ok);
            applyIcon();
            updateActions();
        }
        // Someone asked for the dashboard while it was still coming up. Honour
        // it now rather than making them ask twice.
        if (ok && open_when_ready_ && ui_available_) {
            open_when_ready_ = false;
            QDesktopServices::openUrl(QUrl(baseUrl()));
        }
        if (child_->isRunning()) {
            setStatusText(healthy_
                ? (version_.isEmpty() ? "Status: running"
                                      : "Status: running (" + version_ + ")")
                : "Status: starting...");
        }
    });
}

void TrayShell::onChildState(ChildProcess::State state) {
    applyIcon();
    // Without this the menu keeps whatever it was built with: Start still
    // offered while the service is coming up, Stop missing once it is running,
    // and Open Dashboard greyed out long after the dashboard exists.
    updateActions();
    switch (state) {
        case ChildProcess::State::Starting: setStatusText("Status: starting..."); break;
        case ChildProcess::State::Running:  setStatusText("Status: running");     break;
        case ChildProcess::State::Stopped:  setStatusText("Status: stopped");     break;
        case ChildProcess::State::Failed:   break;   // onChildExited says why
    }
}

void TrayShell::onChildExited(int exit_code, const QString& stderr_tail) {
    healthy_ = false;
    ui_available_ = true;   // re-answered by the next /health
    applyIcon();
    updateActions();
    setStatusText(QStringLiteral("Status: stopped (exit %1)").arg(exit_code));
    retry_action_->setVisible(true);

    // A balloon, NOT a modal dialog. The service can exit hours after the user
    // last touched anything, and stealing focus for something they did not just
    // do is worse than the problem -- a decision TrayApplication.cs made
    // explicitly and worth keeping.
    const QString last = stderr_tail.section('\n', -1).trimmed();
    tray_->showMessage("CpapDash stopped",
                       last.isEmpty() ? QStringLiteral("hms_cpap exited with code %1.").arg(exit_code)
                                      : last,
                       QSystemTrayIcon::Critical, 10000);
}

}  // namespace cpapdash::supervisor
