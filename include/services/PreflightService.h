#pragma once
//
// PreflightService - validate the configuration BEFORE starting anything.
//
// WHY THIS EXISTS. The first cut of the Windows tray shell discovered a busy
// port by spawning hms_cpap, watching it die, and inferring the cause from how
// fast it died. Its error dialog said the port was "most likely" in use, which
// is an admission that nothing had actually checked. That is the wrong shape
// twice over: a retry loop hides a deterministic error behind timing, and the
// user is told a guess.
//
// Configuration that CAN be checked should be checked, once, up front, and
// failure should say exactly what is wrong and what to change. A port either
// binds or it does not. A database either accepts the credentials or it does
// not. A directory is either writable or it is not. None of that needs a
// supervisor, a backoff, or a second attempt to establish.
//
// WHERE IT RUNS. One implementation, four callers:
//   * `hms_cpap --preflight` exits 0 or 1 and prints a report, so an installer
//     can run it as its last step and a user can run it after editing config.
//   * startup calls it before binding anything, so a bad config fails loudly
//     instead of half-starting.
//   * the desktop shell calls it before spawning the child, so its dialog
//     names the real problem.
//   * systemd uses it as ExecStartPre.
//
// WHAT IT DOES NOT DO. It never repairs anything and never writes config. It
// also does not check things that are legitimately absent at boot: an ezShare
// card is usually unreachable when the machine starts, and treating that as a
// startup failure would refuse to run for the entire population of laptop
// users. Transient, retryable, external conditions belong to the collector.
//
#include <string>
#include <vector>

namespace hms_cpap {

struct AppConfig;

class PreflightService {
public:
    /// One validated thing.
    struct Check {
        std::string name;      ///< stable identifier, e.g. "web_port"
        bool        ok = false;
        /// What was found. Written for the person reading the log, not the
        /// developer: it names values rather than describing internals.
        std::string detail;
        /// What to do about it. Empty when ok. A failure without a remedy is
        /// how a user ends up filing a support ticket, which is precisely the
        /// outcome this is meant to prevent.
        std::string remedy;
        /// A problem that stops startup, versus one worth saying out loud but
        /// which the service can run through.
        bool        fatal = true;
    };

    struct Report {
        std::vector<Check> checks;
        /// True when no FATAL check failed. Warnings do not block startup.
        bool ok() const {
            for (const auto& c : checks) if (!c.ok && c.fatal) return false;
            return true;
        }
        /// Human-readable, one line per check, for a log or a terminal.
        std::string render() const;
    };

    /// Run every check against this config. Performs no writes and leaves no
    /// artifacts: a probe that changed the thing it inspected would be a worse
    /// version of the bug this replaces.
    static Report run(const AppConfig& cfg);

    /// Can something bind this TCP port on this address right now?
    ///
    /// Binds and immediately closes, deliberately WITHOUT SO_REUSEADDR: the
    /// question is whether the real listener will succeed, and reuse would
    /// answer a different, more optimistic question.
    static Check checkPort(const std::string& bind_addr, int port);

    /// Is the directory present and writable? Creates it when missing, because
    /// a first run legitimately has no data directory yet, and reports failure
    /// only when it cannot be created or written.
    static Check checkWritableDir(const std::string& name, const std::string& path);

    /// Can the configured database be opened with the configured credentials?
    /// Reuses the SetupService probe, so the wizard's "test connection" button
    /// and startup validation can never disagree about what "reachable" means.
    static Check checkDatabase(const AppConfig& cfg);

    /// Does the configured source have what it needs? Only the parts that are
    /// knowable locally: a local directory must exist, and the Mule and Miner
    /// needs somewhere to write. Network reachability is explicitly not checked.
    static Check checkSource(const AppConfig& cfg);

    /// Is there somewhere to put the files a downloading source produces?
    ///
    /// ezShare and Fysetc hand over raw files that have to land on disk before
    /// anything can read them. `local` and `lowenstein` read in place and need
    /// no archive at all, so this is a per-source question rather than a global
    /// one.
    ///
    /// This exists because the empty case used to be the silent one. run() only
    /// validated archive_dir when a value was already set, so a downloading
    /// source with NO archive directory produced no check, no warning and no
    /// log line. main.cpp then skips exporting CPAP_ARCHIVE_DIR for an empty
    /// value, so OSCAR archiving and SleepHQ export both switch themselves off
    /// while the dashboard keeps filling in normally. A user sees nights in
    /// hms-cpap and nothing on disk, which reads as data loss rather than as
    /// the missing setting it is. Reported as ticket 67.
    ///
    /// Not fatal, for the same reason the local_dir case is not: refusing to
    /// boot would take down the dashboard that explains the problem.
    static Check checkArchiveDir(const AppConfig& cfg);

    /// True when this source downloads files that must be written somewhere,
    /// i.e. ezShare or Fysetc. Shared so the check and its callers cannot
    /// disagree about which sources need an archive.
    static bool sourceNeedsArchive(const std::string& source);
};

}  // namespace hms_cpap
