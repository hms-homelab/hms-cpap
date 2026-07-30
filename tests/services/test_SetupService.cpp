//
// test_SetupService.cpp — SDD-006 phase 1.
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
#include "utils/AppConfig.h"

#include <algorithm>
#include <filesystem>
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

}  // namespace
