#include "ChildProcess.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

#ifdef Q_OS_WIN
/// The child must die with the supervisor even when the supervisor is killed
/// outright.
///
/// This is not defensive programming, it is a bug that already shipped: killing
/// the C# shell from Task Manager left hms_cpap alive holding port 8893, so the
/// NEXT launch failed preflight with a conflict the application had caused
/// itself. The user sees a service that refuses to start for no reason.
///
/// A job object with KILL_ON_JOB_CLOSE is the only mechanism that survives
/// TerminateProcess: cleanup handlers, atexit and destructors are all skipped,
/// but the kernel closes the process's handles regardless, and closing the last
/// handle to the job kills everything in it. So the handle is held for the life
/// of the supervisor and never closed early -- closing it IS the kill.
class JobObject {
public:
    JobObject() {
        handle_ = ::CreateJobObjectW(nullptr, nullptr);
        if (!handle_) return;

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(handle_, JobObjectExtendedLimitInformation,
                                       &info, sizeof(info))) {
            // Degrade rather than refuse to run: no guarantee is worse than a
            // guarantee, but far better than a supervisor that will not start.
            ::CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    ~JobObject() { if (handle_) ::CloseHandle(handle_); }

    bool adopt(qint64 pid) {
        if (!handle_ || pid <= 0) return false;
        HANDLE proc = ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                    FALSE, static_cast<DWORD>(pid));
        if (!proc) return false;
        const bool ok = ::AssignProcessToJobObject(handle_, proc);
        ::CloseHandle(proc);
        return ok;
    }

    bool valid() const { return handle_ != nullptr; }

private:
    HANDLE handle_ = nullptr;
};

/// One job for the supervisor's lifetime. A per-spawn job would be destroyed on
/// restart and take the new child with it.
JobObject& jobObject() {
    static JobObject job;
    return job;
}
#endif  // Q_OS_WIN

}  // namespace

namespace cpapdash::supervisor {

ChildProcess::ChildProcess(QString exe_path, QObject* parent)
    : QObject(parent), exe_path_(std::move(exe_path)) {}

ChildProcess::~ChildProcess() {
    // The destructor is the last line of defence, not the plan. If we get here
    // with a live child, killing it is still better than leaving it holding the
    // port after the window that owns it has gone.
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        stopping_ = true;
        proc_->kill();
        proc_->waitForFinished(5000);
    }
}

QString ChildProcess::resolveServiceExecutable() {
    const QString dir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    const QString name = "hms_cpap.exe";
#else
    const QString name = "hms_cpap";
#endif

    // Installed layout: the service sits beside the supervisor.
    QFileInfo beside(QDir(dir).filePath(name));
    if (beside.exists() && beside.isExecutable()) return beside.absoluteFilePath();

#ifdef Q_OS_MACOS
    // Development layout: the supervisor is inside CpapDashDesktop.app/Contents/
    // MacOS/ and the service is in the build directory three levels up. Without
    // this, running from a build tree on a Mac finds nothing and the supervisor
    // is untestable exactly where it is being written.
    QFileInfo up(QDir(dir).filePath("../../../" + name));
    if (up.exists() && up.isExecutable()) return up.absoluteFilePath();
#endif

    return {};
}

qint64 ChildProcess::pid() const {
    return proc_ ? proc_->processId() : 0;
}

void ChildProcess::setState(State s) {
    if (state_ == s) return;
    state_ = s;
    emit stateChanged(s);
}

void ChildProcess::setHealthy(bool healthy) {
    if (!isRunning()) return;
    setState(healthy ? State::Running : State::Starting);
}

void ChildProcess::start() {
    if (proc_ && proc_->state() != QProcess::NotRunning) return;

    stopping_ = false;
    stderr_tail_.clear();

    proc_ = std::make_unique<QProcess>();
    proc_->setProgram(exe_path_);

    QStringList args{"--no-browser"};
#ifndef Q_OS_WIN
    // The die-with-parent half that has no kernel behind it. The child watches
    // its stdin; QProcess holds the write end, so our death closes the pipe and
    // it exits. On Windows the Job Object does this instead, enforced by the
    // kernel and proven by the CI orphan assertion.
    args << "--exit-with-parent";
#endif
    proc_->setArguments(args);

    auto env = QProcessEnvironment::systemEnvironment();
    // The contract the service already reads: exit(0) on "apply settings"
    // instead of re-execing, and do not install an autostart entry of its own,
    // because the supervisor owns both.
    env.insert("HMS_CPAP_SUPERVISED", "1");
    proc_->setProcessEnvironment(env);

    proc_->setProcessChannelMode(QProcess::SeparateChannels);

#ifdef Q_OS_LINUX
    // Kernel-enforced: when we die, the child gets SIGTERM. Runs in the child
    // between fork and exec.
    proc_->setChildProcessModifier([] {
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        // The parent could have died in the window between fork and the prctl
        // above, in which case the signal we just asked for will never arrive
        // and this child would outlive it. Checking afterwards closes that.
        if (::getppid() == 1) ::_exit(0);
    });
#endif

    connect(proc_.get(), &QProcess::readyReadStandardError,
            this, &ChildProcess::onStderr);
    connect(proc_.get(), &QProcess::finished,
            this, &ChildProcess::onFinished);

    proc_->start();
    if (!proc_->waitForStarted(10000)) {
        setState(State::Failed);
        emit exited(-1, "could not start " + exe_path_ + ": " + proc_->errorString());
        return;
    }

#ifdef Q_OS_WIN
    // Immediately after start, so the window in which a kill could orphan the
    // child is as small as the API allows. A failure here is logged rather than
    // fatal: the service is running and useful, it just loses the guarantee.
    if (!jobObject().adopt(proc_->processId())) {
        qWarning("CpapDash: could not put hms_cpap in a job object; it may "
                 "outlive this supervisor if the supervisor is force-killed");
    }
#endif

    setState(State::Starting);
}

void ChildProcess::stop() {
    if (!proc_ || proc_->state() == QProcess::NotRunning) {
        setState(State::Stopped);
        return;
    }
    stopping_ = true;

    proc_->terminate();
    if (!proc_->waitForFinished(5000)) {
        // It is not coming down politely. Killing it is correct: leaving it
        // alive means the next start fails preflight on a port we are holding.
        proc_->kill();
        proc_->waitForFinished(3000);
    }
    setState(State::Stopped);
}

void ChildProcess::restart() {
    stop();
    start();
}

void ChildProcess::onStderr() {
    if (!proc_) return;
    const QString chunk = QString::fromUtf8(proc_->readAllStandardError());
    for (const QString& line : chunk.split('\n', Qt::SkipEmptyParts)) {
        stderr_tail_.append(line.trimmed());
        while (stderr_tail_.size() > kStderrTailLines) stderr_tail_.removeFirst();
    }
}

void ChildProcess::onFinished(int exit_code, QProcess::ExitStatus status) {
    onStderr();   // drain whatever it said on the way out

    if (stopping_) {
        setState(State::Stopped);
        return;
    }

    // A clean exit we did not ask for is the service telling us to bring it
    // back: it took a settings change that needs a restart and stepped aside
    // for us. See the class comment -- the C# supervisor missed this and turned
    // "Restart now" in the web UI into a stopped service.
    if (status == QProcess::NormalExit && exit_code == 0) {
        emit restarting();
        start();
        return;
    }

    setState(State::Failed);
    emit exited(exit_code, stderr_tail_.join('\n'));
}

}  // namespace cpapdash::supervisor
