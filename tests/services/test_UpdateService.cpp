// SDD-041 §5: the check. A newer stable release is offered, the same one is
// not, a 304 changes nothing, a failure keeps the last answer, and only files
// the manifest vouches for, for this platform, are ever offered.
#include <gtest/gtest.h>
#include "services/UpdateService.h"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <unistd.h>
#include <vector>

using namespace hms_cpap;
using update::compareVersions;

namespace {

const std::string kSha(64, 'a');

void setEnv(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

std::string releaseJson(const std::string& tag, bool with_manifest = true,
                        const std::vector<std::string>& extra_assets = {}) {
    std::string assets;
    auto add = [&](const std::string& name) {
        if (!assets.empty()) assets += ",";
        assets += R"({"name":")" + name + R"(","browser_download_url":"https://dl/)" + name + R"("})";
    };
    if (with_manifest) add("manifest.json");
    for (const auto& a : extra_assets) add(a);
    return R"({"tag_name":")" + tag + R"(","body":"notes for )" + tag
         + R"(","html_url":"https://gh/)" + tag + R"(","prerelease":false,"draft":false,"assets":[)"
         + assets + "]}";
}

std::string manifestJson(const std::string& platform, const std::string& name) {
    return R"({"version":"9.9.9","assets":[{"platform":")" + platform + R"(","kind":"zip","name":")"
         + name + R"(","size":1234,"sha256":")" + kSha + R"("}]})";
}

// A fake GitHub: answers by URL, and records what it was asked.
struct FakeGitHub {
    std::string release;
    std::string manifest;
    long release_status = 200;
    std::string etag = "\"e1\"";
    std::vector<std::string> if_none_match_seen;
    int manifest_calls = 0;

    UpdateService::HttpGet fn() {
        return [this](const std::string& url, const std::string& inm) {
            UpdateService::HttpResult r;
            if (url.find("/releases/latest") != std::string::npos) {
                if_none_match_seen.push_back(inm);
                if (!inm.empty() && inm == etag) { r.status = 304; return r; }
                r.status = release_status;
                if (r.status == 200) { r.body = release; r.etag = etag; }
                return r;
            }
            if (url == "https://dl/manifest.json") {
                ++manifest_calls;
                r.status = 200;
                r.body = manifest;
                return r;
            }
            r.status = 404;
            return r;
        };
    }
};

class UpdateServiceTest : public ::testing::Test {
protected:
    void SetUp() override { setEnv("HMS_CPAP_CONTAINER", "0"); }
    void TearDown() override { setEnv("HMS_CPAP_CONTAINER", nullptr); }
};

}  // namespace

TEST(UpdateVersions, CompareNumericallyNotAsText) {
    EXPECT_EQ(compareVersions("5.3.0", "5.2.19"), 1);
    EXPECT_EQ(compareVersions("5.2.19", "5.3.0"), -1);
    EXPECT_EQ(compareVersions("5.10.0", "5.9.9"), 1);
    EXPECT_EQ(compareVersions("v5.3.0", "5.3.0"), 0);
    EXPECT_EQ(compareVersions("5.3", "5.3.0"), 0);
    EXPECT_EQ(compareVersions("5.3.1", "5.3"), 1);
}

TEST(UpdateManifest, OnlyThisPlatformAndOnlyFilesTheReleaseCarries) {
    std::string manifest = R"({"assets":[
        {"platform":"windows-x64","kind":"installer","name":"W.exe","size":10,"sha256":")" + kSha + R"("},
        {"platform":"macos-arm64","kind":"dmg","name":"M.dmg","size":20,"sha256":")" + kSha + R"("},
        {"platform":"macos-arm64","kind":"zip","name":"gone.zip","size":30,"sha256":")" + kSha + R"("},
        {"platform":"macos-arm64","kind":"zip","name":"bad.zip","size":30,"sha256":"short"}]})";
    std::map<std::string, std::string> urls{
        {"W.exe", "https://dl/W.exe"}, {"M.dmg", "https://dl/M.dmg"}, {"bad.zip", "https://dl/bad.zip"}};

    auto mac = update::assetsForPlatform(manifest, "macos-arm64", urls);
    ASSERT_EQ(mac.size(), 1u) << "gone.zip is not in the release, bad.zip has no real checksum";
    EXPECT_EQ(mac[0].name, "M.dmg");
    EXPECT_EQ(mac[0].url, "https://dl/M.dmg");
    EXPECT_EQ(mac[0].size, 20);

    EXPECT_TRUE(update::assetsForPlatform(manifest, "linux-armhf", urls).empty());
    EXPECT_TRUE(update::assetsForPlatform(manifest, "", urls).empty());
    EXPECT_TRUE(update::assetsForPlatform("not json", "macos-arm64", urls).empty());
}

