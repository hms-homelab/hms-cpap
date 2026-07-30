#pragma once
//
// SetupService (SDD-006, phase 1) — what the first-run wizard needs to know
// about the binary it is talking to, plus where that binary lives on disk.
//
// WHY THIS EXISTS AS A SERVICE. Per CLAUDE.md the test binary links all of
// `src/` EXCEPT main.cpp, controllers, web/ and the CLI. Anything that lives in
// CpapController therefore cannot be unit tested at all. SDD-005 phase 1 hit
// the same wall and answered it the same way: the logic lives here, and the
// controller is a passthrough that does nothing but serialise.
//
// WHY CAPABILITIES ARE REPORTED AT ALL. The three storage backends are
// compile-time optional (BUILD_WITH_POSTGRESQL, BUILD_WITH_MYSQL) and the PDF
// report stack is `#ifndef _WIN32`. Before 4.6.3 a config naming an
// uncompiled backend silently fell through to SQLite; since 4.6.3 it is a
// refuse-to-boot condition, which is the correct behaviour and a terrible
// first-run experience. So the wizard has to ask what this build can actually
// do rather than offering all three and hoping.
//
#include <string>
#include <vector>

namespace hms_cpap {

class SetupService {
public:
    struct Capabilities {
        std::string version;
        /// Storage backends this build can actually open. "sqlite" is always
        /// present; the others appear only when compiled in.
        std::vector<std::string> backends;
        bool pdf_reports = false;      // report stack is POSIX-only
        bool mdns_discovery = false;   // SDD-005 LAN browse
        std::string platform;          // e.g. "darwin-arm64"
        std::string data_dir;          // resolved ~/.hms-cpap
    };

    /// Pure, no I/O beyond reading the data directory path. Safe to call from a
    /// web thread.
    static Capabilities capabilities();

    /// True when this build can open `type` ("sqlite" / "postgresql" /
    /// "mysql"). The wizard uses this to refuse to write a config that would
    /// stop the service from starting.
    static bool supportsBackend(const std::string& type);

    /// Absolute path of the running executable, or "" when the platform call
    /// fails. Windows returns a path with native separators.
    static std::string executablePath();

    /// Directory containing the running executable, or "" when unknown.
    static std::string executableDir();

    /// Where the Angular bundle should be served from.
    ///
    /// The shipped layout is the release zip: the binary and `static/browser`
    /// side by side. The historical default was the CWD-relative
    /// "./static/browser", so double-clicking the binary anywhere other than
    /// its own directory served no UI at all, which makes every other part of
    /// the wizard unreachable. Resolution order:
    ///
    ///   1. `configured` when the user set it explicitly (always wins)
    ///   2. <executable dir>/static/browser when that directory exists
    ///   3. the CWD-relative default, so an existing dev workflow that runs
    ///      from the build directory keeps working
    ///
    /// `configured` is treated as unset when it is empty or still the historical
    /// default, since that value cannot be distinguished from "never touched".
    static std::string resolveStaticDir(const std::string& configured);

    /// The historical CWD-relative default, and the sentinel meaning "unset".
    static constexpr const char* kLegacyStaticDir = "./static/browser";

    // -- First-run browser launch ---------------------------------------------

    /// Inputs to the "should a browser be opened?" question, named rather than
    /// passed as four bare bools so a call site cannot silently transpose them.
    struct LaunchContext {
        bool setup_complete = false;  ///< nothing to prompt for once true
        bool no_browser_flag = false; ///< --no-browser on the command line
        bool supervised = false;      ///< HMS_CPAP_SUPERVISED=1 (SDD-005 shell)
        bool interactive = false;     ///< attached to a terminal / user session
    };

    /// Pure decision, separated from the spawn so it can be tested.
    ///
    /// Opens only on a genuine first run at a human's keyboard. The three
    /// suppressions each exist for a concrete deployment: `--no-browser` for
    /// scripted installs, `supervised` because the SDD-005 shell owns
    /// presentation and would otherwise race us to the same URL, and
    /// `interactive` because a Docker container or a systemd unit has no
    /// browser to open and no user to see it.
    static bool shouldOpenBrowser(const LaunchContext& ctx);

    /// The URL the first-run browser should land on.
    static std::string setupUrl(int web_port);

    /// True when this process looks attached to a human session. Used to fill
    /// LaunchContext::interactive.
    static bool isInteractiveSession();

    /// Fire-and-forget; never throws and never blocks startup. Returns false
    /// when the platform call could not be made.
    static bool openInBrowser(const std::string& url);
};

}  // namespace hms_cpap
