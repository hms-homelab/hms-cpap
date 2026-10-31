#include <gtest/gtest.h>

#include "mule/MuleFirmware.h"
#include "mule/MuleService.h"
#include "services/As11NightWriter.h"

#include <json/json.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Updating a Mule & Miner over the local network: a fake unit sits on the
// receiver (its session names its address) and a fake transport stands in
// for the unit's own update endpoint and for the published manifest. Every
// image here is a few made-up bytes; nothing is flashed anywhere.

using namespace hms_cpap;
using namespace hms_cpap::mule;
namespace fs = std::filesystem;

namespace {

constexpr const char* kUnit = "FE-C3-00000001";
constexpr const char* kIp   = "192.168.1.50";

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

std::string manifestJson(const std::string& mule, const std::string& miner) {
    Json::Value m(Json::objectValue);
    m["mule"]["version"]  = mule;
    m["mule"]["url"]      = "https://fw.example.test/public/" + mule + "/mule.bin";
    m["miner"]["version"] = miner;
    m["miner"]["url"]     = "https://fw.example.test/public/" + miner + "/miner.bin";
    return compact(m);
}

// The unit's end of its stream connection, as far as an update cares.
class FakeUnit {
public:
    FakeUnit(MuleService& svc, const std::string& ip = kIp) : svc_(svc) {
        conn_ = svc_.open(kUnit, [](const std::string&) { return true; }, ip);
    }
    ~FakeUnit() { close(); }
    void close() {
        if (conn_) svc_.closed(conn_);
        conn_ = 0;
    }
    void hello(const std::string& fw, const std::string& miner_fw, bool streaming = false) {
        Json::Value h(Json::objectValue);
        h["t"]        = "hello";
        h["fw"]       = fw;
        h["miner_fw"] = miner_fw;
        h["mode"]     = "as11_ble";
        h["state"]    = state(streaming, 0);
        svc_.onText(conn_, compact(h));
    }
    void sendState(bool streaming, int buffered, const std::string& miner_fw = "") {
        Json::Value f(Json::objectValue);
        f["t"]     = "state";
        f["state"] = state(streaming, buffered);
        if (!miner_fw.empty()) f["state"]["miner_fw_version"] = miner_fw;
        svc_.onText(conn_, compact(f));
    }
    static Json::Value state(bool streaming, int buffered) {
        Json::Value s(Json::objectValue);
        s["as11"]["streaming"]       = streaming;
        s["as11"]["stream_buffered"] = buffered;
        return s;
    }

private:
    MuleService& svc_;
    uint64_t     conn_{0};
};

struct Post {
    std::string url;
    std::string body;
    size_t      progress_calls{0};
};

// The unit's update endpoint and the manifest's host, in one.
struct FakeNet {
    std::string                          manifest = manifestJson("6.1.0", "6.1.0");
    long                                 manifest_status{200};
    std::map<std::string, std::string>   images;         // url -> bytes
    long                                 post_status{200};
    std::string                          post_body{"{\"status\":\"ok\"}"};
    std::string                          post_error;     // when set, the POST never completes
    std::vector<Post>                    posts;
    std::function<void(const Post&)>     after_post;     // the unit's side effect (a restart)

