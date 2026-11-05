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
    // The updater runs each step on the caller's thread: start() runs the
    // first, and the unit's hello (or state) runs what follows.
    void updater() {
        FirmwareUpdater::Options fo;
        fo.manifest_url = "https://fw.example.test/public/latest.json";
        fo.async        = false;
        fo.jobs_dir     = (dir_ / "ota").string();
        up_             = std::make_unique<FirmwareUpdater>(*svc_, net_.transport(), fo);
    }
    Json::Value ota() { return up_->ota(kUnit); }
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

TEST_F(MuleFirmwareTest, AMuleImagePostsToTheUnitsOwnAddressAndEndsOnItsReturn) {
    updater();
    unitRestartsAs("6.1.0", "6.0.0");
    const auto a = up_->start(kUnit, "mule", "MULE-IMAGE-BYTES", "mule-6.1.0.bin");
    ASSERT_EQ(a.status, 202) << compact(a.body);
    ASSERT_EQ(net_.posts.size(), 1u);
    EXPECT_EQ(net_.posts[0].url, std::string("http://") + kIp + "/api/update/mule");
    EXPECT_EQ(net_.posts[0].body, "MULE-IMAGE-BYTES");
    EXPECT_EQ(net_.posts[0].progress_calls, 2u);
    // The older shape, kept one release beside the job object.
    const Json::Value j = up_->job(kUnit);
    EXPECT_EQ(j["phase"].asString(), "done") << compact(j);
    EXPECT_EQ(j["source"].asString(), "file mule-6.1.0.bin");
    EXPECT_EQ(j["fw_before"].asString(), "6.0.0");
    EXPECT_EQ(j["fw_after"].asString(), "6.1.0");
    EXPECT_EQ(j["sent"].asUInt64(), 16u);
    EXPECT_EQ(j["total"].asUInt64(), 16u);
    EXPECT_FALSE(j["running"].asBool());
    // The job object.
    const Json::Value o = ota();
    EXPECT_EQ(o["kind"].asString(), "mule_miner");
    EXPECT_EQ(o["path"].asString(), "local");
    EXPECT_EQ(o["origin"].asString(), "owner");
    EXPECT_EQ(o["phase"].asString(), "done");
    EXPECT_EQ(o["steps"].asUInt64(), 1u);
    EXPECT_EQ(o["steps_detail"][0]["target"].asString(), "mule");
    EXPECT_EQ(o["steps_detail"][0]["bytes"].asUInt64(), 16u);
    EXPECT_EQ(o["steps_detail"][0]["bytes_meaning"].asString(), "sent");
    EXPECT_EQ(o["steps_detail"][0]["version_after"].asString(), "6.1.0");
    EXPECT_TRUE(o["hold_reason"].isNull());
    EXPECT_FALSE(o["ended_at"].isNull());
}

TEST_F(MuleFirmwareTest, AMinerImageGoesThroughTheMuleAndEndsOnTheStateThatShowsIt) {
    updater();
    const auto a = up_->start(kUnit, "miner", "MINER-IMAGE", "miner.bin");
    ASSERT_EQ(a.status, 202) << compact(a.body);
    ASSERT_EQ(net_.posts.size(), 1u);
    EXPECT_EQ(net_.posts[0].url, std::string("http://") + kIp + "/api/update/miner");
    // The mule flashed it inside the request; the miner is restarting.
    EXPECT_EQ(ota()["phase"].asString(), "restarting");
    EXPECT_EQ(up_->job(kUnit)["phase"].asString(), "done");   // the older shape said done here
    // A state with the old version is not a verdict: no clock, no guess.
    unit_->sendState(false, 0, "6.0.0");
    EXPECT_EQ(ota()["phase"].asString(), "restarting");
    // The miner's new version rides the unit's next state frame.
    unit_->sendState(false, 0, "6.1.0");
    EXPECT_EQ(up_->firmware(kUnit)["miner_fw"].asString(), "6.1.0");
    EXPECT_EQ(ota()["phase"].asString(), "done");
    EXPECT_EQ(ota()["steps_detail"][0]["version_after"].asString(), "6.1.0");
}

TEST_F(MuleFirmwareTest, AnUploadedImageIsOneTargetAndNotTooLarge) {
    updater();
    EXPECT_EQ(up_->start(kUnit, "both", "IMAGE").status, 400);
    EXPECT_EQ(up_->start(kUnit, "coyote", "IMAGE").status, 400);
    EXPECT_EQ(up_->start(kUnit, "mule", std::string(kImageMax + 1, 'x')).status, 400);
    EXPECT_TRUE(net_.posts.empty());
}

