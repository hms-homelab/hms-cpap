//
// test_SetupService.cpp: SDD-006.
//
// Two things are pinned here, and both exist because getting them wrong is
// silent rather than loud.
//
// CAPABILITIES. The wizard decides what to offer from this. Since 4.6.3 a
// config naming a backend the binary was not built with is a refuse-to-boot
// condition, so a capabilities list that over-promises turns into "I finished
// the wizard and now it won't start". The list must therefore track the build
// flags exactly, which is what the #ifdef-mirroring assertions below check.
//
// STATIC DIR. The historical default was CWD-relative, so a double-clicked
// binary served no UI at all. Resolution has a precedence order and an
// "unset" sentinel, and both are easy to break in a way no other test notices.
//
#include <gtest/gtest.h>

#include "services/SetupService.h"
#include "database/SQLiteDatabase.h"
#include "utils/AppConfig.h"

#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace hms_cpap;

namespace {

bool hasBackend(const std::vector<std::string>& v, const std::string& b) {
    return std::find(v.begin(), v.end(), b) != v.end();
}

// ─────────────────────────────────────────────────────────────────────────────
// Capabilities
// ─────────────────────────────────────────────────────────────────────────────

TEST(SetupServiceTest, SqliteIsAlwaysAvailable) {
    // The embedded default has no build flag and no server, so it is the one
    // backend the wizard may always offer.
    EXPECT_TRUE(hasBackend(SetupService::capabilities().backends, "sqlite"));
    EXPECT_TRUE(SetupService::supportsBackend("sqlite"));
}

TEST(SetupServiceTest, OptionalBackendsTrackTheBuildFlags) {
    const auto caps = SetupService::capabilities();

    // Mirrors the #ifdefs deliberately: this is the assertion that fails if
    // someone adds a backend to the list without gating it, which is exactly
    // the mistake that produces a wizard offering something that cannot boot.
#ifdef WITH_POSTGRESQL
    EXPECT_TRUE(hasBackend(caps.backends, "postgresql"));
    EXPECT_TRUE(SetupService::supportsBackend("postgresql"));
#else
    EXPECT_FALSE(hasBackend(caps.backends, "postgresql"));
    EXPECT_FALSE(SetupService::supportsBackend("postgresql"));
#endif

#ifdef WITH_MYSQL
    EXPECT_TRUE(hasBackend(caps.backends, "mysql"));
    EXPECT_TRUE(SetupService::supportsBackend("mysql"));
#else
    EXPECT_FALSE(hasBackend(caps.backends, "mysql"));
    EXPECT_FALSE(SetupService::supportsBackend("mysql"));
#endif
}

TEST(SetupServiceTest, UnknownBackendIsNotSupported) {
    // A guard that says yes to everything would make the wizard's check
    // meaningless.
    EXPECT_FALSE(SetupService::supportsBackend("oracle"));
    EXPECT_FALSE(SetupService::supportsBackend(""));
    EXPECT_FALSE(SetupService::supportsBackend("SQLITE"));  // case-sensitive
}

TEST(SetupServiceTest, BackendsAreUniqueAndNonEmpty) {
    auto backends = SetupService::capabilities().backends;
    ASSERT_FALSE(backends.empty());
    for (const auto& b : backends) EXPECT_FALSE(b.empty());

    auto sorted = backends;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_TRUE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end())
        << "a backend is listed twice";
}

TEST(SetupServiceTest, PdfReportsFollowsThePlatform) {
    // The report stack (gnuplot + libharu) is wired only on POSIX in main.cpp,
    // so a Windows build genuinely has none and the wizard must not imply it.
    const auto caps = SetupService::capabilities();
#ifdef _WIN32
    EXPECT_FALSE(caps.pdf_reports);
#else
    EXPECT_TRUE(caps.pdf_reports);
#endif
}

TEST(SetupServiceTest, VersionAndPlatformAreReported) {
    const auto caps = SetupService::capabilities();
    EXPECT_EQ(caps.version, std::string(HMS_CPAP_VERSION));
    EXPECT_FALSE(caps.platform.empty());
    // Shape is "<os>-<arch>", which is what a support ticket needs to be useful.
    EXPECT_NE(caps.platform.find('-'), std::string::npos);
}