TEST(UpdateRelease, ParsesTheFieldsItNeeds) {
    update::ReleaseInfo info;
    ASSERT_TRUE(update::parseRelease(releaseJson("v5.4.0", true, {"a.zip"}), info));
    EXPECT_EQ(info.tag, "v5.4.0");
    EXPECT_EQ(info.html_url, "https://gh/v5.4.0");
    EXPECT_EQ(info.download_urls.at("a.zip"), "https://dl/a.zip");
    EXPECT_FALSE(update::parseRelease("{}", info));
    EXPECT_FALSE(update::parseRelease("<html>rate limited</html>", info));
}

TEST_F(UpdateServiceTest, ANewerReleaseForThisPlatformIsOffered) {
    const std::string platform = update::platformKey();
    if (platform.empty()) GTEST_SKIP() << "no release is published for this platform";
    FakeGitHub gh;
    gh.release = releaseJson("v5.4.0", true, {"app.zip"});
    gh.manifest = manifestJson(platform, "app.zip");

    UpdateService svc("5.3.0", "hms-homelab/hms-cpap", gh.fn());
    auto s = svc.checkNow();
    EXPECT_TRUE(s.checked);
    EXPECT_TRUE(s.available);
    EXPECT_EQ(s.latest, "5.4.0");
    EXPECT_EQ(s.current, "5.3.0");
    EXPECT_EQ(s.notes, "notes for v5.4.0");
    ASSERT_EQ(s.assets.size(), 1u);
    EXPECT_EQ(s.assets[0].url, "https://dl/app.zip");
    EXPECT_TRUE(s.error.empty());
}

TEST_F(UpdateServiceTest, TheSameOrAnOlderVersionIsNotOffered) {
    FakeGitHub gh;
    gh.release = releaseJson("v5.3.0", true, {"app.zip"});
    gh.manifest = manifestJson(update::platformKey(), "app.zip");

    UpdateService same("5.3.0", "hms-homelab/hms-cpap", gh.fn());
    auto s = same.checkNow();
    EXPECT_TRUE(s.checked);
    EXPECT_FALSE(s.available);
    EXPECT_EQ(s.latest, "5.3.0");
    EXPECT_EQ(gh.manifest_calls, 0) << "nothing newer, so the manifest is not even fetched";

    UpdateService ahead("5.4.0", "hms-homelab/hms-cpap", gh.fn());
    EXPECT_FALSE(ahead.checkNow().available);
}

TEST_F(UpdateServiceTest, AManifestForAnotherPlatformIsNotOffered) {
    FakeGitHub gh;
    gh.release = releaseJson("v5.4.0", true, {"other.zip"});
    gh.manifest = manifestJson("some-other-platform", "other.zip");
    UpdateService svc("5.3.0", "hms-homelab/hms-cpap", gh.fn());
    auto s = svc.checkNow();
    EXPECT_EQ(s.latest, "5.4.0");
    EXPECT_FALSE(s.available);
}

TEST_F(UpdateServiceTest, AReleaseWithoutAManifestOffersNothing) {
    FakeGitHub gh;
    gh.release = releaseJson("v5.4.0", false, {"app.zip"});
    UpdateService svc("5.3.0", "hms-homelab/hms-cpap", gh.fn());
    auto s = svc.checkNow();
    EXPECT_FALSE(s.available);
    EXPECT_EQ(s.error, "release has no manifest.json");
}