// ── the machine recording: warned, then sent on confirm ──

TEST_F(MuleFirmwareTest, WarnedWhileTheUnitStreamsAndSentOnConfirm) {
    updater();
    unit_->sendState(true, 0);
    const auto a = up_->start(kUnit, "miner", "IMAGE");
    EXPECT_EQ(a.status, 409);
    EXPECT_EQ(a.body["warning"].asString(), "recording");
    EXPECT_TRUE(a.body["confirm"].asBool());
    EXPECT_NE(a.body["reason"].asString().find("streaming"), std::string::npos);
    EXPECT_TRUE(net_.posts.empty());
    EXPECT_TRUE(up_->firmware(kUnit)["night_open"].asBool());
    EXPECT_EQ(up_->firmware(kUnit)["recording"].asString(), "yes");
    // Confirmed: it goes at once, and the job says it was confirmed.
    const auto b = up_->start(kUnit, "miner", "IMAGE", "m.bin", /*confirm=*/true);
    EXPECT_EQ(b.status, 202) << compact(b.body);
    EXPECT_EQ(net_.posts.size(), 1u);
    EXPECT_TRUE(ota()["confirmed_while_recording"].asBool());
}

TEST_F(MuleFirmwareTest, WarnedWhileTheNightIsStillLandingNotAfter) {
    updater();
    unit_->sendState(true, 0);
    unit_->sendState(false, 3);   // the mask is off, the unit still holds blocks
    const auto a = up_->start(kUnit, "miner", "IMAGE");
    EXPECT_EQ(a.status, 409);
    EXPECT_NE(a.body["reason"].asString().find("not landed"), std::string::npos);
    EXPECT_TRUE(net_.posts.empty());
    unit_->sendState(false, 0);   // landed: downtime, no warning
    EXPECT_FALSE(up_->firmware(kUnit)["night_open"].asBool());
    EXPECT_EQ(up_->start(kUnit, "miner", "IMAGE").status, 202);
    EXPECT_EQ(net_.posts.size(), 1u);
    EXPECT_FALSE(ota()["confirmed_while_recording"].asBool());
}

TEST_F(MuleFirmwareTest, ANightThatOpensBetweenTheImagesDoesNotHoldTheMiner) {
    // The person's update runs as asked; nothing local is ever held.
    updater();
    unitRestartsAs("6.1.0", "6.0.0", /*streaming=*/true);   // mask on the moment it is back
    const auto a = up_->start(kUnit, "both");
    ASSERT_EQ(a.status, 202) << compact(a.body);
    ASSERT_EQ(net_.posts.size(), 2u);
    EXPECT_EQ(ota()["steps_detail"][1]["target"].asString(), "miner");
    EXPECT_EQ(ota()["step"].asUInt64(), 2u);
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
    EXPECT_EQ(ota()["reason"].asString(), "refused_image");
    EXPECT_TRUE(ota()["retryable"].asBool());
}

TEST_F(MuleFirmwareTest, ALostConnectionEndsTheJob) {
    updater();
    net_.post_error = "Connection reset by peer";
    EXPECT_EQ(up_->start(kUnit, "miner", "IMAGE").status, 202);
    EXPECT_EQ(up_->job(kUnit)["phase"].asString(), "failed");
    EXPECT_NE(up_->job(kUnit)["error"].asString().find("reset by peer"), std::string::npos);
    EXPECT_EQ(ota()["reason"].asString(), "connection_lost");
}

TEST_F(MuleFirmwareTest, AMuleThatDoesNotComeBackStaysRestartingUntilItsHello) {
    // D4: no clock turns silence into a failure.
    updater();
    EXPECT_EQ(up_->start(kUnit, "mule", "IMAGE").status, 202);
    EXPECT_EQ(ota()["phase"].asString(), "restarting");
    EXPECT_EQ(up_->job(kUnit)["phase"].asString(), "waiting");
    EXPECT_TRUE(up_->job(kUnit)["running"].asBool());
    EXPECT_GT(ota()["last_heard"].asInt64(), 0);
    // Its hello, whenever it comes, is the outcome.
    unit_->close();
    unit_ = std::make_unique<FakeUnit>(*svc_);
    unit_->hello("6.1.0", "6.0.0");
    EXPECT_EQ(ota()["phase"].asString(), "done");
    EXPECT_FALSE(up_->job(kUnit)["running"].asBool());
}