    Transport transport() {
        Transport t;
        t.get = [this](const std::string& url, int) {
            HttpReply r;
            r.ok = true;
            if (url.find("latest.json") != std::string::npos) {
                r.status = manifest_status;
                r.body   = manifest_status == 200 ? manifest : "MissingKey";
                return r;
            }
            const auto it = images.find(url);
            r.status      = it == images.end() ? 404 : 200;
            if (it != images.end()) r.body = it->second;
            return r;
        };
        t.post = [this](const std::string& url, const std::string& body, int, const Progress& progress) {
            Post p;
            p.url  = url;
            p.body = body;
            if (progress) {
                progress(body.size() / 2, body.size());
                progress(body.size(), body.size());
                p.progress_calls = 2;
            }
            posts.push_back(p);
            HttpReply r;
            if (!post_error.empty()) {
                r.error = post_error;
                return r;
            }
            r.ok     = true;
            r.status = post_status;
            r.body   = post_body;
            if (after_post) after_post(p);
            return r;
        };
        return t;
    }
};

class MuleFirmwareTest : public ::testing::Test {
protected:
    fs::path                         dir_;
    std::shared_ptr<As11NightWriter> writer_;
    std::unique_ptr<MuleService>     svc_;
    std::unique_ptr<FirmwareUpdater> up_;
    FakeNet                          net_;
    std::unique_ptr<FakeUnit>        unit_;

    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("mule_fw_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
        writer_ = std::make_shared<As11NightWriter>(dir_.string(), fixedOffsetClock(0));
        MuleService::Options o;
        o.root     = (dir_ / "mule").string();
        o.data_dir = dir_.string();
        svc_       = std::make_unique<MuleService>(o, writer_);
        net_.images["https://fw.example.test/public/6.1.0/mule.bin"]  = "MULE-6.1.0-IMAGE";
        net_.images["https://fw.example.test/public/6.1.0/miner.bin"] = "MINER-6.1.0-IMAGE";
        unit_ = std::make_unique<FakeUnit>(*svc_);
        unit_->hello("6.0.0", "6.0.0");
    }
    void TearDown() override {
        unit_.reset();
        up_.reset();
        svc_.reset();
        fs::remove_all(dir_);
    }
    // The updater runs the job before start() returns, and waits little.
    void updater(std::chrono::seconds reconnect_wait = std::chrono::seconds(2)) {
        FirmwareUpdater::Options fo;
        fo.manifest_url   = "https://fw.example.test/public/latest.json";
        fo.async          = false;
        fo.reconnect_wait = reconnect_wait;
        fo.poll           = std::chrono::milliseconds(5);
        up_               = std::make_unique<FirmwareUpdater>(*svc_, net_.transport(), fo);
    }
    // What the unit does after it takes a mule image: restarts and comes
    // back here with the new version in its hello.
    void unitRestartsAs(const std::string& fw, const std::string& miner_fw, bool streaming = false) {
        net_.after_post = [this, fw, miner_fw, streaming](const Post& p) {
            if (p.url.find("/api/update/mule") == std::string::npos) return;
            unit_->close();
            unit_ = std::make_unique<FakeUnit>(*svc_);
            unit_->hello(fw, miner_fw, streaming);
        };
    }
};

}  // namespace

// ── versions and the manifest ──

TEST(MuleFirmwareVersions, DottedNumbersCompareFieldByField) {
    EXPECT_LT(compareVersions("6.0.0", "6.1.0"), 0);
    EXPECT_LT(compareVersions("6.9.9", "10.0.0"), 0);
    EXPECT_EQ(compareVersions("6.1", "6.1.0"), 0);
    EXPECT_GT(compareVersions("6.1.1", "6.1.0-rc1"), 0);
    EXPECT_TRUE(updateAvailable("6.0.0", "6.1.0"));
    EXPECT_FALSE(updateAvailable("6.1.0", "6.1.0"));
    EXPECT_FALSE(updateAvailable("6.2.0", "6.1.0"));
    EXPECT_TRUE(updateAvailable("", "6.1.0"));        // a unit whose version is unknown
    EXPECT_FALSE(updateAvailable("6.0.0", ""));       // no manifest
    EXPECT_FALSE(updateAvailable("6.0.0", "x"));
}

TEST(MuleFirmwareVersions, ManifestNeedsBothImages) {
    std::string why;
    const auto  m = parseManifest(manifestJson("6.1.0", "6.1.1"), &why);
    ASSERT_TRUE(m) << why;
    EXPECT_EQ(m->mule.version, "6.1.0");
    EXPECT_EQ(m->miner.version, "6.1.1");
    EXPECT_EQ(m->miner.url, "https://fw.example.test/public/6.1.1/miner.bin");
    EXPECT_FALSE(parseManifest("{\"mule\":{\"version\":\"6.1.0\",\"url\":\"u\"}}", &why));
    EXPECT_NE(why.find("miner"), std::string::npos);
    EXPECT_FALSE(parseManifest("<xml/>", &why));
}