TEST_F(UpdateServiceTest, APreReleaseIsIgnored) {
    FakeGitHub gh;
    gh.release = releaseJson("v5.4.0", true, {"app.zip"});
    gh.release.replace(gh.release.find(R"("prerelease":false)"), 18, R"("prerelease":true)");
    gh.manifest = manifestJson(update::platformKey(), "app.zip");
    UpdateService svc("5.3.0", "hms-homelab/hms-cpap", gh.fn());
    EXPECT_FALSE(svc.checkNow().available);
}

TEST_F(UpdateServiceTest, A304ChangesNothingAndSendsTheETag) {
    const std::string platform = update::platformKey();
    if (platform.empty()) GTEST_SKIP() << "no release is published for this platform";
    FakeGitHub gh;
    gh.release = releaseJson("v5.4.0", true, {"app.zip"});
    gh.manifest = manifestJson(platform, "app.zip");
    UpdateService svc("5.3.0", "hms-homelab/hms-cpap", gh.fn());

    auto first = svc.checkNow();
    auto second = svc.checkNow();
    ASSERT_EQ(gh.if_none_match_seen.size(), 2u);
    EXPECT_EQ(gh.if_none_match_seen[0], "");
    EXPECT_EQ(gh.if_none_match_seen[1], "\"e1\"");
    EXPECT_TRUE(second.available);
    EXPECT_EQ(second.latest, first.latest);
    EXPECT_EQ(second.assets.size(), first.assets.size());
}

TEST_F(UpdateServiceTest, AFailedCheckKeepsTheLastAnswer) {
    const std::string platform = update::platformKey();
    if (platform.empty()) GTEST_SKIP() << "no release is published for this platform";
    FakeGitHub gh;
    gh.release = releaseJson("v5.4.0", true, {"app.zip"});
    gh.manifest = manifestJson(platform, "app.zip");
    UpdateService svc("5.3.0", "hms-homelab/hms-cpap", gh.fn());
    ASSERT_TRUE(svc.checkNow().available);

    gh.etag = "\"e2\"";      // so the next call is not a 304
    gh.release_status = 0;   // unreachable
    auto s = svc.checkNow();
    EXPECT_TRUE(s.available) << "a network blip does not take the banner away";
    EXPECT_EQ(s.error, "GitHub unreachable");

    gh.release_status = 403; // rate limited
    EXPECT_EQ(svc.checkNow().error, "GitHub answered 403");
}

TEST_F(UpdateServiceTest, AFirstFailedCheckOffersNothing) {
    FakeGitHub gh;
    gh.release_status = 500;
    UpdateService svc("5.3.0", "hms-homelab/hms-cpap", gh.fn());
    auto s = svc.checkNow();
    EXPECT_FALSE(s.available);
    EXPECT_FALSE(s.checked);
    EXPECT_EQ(s.error, "GitHub answered 500");
}

TEST(UpdateContainer, AContainerNeverAsksGitHub) {
    setEnv("HMS_CPAP_CONTAINER", "1");
    bool asked = false;
    UpdateService svc("5.3.0", "hms-homelab/hms-cpap",
                      [&](const std::string&, const std::string&) {
                          asked = true;
                          return UpdateService::HttpResult{};
                      });
    auto s = svc.checkNow();
    EXPECT_TRUE(s.containerised);
    EXPECT_FALSE(s.available);
    EXPECT_FALSE(asked);
    setEnv("HMS_CPAP_CONTAINER", nullptr);
}

TEST(UpdateContainer, TheEnvironmentDecidesBothWays) {
    setEnv("HMS_CPAP_CONTAINER", "1");
    EXPECT_TRUE(update::isContainerised());
    setEnv("HMS_CPAP_CONTAINER", "0");
    EXPECT_FALSE(update::isContainerised());
    setEnv("HMS_CPAP_CONTAINER", nullptr);
}

// ── SDD-041 step 3: apply ───────────────────────────────────────────────────
//
// §5: a mismatched SHA-256 refuses, a truncated download refuses, nothing is
// handed off unless the file is exactly what the manifest says. The platform
// and the handoff are pinned per test, so the macOS, Windows and Pi paths all
// run on whatever machine runs the suite.

