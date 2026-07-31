#pragma once
//
// SetupService (SDD-006) - what the first-run wizard needs to know
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

    // -- Database provisioning (SDD-006 phase 2) ------------------------------

    /// Is this safe to interpolate into a DDL statement as an identifier?
    ///
    /// Neither PostgreSQL nor MySQL accepts a bound parameter for the TARGET of
    /// CREATE DATABASE / CREATE USER, so the name cannot be a placeholder and
    /// has to be interpolated. That makes a whitelist the only real defence,
    /// and it is deliberately narrower than what the engines would accept:
    /// letters, digits and underscore, not starting with a digit, at most 63
    /// characters (PostgreSQL truncates at 63, MySQL allows 64).
    ///
    /// Rejecting a legal-but-exotic name costs a user one rename. Accepting a
    /// quote or a semicolon costs them their database. Passwords are never run
    /// through here; they are bound.
    static bool isSafeIdentifier(const std::string& name);

    /// The statements that provision a database and its owner, in order, for
    /// `engine` ("postgresql" or "mysql").
    ///
    /// Returned rather than executed so the exact SQL is unit-testable without
    /// a server: the test binary excludes controllers, and a wrong GRANT here
    /// surfaces as a working CREATE followed by a failure at first boot, which
    /// is the confusing shape this is meant to prevent.
    ///
    /// Every identifier MUST have passed isSafeIdentifier first; this function
    /// re-checks and returns empty rather than emitting anything unsafe.
    ///
    /// The password is NOT a bound parameter, and that is not an oversight.
    /// Neither engine accepts placeholders in UTILITY statements: PostgreSQL
    /// answers `CREATE ROLE x LOGIN PASSWORD $1` with a syntax error at `$1`,
    /// and MySQL will not prepare a parameter in CREATE USER either. Binding
    /// only works in DML. So the statement carries the sentinel kPasswordToken
    /// where a quoted literal belongs, and the executor substitutes a literal
    /// escaped by the client library itself (pqxx quote / mysql_real_escape_string)
    /// rather than by anything hand-rolled here.
    struct ProvisionPlan {
        /// Statements to run against the MAINTENANCE database ("postgres" /
        /// "mysql"), in order. `binds_password` marks the one statement carrying
        /// kPasswordToken.
        struct Statement {
            std::string sql;
            bool binds_password = false;
        };
        std::vector<Statement> statements;
        /// The maintenance database to connect to as the admin.
        std::string maintenance_db;
        /// Empty when the plan is valid; otherwise why it was refused.
        std::string error;
    };

    /// Where the escaped password literal is substituted. Deliberately not a
    /// character an identifier could contain, so it cannot collide with a name.
    static constexpr const char* kPasswordToken = "__HMS_PW__";

    static ProvisionPlan provisionPlan(const std::string& engine,
                                       const std::string& db_name,
                                       const std::string& owner_user);

    // -- Applying the configuration (SDD-006 phase 2) -------------------------

    /// How this process should restart itself to pick up a database change.
    ///
    /// A database change CANNOT be hot-reloaded. main.cpp builds the IDatabase
    /// once and hands it to five consumers, and most config reaches services
    /// through the environment before any of them exist. markConfigDirty() only
    /// re-reads the source, the ezShare URL, the local directory and the burst
    /// interval. Pretending otherwise is the single biggest source of "it says
    /// configured but shows nothing".
    enum class RestartMode {
        SupervisedExit,  ///< HMS_CPAP_SUPERVISED=1: exit 0, the shell respawns us
        ReExec,          ///< standalone: execv ourselves
        Unsupported      ///< no executable path; the caller must say so honestly
    };

    /// Pure decision, split from the act so it can be tested. The supervised
    /// case is checked FIRST and does not need an executable path: under the
    /// SDD-005 shell, exiting is the whole job.
    static RestartMode restartMode(bool supervised, const std::string& exe_path);

    /// True when the SDD-005 shell launched us.
    static bool isSupervised();

    // -- Live database checks (SDD-006 phase 2) -------------------------------

    /// Result of connecting to a candidate database with ordinary credentials.
    struct DbProbe {
        bool ok = false;
        std::string error;          ///< engine message, safe to show the user
        bool schema_present = false;///< cpap_sessions already exists
        long long session_count = 0;///< 0 when the schema is absent
    };

    /// Connect and look, WITHOUT creating anything.
    ///
    /// Deliberately not IDatabase::connect(): that runs schema creation and
    /// migration as a side effect, so a typo in the database name would silently
    /// create twenty tables in whatever the user actually pointed at. A probe
    /// must be able to say "this is not the database you meant" without having
    /// already changed it.
    ///
    /// Distinguishing empty from populated is what lets the wizard say "this
    /// database already has 412 sessions, they will be reused" instead of
    /// leaving someone to guess whether they are merging into another person's
    /// data.
    static DbProbe probeDatabase(const std::string& type,
                                 const std::string& host, int port,
                                 const std::string& name,
                                 const std::string& user,
                                 const std::string& password,
                                 const std::string& sqlite_path);

    /// Run provisionPlan() against the maintenance database as the admin, then
    /// reconnect as the ORDINARY user to prove the result is usable. A
    /// successful CREATE DATABASE followed by a failed GRANT otherwise looks
    /// like success and fails at first boot.
    ///
    /// Admin credentials are request-scoped: never persisted, never returned,
    /// never logged.
    // -- Start at login (SDD-006 phase 4) -------------------------------------

    /// What an autostart entry looks like on this OS, as pure text plus a path.
    ///
    /// Returned rather than written so the exact content is testable: the test
    /// binary cannot install a LaunchAgent, and a malformed plist fails silently
    /// at login, which is the worst place to discover it.
    /// When the service should come up.
    ///
    /// Login is the unprivileged default: a per-user agent that starts when the
    /// user signs in. Boot is a real system service that runs before anyone logs
    /// in, which is what a headless box needs, and it costs elevation on every
    /// platform.
    enum class AutostartScope { Login, Boot };

    struct AutostartEntry {
        /// Absolute path of the file to write. Empty on Windows, where the entry
        /// is a registry value rather than a file.
        std::string path;
        /// File content. Empty on Windows.
        std::string content;
        /// Windows only: the registry value name and its data.
        std::string registry_key;    ///< e.g. HKCU\Software\...\Run
        std::string registry_value;  ///< value NAME, distinct from SDD-005's
        std::string registry_data;   ///< the command line
        /// Empty when the entry is valid; otherwise why it could not be built.
        std::string error;

        // -- Boot scope ------------------------------------------------------

        /// True when installing this needs root / Administrator. The wizard says
        /// so before asking, rather than failing halfway through.
        bool needs_elevation = false;
        /// The privileged command that installs it. Shown to the user to run
        /// themselves; this service never elevates itself.
        std::string install_command;
        std::string uninstall_command;
        /// Non-empty when a third-party helper is required (Windows). Naming it
        /// beats silently producing a service that starts and dies.
        std::string requires_helper;
        /// A no-root alternative, where one exists (Linux lingering).
        std::string alternative_command;
    };

    /// Build the autostart entry for this platform.
    ///
    /// STARTS AT LOGIN, not at boot, and the wizard's label says so. Someone who
    /// wants headless-on-boot needs the Docker image or a hand-written system
    /// unit; implying otherwise would be a promise the entry cannot keep.
    ///
    /// The label and the Windows value name are deliberately DISTINCT from
    /// SDD-005's, so the desktop shell and a zip install can coexist rather than
    /// clobbering each other's entry.
    static AutostartEntry autostartEntry(const std::string& exe_path,
                                         const std::string& home_dir,
                                         AutostartScope scope = AutostartScope::Login,
                                         const std::string& user_name = "");

    /// Boot-scope entry. Split out because it is a different artifact on every
    /// platform, not a variation of the login one.
    static AutostartEntry bootEntry(const std::string& exe_path,
                                    const std::string& home_dir,
                                    const std::string& user_name);

    /// Who is responsible for starting this service.
    ///
    /// Three different owners, and only one of them is us. Getting this wrong is
    /// not cosmetic: inside a container, "Start at Login" would write a systemd
    /// unit into an image that has no systemd and no login, and the user would
    /// reasonably believe their install now survives a reboot.
    enum class AutostartOwner {
        Us,            ///< a plain install; the wizard may install an entry
        DesktopShell,  ///< the SDD-005 installer owns it and supervises us
        Container      ///< Docker or Podman owns it; use a restart policy
    };

    static AutostartOwner autostartOwner(bool supervised, bool containerised);

    /// Stable string for the API and the wizard's label.
    static const char* autostartOwnerString(AutostartOwner owner);

    /// The facts a container check reads, passed in so the decision is testable
    /// without being inside a container.
    struct ContainerHints {
        bool dockerenv_exists = false;     ///< /.dockerenv, written by Docker
        bool containerenv_exists = false;  ///< /run/.containerenv, written by Podman
        std::string pid1_cgroup;           ///< contents of /proc/1/cgroup
    };

    /// Pure. Any ONE of these is enough: the checks are deliberately redundant
    /// because each has a blind spot. /.dockerenv is absent under Podman,
    /// cgroup v2 in a rootless container can look unremarkable, and a
    /// distroless image may lack /proc conveniences. Guessing "not a container"
    /// is the costly direction, since that is what offers a useless checkbox.
    static bool looksContainerised(const ContainerHints& hints);

    /// Reads the real system. Always false on Windows and macOS, where the
    /// container runtime is a Linux VM and this binary is not inside it.
    static bool isContainerised();

    /// Whether the wizard should offer to install autostart at all.
    /// Kept for callers that only need the yes/no.
    static bool canManageAutostart(bool supervised);

    static DbProbe provisionDatabase(const std::string& engine,
                                     const std::string& host, int port,
                                     const std::string& db_name,
                                     const std::string& owner_user,
                                     const std::string& owner_password,
                                     const std::string& admin_user,
                                     const std::string& admin_password);
};

}  // namespace hms_cpap