TEST_F(MuleFirmwareTest, AMuleBackOnItsOldVersionFailsAsSuch) {
    updater();
    unitRestartsAs("6.0.0", "6.0.0");   // it restarted, but on the old image
    ASSERT_EQ(up_->start(kUnit, "both").status, 202);
    EXPECT_EQ(ota()["phase"].asString(), "failed");
    EXPECT_EQ(ota()["reason"].asString(), "back_on_old_version");
    EXPECT_EQ(net_.posts.size(), 1u);   // the miner's image never went
}

TEST_F(MuleFirmwareTest, OneUpdateAtATime) {
    FirmwareUpdater::Options fo;
    fo.manifest_url = "https://fw.example.test/public/latest.json";
    fo.async        = true;
    up_             = std::make_unique<FirmwareUpdater>(*svc_, net_.transport(), fo);
    // The mule never comes back: its job stays restarting, and a second
    // start on that unit is refused until it is heard from.
    EXPECT_EQ(up_->start(kUnit, "mule", "IMAGE").status, 202);
    up_->waitIdle();
    const auto again = up_->start(kUnit, "miner", "IMAGE");
    EXPECT_EQ(again.status, 409);
    EXPECT_NE(again.body["error"].asString().find("has not ended"), std::string::npos);
    EXPECT_TRUE(up_->job(kUnit)["running"].asBool());
}

TEST_F(MuleFirmwareTest, TheJobIsKeptAcrossARestartOfHmsCpap) {
    updater();
    EXPECT_EQ(up_->start(kUnit, "mule", "IMAGE").status, 202);
    EXPECT_EQ(ota()["phase"].asString(), "restarting");
    ASSERT_TRUE(fs::exists(dir_ / "ota" / (std::string(kUnit) + ".json")));
    // hms-cpap restarts: the job comes back restarting, and the unit's hello
    // still resolves it.
    up_.reset();
    updater();
    EXPECT_EQ(ota()["phase"].asString(), "restarting");
    unit_->close();
    unit_ = std::make_unique<FakeUnit>(*svc_);
    unit_->hello("6.1.0", "6.0.0");
    EXPECT_EQ(ota()["phase"].asString(), "done");
}

TEST_F(MuleFirmwareTest, AJobCutMidTransferIsInterruptedAfterARestart) {
    // A job file left in transferring: hms-cpap stopped while sending.
    devices::OtaJob j;
    j.job_id = "7";
    j.serial = kUnit;
    j.kind   = "mule_miner";
    devices::OtaStep s;
    s.target = "miner";
    s.phase  = "transferring";
    s.bytes  = 100;
    s.total  = 1000;
    j.steps.push_back(s);
    ASSERT_TRUE(devices::otaSave((dir_ / "ota").string(), kUnit, j));
    updater();
    EXPECT_EQ(ota()["phase"].asString(), "failed");
    EXPECT_EQ(ota()["reason"].asString(), "interrupted");
    EXPECT_TRUE(ota()["retryable"].asBool());
    // A new update may start.
    EXPECT_EQ(up_->start(kUnit, "miner", "IMAGE").status, 202);
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
    // One job, two steps: the mule done, the miner restarting until its
    // new version shows in a state.
    const Json::Value o = ota();
    EXPECT_EQ(o["steps"].asUInt64(), 2u);
    EXPECT_EQ(o["step"].asUInt64(), 2u);
    EXPECT_EQ(o["steps_detail"][0]["phase"].asString(), "done");
    EXPECT_EQ(o["steps_detail"][1]["phase"].asString(), "restarting");
    EXPECT_EQ(o["steps_detail"][1]["version_target"].asString(), "6.1.0");
    unit_->sendState(false, 0, "6.1.0");
    EXPECT_EQ(ota()["phase"].asString(), "done");
}

TEST_F(MuleFirmwareTest, BothSkipsAnImageTheUnitAlreadyRuns) {
    unit_->hello("6.1.0", "6.0.0");   // the mule is current, the miner is not
    updater();
    ASSERT_EQ(up_->start(kUnit, "both").status, 202);
    ASSERT_EQ(net_.posts.size(), 1u);
    EXPECT_EQ(net_.posts[0].url, std::string("http://") + kIp + "/api/update/miner");
    EXPECT_EQ(up_->job(kUnit)["phase"].asString(), "done");
    EXPECT_EQ(ota()["steps_detail"][0]["phase"].asString(), "skipped");
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
    EXPECT_EQ(ota()["reason"].asString(), "image_unavailable");
}