// ── the owner's file ──

TEST_F(MuleFirmwareTest, AMuleImagePostsToTheUnitsOwnAddressAndWaitsForItsReturn) {
    updater();
    unitRestartsAs("6.1.0", "6.0.0");
    const auto a = up_->start(kUnit, "mule", "MULE-IMAGE-BYTES", "mule-6.1.0.bin");
    ASSERT_EQ(a.status, 202) << compact(a.body);
    ASSERT_EQ(net_.posts.size(), 1u);
    EXPECT_EQ(net_.posts[0].url, std::string("http://") + kIp + "/api/update/mule");
    EXPECT_EQ(net_.posts[0].body, "MULE-IMAGE-BYTES");
    EXPECT_EQ(net_.posts[0].progress_calls, 2u);
    const Json::Value j = up_->job(kUnit);
    EXPECT_EQ(j["phase"].asString(), "done") << compact(j);
    EXPECT_EQ(j["source"].asString(), "file mule-6.1.0.bin");
    EXPECT_EQ(j["fw_before"].asString(), "6.0.0");
    EXPECT_EQ(j["fw_after"].asString(), "6.1.0");
    EXPECT_EQ(j["sent"].asUInt64(), 16u);
    EXPECT_EQ(j["total"].asUInt64(), 16u);
    EXPECT_FALSE(j["running"].asBool());
}

TEST_F(MuleFirmwareTest, AMinerImageGoesThroughTheMuleAndNeedsNoRestart) {
    updater();
    const auto a = up_->start(kUnit, "miner", "MINER-IMAGE", "miner.bin");
    ASSERT_EQ(a.status, 202) << compact(a.body);
    ASSERT_EQ(net_.posts.size(), 1u);
    EXPECT_EQ(net_.posts[0].url, std::string("http://") + kIp + "/api/update/miner");
    EXPECT_EQ(up_->job(kUnit)["phase"].asString(), "done");
    // The miner's new version rides the unit's next state frame.
    unit_->sendState(false, 0, "6.1.0");
    EXPECT_EQ(up_->firmware(kUnit)["miner_fw"].asString(), "6.1.0");
}

TEST_F(MuleFirmwareTest, AnUploadedImageIsOneTargetAndNotTooLarge) {
    updater();
    EXPECT_EQ(up_->start(kUnit, "both", "IMAGE").status, 400);
    EXPECT_EQ(up_->start(kUnit, "coyote", "IMAGE").status, 400);
    EXPECT_EQ(up_->start(kUnit, "mule", std::string(kImageMax + 1, 'x')).status, 400);
    EXPECT_TRUE(net_.posts.empty());
}

// ── downtime ──

TEST_F(MuleFirmwareTest, RefusedWhileTheUnitStreams) {
    updater();
    unit_->sendState(true, 0);
    const auto a = up_->start(kUnit, "mule", "IMAGE");
    EXPECT_EQ(a.status, 409);
    EXPECT_NE(a.body["error"].asString().find("night is open"), std::string::npos);
    EXPECT_TRUE(net_.posts.empty());
    EXPECT_TRUE(up_->firmware(kUnit)["night_open"].asBool());
}

TEST_F(MuleFirmwareTest, RefusedWhileTheNightIsStillLanding) {
    updater();
    unit_->sendState(true, 0);
    unit_->sendState(false, 3);   // the mask is off, the unit still holds blocks
    EXPECT_EQ(up_->start(kUnit, "miner", "IMAGE").status, 409);
    EXPECT_TRUE(net_.posts.empty());
    unit_->sendState(false, 0);   // landed: downtime
    EXPECT_FALSE(up_->firmware(kUnit)["night_open"].asBool());
    EXPECT_EQ(up_->start(kUnit, "miner", "IMAGE").status, 202);
    EXPECT_EQ(net_.posts.size(), 1u);
}

