#pragma once

#include <QObject>
#include <QProcess>
#include <QStringList>
#include <memory>

namespace cpapdash::supervisor {

/**
 * SDD-016: the supervisor owns hms_cpap.
 *
 * The OS starts the supervisor and knows nothing else. That is what collapses
 * three service backends into one implementation and makes status
 * authoritative: this class is the parent, so it does not have to ask systemd
 * or poll a port to find out whether the service is running. A port probe would
 * answer "something is listening on 8893", which is a different question and
 * gives the wrong answer on a machine with a stale process.
 *
 * Two behaviours are ported from Supervisor.cs deliberately, and one bug is
 * fixed rather than carried over.
 *
 * PORTED -- preflight gates the start. Configuration errors are deterministic:
 * a busy port, a wrong password and an unwritable folder do not become correct
 * on a second attempt, so retrying hides the cause and tells the user a guess.
 *
 * PORTED -- this is NOT a retry loop. A child that exits non-zero stays exited
 * and the reason is shown.
 *
 * FIXED -- a child that exits ZERO without being asked to stop is a SUPERVISED
 * RESTART, and gets restarted. The backend already assumes this: POST
 * /api/config/restart under HMS_CPAP_SUPERVISED=1 answers restart_mode
 * "supervised" and calls exit(0) expecting the parent to bring it back
 * (CpapController.cpp respondAndRestart). The C# supervisor never did, so
 * pressing "Restart now" in the web UI on Windows stops the service and leaves
 * the user to find the tray's Retry item.
 */
class ChildProcess : public QObject {
    Q_OBJECT

public:
    enum class State {
        Stopped,      ///< not running, and we did not just fail
        Starting,     ///< spawned, no health yet
        Running,
        Failed,       ///< exited non-zero, or could not be started
    };

    explicit ChildProcess(QString exe_path, QObject* parent = nullptr);
    ~ChildProcess() override;

    /// Absolute path of hms_cpap, resolved next to the supervisor executable.
    /// That layout IS the contract -- the Windows installer test asserts it --
    /// so a missing binary is reported rather than searched for on PATH, where
    /// we might find a different build.
    static QString resolveServiceExecutable();

    void start();
    void stop();
    void restart();

    State state()     const { return state_; }
    bool  isRunning() const { return state_ == State::Starting || state_ == State::Running; }
    qint64 pid()      const;

    /// The last lines the service wrote to stderr, newest last. Bounded, so a
    /// service that fails in a loop cannot grow the supervisor's memory.
    QStringList stderrTail() const { return stderr_tail_; }

    /// Marks the child as answering /health. Liveness and health are separate:
    /// a process can be up and not yet serving, and saying "running" for that
    /// is how a user ends up staring at a browser error.
    void setHealthy(bool healthy);

signals:
    void stateChanged(State state);
    void exited(int exit_code, const QString& stderr_tail);
    void restarting();   ///< a supervised restart was detected and honoured

private:
    void onFinished(int exit_code, QProcess::ExitStatus status);
    void onStderr();
    void setState(State s);

    QString  exe_path_;
    std::unique_ptr<QProcess> proc_;
    State    state_ = State::Stopped;
    bool     stopping_ = false;     ///< set by stop(), so an exit is not a surprise
    QStringList stderr_tail_;

    static constexpr int kStderrTailLines = 40;
};

}  // namespace cpapdash::supervisor