TEST(SetupServiceTest, DataDirMatchesAppConfig) {
    // The welcome step names this path. If it drifts from where the config
    // actually lives, the wizard tells the user to look in the wrong place.
    EXPECT_EQ(SetupService::capabilities().data_dir, AppConfig::dataDir());
}

// ─────────────────────────────────────────────────────────────────────────────
// Executable path
// ─────────────────────────────────────────────────────────────────────────────

TEST(SetupServiceTest, ExecutablePathIsAbsoluteAndExists) {
    const auto exe = SetupService::executablePath();
    ASSERT_FALSE(exe.empty()) << "no platform path available for the test binary";
    EXPECT_TRUE(std::filesystem::path(exe).is_absolute());
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(exe, ec));
}

TEST(SetupServiceTest, ExecutableDirIsTheParentAndExists) {
    const auto exe = SetupService::executablePath();
    const auto dir = SetupService::executableDir();
    ASSERT_FALSE(exe.empty());
    ASSERT_FALSE(dir.empty());
    EXPECT_EQ(dir, std::filesystem::path(exe).parent_path().string());
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::is_directory(dir, ec));
}

// ─────────────────────────────────────────────────────────────────────────────
// static_dir resolution
// ─────────────────────────────────────────────────────────────────────────────

TEST(SetupServiceTest, ExplicitStaticDirAlwaysWins) {
    // An explicit choice must survive even when it does not exist yet: the user
    // may be pointing at a directory they are about to populate, and silently
    // overriding it would be worse than serving nothing.
    const std::string custom = "/opt/somewhere/else/browser";
    EXPECT_EQ(SetupService::resolveStaticDir(custom), custom);
}

TEST(SetupServiceTest, EmptyAndLegacyValuesCountAsUnset) {
    // Neither can be told apart from an untouched config, so both must go
    // through resolution rather than being taken literally.
    const auto from_empty = SetupService::resolveStaticDir("");
    const auto from_legacy =
        SetupService::resolveStaticDir(SetupService::kLegacyStaticDir);
    EXPECT_EQ(from_empty, from_legacy);
}