TEST_F(MuleFirmwareTest, ANightThatOpensBetweenTheImagesStopsTheMiner) {
    updater();
    unitRestartsAs("6.1.0", "6.0.0", /*streaming=*/true);   // mask on the moment it is back
    const auto a = up_->start(kUnit, "both");
    ASSERT_EQ(a.status, 202) << compact(a.body);
    ASSERT_EQ(net_.posts.size(), 1u);   // the mule only
    const Json::Value j = up_->job(kUnit);
    EXPECT_EQ(j["phase"].asString(), "failed");
    EXPECT_NE(j["error"].asString().find("night opened"), std::string::npos) << compact(j);
    EXPECT_EQ(j["step"].asString(), "miner");
}

TEST_F(MuleFirmwareTest, RefusedWhenTheUnitIsNotHere) {
    updater();
    EXPECT_EQ(up_->start("FE-C3-00000002", "mule", "IMAGE").status, 404);
    unit_->close();
    const auto a = up_->start(kUnit, "mule", "IMAGE");
    EXPECT_EQ(a.status, 409);
    EXPECT_NE(a.body["error"].asString().find("not connected"), std::string::npos);
    EXPECT_TRUE(net_.posts.empty());
}

// ── the unit's answers ──

TEST_F(MuleFirmwareTest, TheUnitsBusyAnswerEndsTheJob) {
    updater();
    net_.post_status = 503;
    net_.post_body   = "Firmware update in progress";
    EXPECT_EQ(up_->start(kUnit, "miner", "IMAGE").status, 202);
    const Json::Value j = up_->job(kUnit);
    EXPECT_EQ(j["phase"].asString(), "failed");
    EXPECT_NE(j["error"].asString().find("HTTP 503"), std::string::npos);
    EXPECT_NE(j["error"].asString().find("in progress"), std::string::npos);
}

TEST_F(MuleFirmwareTest, ALostConnectionEndsTheJob) {
    updater();
    net_.post_error = "Connection reset by peer";
    EXPECT_EQ(up_->start(kUnit, "miner", "IMAGE").status, 202);
    EXPECT_EQ(up_->job(kUnit)["phase"].asString(), "failed");
    EXPECT_NE(up_->job(kUnit)["error"].asString().find("reset by peer"), std::string::npos);
}

TEST_F(MuleFirmwareTest, AMuleThatDoesNotComeBackIsReported) {
    updater(std::chrono::seconds(0));
    EXPECT_EQ(up_->start(kUnit, "mule", "IMAGE").status, 202);
    const Json::Value j = up_->job(kUnit);
    EXPECT_EQ(j["phase"].asString(), "failed");
    EXPECT_NE(j["error"].asString().find("not come back"), std::string::npos);
}

TEST_F(MuleFirmwareTest, OneUpdateAtATime) {
    FirmwareUpdater::Options fo;
    fo.manifest_url   = "https://fw.example.test/public/latest.json";
    fo.async          = true;
    fo.reconnect_wait = std::chrono::seconds(3);
    fo.poll           = std::chrono::milliseconds(5);
    up_               = std::make_unique<FirmwareUpdater>(*svc_, net_.transport(), fo);
    // The mule never comes back, so the job waits its 3 s: a second start
    // in that window is refused.
    EXPECT_EQ(up_->start(kUnit, "mule", "IMAGE").status, 202);
    const auto again = up_->start(kUnit, "miner", "IMAGE");
    EXPECT_EQ(again.status, 409);
    EXPECT_NE(again.body["error"].asString().find("already running"), std::string::npos);
    EXPECT_TRUE(up_->job(kUnit)["running"].asBool());
}

// ── the manifest ──

