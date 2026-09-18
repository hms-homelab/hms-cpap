// SDD-041 §5: the check. A newer stable release is offered, the same one is
// not, a 304 changes nothing, a failure keeps the last answer, and only files
// the manifest vouches for, for this platform, are ever offered.
#include <gtest/gtest.h>
#include "services/UpdateService.h"

#include <cstdlib>
#include <string>
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
