#include <gtest/gtest.h>

#include "mule/MuleFirmware.h"
#include "mule/MuleService.h"
#include "services/As11NightWriter.h"

#include <json/json.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Updating a Mule & Miner over the local network: a fake unit sits on the
// receiver (its session names its address) and a fake transport stands in
// for the unit's own update endpoint and for CpapDash's public firmware
// routes (resolve by pairing code, the fleet map, the signed images). The
// images are made-up bytes shaped like an ESP32 application image, so the
// checks read them as the real ones are read; nothing is flashed anywhere.

using namespace hms_cpap;
using namespace hms_cpap::mule;
namespace fs = std::filesystem;

namespace {

constexpr const char* kUnit    = "FE-C3-00000001";
constexpr const char* kCode    = "055170";
constexpr const char* kV2Unit  = "FE-C3-00000009";
constexpr const char* kV2Code  = "239953";
constexpr const char* kOther   = "FE-C3-00000002";
constexpr const char* kOtherCode = "111111";
constexpr const char* kIp      = "192.168.1.50";
constexpr const char* kBase    = "https://www.example.test/fwbake";

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

// An ESP32 application image as far as its header and app descriptor go:
// the image magic, the descriptor's magic, version and project, then
// `body` (a mule's identity rides in there).
std::string espImage(const std::string& project, const std::string& version, const std::string& body = "") {
    std::string img(32 + 256, '\0');
    img[0] = static_cast<char>(0xE9);
    const uint32_t magic = 0xABCD5432u;
    for (int i = 0; i < 4; i++) img[32 + i] = static_cast<char>((magic >> (8 * i)) & 0xFF);
    img.replace(32 + 16, version.size(), version);
    img.replace(32 + 48, project.size(), project);
    return img + body;
}

std::string pathOnly(const std::string& url) {
    std::string p = url.substr(0, url.find('?'));
    const auto  s = p.find("://");
    if (s != std::string::npos) p = p.substr(p.find('/', s + 3));
    return p;
}

// The unit's end of its stream connection, as far as an update cares.
class FakeUnit {
public:
    FakeUnit(MuleService& svc, const std::string& serial = kUnit, const std::string& ip = kIp) : svc_(svc) {
        conn_ = svc_.open(serial, [](const std::string&) { return true; }, ip);
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

// The unit's update endpoint and CpapDash's public firmware routes, in one.
struct FakeNet {
    struct Unit {
        std::string serial, pinout;
    };
    std::map<std::string, Unit>        codes{{kCode, {kUnit, "official"}},
                                             {kV2Code, {kV2Unit, "v2"}},
                                             {kOtherCode, {kOther, "official"}}};
    std::string                        mule_version{"6.1.0"}, miner_version{"6.1.0"};
    std::set<std::string>              baked{kUnit, kV2Unit, kOther};   // serials whose mule image exists
    bool                               bake_on_request{true};           // a 202 builds it for the next ask
    std::string                        resolve_error;                   // set: CpapDash out of reach
    long                               resolve_status{0};               // set: answered with this instead
    std::map<std::string, std::string> pinout_answered;                 // serial -> the pinout resolve picks
    std::string                        fleet_map;                       // empty: built from `codes`
    long                               fleet_status{200};
    std::map<std::string, std::string> images;                          // path -> bytes
    std::vector<std::string>           gets, resolves;
    long                               post_status{200};
    std::string                        post_body{"{\"status\":\"ok\"}"};
    std::string                        post_error;     // when set, the POST never completes
    std::vector<Post>                  posts;
    std::function<void(const Post&)>   after_post;     // the unit's side effect (a restart)

    static std::string minerPath(const std::string& pinout, const std::string& v) {
        return "/fwbake/v1/flasher/fw/" + v + "/miner-" + (pinout == "official" ? "" : pinout + "-") + v + ".bin";
    }
    static std::string mulePath(const std::string& serial, const std::string& v) {
        return "/fwbake/v1/flasher/fw/" + v + "/" + serial + "/mule-" + v + ".bin";
    }
    // The images CpapDash serves for the current versions.
    void publish() {
        for (const auto& [code, u] : codes) images[mulePath(u.serial, mule_version)] = espImage("mule", mule_version, u.serial);
        images[minerPath("official", miner_version)] = espImage("miner", miner_version, "official");
        images[minerPath("v2", miner_version)]       = espImage("miner", miner_version, "v2");
    }

    std::string fleetMap() const {
        if (!fleet_map.empty()) return fleet_map;
        Json::Value a(Json::arrayValue);
        for (const auto& [code, u] : codes)
            for (const char* role : {"mule", "miner"}) {
                Json::Value e(Json::objectValue);
                e["mac"]     = "aa:bb:cc:dd:ee:ff";
                e["role"]    = role;
                e["serial"]  = u.serial;
                e["variant"] = u.pinout;
                a.append(e);
            }
        return compact(a);
    }

    HttpReply resolve(const std::string& body) {
        HttpReply r;
        if (!resolve_error.empty()) {
            r.error = resolve_error;
            return r;
        }
        r.ok = true;
        Json::Value             req;
        Json::CharReaderBuilder rb;
        std::istringstream      ss(body);
        Json::parseFromStream(rb, ss, &req, nullptr);
        const std::string code = req["pairing_code"].asString(), role = req["role"].asString();
        resolves.push_back(role + " " + code);
        if (resolve_status) {
            r.status = resolve_status;
            r.body   = "{\"error\":\"down\"}";
            return r;
        }
        const auto it = codes.find(code);
        if (it == codes.end()) {
            r.status = 404;
            r.body   = "{\"error\":\"pairing code not recognized\"}";
            return r;
        }
        const Unit& u = it->second;
        Json::Value v(Json::objectValue);
        v["serial"] = u.serial;
        v["role"]   = role;
        if (role == "mule" && !baked.count(u.serial)) {
            if (bake_on_request) baked.insert(u.serial);
            v["status"]          = "baking";
            v["version"]         = mule_version;
            v["retry_after_sec"] = 10;
            r.status             = 202;
            r.body               = compact(v);
            return r;
        }
        const std::string ver = role == "mule" ? mule_version : miner_version;
        const auto        pa  = pinout_answered.find(u.serial);
        const std::string app = role == "mule" ? mulePath(u.serial, ver)
                                               : minerPath(pa == pinout_answered.end() ? u.pinout : pa->second, ver);
        v["version"] = ver;
        Json::Value flash(Json::arrayValue);
        for (const auto& [off, path] : std::vector<std::pair<std::string, std::string>>{
                 {"0x0", "/fwbake/v1/flasher/fw/" + ver + "/" + role + "-bootloader.bin"},
                 {"0x8000", "/fwbake/v1/flasher/fw/" + ver + "/" + role + "-partition-table.bin"},
                 {"0x20000", app}}) {
            Json::Value e(Json::objectValue);
            e["offset"] = off;
            e["url"]    = path + "?exp=1793900000&sig=SIGNATURE";
            flash.append(e);
        }
        v["flash"] = flash;
        r.status   = 200;
        r.body     = compact(v);
        return r;
    }

    Transport transport() {
        Transport t;
        t.get = [this](const std::string& url, int) {
            gets.push_back(url);
            HttpReply r;
            r.ok = true;
            if (url.find("latest.json") != std::string::npos) {   // what production answers today
                r.status = 403;
                r.body   = "<Error><Code>MissingKey</Code></Error>";
                return r;
            }
            if (url == std::string(kBase) + "/v1/flasher/fleet-map") {
                r.status = fleet_status;
                r.body   = fleet_status == 200 ? fleetMap() : "{\"error\":\"fleet map unavailable\"}";
                return r;
            }
            if (url.rfind("https://www.example.test/", 0) != 0 || url.find("sig=") == std::string::npos) {
                r.status = 403;
                return r;
            }
            const auto it = images.find(pathOnly(url));
            r.status      = it == images.end() ? 404 : 200;
            if (it != images.end()) r.body = it->second;
            return r;
        };
        t.post_json = [this](const std::string& url, const std::string& body, int) {
            if (url != std::string(kBase) + "/v1/flasher/resolve") {
                HttpReply r;
                r.ok     = true;
                r.status = 404;
                return r;
            }
            return resolve(body);
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
        net_.publish();
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
    void updater(bool async = false) {
        FirmwareUpdater::Options fo;
        fo.release_base = kBase;
        fo.async        = async;
        fo.jobs_dir     = (dir_ / "ota").string();
        fo.pairing_file = (dir_ / "ota" / "mule-pairing.json").string();
        up_             = std::make_unique<FirmwareUpdater>(*svc_, net_.transport(), fo);
    }
    // The updater, with the unit's pairing code given.
    void paired(const std::string& serial = kUnit, const std::string& code = kCode) {
        if (!up_) updater();
        const auto a = up_->setPairingCode(serial, code);
        ASSERT_EQ(a.status, 200) << compact(a.body);
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

// ── versions ──

TEST(MuleFirmwareVersions, DottedNumbersCompareFieldByField) {
    EXPECT_LT(compareVersions("6.0.0", "6.1.0"), 0);
    EXPECT_LT(compareVersions("6.9.9", "10.0.0"), 0);
    EXPECT_EQ(compareVersions("6.1", "6.1.0"), 0);
    EXPECT_GT(compareVersions("6.1.1", "6.1.0-rc1"), 0);
    EXPECT_TRUE(updateAvailable("6.0.0", "6.1.0"));
    EXPECT_FALSE(updateAvailable("6.1.0", "6.1.0"));
    EXPECT_FALSE(updateAvailable("6.2.0", "6.1.0"));
    EXPECT_TRUE(updateAvailable("", "6.1.0"));        // a unit whose version is unknown
    EXPECT_FALSE(updateAvailable("6.0.0", ""));       // no release read
    EXPECT_FALSE(updateAvailable("6.0.0", "x"));
}

// ── what CpapDash's resolve answers, parsed ──

TEST(MuleFirmwareRelease, ResolveTakesTheApplicationImageAndMakesItAbsolute) {
    const std::string body =
        "{\"serial\":\"FE-C3-00000001\",\"role\":\"miner\",\"version\":\"6.0.0\",\"flash\":["
        "{\"offset\":\"0x0\",\"url\":\"/fwbake/v1/flasher/fw/6.0.0/miner-bootloader.bin?exp=1&sig=a\"},"
        "{\"offset\":\"0x20000\",\"url\":\"/fwbake/v1/flasher/fw/6.0.0/miner-6.0.0.bin?exp=1&sig=b\"}]}";
    std::string why;
    const auto  r = parseResolve(body, 200, "miner", kUnit, "https://www.cpapdash.com/fwbake", &why);
    ASSERT_TRUE(r) << why;
    EXPECT_EQ(r->version, "6.0.0");
    EXPECT_EQ(r->url, "https://www.cpapdash.com/fwbake/v1/flasher/fw/6.0.0/miner-6.0.0.bin?exp=1&sig=b");
    EXPECT_FALSE(r->baking);
}

TEST(MuleFirmwareRelease, ABakingMuleStillNamesItsVersion) {
    const std::string body = "{\"status\":\"baking\",\"serial\":\"FE-C3-00000001\",\"role\":\"mule\","
                             "\"version\":\"6.0.0\",\"retry_after_sec\":10}";
    std::string why;
    const auto  r = parseResolve(body, 202, "mule", kUnit, kBase, &why);
    ASSERT_TRUE(r) << why;
    EXPECT_TRUE(r->baking);
    EXPECT_EQ(r->version, "6.0.0");
    EXPECT_EQ(r->url, "");
}

TEST(MuleFirmwareRelease, ResolveIsRefusedWhenItIsNotThisUnitsRelease) {
    std::string why;
    const std::string other = "{\"serial\":\"FE-C3-00000002\",\"role\":\"mule\",\"version\":\"6.0.0\","
                              "\"flash\":[{\"offset\":\"0x20000\",\"url\":\"/x.bin\"}]}";
    EXPECT_FALSE(parseResolve(other, 200, "mule", kUnit, kBase, &why));
    EXPECT_NE(why.find("belongs to FE-C3-00000002"), std::string::npos) << why;
    EXPECT_FALSE(parseResolve(other, 200, "miner", "FE-C3-00000002", kBase, &why));   // another role
    EXPECT_FALSE(parseResolve("<html/>", 200, "mule", kUnit, kBase, &why));
    EXPECT_NE(why.find("not JSON"), std::string::npos);
    // No application image in the plan.
    EXPECT_FALSE(parseResolve("{\"serial\":\"FE-C3-00000001\",\"role\":\"mule\",\"version\":\"6.0.0\",\"flash\":[]}",
                              200, "mule", kUnit, kBase, &why));
    EXPECT_NE(why.find("no application image"), std::string::npos);
    // An error answer says what CpapDash said.
    EXPECT_FALSE(parseResolve("{\"error\":\"This unit's SPI pinout is not on record\"}", 409, "miner", kUnit, kBase, &why));
    EXPECT_NE(why.find("HTTP 409"), std::string::npos);
    EXPECT_NE(why.find("pinout is not on record"), std::string::npos);
    // A version that is not one.
    EXPECT_FALSE(parseResolve("{\"serial\":\"FE-C3-00000001\",\"role\":\"mule\",\"version\":\"\",\"status\":\"baking\"}",
                              202, "mule", kUnit, kBase, &why));
}

TEST(MuleFirmwareRelease, AbsoluteUrlUsesTheBasesOrigin) {
    EXPECT_EQ(absoluteUrl("https://www.cpapdash.com/fwbake", "/fwbake/v1/x?y=1"),
              "https://www.cpapdash.com/fwbake/v1/x?y=1");
    EXPECT_EQ(absoluteUrl("https://h.test", "a/b"), "https://h.test/a/b");
    EXPECT_EQ(absoluteUrl("https://h.test/base", "https://other.test/z"), "https://other.test/z");
}

// ── the miner's pinout ──

TEST(MuleFirmwarePinout, TheMinerImagesNameSaysItsPinout) {
    EXPECT_EQ(minerVariantOf("https://h/fwbake/v1/flasher/fw/6.0.0/miner-6.0.0.bin?exp=1&sig=a", "6.0.0"), "official");
    EXPECT_EQ(minerVariantOf("https://h/fwbake/v1/flasher/fw/6.0.0/miner-v2-6.0.0.bin?exp=1&sig=a", "6.0.0"), "v2");
    EXPECT_EQ(minerVariantOf("/fw/6.0.0/miner-unit3-6.0.0.bin", "6.0.0"), "unit3");
    EXPECT_FALSE(minerVariantOf("https://h/fw/6.0.0/miner-6.0.1.bin", "6.0.0"));       // another version
    EXPECT_FALSE(minerVariantOf("https://h/fw/6.0.0/mule-6.0.0.bin", "6.0.0"));        // not a miner
    EXPECT_FALSE(minerVariantOf("https://h/fw/6.0.0/miner-a-b-6.0.0.bin", "6.0.0"));   // not one variant
    EXPECT_FALSE(minerVariantOf("https://h/fw/6.0.0/miner-6.0.0.bin", ""));
}

TEST(MuleFirmwarePinout, TheFleetMapNamesTheUnitsMinerVariant) {
    const std::string map =
        "[{\"mac\":\"a\",\"role\":\"mule\",\"serial\":\"FE-C3-00000009\",\"variant\":\"official\"},"
        "{\"mac\":\"b\",\"role\":\"miner\",\"serial\":\"FE-C3-00000009\",\"variant\":\"v2\"},"
        "{\"mac\":\"c\",\"role\":\"miner\",\"serial\":\"FE-C3-00000001\",\"variant\":\"official\"}]";
    EXPECT_EQ(fleetVariant(map, "FE-C3-00000009"), "v2");   // the miner's row, not the mule's
    EXPECT_EQ(fleetVariant(map, "FE-C3-00000001"), "official");
    EXPECT_FALSE(fleetVariant(map, "FE-C3-00000077"));
    EXPECT_FALSE(fleetVariant("{\"error\":\"x\"}", "FE-C3-00000001"));
}

// ── the image's own checks ──

TEST(MuleFirmwareImage, AReleaseImageMustBeTheOneAskedFor) {
    std::string why;
    EXPECT_TRUE(checkReleaseImage(espImage("miner", "6.1.0"), "miner", "6.1.0", kUnit, &why)) << why;
    EXPECT_TRUE(checkReleaseImage(espImage("mule", "6.1.0", kUnit), "mule", "6.1.0", kUnit, &why)) << why;
    EXPECT_FALSE(checkReleaseImage(espImage("mule", "6.1.0", kOther), "mule", "6.1.0", kUnit, &why));
    EXPECT_NE(why.find("not built for"), std::string::npos);
    EXPECT_FALSE(checkReleaseImage(espImage("miner", "6.0.9"), "miner", "6.1.0", kUnit, &why));
    EXPECT_NE(why.find("version 6.0.9"), std::string::npos);
    EXPECT_FALSE(checkReleaseImage(espImage("sd_capture", "6.1.0"), "miner", "6.1.0", kUnit, &why));
    EXPECT_NE(why.find("not a miner image"), std::string::npos);
    EXPECT_FALSE(checkReleaseImage(std::string(400, 'x'), "miner", "6.1.0", kUnit, &why));
    EXPECT_FALSE(checkReleaseImage("", "miner", "6.1.0", kUnit, &why));
    EXPECT_FALSE(checkReleaseImage(espImage("miner", "6.1.0", std::string(kImageMax, 'x')), "miner", "6.1.0", kUnit, &why));
    EXPECT_NE(why.find("larger"), std::string::npos);
}

TEST(MuleFirmwareImage, APairingCodeIsSixDigits) {
    EXPECT_TRUE(validPairingCode("055170"));
    EXPECT_FALSE(validPairingCode("55170"));
    EXPECT_FALSE(validPairingCode("05517a"));
    EXPECT_FALSE(validPairingCode(""));
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
    // The owner's file needs no pairing code and asks CpapDash nothing.
    EXPECT_TRUE(net_.resolves.empty());
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
    EXPECT_EQ(up_->firmware(kUnit, false)["miner_fw"].asString(), "6.1.0");
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
    paired();
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
    paired();
    unitRestartsAs("6.0.0", "6.0.0");   // it restarted, but on the old image
    ASSERT_EQ(up_->start(kUnit, "both").status, 202);
    EXPECT_EQ(ota()["phase"].asString(), "failed");
    EXPECT_EQ(ota()["reason"].asString(), "back_on_old_version");
    EXPECT_EQ(net_.posts.size(), 1u);   // the miner's image never went
}

TEST_F(MuleFirmwareTest, OneUpdateAtATime) {
    updater(/*async=*/true);
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

// ── the release, from CpapDash's flasher route ──

TEST_F(MuleFirmwareTest, TheLatestReleaseComesFromTheFlasherRouteNotTheRetiredLatestJson) {
    // Production today: public/latest.json answers 403 MissingKey (the fake
    // does too), and nothing writes it. The check before this release read
    // only that file, so it never saw a version; this one asks the route the
    // web flasher uses, with the unit's pairing code.
    paired();
    const Json::Value f = up_->firmware(kUnit);
    EXPECT_TRUE(f["known"].asBool());
    EXPECT_TRUE(f["online"].asBool());
    EXPECT_EQ(f["ip"].asString(), kIp);
    EXPECT_EQ(f["fw"].asString(), "6.0.0");
    EXPECT_EQ(f["miner_fw"].asString(), "6.0.0");
    EXPECT_EQ(f["latest"]["mule"].asString(), "6.1.0") << compact(f);
    EXPECT_EQ(f["latest"]["miner"].asString(), "6.1.0");
    EXPECT_TRUE(f["update_available"]["mule"].asBool());
    EXPECT_TRUE(f["update_available"]["miner"].asBool());
    EXPECT_TRUE(f["pairing_set"].asBool());
    EXPECT_EQ(f["manifest"]["error"].asString(), "");
    EXPECT_EQ(f["manifest"]["url"].asString(), std::string(kBase) + "/v1/flasher/resolve");
    EXPECT_TRUE(f["job"].isNull());
    for (const auto& g : net_.gets) EXPECT_EQ(g.find("latest.json"), std::string::npos) << g;
}

TEST_F(MuleFirmwareTest, BothFromTheReleaseSendsTheMuleThenTheMiner) {
    paired();
    unitRestartsAs("6.1.0", "6.0.0");
    const auto a = up_->start(kUnit, "both");
    ASSERT_EQ(a.status, 202) << compact(a.body);
    ASSERT_EQ(net_.posts.size(), 2u);
    EXPECT_EQ(net_.posts[0].url, std::string("http://") + kIp + "/api/update/mule");
    EXPECT_EQ(net_.posts[0].body, net_.images[FakeNet::mulePath(kUnit, "6.1.0")]);
    EXPECT_EQ(net_.posts[1].url, std::string("http://") + kIp + "/api/update/miner");
    EXPECT_EQ(net_.posts[1].body, net_.images[FakeNet::minerPath("official", "6.1.0")]);
    const Json::Value j = up_->job(kUnit);
    EXPECT_EQ(j["phase"].asString(), "done") << compact(j);
    EXPECT_EQ(j["source"].asString(), "release 6.1.0");
    EXPECT_EQ(j["fw_after"].asString(), "6.1.0");
    // The signature in an image URL never reaches the job's log.
    for (const auto& l : j["log"]) EXPECT_EQ(l.asString().find("SIGNATURE"), std::string::npos) << l.asString();
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

TEST_F(MuleFirmwareTest, AV2UnitGetsTheV2MinerImage) {
    FakeUnit v2(*svc_, kV2Unit, "192.168.1.60");
    v2.hello("6.1.0", "6.0.0");
    paired(kV2Unit, kV2Code);
    const auto a = up_->start(kV2Unit, "miner");
    ASSERT_EQ(a.status, 202) << compact(a.body);
    ASSERT_EQ(net_.posts.size(), 1u);
    EXPECT_EQ(net_.posts[0].url, "http://192.168.1.60/api/update/miner");
    EXPECT_EQ(net_.posts[0].body, net_.images[FakeNet::minerPath("v2", "6.1.0")]);
    EXPECT_NE(net_.posts[0].body, net_.images[FakeNet::minerPath("official", "6.1.0")]);
}

TEST_F(MuleFirmwareTest, AMinerImageForAnotherPinoutIsNeverSent) {
    // CpapDash's answer and its own fleet map disagree on the unit's pinout:
    // the wrong miner image kills the SPI bus, so nothing goes.
    paired();
    net_.pinout_answered[kUnit] = "v2";
    ASSERT_EQ(up_->start(kUnit, "miner").status, 202);
    EXPECT_TRUE(net_.posts.empty());
    EXPECT_EQ(ota()["phase"].asString(), "failed");
    EXPECT_EQ(ota()["reason"].asString(), "image_rejected");
    EXPECT_NE(ota()["detail"].asString().find("for the v2 pinout, but FE-C3-00000001 is official"), std::string::npos)
        << ota()["detail"].asString();
}

TEST_F(MuleFirmwareTest, AMinerIsNotSentWhenItsPinoutCannotBeConfirmed) {
    paired();
    net_.fleet_map = "[]";   // the map does not list the unit
    ASSERT_EQ(up_->start(kUnit, "miner").status, 202);
    EXPECT_TRUE(net_.posts.empty());
    EXPECT_EQ(ota()["reason"].asString(), "image_rejected");
    EXPECT_NE(ota()["detail"].asString().find("does not list"), std::string::npos);
    // Nor when the map cannot be read.
    unit_->hello("6.0.0", "6.0.0");
    net_.fleet_map.clear();
    net_.fleet_status = 502;
    ASSERT_EQ(up_->start(kUnit, "miner").status, 202);
    EXPECT_TRUE(net_.posts.empty());
    EXPECT_NE(ota()["detail"].asString().find("could not be read"), std::string::npos);
}

TEST_F(MuleFirmwareTest, AnImageThatIsNotTheReleaseIsNeverSent) {
    paired();
    // A mule image built for another unit, served at this unit's path.
    net_.images[FakeNet::mulePath(kUnit, "6.1.0")] = espImage("mule", "6.1.0", kOther);
    ASSERT_EQ(up_->start(kUnit, "mule").status, 202);
    EXPECT_TRUE(net_.posts.empty());
    EXPECT_EQ(ota()["reason"].asString(), "image_rejected");
    EXPECT_NE(ota()["detail"].asString().find("not built for FE-C3-00000001"), std::string::npos);
    // A miner image of another version.
    unit_->hello("6.0.0", "6.0.0");
    net_.images[FakeNet::minerPath("official", "6.1.0")] = espImage("miner", "6.0.5");
    ASSERT_EQ(up_->start(kUnit, "miner").status, 202);
    EXPECT_TRUE(net_.posts.empty());
    EXPECT_NE(ota()["detail"].asString().find("version 6.0.5"), std::string::npos);
}

TEST_F(MuleFirmwareTest, AMuleStillBeingBuiltIsReportedAndSentOnTheNextPress) {
    net_.baked.clear();   // this unit's mule image is not built yet
    paired();
    // The check names the version while CpapDash builds the image.
    const Json::Value f = up_->firmware(kUnit);
    EXPECT_EQ(f["latest"]["mule"].asString(), "6.1.0");
    EXPECT_TRUE(f["update_available"]["mule"].asBool());
    // A press while it builds: said plainly, nothing sent.
    net_.baked.clear();
    net_.bake_on_request = false;
    const auto a = up_->start(kUnit, "both");
    EXPECT_EQ(a.status, 503);
    EXPECT_NE(a.body["error"].asString().find("building this unit's mule 6.1.0"), std::string::npos);
    EXPECT_TRUE(net_.posts.empty());
    EXPECT_TRUE(ota().isNull());
    // Built: the next press sends it.
    net_.baked.insert(kUnit);
    unitRestartsAs("6.1.0", "6.0.0");
    ASSERT_EQ(up_->start(kUnit, "both").status, 202);
    EXPECT_EQ(net_.posts.size(), 2u);
}

TEST_F(MuleFirmwareTest, BothSkipsAnImageTheUnitAlreadyRuns) {
    unit_->hello("6.1.0", "6.0.0");   // the mule is current, the miner is not
    paired();
    ASSERT_EQ(up_->start(kUnit, "both").status, 202);
    ASSERT_EQ(net_.posts.size(), 1u);
    EXPECT_EQ(net_.posts[0].url, std::string("http://") + kIp + "/api/update/miner");
    EXPECT_EQ(up_->job(kUnit)["phase"].asString(), "done");
    EXPECT_EQ(ota()["steps_detail"][0]["phase"].asString(), "skipped");
}

TEST_F(MuleFirmwareTest, NothingNewerIsRefused) {
    unit_->hello("6.1.0", "6.1.0");
    paired();
    const auto a = up_->start(kUnit, "both");
    EXPECT_EQ(a.status, 409);
    EXPECT_NE(a.body["error"].asString().find("already runs"), std::string::npos);
    EXPECT_EQ(up_->start(kUnit, "mule").status, 409);
    EXPECT_TRUE(net_.posts.empty());
    EXPECT_FALSE(up_->firmware(kUnit)["update_available"]["mule"].asBool());
}

TEST_F(MuleFirmwareTest, CpapDashOutOfReachLeavesTheFileUpload) {
    paired();
    net_.resolve_error = "Could not resolve host: www.example.test";
    const auto a = up_->start(kUnit, "both");
    EXPECT_EQ(a.status, 502);
    EXPECT_NE(a.body["error"].asString().find("could not be reached"), std::string::npos) << compact(a.body);
    EXPECT_NE(a.body["error"].asString().find("Upload the image"), std::string::npos);
    // The page says why, once the cache has the failed read.
    const Json::Value f = up_->firmware(kUnit, false);
    EXPECT_EQ(f["latest"]["mule"].asString(), "");
    EXPECT_FALSE(f["update_available"]["mule"].asBool());
    EXPECT_NE(f["manifest"]["error"].asString().find("could not be reached"), std::string::npos);
    // The owner's own file still goes.
    EXPECT_EQ(up_->start(kUnit, "miner", "IMAGE").status, 202);
    EXPECT_EQ(net_.posts.size(), 1u);
}

TEST_F(MuleFirmwareTest, AnImageThatCannotBeDownloadedEndsTheJobBeforeAnyPost) {
    paired();
    net_.images.clear();
    EXPECT_EQ(up_->start(kUnit, "miner").status, 202);
    EXPECT_TRUE(net_.posts.empty());
    const Json::Value j = up_->job(kUnit);
    EXPECT_EQ(j["phase"].asString(), "failed");
    EXPECT_NE(j["error"].asString().find("HTTP 404"), std::string::npos);
    EXPECT_EQ(ota()["reason"].asString(), "image_unavailable");
}

// ── the pairing code ──

TEST_F(MuleFirmwareTest, WithoutAPairingCodeTheCardAsksForIt) {
    updater();
    const Json::Value f = up_->firmware(kUnit);
    EXPECT_FALSE(f["pairing_set"].asBool());
    EXPECT_EQ(f["latest"]["mule"].asString(), "");
    EXPECT_NE(f["manifest"]["error"].asString().find("pairing code"), std::string::npos);
    EXPECT_TRUE(net_.resolves.empty());   // nothing asked without a code
    const auto a = up_->start(kUnit, "both");
    EXPECT_EQ(a.status, 409);
    EXPECT_NE(a.body["error"].asString().find("pairing code"), std::string::npos);
    EXPECT_TRUE(net_.posts.empty());
}

TEST_F(MuleFirmwareTest, APairingCodeIsKeptOnlyOnceCpapDashSaysItIsThisUnits) {
    updater();
    EXPECT_EQ(up_->setPairingCode(kUnit, "12345").status, 400);
    const auto unknown = up_->setPairingCode(kUnit, "999999");
    EXPECT_EQ(unknown.status, 409);
    EXPECT_NE(unknown.body["error"].asString().find("does not recognise"), std::string::npos);
    const auto other = up_->setPairingCode(kUnit, kOtherCode);
    EXPECT_EQ(other.status, 409);
    EXPECT_NE(other.body["error"].asString().find("belongs to FE-C3-00000002"), std::string::npos);
    net_.resolve_error = "Could not connect";
    EXPECT_EQ(up_->setPairingCode(kUnit, kCode).status, 502);
    net_.resolve_error.clear();
    EXPECT_FALSE(up_->firmware(kUnit, false)["pairing_set"].asBool());
    // Confirmed with a miner resolve only: checking a code builds nothing.
    net_.resolves.clear();
    const auto ok = up_->setPairingCode(kUnit, "055 170");   // as printed, spaced
    EXPECT_EQ(ok.status, 200) << compact(ok.body);
    ASSERT_EQ(net_.resolves.size(), 1u);
    EXPECT_EQ(net_.resolves[0], std::string("miner ") + kCode);
    // Kept across a restart of hms-cpap.
    up_.reset();
    updater();
    EXPECT_TRUE(up_->firmware(kUnit, false)["pairing_set"].asBool());
    // "" forgets it.
    EXPECT_EQ(up_->setPairingCode(kUnit, "").status, 200);
    up_.reset();
    updater();
    EXPECT_FALSE(up_->firmware(kUnit, false)["pairing_set"].asBool());
}

TEST_F(MuleFirmwareTest, ThePagesRefreshNeverAsksCpapDashAndAPressAlwaysDoes) {
    paired();
    net_.resolves.clear();
    up_->firmware(kUnit);          // the page loads: one read
    const size_t after_load = net_.resolves.size();
    EXPECT_EQ(after_load, 2u);     // the miner, then the mule
    up_->firmware(kUnit, false);   // its 3 s refreshes
    up_->firmware(kUnit, false);
    up_->firmware(kUnit);          // a reload inside ten minutes: the cache
    EXPECT_EQ(net_.resolves.size(), after_load);
    // A press reads fresh: the signed URLs are short-lived.
    unitRestartsAs("6.1.0", "6.0.0");
    ASSERT_EQ(up_->start(kUnit, "both").status, 202);
    EXPECT_EQ(net_.resolves.size(), after_load + 2);
}