namespace {

namespace fs = std::filesystem;

const std::string kPayload = "pretend this is CpapDash.dmg";

struct ApplyRig {
    fs::path dir;
    FakeGitHub gh;
    std::string payload = kPayload;   // what the fake download writes
    int exit_code = -1;
    int downloads = 0;
    int backups = 0;
    bool busy = false;
    std::unique_ptr<UpdateService> svc;

    ApplyRig(const std::string& platform, update::Handoff handoff, const std::string& kind,
             const std::string& name) {
        dir = fs::temp_directory_path() / ("hms_update_" + std::to_string(::getpid()) + "_" + kind);
        fs::remove_all(dir);
        fs::create_directories(dir);

        // The manifest's checksum is of the REAL payload, written once here.
        const fs::path ref = dir / "reference";
        { std::ofstream(ref, std::ios::binary) << kPayload; }
        nlohmann::json man = {{"version", "5.4.0"},
                              {"assets", {{{"platform", platform}, {"kind", kind}, {"name", name},
                                           {"size", kPayload.size()},
                                           {"sha256", update::sha256File(ref.string())}}}}};
        gh.release = releaseJson("v5.4.0", true, {name});
        gh.manifest = man.dump();

        svc = std::make_unique<UpdateService>("5.3.0", "hms-homelab/hms-cpap", gh.fn());
        svc->setPlatform(platform);
        svc->setHandoff(handoff);
        svc->setDataDir(dir.string());
        UpdateService::Hooks hooks;
        hooks.busy = [this] { return busy; };
        hooks.backup = [this](const std::string&) { ++backups; return std::string(); };
        hooks.download = [this](const std::string&, const std::string& path) {
            ++downloads;
            std::ofstream(path, std::ios::binary) << payload;
            return true;
        };
        hooks.exit = [this](int code) { exit_code = code; };
        svc->setHooks(std::move(hooks));
        svc->checkNow();
    }
    ~ApplyRig() { svc.reset(); fs::remove_all(dir); }
};

}  // namespace

TEST_F(UpdateServiceTest, AVerifiedDownloadIsStagedAndHandedToTheSupervisor) {
    ApplyRig rig("macos-arm64", update::Handoff::Supervisor, "dmg", "CpapDash.dmg");
    ASSERT_TRUE(rig.svc->status().can_apply);

    ASSERT_EQ(rig.svc->apply(false), UpdateService::ApplyRefusal::None);
    rig.svc->waitForApply();

    EXPECT_EQ(rig.exit_code, kExitApplyUpdate);
    EXPECT_EQ(rig.backups, 1) << "D6: the database is copied before the handoff";
    const fs::path staged = rig.dir / "update" / "CpapDash.dmg";
    EXPECT_TRUE(fs::exists(staged));
    EXPECT_FALSE(fs::exists(staged.string() + ".part"));

    std::ifstream in(rig.dir / "update" / "pending.json");
    auto pending = nlohmann::json::parse(in);
    EXPECT_EQ(pending["version"], "5.4.0");
    EXPECT_EQ(pending["from"], "5.3.0");
    EXPECT_EQ(pending["kind"], "dmg");
    EXPECT_EQ(pending["file"], fs::absolute(staged).string());
    EXPECT_EQ(pending["sha256"], update::sha256File(staged.string()));
    EXPECT_EQ(rig.svc->status().apply_step, "handing_off");
}

TEST_F(UpdateServiceTest, CheckNowAnswersWithTheSameFieldsAsStatus) {
    // "Check now" returned the raw state, without can_apply: the Apply button
    // stayed hidden until the page was reloaded. Found in the macOS e2e.
    ApplyRig rig("macos-arm64", update::Handoff::Supervisor, "dmg", "CpapDash.dmg");
    const auto checked = rig.svc->checkNow();
    EXPECT_TRUE(checked.can_apply);
    EXPECT_EQ(checked.installer, "supervisor");
    EXPECT_EQ(checked.can_apply, rig.svc->status().can_apply);
}