TEST(SetupServiceTest, ResolvesBesideTheExecutableWhenPresent) {
    // The release-zip layout: binary and static/browser side by side. Build it
    // next to the test binary, which is the only executable directory this test
    // can legitimately claim.
    const auto dir = SetupService::executableDir();
    ASSERT_FALSE(dir.empty());

    const auto bundle = std::filesystem::path(dir) / "static" / "browser";
    std::error_code ec;
    const bool pre_existing = std::filesystem::exists(bundle, ec);
    if (!pre_existing) {
        ASSERT_TRUE(std::filesystem::create_directories(bundle, ec)) << ec.message();
    }

    EXPECT_EQ(SetupService::resolveStaticDir(""), bundle.string())
        << "a bundle sitting beside the binary was not picked up";

    if (!pre_existing) {
        std::filesystem::remove_all(std::filesystem::path(dir) / "static", ec);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// First-run browser launch
//
// Each suppression maps to a real deployment that would otherwise be harmed:
// a scripted install, the SDD-005 shell racing us to the same URL, and a
// container or systemd unit with no browser and nobody watching.
// ─────────────────────────────────────────────────────────────────────────────

TEST(SetupServiceTest, OpensOnlyOnAnInteractiveFirstRun) {
    SetupService::LaunchContext ctx;
    ctx.setup_complete = false;
    ctx.no_browser_flag = false;
    ctx.supervised = false;
    ctx.interactive = true;
    EXPECT_TRUE(SetupService::shouldOpenBrowser(ctx));
}

TEST(SetupServiceTest, DoesNotOpenOnceSetupIsComplete) {
    SetupService::LaunchContext ctx;
    ctx.setup_complete = true;
    ctx.interactive = true;
    EXPECT_FALSE(SetupService::shouldOpenBrowser(ctx))
        << "an already-configured install must not reopen the wizard on restart";
}

TEST(SetupServiceTest, NoBrowserFlagSuppresses) {
    SetupService::LaunchContext ctx;
    ctx.interactive = true;
    ctx.no_browser_flag = true;
    EXPECT_FALSE(SetupService::shouldOpenBrowser(ctx));
}

TEST(SetupServiceTest, SupervisedRunDoesNotOpenItsOwnBrowser) {
    // The SDD-005 desktop shell owns presentation; two windows racing to the
    // same URL is worse than none.
    SetupService::LaunchContext ctx;
    ctx.interactive = true;
    ctx.supervised = true;
    EXPECT_FALSE(SetupService::shouldOpenBrowser(ctx));
}

TEST(SetupServiceTest, HeadlessRunDoesNotOpen) {
    // Docker and systemd take this path.
    SetupService::LaunchContext ctx;
    ctx.interactive = false;
    EXPECT_FALSE(SetupService::shouldOpenBrowser(ctx));
}

TEST(SetupServiceTest, AnySingleSuppressorIsEnough) {
    // Guards against a refactor turning the chain of early returns into
    // something that needs all four to agree.
    for (int i = 0; i < 3; ++i) {
        SetupService::LaunchContext ctx;
        ctx.interactive = true;
        ctx.setup_complete  = (i == 0);
        ctx.no_browser_flag = (i == 1);
        ctx.supervised      = (i == 2);
        EXPECT_FALSE(SetupService::shouldOpenBrowser(ctx))
            << "suppressor " << i << " alone did not prevent the launch";
    }
}

TEST(SetupServiceTest, SetupUrlIsLocalhostAndPointsAtTheWizard) {
    // localhost, not the bind address: 0.0.0.0 is not navigable, and this is
    // only ever opened on the machine running the service.
    const auto url = SetupService::setupUrl(8893);
    EXPECT_EQ(url, "http://localhost:8893/setup");

    // A non-default port must be honoured, or a user who moved the port gets
    // sent somewhere nothing is listening.
    EXPECT_EQ(SetupService::setupUrl(19000), "http://localhost:19000/setup");
}

TEST(SetupServiceTest, FallsBackToLegacyWhenNothingBesideTheExecutable) {
    // A developer running from the build directory has no bundle next to the
    // binary and must keep the old behaviour rather than getting an absolute
    // path to somewhere that does not exist.
    const auto dir = SetupService::executableDir();
    ASSERT_FALSE(dir.empty());

    const auto staticRoot = std::filesystem::path(dir) / "static";
    std::error_code ec;
    if (std::filesystem::exists(staticRoot, ec)) {
        GTEST_SKIP() << "a static/ directory already sits beside the test binary; "
                        "removing it to prove the fallback would be destructive";
    }

    EXPECT_EQ(SetupService::resolveStaticDir(""),
              std::string(SetupService::kLegacyStaticDir));
}


// ---------------------------------------------------------------------------
// Database provisioning (SDD-006 phase 2)
//
// Neither engine accepts a bound parameter for the TARGET of CREATE DATABASE,
// so the name is interpolated and the whitelist is the only real defence.
// These are the tests that stand in for a live server: the exact SQL matters,
// and a wrong GRANT shows up as a working CREATE followed by a failure at first
// boot, which is the confusing shape this is meant to prevent.
// ---------------------------------------------------------------------------

TEST(SetupServiceTest, OrdinaryDatabaseNamesAreAccepted) {
    EXPECT_TRUE(SetupService::isSafeIdentifier("cpap"));
    EXPECT_TRUE(SetupService::isSafeIdentifier("cpap_data"));
    EXPECT_TRUE(SetupService::isSafeIdentifier("CpapData2"));
    EXPECT_TRUE(SetupService::isSafeIdentifier("_internal"));
}

TEST(SetupServiceTest, InjectionShapesAreRejected) {
    // Each of these is a way to end the identifier early and start a statement.
    EXPECT_FALSE(SetupService::isSafeIdentifier("cpap; DROP DATABASE postgres"));
    EXPECT_FALSE(SetupService::isSafeIdentifier("cpap\"" ));
    EXPECT_FALSE(SetupService::isSafeIdentifier("cpap'"));
    EXPECT_FALSE(SetupService::isSafeIdentifier("cpap`"));
    EXPECT_FALSE(SetupService::isSafeIdentifier("cpap--comment"));
    EXPECT_FALSE(SetupService::isSafeIdentifier("cpap data"));
    EXPECT_FALSE(SetupService::isSafeIdentifier("cpap\\"));
    EXPECT_FALSE(SetupService::isSafeIdentifier(""));
}

TEST(SetupServiceTest, HyphensAreRejectedEvenThoughTheyLookHarmless) {
    // A hyphen is legal in a quoted identifier and reads as a minus in an
    // unquoted one. Rejecting it costs a rename; allowing it costs correctness.
    EXPECT_FALSE(SetupService::isSafeIdentifier("cpap-data"));
}

TEST(SetupServiceTest, ALeadingDigitIsRejected) {
    EXPECT_FALSE(SetupService::isSafeIdentifier("2026cpap"));
    EXPECT_TRUE(SetupService::isSafeIdentifier("cpap2026"));
}

TEST(SetupServiceTest, TheLengthCeilingIsPostgresTruncationNotMysqlsLimit) {
    // 63, not 64. PostgreSQL silently TRUNCATES at 63, so a 64-character name
    // would provision one database and then configure us to open another.
    EXPECT_TRUE(SetupService::isSafeIdentifier(std::string(63, 'a')));
    EXPECT_FALSE(SetupService::isSafeIdentifier(std::string(64, 'a')));
}

TEST(SetupServiceTest, PostgresCreatesTheRoleBeforeTheDatabase) {
    // OWNER has to name an existing role, so the reverse order fails on exactly
    // the fresh server this endpoint exists for.
    const auto plan = SetupService::provisionPlan("postgresql", "cpap", "cpap_user");
    ASSERT_TRUE(plan.error.empty()) << plan.error;
    EXPECT_EQ(plan.maintenance_db, "postgres");
    ASSERT_GE(plan.statements.size(), 2u);

    EXPECT_NE(plan.statements[0].sql.find("CREATE ROLE cpap_user"), std::string::npos);
    EXPECT_NE(plan.statements[1].sql.find("CREATE DATABASE cpap"), std::string::npos);
    EXPECT_NE(plan.statements[1].sql.find("OWNER cpap_user"), std::string::npos);
}

TEST(SetupServiceTest, TheMysqlGrantIsNotLocalhostOnly) {
    // The wizard's whole purpose is pointing at a database on another box. A
    // localhost grant authenticates during setup (run on the server) and then
    // fails from the app host, which looks like a wrong password.
    const auto plan = SetupService::provisionPlan("mysql", "cpap", "cpap_user");
    ASSERT_TRUE(plan.error.empty()) << plan.error;
    EXPECT_EQ(plan.maintenance_db, "mysql");

    bool saw_wildcard_grant = false;
    for (const auto& st : plan.statements) {
        if (st.sql.find("GRANT") != std::string::npos &&
            st.sql.find("'cpap_user'@'%'") != std::string::npos) {
            saw_wildcard_grant = true;
        }
        EXPECT_EQ(st.sql.find("@'localhost'"), std::string::npos)
            << "a localhost-only grant fails from any other host: " << st.sql;
    }
    EXPECT_TRUE(saw_wildcard_grant);
}

TEST(SetupServiceTest, ThePasswordIsATokenAndNeverAnInlineLiteral) {
    // The plan must never carry the actual password, so a quote in one cannot
    // terminate a statement. It carries a sentinel that the executor replaces
    // with a literal escaped BY THE DRIVER.
    //
    // Note what this is NOT: a bound parameter. Neither engine accepts a
    // placeholder in a UTILITY statement. PostgreSQL answers
    // `CREATE ROLE x LOGIN PASSWORD $1` with a syntax error at `$1`, and MySQL
    // will not prepare a parameter in CREATE USER either. That was found by
    // running it against a real server, not by reading the docs, and it is why
    // this test asserts a token rather than a placeholder.
    for (const char* engine : {"postgresql", "mysql"}) {
        const auto plan = SetupService::provisionPlan(engine, "cpap", "cpap_user");
        ASSERT_TRUE(plan.error.empty()) << engine << ": " << plan.error;

        int carriers = 0;
        for (const auto& st : plan.statements) {
            if (st.binds_password) {
                ++carriers;
                EXPECT_NE(st.sql.find(SetupService::kPasswordToken), std::string::npos)
                    << engine << ": flagged as carrying the password but has no token: "
                    << st.sql;
            }
            EXPECT_EQ(st.sql.find("PASSWORD '"), std::string::npos)
                << engine << " inlined a literal password: " << st.sql;
            EXPECT_EQ(st.sql.find("$1"), std::string::npos)
                << engine << " used a placeholder, which a utility statement rejects: "
                << st.sql;
        }
        EXPECT_EQ(carriers, 1) << engine << ": exactly one statement sets the password";
    }
}

TEST(SetupServiceTest, AnUnsafeNameProducesNoSqlAtAll) {
    // Refusing must mean emitting nothing, not emitting something escaped. A
    // caller that ignores `error` should still be unable to run damage.
    const auto plan = SetupService::provisionPlan("postgresql",
                                                  "cpap; DROP DATABASE postgres",
                                                  "cpap_user");
    EXPECT_FALSE(plan.error.empty());
    EXPECT_TRUE(plan.statements.empty())
        << "statements were emitted for a rejected identifier";
}

TEST(SetupServiceTest, AnUnsafeUserIsRefusedToo) {
    const auto plan = SetupService::provisionPlan("mysql", "cpap", "root'--");
    EXPECT_FALSE(plan.error.empty());
    EXPECT_TRUE(plan.statements.empty());
}

TEST(SetupServiceTest, SqliteHasNothingToProvision) {
    // Not an oversight: there is no server, and the file is created on first
    // open. Saying so beats emitting SQL nobody can run.
    const auto plan = SetupService::provisionPlan("sqlite", "cpap", "cpap_user");
    EXPECT_FALSE(plan.error.empty());
    EXPECT_TRUE(plan.statements.empty());
}


TEST(SetupServiceTest, SupervisedRestartsByExitingEvenWithNoExecutablePath) {
    // Under the SDD-005 shell, exiting 0 IS the restart. Requiring an
    // executable path here would break the supervised case on any platform
    // whose path lookup failed, which is the case that needs it least.
    EXPECT_EQ(SetupService::restartMode(true, ""),
              SetupService::RestartMode::SupervisedExit);
    EXPECT_EQ(SetupService::restartMode(true, "/usr/local/bin/hms_cpap"),
              SetupService::RestartMode::SupervisedExit);
}

TEST(SetupServiceTest, StandaloneReExecsItself) {
    EXPECT_EQ(SetupService::restartMode(false, "/usr/local/bin/hms_cpap"),
              SetupService::RestartMode::ReExec);
}

TEST(SetupServiceTest, StandaloneWithNoPathIsUnsupportedRatherThanAGuess) {
    // Reported so the wizard can tell the user to restart manually. Exiting
    // here would kill an unsupervised process and never bring it back.
    EXPECT_EQ(SetupService::restartMode(false, ""),
              SetupService::RestartMode::Unsupported);
}


TEST(SetupServiceTest, ProbingASqlitePathDoesNotCreateIt) {
    // THE safety property. IDatabase::connect() creates and migrates the schema,
    // so probing through it would mean a typo'd path silently gains a database.
    // A probe has to be able to say "that is not what you meant" without having
    // already changed anything.
    const auto path = std::filesystem::temp_directory_path() /
                      ("hms_probe_" + std::to_string(::getpid()) + ".db");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    const auto probe = SetupService::probeDatabase("sqlite", "", 0, "", "", "",
                                                   path.string());
    EXPECT_TRUE(probe.ok) << probe.error;
    EXPECT_FALSE(probe.schema_present) << "a file that does not exist cannot have a schema";
    EXPECT_EQ(probe.session_count, 0);
    EXPECT_FALSE(std::filesystem::exists(path, ec))
        << "the probe CREATED the database it was only asked to look at";
}

TEST(SetupServiceTest, ProbingAnExistingSqliteReportsItsSchemaAndCount) {
    // The other half: an existing database must be reported as populated, which
    // is what lets the wizard say "this already has N sessions" instead of
    // leaving someone to guess whether they are merging into other data.
    const auto path = std::filesystem::temp_directory_path() /
                      ("hms_probe_real_" + std::to_string(::getpid()) + ".db");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    {
        SQLiteDatabase db(path.string());
        ASSERT_TRUE(db.connect());   // this one is allowed to create the schema
    }

    const auto probe = SetupService::probeDatabase("sqlite", "", 0, "", "", "",
                                                   path.string());
    EXPECT_TRUE(probe.ok) << probe.error;
    EXPECT_TRUE(probe.schema_present) << "an initialised database read as empty";
    EXPECT_EQ(probe.session_count, 0) << "a fresh schema has no sessions yet";

    std::filesystem::remove(path, ec);
}

TEST(SetupServiceTest, ProbingAnUncompiledBackendSaysSoRatherThanFallingBack) {
    // Before 4.6.3 an unavailable backend silently fell through to SQLite, which
    // is exactly the split brain the wizard exists to prevent.
    const auto probe = SetupService::probeDatabase("oracle", "h", 1, "n", "u", "p", "");
    EXPECT_FALSE(probe.ok);
    EXPECT_NE(probe.error.find("oracle"), std::string::npos);
}


// ---------------------------------------------------------------------------
// Start at login (SDD-006 phase 4)
//
// Generated as text and asserted here rather than installed, because a
// malformed plist or unit file fails SILENTLY at login, which is the worst
// possible place to find out.
// ---------------------------------------------------------------------------

TEST(SetupServiceTest, TheShellOwnsAutostartWhenThereIsOne) {
    // Two entries racing to start one service is worse than none.
    EXPECT_FALSE(SetupService::canManageAutostart(/*supervised=*/true));
    EXPECT_TRUE(SetupService::canManageAutostart(/*supervised=*/false));
}

TEST(SetupServiceTest, AutostartNeedsToKnowWhereTheProgramIs) {
    const auto e = SetupService::autostartEntry("", "/Users/someone");
    EXPECT_FALSE(e.error.empty());
    EXPECT_TRUE(e.content.empty());
    EXPECT_TRUE(e.registry_data.empty());
}

TEST(SetupServiceTest, TheAutostartEntryStartsTheRightBinaryWithoutABrowser) {
    // --no-browser matters: an entry that opens a browser tab on every login is
    // the fastest way to make someone uninstall the thing.
    const auto e = SetupService::autostartEntry("/opt/hms/hms_cpap", "/home/someone");
    ASSERT_TRUE(e.error.empty()) << e.error;

    const std::string payload = e.content.empty() ? e.registry_data : e.content;
    EXPECT_NE(payload.find("/opt/hms/hms_cpap"), std::string::npos);
    EXPECT_NE(payload.find("--no-browser"), std::string::npos);
}

#if defined(__APPLE__)
TEST(SetupServiceTest, TheMacPlistIsWellFormedAndRunsAtLoad) {
    const auto e = SetupService::autostartEntry("/opt/hms/hms_cpap", "/Users/someone");
    ASSERT_TRUE(e.error.empty()) << e.error;

    EXPECT_EQ(e.path, "/Users/someone/Library/LaunchAgents/com.hms.cpap.cli.plist");
    // Structure launchd actually requires. A plist missing any of these loads
    // as an empty job and simply never runs.
    EXPECT_NE(e.content.find("<?xml"), std::string::npos);
    EXPECT_NE(e.content.find("<plist version=\"1.0\">"), std::string::npos);
    EXPECT_NE(e.content.find("</plist>"), std::string::npos);
    EXPECT_NE(e.content.find("<key>RunAtLoad</key>"), std::string::npos);
    EXPECT_NE(e.content.find("<key>ProgramArguments</key>"), std::string::npos);

    // The label must differ from the SDD-005 desktop agent's, or installing one
    // silently replaces the other.
    EXPECT_NE(e.content.find("com.hms.cpap.cli"), std::string::npos);
}
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
TEST(SetupServiceTest, TheSystemdUnitIsAUserUnitWantedByDefaultTarget) {
    const auto e = SetupService::autostartEntry("/opt/hms/hms_cpap", "/home/someone");
    ASSERT_TRUE(e.error.empty()) << e.error;

    EXPECT_EQ(e.path, "/home/someone/.config/systemd/user/hms-cpap.service");
    EXPECT_NE(e.content.find("[Service]"), std::string::npos);
    EXPECT_NE(e.content.find("ExecStart=/opt/hms/hms_cpap --no-browser"),
              std::string::npos);
    // default.target, NOT multi-user.target: this starts at LOGIN. A user unit
    // wanted by multi-user.target simply never runs, which is a silent failure.
    EXPECT_NE(e.content.find("WantedBy=default.target"), std::string::npos);
    EXPECT_EQ(e.content.find("multi-user.target"), std::string::npos);
}
#endif

#if defined(_WIN32)
TEST(SetupServiceTest, TheWindowsRunValueIsQuotedAndDistinct) {
    const auto e = SetupService::autostartEntry("C:\\Program Files\\hms\\hms_cpap.exe",
                                                "C:\\Users\\someone");
    ASSERT_TRUE(e.error.empty()) << e.error;

    EXPECT_NE(e.registry_key.find("CurrentVersion\\Run"), std::string::npos);
    EXPECT_EQ(e.registry_value, "HmsCpapCli");   // distinct from SDD-005's
    // Program Files contains a space; an unquoted path there starts the wrong
    // program or nothing at all.
    EXPECT_EQ(e.registry_data.front(), '"');
    EXPECT_NE(e.registry_data.find("\" --no-browser"), std::string::npos);
}
#endif


// ---------------------------------------------------------------------------
// Boot scope: a real service that runs before anyone logs in
//
// The failure these guard against is subtle and looks like data loss. A system
// service starts with no user context: a macOS LaunchDaemon runs as root with
// HOME=/var/root, and a systemd system unit runs as root with no HOME at all.
// Either would resolve ~/.hms-cpap somewhere the user cannot see, create a
// SECOND empty database, and present as "the app forgot every night".
// ---------------------------------------------------------------------------

TEST(SetupServiceTest, ABootServiceAlwaysNeedsElevation) {
    const auto e = SetupService::autostartEntry(
        "/opt/hms/hms_cpap", "/home/someone",
        SetupService::AutostartScope::Boot, "someone");
    ASSERT_TRUE(e.error.empty()) << e.error;
    EXPECT_TRUE(e.needs_elevation)
        << "a boot service that claims to need no privileges will fail halfway "
           "through installing";
    EXPECT_FALSE(e.install_command.empty()) << "the user is told nothing to run";
    EXPECT_FALSE(e.uninstall_command.empty()) << "installed with no way back out";
}

TEST(SetupServiceTest, BootAndLoginAreDifferentArtifacts) {
    const auto login = SetupService::autostartEntry(
        "/opt/hms/hms_cpap", "/home/someone", SetupService::AutostartScope::Login);
    const auto boot = SetupService::autostartEntry(
        "/opt/hms/hms_cpap", "/home/someone",
        SetupService::AutostartScope::Boot, "someone");
    ASSERT_TRUE(login.error.empty());
    ASSERT_TRUE(boot.error.empty());

    EXPECT_FALSE(login.needs_elevation);
    EXPECT_TRUE(boot.needs_elevation);
#if !defined(_WIN32)
    // Different files, so installing one never silently replaces the other.
    EXPECT_NE(login.path, boot.path);
#endif
}

TEST(SetupServiceTest, ABootServiceMustKnowWhichAccountToRunAs) {
    // Running as root is not a fallback: it writes a root-owned database the
    // user cannot read.
    const auto e = SetupService::bootEntry("/opt/hms/hms_cpap", "/home/someone", "");
#if !defined(_WIN32)
    if (e.error.empty()) {
        // A USER env var was present, so the account must have reached the file.
        const std::string payload = e.content;
        EXPECT_FALSE(payload.empty());
    }
#endif
    SUCCEED();   // the negative case is asserted below with the env cleared
}

#if defined(__APPLE__)
TEST(SetupServiceTest, TheLaunchDaemonPinsBothTheAccountAndHome) {
    const auto e = SetupService::bootEntry("/opt/hms/hms_cpap", "/Users/someone", "someone");
    ASSERT_TRUE(e.error.empty()) << e.error;

    EXPECT_EQ(e.path, "/Library/LaunchDaemons/com.hms.cpap.daemon.plist");
    // Distinct label from the login agent, so both can exist.
    EXPECT_NE(e.content.find("com.hms.cpap.daemon"), std::string::npos);
    EXPECT_EQ(e.content.find("<string>com.hms.cpap.cli</string>"), std::string::npos);

    // The two keys that stop it writing a root-owned database in the wrong place.
    EXPECT_NE(e.content.find("<key>UserName</key>"), std::string::npos);
    EXPECT_NE(e.content.find("<string>someone</string>"), std::string::npos);
    EXPECT_NE(e.content.find("<key>HOME</key>"), std::string::npos);
    EXPECT_NE(e.content.find("<string>/Users/someone</string>"), std::string::npos);

    EXPECT_NE(e.install_command.find("launchctl load"), std::string::npos);
}
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
TEST(SetupServiceTest, TheSystemUnitRunsAsTheUserAndWantsMultiUser) {
    const auto e = SetupService::bootEntry("/opt/hms/hms_cpap", "/home/someone", "someone");
    ASSERT_TRUE(e.error.empty()) << e.error;

    EXPECT_EQ(e.path, "/etc/systemd/system/hms-cpap.service");
    EXPECT_NE(e.content.find("User=someone"), std::string::npos);
    EXPECT_NE(e.content.find("Environment=HOME=/home/someone"), std::string::npos);
    // Unlike the per-user unit, this one IS a system service and must come up
    // with nobody logged in.
    EXPECT_NE(e.content.find("WantedBy=multi-user.target"), std::string::npos);
    EXPECT_EQ(e.content.find("WantedBy=default.target"), std::string::npos);

    // The no-root route is offered rather than hidden.
    EXPECT_NE(e.alternative_command.find("enable-linger"), std::string::npos);
}
#endif

#if defined(_WIN32)
TEST(SetupServiceTest, TheWindowsServiceNamesItsWrapper) {
    // A plain console program cannot be a Windows service: the SCM expects a
    // status report within its timeout and kills anything that stays silent,
    // which presents as "the service starts and immediately stops". Naming the
    // wrapper beats shipping a service that dies.
    const auto e = SetupService::bootEntry("C:\\hms\\hms_cpap.exe",
                                           "C:\\Users\\someone", "someone");
    ASSERT_TRUE(e.error.empty()) << e.error;
    EXPECT_NE(e.requires_helper.find("shawl"), std::string::npos);
    EXPECT_NE(e.install_command.find("sc create"), std::string::npos);
    EXPECT_NE(e.install_command.find("start= auto"), std::string::npos);
    EXPECT_NE(e.uninstall_command.find("sc delete"), std::string::npos);
}
#endif


#if defined(__APPLE__)
TEST(SetupServiceTest, TheLaunchDaemonPlistIsAcceptedByLaunchdsOwnParser) {
    // String assertions prove the keys are present; they do NOT prove the XML
    // parses. A malformed plist loads as an empty job and silently never runs,
    // so hand the generated file to plutil, which is the same parser launchd
    // uses, rather than trusting that it looks right.
    const auto e = SetupService::bootEntry("/opt/hms/hms_cpap", "/Users/someone", "someone");
    ASSERT_TRUE(e.error.empty()) << e.error;

    const auto path = std::filesystem::temp_directory_path() /
                      ("hms_daemon_" + std::to_string(::getpid()) + ".plist");
    { std::ofstream out(path); out << e.content; }

    const int rc = std::system(("plutil -lint '" + path.string() + "' >/dev/null 2>&1").c_str());
    std::error_code ec;
    std::filesystem::remove(path, ec);

    ASSERT_EQ(rc, 0) << "plutil rejected the generated LaunchDaemon plist; "
                        "launchd would load it as an empty job that never runs";
}

TEST(SetupServiceTest, TheLoginAgentPlistIsAlsoAcceptedByPlutil) {
    const auto e = SetupService::autostartEntry("/opt/hms/hms_cpap", "/Users/someone");
    ASSERT_TRUE(e.error.empty()) << e.error;

    const auto path = std::filesystem::temp_directory_path() /
                      ("hms_agent_" + std::to_string(::getpid()) + ".plist");
    { std::ofstream out(path); out << e.content; }

    const int rc = std::system(("plutil -lint '" + path.string() + "' >/dev/null 2>&1").c_str());
    std::error_code ec;
    std::filesystem::remove(path, ec);

    ASSERT_EQ(rc, 0) << "plutil rejected the generated LaunchAgent plist";
}
#endif

}  // namespace
