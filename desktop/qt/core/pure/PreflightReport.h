#pragma once

#include <string>
#include <vector>

namespace cpapdash::supervisor {

/**
 * SDD-016: the supervisor's reading of `hms_cpap --preflight`.
 *
 * The checks are NOT reimplemented here. The service already owns what "usable
 * configuration" means -- it binds the port, writes to the data directory and
 * opens the database for real -- and a second opinion compiled into a GUI would
 * eventually disagree with the thing that actually has to start. So the
 * supervisor runs the service and reads its answer.
 *
 * What it needs from the text is structure: which check failed, what it said,
 * and what to do about it, so the configurator can put the remedy next to the
 * field that caused it instead of dumping a wall of output in a message box the
 * way the C# tray does today.
 *
 * The format, from PreflightService::Report::render():
 *
 *     Preflight checks:
 *       ok    data_dir: /home/me/.hms-cpap is writable
 *       FAIL  web_port: port 8893 is already in use
 *             -> Stop whatever is listening on 8893, or set "web_port" ...
 *       warn  archive_dir: source 'ezshare' downloads files but no archive ...
 *             -> Set the Archive Directory in Settings under Data Source ...
 *     FAILED: fix the items marked FAIL above.
 *
 * An 8-character status field, then `name: detail`, then an optional remedy
 * line indented 8 spaces and introduced by `-> `. Two things about that are
 * easy to get wrong and are pinned by tests: a detail may itself contain a
 * colon (archive_dir's does), so only the FIRST one separates name from detail;
 * and `warn` means "not ok but not fatal", which is a third state rather than a
 * prettier failure -- preflight exits 0 with warnings present.
 */

enum class CheckStatus {
    Ok,
    Warn,   ///< a real problem, but the service will still start
    Fail,   ///< the service refuses to start until it is fixed
};

struct PreflightCheck {
    CheckStatus status = CheckStatus::Ok;
    std::string name;     ///< "web_port", "database", ...
    std::string detail;   ///< the sentence after the name
    std::string remedy;   ///< the "-> ..." line, empty when there is none

    bool blocking() const { return status == CheckStatus::Fail; }
};

enum class Verdict {
    Ok,                 ///< exit 0: nothing fatal, warnings may still be present
    Failed,             ///< exit non-zero and we parsed the checks
    ConfigUnparseable,  ///< config.json is not valid JSON -- see below
    Unreadable,         ///< we got something we do not recognise at all
};

struct PreflightReport {
    Verdict verdict = Verdict::Unreadable;
    std::vector<PreflightCheck> checks;

    /// Everything the process printed, kept verbatim so the UI can offer the
    /// original text. A parser that loses the raw output leaves the user with
    /// nothing when the format changes under it.
    std::string raw;

    /// Set when the whole run produced no usable structure -- an unparseable
    /// config, a crash, a timeout. This is what the UI shows instead of checks.
    std::string message;

    bool ok() const { return verdict == Verdict::Ok; }

    /// Fatal checks only. These are what block the service.
    std::vector<PreflightCheck> failures() const;

    /// Non-fatal problems. Worth showing, must not block a Save.
    std::vector<PreflightCheck> warnings() const;

    /// The check named `name`, or nullptr. Lets the configurator put
    /// web_port's complaint under the web_port field.
    const PreflightCheck* find(const std::string& name) const;
};

/**
 * Parse a completed `--preflight` run.
 *
 * @param out        the process's stdout
 * @param err        its stderr
 * @param exit_code  0 means usable
 *
 * The stderr argument is not decoration. main.cpp refuses to start when
 * config.json is not valid JSON, and it does that BEFORE preflight runs: it
 * prints "Refusing to start. The configuration file is not valid JSON" to
 * stderr and exits 1 with no `Preflight checks:` header at all. That case must
 * be distinguishable, because it is the one state where the supervisor must NOT
 * offer to rewrite the file -- doing so would overwrite something the user
 * hand-edited and still wants.
 */
PreflightReport parsePreflight(const std::string& out,
                               const std::string& err,
                               int exit_code);

}  // namespace cpapdash::supervisor
