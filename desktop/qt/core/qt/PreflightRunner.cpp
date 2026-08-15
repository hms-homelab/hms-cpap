#include "PreflightRunner.h"

#include <QProcess>
#include <QFileInfo>

namespace cpapdash::supervisor {

PreflightReport runPreflight(const QString& exe,
                             const QString& config_path,
                             int timeout_ms) {
    if (exe.isEmpty() || !QFileInfo::exists(exe)) {
        PreflightReport r;
        r.verdict = Verdict::Unreadable;
        r.message = exe.isEmpty()
            ? QStringLiteral("hms_cpap was not found next to CpapDash.").toStdString()
            : ("hms_cpap is missing: " + exe).toStdString();
        return r;
    }

    QStringList args{"--preflight"};
    if (!config_path.isEmpty()) args << "--config" << config_path;

    QProcess p;
    p.setProgram(exe);
    p.setArguments(args);
    p.setWorkingDirectory(QFileInfo(exe).absolutePath());
    p.start();

    if (!p.waitForStarted(5000)) {
        PreflightReport r;
        r.verdict = Verdict::Unreadable;
        r.message = ("Could not run the configuration check: " + p.errorString()).toStdString();
        return r;
    }

    if (!p.waitForFinished(timeout_ms)) {
        // Kill the tree, not just the process: preflight opens a database
        // connection, and a stuck one can leave a child of its own.
        p.kill();
        p.waitForFinished(2000);
        PreflightReport r;
        r.verdict = Verdict::Unreadable;
        r.message = QStringLiteral("The configuration check did not finish in %1 seconds.")
                        .arg(timeout_ms / 1000).toStdString();
        return r;
    }

    const auto out = QString::fromUtf8(p.readAllStandardOutput()).toStdString();
    const auto err = QString::fromUtf8(p.readAllStandardError()).toStdString();
    return parsePreflight(out, err, p.exitCode());
}

}  // namespace cpapdash::supervisor
