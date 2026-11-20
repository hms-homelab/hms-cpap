#include "CardDrive.h"

#include "BridgeSetup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStorageInfo>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbt.h>
#include <shellapi.h>
#include <QWinEventNotifier>
#endif

namespace cpapdash::supervisor {

// ── DriveWatch ─────────────────────────────────────────────────────────────

DriveWatch::DriveWatch(QObject* parent) : QObject(parent) {
#if defined(Q_OS_MACOS)
    watcher_.addPath("/Volumes");
#elif defined(Q_OS_LINUX)
    const QString user = qEnvironmentVariable("USER");
    for (const QString& dir : {QString("/media/") + user, QString("/run/media/") + user, QString("/media")})
        if (QFileInfo(dir).isDir()) watcher_.addPath(dir);
#endif
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { emit changed(); });
    QCoreApplication::instance()->installNativeEventFilter(this);
}

DriveWatch::~DriveWatch() {
    if (auto* app = QCoreApplication::instance()) app->removeNativeEventFilter(this);
}

bool DriveWatch::nativeEventFilter(const QByteArray& type, void* message, qintptr*) {
#ifdef Q_OS_WIN
    if (type == "windows_generic_MSG") {
        const MSG* m = static_cast<const MSG*>(message);
        if (m->message == WM_DEVICECHANGE &&
            (m->wParam == DBT_DEVICEARRIVAL || m->wParam == DBT_DEVICEREMOVECOMPLETE))
            emit changed();
    }
#else
    Q_UNUSED(type);
    Q_UNUSED(message);
#endif
    return false;
}

CardVolume findCardVolume() {
    for (const QStorageInfo& v : QStorageInfo::mountedVolumes()) {
        if (!v.isValid() || !v.isReady() || v.isRoot()) continue;
        const QString root = v.rootPath();
#ifdef Q_OS_MACOS
        if (!root.startsWith("/Volumes/")) continue;
#endif
        std::vector<std::string> names;
        for (const QString& n : QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot))
            names.push_back(n.toStdString());
        if (!looksLikeCpapCard(names)) continue;
        CardVolume c;
        c.root = root;
#ifdef Q_OS_WIN
        c.device = root.left(2);   // "E:"
#else
        c.device = QString::fromLocal8Bit(v.device());
#endif
        c.name = v.displayName().isEmpty() ? root : v.displayName();
        return c;
    }
    return {};
}

// ── CardReadJob ────────────────────────────────────────────────────────────

namespace {

QString shellQuote(const QString& s) {
    QString q = s;
    q.replace("'", "'\\''");
    return "'" + q + "'";
}

QString appleScriptString(const QString& s) {
    QString q = s;
    q.replace("\\", "\\\\");
    q.replace("\"", "\\\"");
    return "\"" + q + "\"";
}

}  // namespace

CardReadJob::CardReadJob(QString service_exe, QString work_dir, QObject* parent)
    : QObject(parent), service_exe_(std::move(service_exe)), work_dir_(std::move(work_dir)) {
    connect(&watch_, &QFileSystemWatcher::directoryChanged, this, &CardReadJob::readProgress);
    connect(&watch_, &QFileSystemWatcher::fileChanged, this, &CardReadJob::readProgress);
}

CardReadJob::~CardReadJob() {
#ifdef Q_OS_WIN
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
#endif
}

QString CardReadJob::outPath() const { return work_dir_ + "/card.cpapcard"; }

void CardReadJob::start(const CardVolume& card, const QString& prompt) {
    if (running_) return;
    QDir().mkpath(work_dir_);
    const QString progress = work_dir_ + "/card-read.json";
    QFile::remove(progress);
    watch_.addPath(work_dir_);
    running_ = true;

    const QStringList args = {"--read-card", card.device, "--card-root", card.root,
                              "--out", outPath(), "--progress", progress};
#if defined(Q_OS_WIN)
    QString params;
    for (const QString& a : args) params += "\"" + QString(a).replace("\"", "") + "\" ";
    const std::wstring file = QDir::toNativeSeparators(service_exe_).toStdWString();
    const std::wstring par  = params.toStdWString();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof sei;
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb       = L"runas";
    sei.lpFile       = file.c_str();
    sei.lpParameters = par.c_str();
    sei.nShow        = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        running_ = false;
        const bool declined = GetLastError() == ERROR_CANCELLED;
        emit finished(false, declined, QString(), {});
        return;
    }
    handle_   = sei.hProcess;
    notifier_ = new QWinEventNotifier(sei.hProcess, this);
    connect(notifier_, &QWinEventNotifier::activated, this, [this] {
        notifier_->setEnabled(false);
        DWORD code = 1;
        GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
        ended(static_cast<int>(code));
    });
#else
    proc_ = std::make_unique<QProcess>();
    connect(proc_.get(), &QProcess::finished, this, [this](int code, QProcess::ExitStatus) { ended(code); });
    connect(proc_.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) ended(-1);
    });
#if defined(Q_OS_MACOS)
    QString cmd = shellQuote(service_exe_);
    for (const QString& a : args) cmd += " " + shellQuote(a);
    proc_->start("/usr/bin/osascript",
                 {"-e", "do shell script " + appleScriptString(cmd) + " with administrator privileges with prompt " +
                            appleScriptString(prompt)});
#else
    Q_UNUSED(prompt);
    proc_->start("pkexec", QStringList{service_exe_} + args);
#endif
#endif
}

void CardReadJob::readProgress() {
    QFile f(work_dir_ + "/card-read.json");
    if (!f.open(QIODevice::ReadOnly)) return;
    const auto o = QJsonDocument::fromJson(f.readAll()).object();
    const QString phase = o.value("phase").toString();
    if (phase == "layout" || phase == "files")
        emit progress(phase, o.value("done").toInteger(), o.value("total").toInteger());
}

void CardReadJob::ended(int exit_code) {
    if (!running_) return;
    running_ = false;
    watch_.removePaths(watch_.directories());
    QJsonObject answer;
    QFile f(work_dir_ + "/card-read.json");
    if (f.open(QIODevice::ReadOnly)) answer = QJsonDocument::fromJson(f.readAll()).object();
    const QString phase = answer.value("phase").toString();
    if (exit_code == 0 && phase == "done" && QFileInfo::exists(outPath())) {
        emit finished(true, false, QString(), answer);
        return;
    }
    // No answer from the reader at all: it never ran, which is the
    // permission being declined (osascript's -128, pkexec's 126) or the
    // reader missing.
    bool declined = false;
    if (phase.isEmpty()) {
#if !defined(Q_OS_WIN)
        const QString err = proc_ ? QString::fromLocal8Bit(proc_->readAllStandardError()) : QString();
        declined = err.contains("-128") || exit_code == 126 || exit_code == 127;
#endif
    }
    emit finished(false, declined, answer.value("error").toString(), answer);
}

}  // namespace cpapdash::supervisor
