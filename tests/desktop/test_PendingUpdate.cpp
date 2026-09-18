// SDD-041 §3.4: the supervisor acts on pending.json without asking again, so
// what it accepts, and the exact command line it hands the helper, are pinned
// here rather than discovered by installing things.

#include <gtest/gtest.h>

#include "PendingUpdate.h"

#include <nlohmann/json.hpp>

using namespace cpapdash::supervisor;

namespace {

nlohmann::json goodMac() {
    return {{"version", "5.4.0"}, {"from", "5.3.0"},
            {"file", "/Users/me/.hms-cpap/update/CpapDash.dmg"}, {"name", "CpapDash.dmg"},
            {"kind", "dmg"}, {"platform", "macos-arm64"},
            {"sha256", std::string(64, 'a')}, {"size", 47238335}};
}

}  // namespace

TEST(PendingUpdate, TheExitCodeMatchesTheService) {
    EXPECT_EQ(kExitPendingUpdate, 42) << "hms_cpap::kExitApplyUpdate; both sides must agree";
}

TEST(PendingUpdate, AWellFormedFileIsAccepted) {
    std::string why;
    auto p = parsePendingUpdate(goodMac().dump(), &why);
    ASSERT_TRUE(p.has_value()) << why;
    EXPECT_EQ(p->version, "5.4.0");
    EXPECT_EQ(p->kind, "dmg");
    EXPECT_EQ(p->size, 47238335);
}

TEST(PendingUpdate, AWindowsInstallerIsAccepted) {
    auto j = goodMac();
    j["platform"] = "windows-x64";
    j["kind"] = "installer";
    j["name"] = "CpapDashDesktop-Setup.exe";
    EXPECT_TRUE(parsePendingUpdate(j.dump(), nullptr).has_value());
}

TEST(PendingUpdate, AnythingMissingOrWrongIsRefusedWithAReason) {
    struct Case { const char* field; nlohmann::json value; const char* expect; };
    const Case cases[] = {
        {"version", "", "no version"},
        {"file", "", "no file"},
        {"sha256", "abc", "no valid SHA-256"},
        {"sha256", std::string(64, 'G'), "no valid SHA-256"},
        {"size", 0, "no size"},
        {"platform", "linux-armhf", "has no helper"},
        {"kind", "zip", "cannot be installed"},
    };
    for (const auto& c : cases) {
        auto j = goodMac();
        j[c.field] = c.value;
        std::string why;
        EXPECT_FALSE(parsePendingUpdate(j.dump(), &why).has_value()) << c.field;
        EXPECT_NE(why.find(c.expect), std::string::npos) << c.field << ": " << why;
    }
    std::string why;
    EXPECT_FALSE(parsePendingUpdate("not json", &why).has_value());
    EXPECT_FALSE(why.empty());
}

TEST(PendingUpdate, TheMacHelperGetsEverythingByFlag) {
    HelperParams p;
    p.platform = "macos-arm64";
    p.helper = "/tmp/cpapdash-update-1.sh";
    p.pending = "/Users/me/.hms-cpap/update/pending.json";
    p.install = "/Applications/CpapDash.app";
    p.data_dir = "/Users/me/.hms-cpap";
    p.supervisor_pid = 4242;
    p.port = 8893;
    const auto l = helperLaunch(p);
    EXPECT_EQ(l.program, "/bin/bash");
    const std::vector<std::string> expect = {
        "/tmp/cpapdash-update-1.sh",
        "--pending", "/Users/me/.hms-cpap/update/pending.json",
        "--app", "/Applications/CpapDash.app",
        "--data-dir", "/Users/me/.hms-cpap",
        "--pid", "4242",
        "--port", "8893"};
    EXPECT_EQ(l.args, expect);
}

TEST(PendingUpdate, TheWindowsHelperRunsHiddenWithoutAPolicyPrompt) {
    HelperParams p;
    p.platform = "windows-x64";
    p.helper = "C:\\Users\\me\\AppData\\Local\\Temp\\cpapdash-update-1.ps1";
    p.pending = "C:\\Users\\me\\.hms-cpap\\update\\pending.json";
    p.install = "C:\\Users\\me\\AppData\\Local\\Programs\\HMS-CPAP";
    p.data_dir = "C:\\Users\\me\\.hms-cpap";
    p.supervisor_pid = 77;
    p.port = 9000;
    const auto l = helperLaunch(p);
    EXPECT_EQ(l.program, "powershell.exe");
    ASSERT_GE(l.args.size(), 7u);
    EXPECT_EQ(l.args[0], "-NoProfile");
    EXPECT_EQ(l.args[2], "Bypass");
    EXPECT_EQ(l.args[5], "-File");
    EXPECT_EQ(l.args[6], p.helper);
    EXPECT_EQ(l.args.back(), "9000");
}

TEST(PendingUpdate, NoHelperForAPlatformWithoutOne) {
    HelperParams p;
    p.platform = "linux-armhf";
    EXPECT_TRUE(helperLaunch(p).program.empty());
}
