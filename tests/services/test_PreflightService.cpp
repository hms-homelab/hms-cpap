//
// test_PreflightService.cpp
//
// These exist because of a design mistake worth remembering. The first Windows
// tray shell found a busy port by spawning hms_cpap, watching it die, and
// inferring the cause from how quickly it died; its dialog told the user the
// port was "most likely" in use. Retrying a deterministic failure only delays
// and obscures it, and the user gets a guess instead of an answer.
//
// So the property under test throughout is: a knowable problem is DETECTED,
// NAMED, and paired with something the user can act on.
//
#include <gtest/gtest.h>

#include "services/PreflightService.h"
#include "utils/AppConfig.h"

#include <filesystem>
#include <string>
#include <unistd.h>

#ifndef _WIN32
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

using namespace hms_cpap;

namespace {

TEST(PreflightServiceTest, AFreePortPasses) {
    // Port 0 is special-cased by the OS, so pick something high and unlikely.
    const auto c = PreflightService::checkPort("0.0.0.0", 47913);
    EXPECT_TRUE(c.ok) << c.detail;
    EXPECT_EQ(c.name, "web_port");
}

TEST(PreflightServiceTest, AGenuinelyBusyPortIsDetectedAndNamed) {
    // Bind a real socket and ask the checker about it. This is the case the old
    // supervisor could only infer.
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;                       // let the OS choose a free one
    ASSERT_EQ(::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    socklen_t len = sizeof(addr);
    ASSERT_EQ(::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    const int port = ntohs(addr.sin_port);
    ASSERT_EQ(::listen(sock, 1), 0);

    const auto c = PreflightService::checkPort("0.0.0.0", port);
    ::close(sock);

    EXPECT_FALSE(c.ok) << "a bound port was reported as free";
    EXPECT_NE(c.detail.find(std::to_string(port)), std::string::npos)
        << "the message must name the port; 'something is wrong' is not actionable";
    EXPECT_FALSE(c.remedy.empty())
        << "a failure with no remedy is how a user ends up filing a ticket";
    EXPECT_NE(c.remedy.find("web_port"), std::string::npos)
        << "the remedy must name the setting to change";
}

TEST(PreflightServiceTest, ANonsensePortIsRejectedWithoutTouchingTheNetwork) {
    for (int p : {0, -1, 70000}) {
        const auto c = PreflightService::checkPort("0.0.0.0", p);
        EXPECT_FALSE(c.ok) << "port " << p << " was accepted";
        EXPECT_FALSE(c.remedy.empty());
    }
}

TEST(PreflightServiceTest, TheCheckLeavesNothingBehind) {
    // A probe that changed what it inspected would be a worse version of the bug
    // it replaces: the check must not itself occupy the port it just tested.
    const auto first = PreflightService::checkPort("0.0.0.0", 47914);
    const auto second = PreflightService::checkPort("0.0.0.0", 47914);
    EXPECT_TRUE(first.ok) << first.detail;
    EXPECT_TRUE(second.ok)
        << "the first check was still holding the port when the second ran";
}

TEST(PreflightServiceTest, AWritableDirectoryPassesAndIsCreatedIfMissing) {
    // A first run legitimately has no data directory, and refusing to start over
    // that would break every fresh install.
    const auto dir = std::filesystem::temp_directory_path() /
                     ("hms_pf_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    const auto c = PreflightService::checkWritableDir("data_dir", dir.string());
    EXPECT_TRUE(c.ok) << c.detail;
    EXPECT_TRUE(std::filesystem::exists(dir, ec));

    std::filesystem::remove_all(dir, ec);
}

TEST(PreflightServiceTest, TheWriteTestFileIsCleanedUp) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("hms_pf_clean_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    ASSERT_TRUE(PreflightService::checkWritableDir("data_dir", dir.string()).ok);
    EXPECT_FALSE(std::filesystem::exists(dir / ".hms-write-test", ec))
        << "the probe left its own scratch file behind";

    std::filesystem::remove_all(dir, ec);
}

TEST(PreflightServiceTest, AnUnconfiguredDirectoryIsNotAFailure) {
    // Empty means "not set", which is a legitimate state for an optional path.
    const auto c = PreflightService::checkWritableDir("archive_dir", "");
    EXPECT_TRUE(c.ok);
}

TEST(PreflightServiceTest, AnUncompiledBackendIsRefusedWithTheBackendNamed) {
    AppConfig cfg;
    cfg.database.type = "oracle";
    const auto c = PreflightService::checkDatabase(cfg);
    EXPECT_FALSE(c.ok);
    EXPECT_NE(c.detail.find("oracle"), std::string::npos);
    EXPECT_FALSE(c.remedy.empty());
}

TEST(PreflightServiceTest, ALocalSourceWithNoFolderIsFatal) {
    AppConfig cfg;
    cfg.source = "local";
    cfg.local_dir = "";
    const auto c = PreflightService::checkSource(cfg);
    EXPECT_FALSE(c.ok);
    EXPECT_TRUE(c.fatal);
    EXPECT_NE(c.remedy.find("local_dir"), std::string::npos);
}

TEST(PreflightServiceTest, ALocalSourcePointingNowhereIsFatalAndNamesThePath) {
    AppConfig cfg;
    cfg.source = "local";
    cfg.local_dir = "/definitely/not/here/DATALOG";
    const auto c = PreflightService::checkSource(cfg);
    EXPECT_FALSE(c.ok);
    EXPECT_NE(c.detail.find("/definitely/not/here/DATALOG"), std::string::npos);
}

TEST(PreflightServiceTest, AnUnreachableNetworkSourceDoesNotBlockStartup) {
    // THE regression that matters most. The ezShare card is powered by the CPAP
    // machine and is normally unreachable when a laptop boots. Treating that as
    // a startup failure would refuse to run for every laptop user, which is most
    // of them.
    AppConfig cfg;
    cfg.source = "ezshare";
    cfg.ezshare_url = "http://192.0.2.1";     // TEST-NET-1, guaranteed dead
    const auto c = PreflightService::checkSource(cfg);
    EXPECT_TRUE(c.ok);
    EXPECT_FALSE(c.fatal) << "network reachability must never be fatal at startup";
}

TEST(PreflightServiceTest, AWarningDoesNotFailTheReport) {
    PreflightService::Report r;
    r.checks.push_back({"a", true,  "fine",   "",      true});
    r.checks.push_back({"b", false, "iffy",   "maybe", false});   // warning
    EXPECT_TRUE(r.ok()) << "a non-fatal check blocked startup";

    r.checks.push_back({"c", false, "broken", "fix it", true});   // fatal
    EXPECT_FALSE(r.ok());
}

TEST(PreflightServiceTest, TheRenderedReportShowsFailuresAndTheirRemedies) {
    // The report IS the user interface here: the installer prints it, systemd
    // logs it, and the tray dialog quotes it verbatim. If a remedy does not
    // survive rendering, nobody ever sees it.
    PreflightService::Report r;
    r.checks.push_back({"web_port", false, "port 8893 is already in use",
                        "Set \"web_port\" in config.json", true});
    const auto text = r.render();

    EXPECT_NE(text.find("FAIL"), std::string::npos);
    EXPECT_NE(text.find("web_port"), std::string::npos);
    EXPECT_NE(text.find("8893"), std::string::npos);
    EXPECT_NE(text.find("config.json"), std::string::npos);
}

TEST(PreflightServiceTest, AFullRunCoversTheThingsThatCanBeKnownLocally) {
    AppConfig cfg;
    cfg.web_port = 47915;
    cfg.database.type = "sqlite";
    cfg.database.sqlite_path =
        (std::filesystem::temp_directory_path() /
         ("hms_pf_run_" + std::to_string(::getpid()) + ".db")).string();
    cfg.source = "ezshare";

    const auto r = PreflightService::run(cfg);
    EXPECT_TRUE(r.ok()) << r.render();

    // Named rather than counted: a future check should extend this list, and a
    // count would pass while silently having dropped one of these.
    for (const char* want : {"data_dir", "web_port", "database", "source"}) {
        bool found = false;
        for (const auto& c : r.checks) if (c.name == want) found = true;
        EXPECT_TRUE(found) << "the report is missing the " << want << " check";
    }
}

}  // namespace