TEST_F(UpdateServiceTest, WindowsHandsOffTheInstaller) {
    ApplyRig rig("windows-x64", update::Handoff::Supervisor, "installer", "CpapDashDesktop-Setup.exe");
    ASSERT_EQ(rig.svc->apply(false), UpdateService::ApplyRefusal::None);
    rig.svc->waitForApply();
    EXPECT_EQ(rig.exit_code, kExitApplyUpdate);
    EXPECT_TRUE(fs::exists(rig.dir / "update" / "CpapDashDesktop-Setup.exe"));
}

TEST_F(UpdateServiceTest, AMismatchedChecksumRefusesAndLeavesNothingBehind) {
    ApplyRig rig("macos-arm64", update::Handoff::Supervisor, "dmg", "CpapDash.dmg");
    rig.payload = "pretend this is CpapDash.dmX";   // same size, different bytes
    ASSERT_EQ(rig.svc->apply(false), UpdateService::ApplyRefusal::None);
    rig.svc->waitForApply();

    EXPECT_EQ(rig.exit_code, -1) << "nothing is handed off";
    EXPECT_EQ(rig.backups, 0);
    EXPECT_FALSE(fs::exists(rig.dir / "update" / "CpapDash.dmg"));
    EXPECT_FALSE(fs::exists(rig.dir / "update" / "CpapDash.dmg.part"));
    EXPECT_FALSE(fs::exists(rig.dir / "update" / "pending.json"));
    const auto s = rig.svc->status();
    EXPECT_FALSE(s.applying);
    EXPECT_EQ(s.error, "CpapDash.dmg does not match the manifest's SHA-256");
}

TEST_F(UpdateServiceTest, ATruncatedDownloadRefuses) {
    ApplyRig rig("macos-arm64", update::Handoff::Supervisor, "dmg", "CpapDash.dmg");
    rig.payload = kPayload.substr(0, 10);
    ASSERT_EQ(rig.svc->apply(false), UpdateService::ApplyRefusal::None);
    rig.svc->waitForApply();
    EXPECT_EQ(rig.exit_code, -1);
    EXPECT_NE(rig.svc->status().error.find("the manifest says"), std::string::npos);
}

TEST_F(UpdateServiceTest, ABusyCollectorWaitsUnlessTheUserSaysNow) {
    ApplyRig rig("macos-arm64", update::Handoff::Supervisor, "dmg", "CpapDash.dmg");
    rig.busy = true;
    EXPECT_EQ(rig.svc->apply(false), UpdateService::ApplyRefusal::Busy);
    EXPECT_EQ(rig.downloads, 0);
    EXPECT_FALSE(rig.svc->status().applying);

    EXPECT_EQ(rig.svc->apply(true), UpdateService::ApplyRefusal::None);
    rig.svc->waitForApply();
    EXPECT_EQ(rig.exit_code, kExitApplyUpdate);
}

TEST_F(UpdateServiceTest, NothingToInstallWithNoSupervisorOrSystemd) {
    ApplyRig rig("macos-arm64", update::Handoff::None, "dmg", "CpapDash.dmg");
    const auto s = rig.svc->status();
    EXPECT_TRUE(s.available) << "the banner still says an update exists";
    EXPECT_FALSE(s.can_apply) << "but offers no button that cannot work";
    EXPECT_EQ(s.installer, "");
    EXPECT_EQ(rig.svc->apply(true), UpdateService::ApplyRefusal::NoInstaller);
    EXPECT_EQ(rig.downloads, 0);
}

TEST_F(UpdateServiceTest, ThePiAsksRootByVersionAndDownloadsNothing) {
    ApplyRig rig("linux-armhf", update::Handoff::Systemd, "zip", "hms-cpap-linux-armhf.zip");
    ASSERT_TRUE(rig.svc->status().can_apply);
    EXPECT_EQ(rig.svc->status().installer, "systemd");
    ASSERT_EQ(rig.svc->apply(false), UpdateService::ApplyRefusal::None);
    rig.svc->waitForApply();

    EXPECT_EQ(rig.downloads, 0) << "D10: root fetches and verifies the release itself";
    EXPECT_EQ(rig.exit_code, -1) << "systemd stops the service; it does not leave on its own";
    EXPECT_EQ(rig.backups, 1);
    std::ifstream req(rig.dir / "update" / "request");
    std::string version;
    std::getline(req, version);
    EXPECT_EQ(version, "5.4.0");
}