TEST_F(MuleFirmwareTest, FirmwareShowsWhatRunsAndWhatIsPublished) {
    updater();
    const Json::Value f = up_->firmware(kUnit);
    EXPECT_TRUE(f["known"].asBool());
    EXPECT_TRUE(f["online"].asBool());
    EXPECT_EQ(f["ip"].asString(), kIp);
    EXPECT_EQ(f["fw"].asString(), "6.0.0");
    EXPECT_EQ(f["miner_fw"].asString(), "6.0.0");
    EXPECT_EQ(f["latest"]["mule"].asString(), "6.1.0");
    EXPECT_EQ(f["latest"]["miner"].asString(), "6.1.0");
    EXPECT_TRUE(f["update_available"]["mule"].asBool());
    EXPECT_TRUE(f["update_available"]["miner"].asBool());
    EXPECT_EQ(f["manifest"]["error"].asString(), "");
    EXPECT_TRUE(f["job"].isNull());
}

TEST_F(MuleFirmwareTest, BothFromTheManifestSendsTheMuleThenTheMiner) {
    updater();
    unitRestartsAs("6.1.0", "6.0.0");
    const auto a = up_->start(kUnit, "both");
    ASSERT_EQ(a.status, 202) << compact(a.body);
    ASSERT_EQ(net_.posts.size(), 2u);
    EXPECT_EQ(net_.posts[0].url, std::string("http://") + kIp + "/api/update/mule");
    EXPECT_EQ(net_.posts[0].body, "MULE-6.1.0-IMAGE");
    EXPECT_EQ(net_.posts[1].url, std::string("http://") + kIp + "/api/update/miner");
    EXPECT_EQ(net_.posts[1].body, "MINER-6.1.0-IMAGE");
    const Json::Value j = up_->job(kUnit);
    EXPECT_EQ(j["phase"].asString(), "done") << compact(j);
    EXPECT_EQ(j["source"].asString(), "manifest 6.1.0");
    EXPECT_EQ(j["fw_after"].asString(), "6.1.0");
}

TEST_F(MuleFirmwareTest, BothSkipsAnImageTheUnitAlreadyRuns) {
    unit_->hello("6.1.0", "6.0.0");   // the mule is current, the miner is not
    updater();
    ASSERT_EQ(up_->start(kUnit, "both").status, 202);
    ASSERT_EQ(net_.posts.size(), 1u);
    EXPECT_EQ(net_.posts[0].url, std::string("http://") + kIp + "/api/update/miner");
    EXPECT_EQ(up_->job(kUnit)["phase"].asString(), "done");
}

TEST_F(MuleFirmwareTest, NothingNewerIsRefused) {
    unit_->hello("6.1.0", "6.1.0");
    updater();
    const auto a = up_->start(kUnit, "both");
    EXPECT_EQ(a.status, 409);
    EXPECT_NE(a.body["error"].asString().find("already runs"), std::string::npos);
    EXPECT_EQ(up_->start(kUnit, "mule").status, 409);
    EXPECT_TRUE(net_.posts.empty());
}

TEST_F(MuleFirmwareTest, AManifestBehindAKeyLeavesTheFileUpload) {
    net_.manifest_status = 403;
    updater();
    const Json::Value f = up_->firmware(kUnit);
    EXPECT_FALSE(f["update_available"]["mule"].asBool());
    EXPECT_EQ(f["latest"]["mule"].asString(), "");
    EXPECT_NE(f["manifest"]["error"].asString().find("403"), std::string::npos);
    const auto a = up_->start(kUnit, "both");
    EXPECT_EQ(a.status, 502);
    EXPECT_NE(a.body["error"].asString().find("Upload the image"), std::string::npos);
    // The owner's own file still goes.
    EXPECT_EQ(up_->start(kUnit, "miner", "IMAGE").status, 202);
    EXPECT_EQ(net_.posts.size(), 1u);
}

TEST_F(MuleFirmwareTest, AnImageThatCannotBeDownloadedEndsTheJobBeforeAnyPost) {
    net_.images.clear();
    updater();
    EXPECT_EQ(up_->start(kUnit, "miner").status, 202);
    EXPECT_TRUE(net_.posts.empty());
    const Json::Value j = up_->job(kUnit);
    EXPECT_EQ(j["phase"].asString(), "failed");
    EXPECT_NE(j["error"].asString().find("HTTP 404"), std::string::npos);
}
