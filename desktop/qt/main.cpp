// SDD-016: the CpapDash supervisor.
//
// The supervisor decides whether hms_cpap can run, BEFORE running it, by
// reading config.json itself. That is the reason this program exists rather
// than a shortcut that launches a binary:
//
//     read config.json
//       |- absent / incomplete / unusable  -> open the WIZARD, then start
//       '- good                            -> start the child, sit in the tray
//
// Readiness is never inferred by launching the child and asking it over HTTP.
// On a first run there is no config, no service and no port, so there is
// nothing to ask -- and the one case that most needs an answer is exactly the
// case with nobody to answer it. Worse, spawning a doomed child and reporting
// "hms_cpap exited with code 1" tells someone their software is broken; reading
// the file, seeing no data source, and opening a window tells them what to do.

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QMessageBox>
#include <QObject>

#include <nlohmann/json.hpp>
#include <optional>

#include "ChildProcess.h"
#include "ConfigGate.h"
#include "ConfigModel.h"
#include "ConfiguratorWindow.h"
#include "SingleInstance.h"
#include "TrayShell.h"

using namespace cpapdash::supervisor;

namespace {

QString dataDir() {
    const QByteArray override_dir = qgetenv("HMS_CPAP_DATA_DIR");
    if (!override_dir.isEmpty()) return QString::fromLocal8Bit(override_dir);
    return QDir::homePath() + "/.hms-cpap";
}

QString configPath() { return dataDir() + "/config.json"; }

/// Read config.json. Absent and unparseable are different answers: the second
/// means a file exists that we must not silently overwrite.
std::optional<nlohmann::json> readConfig(bool* parse_failed) {
    *parse_failed = false;
    QFile f(configPath());
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    try {
        return nlohmann::json::parse(f.readAll().toStdString());
    } catch (const std::exception&) {
        *parse_failed = true;
        return std::nullopt;
    }
}

bool writeConfig(const nlohmann::json& doc) {
    QDir().mkpath(dataDir());
    QFile f(configPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QByteArray::fromStdString(doc.dump(2)));
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("CpapDash");
    QApplication::setOrganizationName("HMS");
    QApplication::setWindowIcon(QIcon(":/icons/app-256.png"));

    // A status-area app has no main window, so closing the wizard or the
    // settings dialog must not take the process with it.
    QApplication::setQuitOnLastWindowClosed(false);

    // Before anything else. A second supervisor would spawn a second service,
    // both would try the same port, and one would die complaining about a port
    // its own sibling was holding.
    SingleInstance instance;
    if (!instance.tryBecomePrimary()) {
        // Someone launched us again, which is a person asking for the
        // application. Tell the running copy to show itself rather than exiting
        // silently -- clicking an app and seeing nothing happen is
        // indistinguishable from it being broken.
        SingleInstance::sendToPrimary("show");
        return 0;
    }

    const QString service = ChildProcess::resolveServiceExecutable();
    if (service.isEmpty()) {
        QMessageBox::critical(nullptr, "CpapDash",
            "hms_cpap was not found next to CpapDash.\n\n"
            "This usually means the installation is incomplete. Reinstalling "
            "should fix it.");
        return 1;
    }

    ChildProcess child(service);
    TrayShell tray(&child, service);

    QObject::connect(&instance, &SingleInstance::messageReceived,
                     &tray, [&tray](const QString& verb) {
                         if (verb == "settings") tray.showSettings();
                         else                    tray.announcePresence();
                     });

    if (!tray.available()) {
        QMessageBox::warning(nullptr, "CpapDash",
            "This desktop has no system tray, so CpapDash cannot show its "
            "status icon.\n\nThe service will still run.");
    }

    // ── The gate ───────────────────────────────────────────────────────────
    bool parse_failed = false;
    const auto config = readConfig(&parse_failed);
    const auto gate   = evaluateConfig(config, parse_failed);

    if (gate.needsSetup()) {
        ConfigModel model;
        if (config.has_value()) model.loadFromDisk(*config);

        ConfiguratorWindow wizard(model, gate, service);
        if (wizard.exec() != QDialog::Accepted) {
            // Declining setup is allowed. The supervisor stays in the tray with
            // the service stopped, and Settings or Start Over can reopen this
            // later -- quitting outright would leave someone with an installed
            // application and no way back to it.
            return app.exec();
        }

        if (!writeConfig(wizard.model().current())) {
            QMessageBox::critical(nullptr, "CpapDash",
                "Could not save your settings to " + configPath());
            return app.exec();
        }
    }

    // Configured, so run it.
    child.start();
    return app.exec();
}