TEST_F(UpdateServiceTest, NoUpdateNothingToApply) {
    FakeGitHub gh;
    gh.release = releaseJson("v5.3.0", true, {});
    UpdateService svc("5.3.0", "hms-homelab/hms-cpap", gh.fn());
    svc.setHandoff(update::Handoff::Supervisor);
    svc.checkNow();
    EXPECT_EQ(svc.apply(true), UpdateService::ApplyRefusal::NotAvailable);
}

TEST(UpdateHelpers, Sha256MatchesTheStandardVector) {
    auto p = fs::temp_directory_path() / ("hms_sha_" + std::to_string(::getpid()));
    { std::ofstream(p, std::ios::binary) << "abc"; }
    EXPECT_EQ(update::sha256File(p.string()),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    fs::remove(p);
    EXPECT_EQ(update::sha256File("/no/such/file"), "");
}

TEST(UpdateHelpers, TheHelpersResultIsReadBack) {
    auto p = fs::temp_directory_path() / ("hms_result_" + std::to_string(::getpid()) + ".json");
    { std::ofstream(p) << R"({"ok":false,"version":"5.4.0","step":"health","message":"old version answered","at":"2026-09-18T20:00:00Z"})"; }
    auto r = update::readResult(p.string());
    EXPECT_TRUE(r.present);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.step, "health");
    EXPECT_EQ(r.message, "old version answered");
    fs::remove(p);
    EXPECT_FALSE(update::readResult(p.string()).present);
}

TEST(UpdateHelpers, SqliteIsCopiedWhileOpen) {
    auto dir = fs::temp_directory_path() / ("hms_bak_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const auto src = (dir / "cpap.db").string();
    const auto dst = (dir / "backup.db").string();

    sqlite3* live = nullptr;   // held open, as the service holds it
    ASSERT_EQ(sqlite3_open(src.c_str(), &live), SQLITE_OK);
    sqlite3_exec(live, "CREATE TABLE t(x); INSERT INTO t VALUES (1),(2),(3);", nullptr, nullptr, nullptr);

    EXPECT_EQ(update::backupSqlite(src, dst), "");
    EXPECT_EQ(update::backupSqlite(src, dst), "") << "a second backup replaces the first";

    sqlite3* copy = nullptr;
    ASSERT_EQ(sqlite3_open(dst.c_str(), &copy), SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(copy, "SELECT COUNT(*) FROM t", -1, &st, nullptr);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(st, 0), 3);
    sqlite3_finalize(st);
    sqlite3_close(copy);
    sqlite3_close(live);

    EXPECT_NE(update::backupSqlite((dir / "missing.db").string(), dst), "");
    fs::remove_all(dir);
}

TEST(UpdateHelpers, ThePiUnitIsSystemdEvenThoughItAlsoSetsSupervised) {
    // packaging/pi/hms-cpap.service sets BOTH. Read as a desktop, a Pi would
    // exit 42 for a helper it does not have and systemd would loop it.
    setEnv("HMS_CPAP_SUPERVISED", "1");
    setEnv("HMS_CPAP_UPDATER", "systemd");
    EXPECT_EQ(update::handoffMode(), update::Handoff::Systemd);
    setEnv("HMS_CPAP_UPDATER", nullptr);
    EXPECT_EQ(update::handoffMode(), update::Handoff::Supervisor);
    setEnv("HMS_CPAP_SUPERVISED", nullptr);
    EXPECT_EQ(update::handoffMode(), update::Handoff::None);
}

TEST(UpdateHelpers, EachHandoffInstallsItsOwnKindOfFile) {
    using update::Handoff;
    EXPECT_EQ(update::assetKindFor(Handoff::Supervisor, "macos-arm64"), "dmg");
    EXPECT_EQ(update::assetKindFor(Handoff::Supervisor, "windows-x64"), "installer");
    EXPECT_EQ(update::assetKindFor(Handoff::Systemd, "linux-armhf"), "zip");
    EXPECT_EQ(update::assetKindFor(Handoff::None, "macos-arm64"), "");
    EXPECT_EQ(update::assetKindFor(Handoff::Systemd, "macos-arm64"), "");
}
