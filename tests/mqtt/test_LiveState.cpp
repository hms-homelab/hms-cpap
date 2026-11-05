// The night, live, on the same MQTT topics from every source: the mask with
// the moment it changed (occurred_at, detected_at, delay_s, as the CpapDash
// bus's mask.on / mask.off carry them), the events and their counts, and the
// Home Assistant entities. Per source: on at therapy start, off at its stop,
// and what a restart leaves on the broker. The last test talks to a throwaway
// broker (TestBroker.h) and skips without one.

#include "TestBroker.h"
#include "mqtt/LiveState.h"
#include "mqtt_client.h"
#include "mule/LivePublisher.h"
#include "services/BurstCollectorService.h"
#include "sleepbridge/LiveNight.h"
#include "utils/AppConfig.h"
#include "utils/CardMask.h"

#include <gtest/gtest.h>
#include <json/json.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using hms_cpap::BurstCollectorService;
using hms_cpap::CardMask;
using hms_cpap::LiveState;
using hms_cpap::MachineClock;
using hms_cpap::SessionFileSet;
using hms_cpap::mule::LivePublisher;
using hms_cpap::sleepbridge::LiveNight;
using hms_cpap::sleepbridge::LiveSink;
using Clock = std::chrono::system_clock;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

struct Sent {
    std::string topic, payload;
    int         qos;
    bool        retain;
};

Json::Value parse(const std::string& s) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream in(s);
    std::string errs;
    EXPECT_TRUE(Json::parseFromStream(rb, in, &v, &errs)) << errs << ": " << s;
    return v;
}

Clock::time_point at(long long epoch_s) { return Clock::time_point(std::chrono::seconds(epoch_s)); }

// 2026-11-05T04:15:02Z
constexpr long long kT0 = 1793852102;

struct Recorder {
    std::vector<Sent> out;
    bool              up{true};
    LiveState::Publish publish() {
        return [this](const std::string& t, const std::string& p, int q, bool r) {
            if (!up) return false;
            out.push_back({t, p, q, r});
            return true;
        };
    }
    std::vector<Sent> on(const std::string& leaf) const {
        std::vector<Sent> v;
        for (const auto& s : out)
            if (s.topic == "cpap/cpap_test/realtime/" + leaf) v.push_back(s);
        return v;
    }
    std::string last(const std::string& leaf) const {
        const auto v = on(leaf);
        return v.empty() ? std::string() : v.back().payload;
    }
    Json::Value change() const { return parse(last("mask_changed")); }
};

/// A session file as a ResMed writes it: one signal, `records` records of
/// `duration` seconds.
void writeEdf(const fs::path& path, int records, int duration = 60) {
    auto field = [](const std::string& s, size_t w) {
        std::string f = s.substr(0, w);
        f.resize(w, ' ');
        return f;
    };
    std::string h;
    h += field("0", 8) + field("", 80) + field("", 80) + field("05.11.26", 8) + field("04.15.02", 8);
    h += field("512", 8) + field("", 44) + field(std::to_string(records), 8);
    h += field(std::to_string(duration), 8) + field("1", 4);
    h += field("Flow.40ms", 16) + field("", 80) + field("L/s", 8) + field("-100", 8) + field("100", 8);
    h += field("-32768", 8) + field("32767", 8) + field("", 80) + field("2", 8) + field("", 32);
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << h << std::string(static_cast<size_t>(records) * 4, '\0');
}

/// A session stem read in the host's zone, as the Sleep Bridge reads it.
Clock::time_point localStem(int y, int mo, int d, int h, int mi, int s) {
    std::tm tm{};
    tm.tm_year  = y - 1900;
    tm.tm_mon   = mo - 1;
    tm.tm_mday  = d;
    tm.tm_hour  = h;
    tm.tm_min   = mi;
    tm.tm_sec   = s;
    tm.tm_isdst = -1;
    return Clock::from_time_t(std::mktime(&tm));
}

struct TempDir {
    fs::path p;
    TempDir() {
        p = fs::temp_directory_path() /
            ("livestate_" + std::to_string(Clock::now().time_since_epoch().count()));
        fs::create_directories(p);
    }
    ~TempDir() { fs::remove_all(p); }
};

}  // namespace

// ── the shared state ────────────────────────────────────────────────────────

TEST(LiveState, AnnouncesTheMaskAndTheEventCountOnTheSameDevice) {
    Recorder rec;
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", at(kT0));
    ASSERT_TRUE(live.announce());
    ASSERT_EQ(rec.out.size(), 2u);

    const auto& mc = rec.out[0];
    EXPECT_EQ(mc.topic, "homeassistant/binary_sensor/cpap_test/mask/config");
    EXPECT_TRUE(mc.retain);
    const auto m = parse(mc.payload);
    EXPECT_EQ(m["name"].asString(), "Mask");
    EXPECT_EQ(m["unique_id"].asString(), "cpap_test_mask");
    EXPECT_EQ(m["state_topic"].asString(), "cpap/cpap_test/realtime/mask");
    EXPECT_EQ(m["json_attributes_topic"].asString(), "cpap/cpap_test/realtime/mask_changed");
    EXPECT_EQ(m["payload_on"].asString(), "ON");
    EXPECT_EQ(m["payload_off"].asString(), "OFF");
    EXPECT_EQ(m["device_class"].asString(), "running");
    EXPECT_EQ(m["device"]["identifiers"][0].asString(), "cpap_test");
    EXPECT_EQ(m["device"]["name"].asString(), "Test CPAP");

    const auto& ec = rec.out[1];
    EXPECT_EQ(ec.topic, "homeassistant/sensor/cpap_test/live_events/config");
    const auto e = parse(ec.payload);
    EXPECT_EQ(e["unique_id"].asString(), "cpap_test_live_events");
    EXPECT_EQ(e["state_topic"].asString(), "cpap/cpap_test/realtime/live_events");
    EXPECT_EQ(e["value_template"].asString(), "{{ value_json.total }}");
    EXPECT_EQ(e["json_attributes_topic"].asString(), "cpap/cpap_test/realtime/live_events");
    EXPECT_EQ(e["device"]["identifiers"][0].asString(), "cpap_test");
}

TEST(LiveState, ASourceSeenForTheFirstTimeAnnouncesAgainOnce) {
    Recorder rec;
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", at(kT0));
    live.sourceSeen("TAP-S3-00000123");
    live.sourceSeen("TAP-S3-00000123");
    live.sourceSeen("card");
    size_t configs = 0;
    for (const auto& s : rec.out)
        if (s.topic == "homeassistant/binary_sensor/cpap_test/mask/config") ++configs;
    EXPECT_EQ(configs, 2u);
}

TEST(LiveState, AChangeCarriesWhenItHappenedAndWhenItWasSeen) {
    Recorder rec;
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", at(kT0));
    live.mask({LiveState::kEzShare, "card", true, at(kT0), "20261105_041502"}, at(kT0 + 128));

    ASSERT_EQ(rec.on("mask").size(), 1u);
    EXPECT_EQ(rec.last("mask"), "ON");
    EXPECT_TRUE(rec.on("mask")[0].retain);
    const auto c = rec.change();
    EXPECT_EQ(c["state"].asString(), "on");
    EXPECT_EQ(c["source"].asString(), "ezshare");
    EXPECT_EQ(c["source_id"].asString(), "card");
    EXPECT_EQ(c["session"].asString(), "20261105_041502");
    EXPECT_EQ(c["occurred_at"].asString(), "2026-11-05T04:15:02Z");
    EXPECT_EQ(c["detected_at"].asString(), "2026-11-05T04:17:10Z");
    EXPECT_EQ(c["delay_s"].asInt(), 128);
    EXPECT_TRUE(rec.on("mask_changed")[0].retain);
    // The attributes land before the state, so an automation on the state
    // reads the change that set it.
    size_t attrs = 0, state = 0;
    for (size_t i = 0; i < rec.out.size(); i++) {
        if (rec.out[i].topic == "cpap/cpap_test/realtime/mask_changed") attrs = i;
        if (rec.out[i].topic == "cpap/cpap_test/realtime/mask") state = i;
    }
    EXPECT_LT(attrs, state);

    live.mask({LiveState::kEzShare, "card", false, at(kT0 + 3600), "20261105_041502"}, at(kT0 + 3740));
    EXPECT_EQ(rec.last("mask"), "OFF");
    EXPECT_EQ(rec.change()["state"].asString(), "off");
    EXPECT_EQ(rec.change()["delay_s"].asInt(), 140);
}

TEST(LiveState, TheMaskIsOnWhileAnySourceSaysSo) {
    Recorder rec;
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", at(kT0));
    live.mask({LiveState::kSleepBridge, "TAP-1", true, at(kT0), "s"}, at(kT0 + 2));
    live.mask({LiveState::kMuleBle, "FE-1", true, at(kT0 + 1), ""}, at(kT0 + 3));
    EXPECT_EQ(rec.on("mask").size(), 1u) << "already on: nothing new to say";
    live.mask({LiveState::kSleepBridge, "TAP-1", false, at(kT0 + 60), "s"}, at(kT0 + 62));
    EXPECT_EQ(rec.on("mask").size(), 1u) << "the Mule still says on";
    live.mask({LiveState::kMuleBle, "FE-1", false, at(kT0 + 70), ""}, at(kT0 + 71));
    ASSERT_EQ(rec.on("mask").size(), 2u);
    EXPECT_EQ(rec.last("mask"), "OFF");
    EXPECT_EQ(rec.change()["source"].asString(), "mule_ble");
}

TEST(LiveState, ANewSessionWhileOnIsSaidWithItsOwnMoment) {
    Recorder rec;
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", at(kT0));
    live.mask({LiveState::kLocal, "card", true, at(kT0), "20261105_041502"}, at(kT0 + 120));
    live.mask({LiveState::kLocal, "card", true, at(kT0 + 900), "20261105_043002"}, at(kT0 + 1020));
    EXPECT_EQ(rec.last("mask"), "ON");
    EXPECT_EQ(rec.change()["session"].asString(), "20261105_043002");
    EXPECT_EQ(rec.change()["occurred_at"].asString(), "2026-11-05T04:30:02Z");
}

TEST(LiveState, EventsCarryTheirMomentAndTheCountsStartOverAtMaskOn) {
    Recorder rec;
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", at(kT0));
    live.mask({LiveState::kSleepBridge, "TAP-1", true, at(kT0), "s"}, at(kT0 + 2));
    EXPECT_EQ(parse(rec.last("live_events"))["total"].asInt(), 0);

    Json::Value e(Json::objectValue);
    e["type"]        = "OA";
    e["description"] = "Obstructive apnea";
    e["duration_s"]  = 14;
    e["source"]      = LiveState::kSleepBridge;
    live.event(e, at(kT0 + 600), at(kT0 + 662));
    e["type"] = "H";
    live.event(e, at(kT0 + 700), at(kT0 + 722));

    ASSERT_EQ(rec.on("event").size(), 2u);
    EXPECT_FALSE(rec.on("event")[0].retain);
    const auto first = parse(rec.on("event")[0].payload);
    EXPECT_EQ(first["type"].asString(), "OA");
    EXPECT_EQ(first["occurred_at"].asString(), "2026-11-05T04:25:02Z");
    EXPECT_EQ(first["delay_s"].asInt(), 62);
    const auto counts = parse(rec.last("live_events"));
    EXPECT_EQ(counts["total"].asInt(), 2);
    EXPECT_EQ(counts["OA"].asInt(), 1);
    EXPECT_EQ(counts["H"].asInt(), 1);

    live.mask({LiveState::kSleepBridge, "TAP-1", false, at(kT0 + 3600), "s"}, at(kT0 + 3602));
    live.mask({LiveState::kSleepBridge, "TAP-1", true, at(kT0 + 7200), "s2"}, at(kT0 + 7202));
    EXPECT_EQ(parse(rec.last("live_events"))["total"].asInt(), 0) << "a new mask-on, a new count";
}

// After a restart the broker may hold an ON from a night that ended while
// hms-cpap was away. Nothing said for the settle time: OFF, once.
TEST(LiveState, AfterARestartWithNoSourceTheRetainedMaskBecomesOff) {
    Recorder rec;
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", at(kT0), 300s);
    live.tick(at(kT0 + 299));
    EXPECT_TRUE(rec.on("mask").empty());
    live.tick(at(kT0 + 300));
    ASSERT_EQ(rec.on("mask").size(), 1u);
    EXPECT_EQ(rec.last("mask"), "OFF");
    EXPECT_EQ(rec.change()["source"].asString(), "startup");
    live.tick(at(kT0 + 3600));
    EXPECT_EQ(rec.on("mask").size(), 1u) << "said once";
}

TEST(LiveState, AfterARestartASourceThatSaysOnFirstLeavesNoOffBlip) {
    Recorder rec;
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", at(kT0), 300s);
    live.mask({LiveState::kSleepBridge, "TAP-1", true, at(kT0 - 3000), "s"}, at(kT0 + 50));
    live.tick(at(kT0 + 300));
    live.tick(at(kT0 + 900));
    ASSERT_EQ(rec.on("mask").size(), 1u);
    EXPECT_EQ(rec.last("mask"), "ON");
}

TEST(LiveState, AfterARestartASourceThatSaysOffIsBelievedAtOnce) {
    Recorder rec;
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", at(kT0), 300s);
    live.mask({LiveState::kMuleBle, "FE-1", false, at(kT0 + 5), ""}, at(kT0 + 5));
    EXPECT_EQ(rec.last("mask"), "OFF");
    EXPECT_EQ(rec.change()["source"].asString(), "mule_ble");
}

TEST(LiveState, ABrokerAwayAtTheSettleTimeIsToldAtTheNextTick) {
    Recorder rec;
    rec.up = false;
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", at(kT0), 300s);
    live.tick(at(kT0 + 300));
    rec.up = true;
    live.tick(at(kT0 + 330));
    EXPECT_EQ(rec.last("mask"), "OFF");
}

// ── a card: the mask as the bursts read it ──────────────────────────────────

TEST(CardMask, ASessionThatGrowsIsOnFromItsStart) {
    CardMask m;
    const auto c = m.grew("20261105_041502", kT0, kT0 + 180, kT0 + 250);
    ASSERT_TRUE(c);
    EXPECT_TRUE(c->on);
    EXPECT_EQ(c->session, "20261105_041502");
    EXPECT_EQ(c->occurred_s, kT0);
    EXPECT_FALSE(m.grew("20261105_041502", kT0, kT0 + 240, kT0 + 315)) << "still on";
}

TEST(CardMask, ItIsOffOnceTheSessionHasNotGrownForTwoMissedMinutes) {
    CardMask m;
    m.grew("20261105_041502", kT0, kT0 + 180, kT0 + 250);
    m.grew("20261105_041502", kT0, kT0 + 240, kT0 + 315);
    EXPECT_FALSE(m.tick(kT0 + 380));
    EXPECT_FALSE(m.tick(kT0 + 315 + CardMask::kMaskOffAfterS - 1));
    const auto c = m.tick(kT0 + 315 + CardMask::kMaskOffAfterS);
    ASSERT_TRUE(c);
    EXPECT_FALSE(c->on);
    EXPECT_EQ(c->occurred_s, kT0 + 240) << "the end of its last record";
    EXPECT_FALSE(m.tick(kT0 + 900)) << "said once";
}

TEST(CardMask, AnOldNightReadLateSaysNothing) {
    CardMask m;
    EXPECT_FALSE(m.grew("20261104_231502", kT0 - 20000, kT0 - 7200, kT0));
    EXPECT_FALSE(m.on());
}

TEST(CardMask, AnOlderSessionReadAgainSaysNothing) {
    CardMask m;
    m.grew("20261105_041502", kT0, kT0 + 180, kT0 + 250);
    EXPECT_FALSE(m.grew("20261105_031502", kT0 - 3600, kT0 + 100, kT0 + 250));
    EXPECT_EQ(m.session(), "20261105_041502");
}

TEST(CardMask, ALaterSessionIsTheMaskOnAgain) {
    CardMask m;
    m.grew("20261105_041502", kT0, kT0 + 180, kT0 + 250);
    const auto c = m.grew("20261105_043002", kT0 + 900, kT0 + 960, kT0 + 1000);
    ASSERT_TRUE(c);
    EXPECT_TRUE(c->on);
    EXPECT_EQ(c->session, "20261105_043002");
    EXPECT_EQ(c->occurred_s, kT0 + 900);
}

TEST(CardMask, ASessionThatGrowsAfterItsOffWasOnThroughAMissedBurst) {
    CardMask m;
    m.grew("20261105_041502", kT0, kT0 + 180, kT0 + 250);
    ASSERT_TRUE(m.tick(kT0 + 400));
    EXPECT_FALSE(m.grew("20261105_041502", kT0, kT0 + 180, kT0 + 460)) << "no new record: still off";
    const auto c = m.grew("20261105_041502", kT0, kT0 + 420, kT0 + 470);
    ASSERT_TRUE(c);
    EXPECT_TRUE(c->on);
    EXPECT_EQ(c->occurred_s, kT0 + 180);
}

TEST(CardMask, AFileThatWillNotReadIsJudgedByItsStart) {
    CardMask m;
    EXPECT_TRUE(m.grew("20261105_041502", kT0, 0, kT0 + 250));
    CardMask old;
    EXPECT_FALSE(old.grew("20261104_041502", kT0 - 86400, 0, kT0 + 250));
}

// ── per source ──────────────────────────────────────────────────────────────

// A Sleep Bridge: on at the first therapy write (its session's start), off at
// the STR written at mask-off (the end of the session's last record), or
// after five quiet minutes.
TEST(LiveSources, SleepBridgeOnAtTherapyOffAtTheStrWithTheSessionsMoments) {
    TempDir dir;
    const std::string brp = "DATALOG/20261105/20261105_041502_BRP.edf";
    writeEdf(dir.p / brp, 3);
    Recorder rec;
    const auto t0 = localStem(2026, 11, 5, 4, 15, 2);
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", t0);
    LiveSink sink;
    sink.mask_change = [&](bool on, const std::string& session, Clock::time_point occurred) {
        live.mask({LiveState::kSleepBridge, "TAP-1", on, occurred, session}, t0 + 240s);
    };
    LiveNight night(dir.p.string(), std::move(sink));

    night.onWritten({brp}, t0 + 60s);
    EXPECT_EQ(rec.last("mask"), "ON");
    EXPECT_EQ(rec.change()["source"].asString(), "sleep_bridge");
    EXPECT_EQ(rec.change()["source_id"].asString(), "TAP-1");
    EXPECT_EQ(rec.change()["session"].asString(), "20261105_041502");
    EXPECT_EQ(rec.change()["occurred_at"].asString(), LiveState::isoUtc(t0));

    night.onWritten({"STR.edf"}, t0 + 200s);
    EXPECT_EQ(rec.last("mask"), "OFF");
    EXPECT_EQ(rec.change()["occurred_at"].asString(), LiveState::isoUtc(t0 + 180s))
        << "three one-minute records";
    EXPECT_EQ(rec.change()["delay_s"].asInt(), 60);
}

TEST(LiveSources, SleepBridgeOffAfterFiveQuietMinutes) {
    TempDir dir;
    const std::string brp = "DATALOG/20261105/20261105_041502_BRP.edf";
    writeEdf(dir.p / brp, 2);
    Recorder rec;
    const auto t0 = localStem(2026, 11, 5, 4, 15, 2);
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", t0);
    LiveSink sink;
    sink.mask_change = [&](bool on, const std::string& session, Clock::time_point occurred) {
        live.mask({LiveState::kSleepBridge, "TAP-1", on, occurred, session});
    };
    LiveNight night(dir.p.string(), std::move(sink), 300s);
    night.onWritten({brp}, t0 + 120s);
    night.onDelivered(t0 + 180s);   // the bridge still delivering, no therapy in it
    night.tick(t0 + 419s);
    EXPECT_EQ(rec.last("mask"), "ON");
    night.tick(t0 + 420s);
    EXPECT_EQ(rec.last("mask"), "OFF");
    EXPECT_EQ(rec.change()["occurred_at"].asString(), LiveState::isoUtc(t0 + 120s));
}

// Issue #57, replayed on the shared topics: the bridge's link went quiet for
// four and a half minutes mid-night and then delivered the backlog. Before,
// realtime/mask said OFF at five quiet minutes and ON 33 s later; a stall now
// says nothing.
TEST(LiveSources, SleepBridgeDeliveryStallPublishesNoOff) {
    TempDir dir;
    const std::string brp = "DATALOG/20261105/20261105_041502_BRP.edf";
    writeEdf(dir.p / brp, 400);
    Recorder rec;
    const auto t0 = localStem(2026, 11, 5, 4, 15, 2);
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", t0);
    LiveSink sink;
    sink.mask_change = [&](bool on, const std::string& session, Clock::time_point occurred) {
        live.mask({LiveState::kSleepBridge, "TAP-1", on, occurred, session});
    };
    LiveNight night(dir.p.string(), std::move(sink));
    const auto stall = t0 + 6h;   // the last batch before the stall
    night.onWritten({brp}, t0 + 60s);
    night.onDelivered(stall);
    night.onWritten({brp}, stall + 2s);
    for (auto t = stall + 30s; t < stall + 272s; t += 30s) night.tick(t);
    night.tick(stall + 2s + 300s);
    night.onDelivered(stall + 272s);   // the backlog
    night.onWritten({brp}, stall + 275s);
    for (auto t = stall + 300s; t < stall + 600s; t += 30s) night.tick(t);
    ASSERT_EQ(rec.on("mask").size(), 1u);
    EXPECT_EQ(rec.last("mask"), "ON");
    EXPECT_FALSE(live.streamSaidOff());
}

// An off on quiet that the same session growing disproves: ON again, and the
// night's event counts are kept, since it is the same night going on. A new
// session after an off starts them over, as before.
TEST(LiveState, TheSameSessionBackOnAfterAnOffKeepsItsCounts) {
    Recorder rec;
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", at(kT0));
    live.mask({LiveState::kSleepBridge, "TAP-1", true, at(kT0), "20261105_041502"}, at(kT0 + 5));
    Json::Value e(Json::objectValue);
    e["type"] = "H";
    live.event(e, at(kT0 + 100), at(kT0 + 110));
    live.mask({LiveState::kSleepBridge, "TAP-1", false, at(kT0 + 200), "20261105_041502"},
              at(kT0 + 500));
    EXPECT_TRUE(live.streamSaidOff());
    live.mask({LiveState::kSleepBridge, "TAP-1", true, at(kT0), "20261105_041502"}, at(kT0 + 530));
    EXPECT_FALSE(live.streamSaidOff());
    EXPECT_EQ(rec.last("mask"), "ON");
    EXPECT_EQ(parse(rec.last("live_events"))["total"].asInt(), 1) << "the same night";
    EXPECT_EQ(rec.change()["occurred_at"].asString(), LiveState::isoUtc(at(kT0)))
        << "the session's own start, the moment it went on";

    live.mask({LiveState::kSleepBridge, "TAP-1", false, at(kT0 + 900), "20261105_041502"},
              at(kT0 + 1200));
    live.mask({LiveState::kSleepBridge, "TAP-1", true, at(kT0 + 1500), "20261105_044002"},
              at(kT0 + 1505));
    EXPECT_EQ(parse(rec.last("live_events"))["total"].asInt(), 0) << "a new session";
}

// After a restart the bridge's first step only catches up; the night still
// running says ON at its next therapy write, with no OFF in between.
TEST(LiveSources, SleepBridgeRestartMidNightStaysOn) {
    TempDir dir;
    const std::string brp = "DATALOG/20261105/20261105_041502_BRP.edf";
    writeEdf(dir.p / brp, 30);
    Recorder rec;
    const auto start = localStem(2026, 11, 5, 4, 15, 2) + 1800s;   // hms-cpap restarts
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", start, 300s);
    LiveSink sink;
    sink.mask_change = [&](bool on, const std::string& session, Clock::time_point occurred) {
        live.mask({LiveState::kSleepBridge, "TAP-1", on, occurred, session});
    };
    LiveNight night(dir.p.string(), std::move(sink));
    night.onWritten({brp, "STR.edf"}, start + 5s, /*catch_up=*/true);
    live.tick(start + 30s);
    EXPECT_TRUE(rec.on("mask").empty());
    night.onWritten({brp}, start + 60s);
    live.tick(start + 300s);
    ASSERT_EQ(rec.on("mask").size(), 1u);
    EXPECT_EQ(rec.last("mask"), "ON");
}

// The AirSense 11 over this computer's Bluetooth: no live/* topics of its own,
// the shared ones only.
TEST(LiveSources, As11BluetoothOnAtTheStreamOffAtItsEndAndEventsWithTheirMoment) {
    Recorder rec;
    auto live = std::make_shared<LiveState>(rec.publish(), "cpap_test", "Test CPAP");
    LivePublisher ble(nullptr, "cpap_test", "");
    ble.setState(live, LiveState::kAs11Ble);

    ble.therapy("23000000001", "23000000001", true);
    EXPECT_EQ(rec.last("mask"), "ON");
    EXPECT_EQ(rec.change()["source"].asString(), "as11_ble");
    EXPECT_EQ(rec.change()["source_id"].asString(), "23000000001");
    EXPECT_LE(rec.change()["delay_s"].asInt(), 1);

    hms_cpap::as11::RespEvent e;
    e.event           = "ObstructiveApneaEnd";
    e.reportTime      = "2026-11-05T04:25:16.000Z";
    e.durationSeconds = 14;
    e.backdateSeconds = 14;
    ble.event("23000000001", "23000000001", e);
    ASSERT_EQ(rec.on("event").size(), 1u);
    const auto ev = parse(rec.last("event"));
    EXPECT_EQ(ev["type"].asString(), "OA");
    EXPECT_EQ(ev["source"].asString(), "as11_ble");
    EXPECT_EQ(ev["occurred_at"].asString(), "2026-11-05T04:25:02Z") << "the report less its backdate";
    EXPECT_EQ(parse(rec.last("live_events"))["OA"].asInt(), 1);

    ble.therapy("23000000001", "23000000001", false);
    EXPECT_EQ(rec.last("mask"), "OFF");
    for (const auto& s : rec.out)
        EXPECT_EQ(s.topic.find("/live/"), std::string::npos) << s.topic;
}

TEST(LiveSources, As11BluetoothRestartWithNoStreamSettlesOff) {
    Recorder rec;
    LiveState live(rec.publish(), "cpap_test", "Test CPAP", at(kT0), 300s);
    live.tick(at(kT0 + 300));
    EXPECT_EQ(rec.last("mask"), "OFF");
}

// A card read by the bursts (an ez Share, a FlashAir, a local folder): on
// from the newest BRP's start, off after two missed minutes at its last
// record's end, both a burst or two late and saying so.
TEST(LiveSources, CardOnAtTheSessionsStartOffAtItsEndWithTheDelay) {
    TempDir dir;
    writeEdf(dir.p / "20261105_041502_BRP.edf", 3);
    Recorder rec;
    auto live = std::make_shared<LiveState>(rec.publish(), "cpap_test", "Test CPAP", at(kT0));
    BurstCollectorService svc(300);
    svc.setSourceForTest("ezshare");
    svc.setMachineClockForTest(MachineClock{0, 0, true});   // the machine keeps UTC
    svc.setLiveState(live);

    SessionFileSet s;
    s.date_folder    = "20261105";
    s.session_prefix = "20261105_041502";
    s.brp_files      = {"20261105_041502_BRP.edf"};
    svc.observeCardSession(s, dir.p.string(), kT0 + 250);
    EXPECT_EQ(rec.last("mask"), "ON");
    auto c = rec.change();
    EXPECT_EQ(c["source"].asString(), "ezshare");
    EXPECT_EQ(c["source_id"].asString(), "card");
    EXPECT_EQ(c["session"].asString(), "20261105_041502");
    EXPECT_EQ(c["occurred_at"].asString(), "2026-11-05T04:15:02Z");
    EXPECT_EQ(c["delay_s"].asInt(), 250);

    svc.cardMaskTick(kT0 + 250 + CardMask::kMaskOffAfterS - 1);
    EXPECT_EQ(rec.last("mask"), "ON");
    svc.cardMaskTick(kT0 + 250 + CardMask::kMaskOffAfterS);
    EXPECT_EQ(rec.last("mask"), "OFF");
    c = rec.change();
    EXPECT_EQ(c["occurred_at"].asString(), "2026-11-05T04:18:02Z");
    EXPECT_EQ(c["delay_s"].asInt(), 250 + CardMask::kMaskOffAfterS - 180);
}

TEST(LiveSources, CardSourcesAndTheFoldersALiveSourceAlreadyReports) {
    BurstCollectorService svc(300);
    svc.setSourceForTest("ezshare");
    EXPECT_EQ(svc.cardMaskSource(), "ezshare");
    svc.setSourceForTest("flashair");
    EXPECT_EQ(svc.cardMaskSource(), "flashair");
    svc.setSourceForTest("local");
    svc.setLocalDirForTest("/media/card");
    EXPECT_EQ(svc.cardMaskSource(), "local");
    // A Sleep Bridge's or an AirSense 11 stream's folder: that source reports.
    svc.setLocalDirForTest(
        (fs::path(hms_cpap::AppConfig::dataDir()) / "sleepbridge" / "TAP-S3-00000123").string());
    EXPECT_EQ(svc.cardMaskSource(), "");
    svc.setLocalDirForTest((fs::path(hms_cpap::AppConfig::dataDir()) / "as11" / "23000000001").string());
    EXPECT_EQ(svc.cardMaskSource(), "");
    // No BRP on these cards.
    svc.setSourceForTest("lowenstein");
    EXPECT_EQ(svc.cardMaskSource(), "");
}

TEST(LiveSources, CardRestartAfterTheNightSettlesOff) {
    Recorder rec;
    auto live = std::make_shared<LiveState>(rec.publish(), "cpap_test", "Test CPAP", at(kT0), 300s);
    BurstCollectorService svc(300);
    svc.setSourceForTest("ezshare");
    svc.setLiveState(live);
    svc.cardMaskTick(kT0 + 65);
    EXPECT_TRUE(rec.on("mask").empty());
    svc.cardMaskTick(kT0 + 325);
    EXPECT_EQ(rec.last("mask"), "OFF");
    EXPECT_EQ(rec.change()["source"].asString(), "startup");
}

// Against a broker: a client that subscribes later (Home Assistant starting
// after hms-cpap) reads the entities, the state and its attributes, retained.
TEST(LiveStateBroker, TheEntitiesTheStateAndItsChangeAreRetained) {
    auto pub = std::make_shared<hms::MqttClient>(testMqttConfig("livestate_pub"));
    if (!pub->connect()) GTEST_SKIP() << "no MQTT broker";
    const std::string id = "livestate_" + std::to_string(::getpid());
    LiveState live(
        [pub](const std::string& t, const std::string& p, int q, bool r) {
            return pub->isConnected() && pub->publish(t, p, q, r);
        },
        id, "Live test");
    ASSERT_TRUE(live.announce());
    live.mask({LiveState::kEzShare, "card", true, Clock::now() - 120s, "20261105_041502"});
    std::this_thread::sleep_for(300ms);

    hms::MqttClient probe(testMqttConfig("livestate_probe"));
    ASSERT_TRUE(probe.connect());
    std::map<std::string, std::string> got;
    std::mutex mu;
    probe.subscribe(std::vector<std::string>{live.maskConfigTopic(), live.eventsConfigTopic(),
                                             live.topic("mask"), live.topic("mask_changed")},
                    [&](const std::string& t, const std::string& p) {
                        std::lock_guard<std::mutex> lk(mu);
                        got[t] = p;
                    },
                    1);
    std::this_thread::sleep_for(1s);
    {
        std::lock_guard<std::mutex> lk(mu);
        EXPECT_EQ(parse(got[live.maskConfigTopic()])["unique_id"].asString(), id + "_mask");
        EXPECT_EQ(parse(got[live.eventsConfigTopic()])["unique_id"].asString(), id + "_live_events");
        EXPECT_EQ(got[live.topic("mask")], "ON");
        const auto c = parse(got[live.topic("mask_changed")]);
        EXPECT_EQ(c["source"].asString(), "ezshare");
        EXPECT_GE(c["delay_s"].asInt(), 119);
    }
    for (const auto& t : {live.maskConfigTopic(), live.eventsConfigTopic(), live.topic("mask"),
                          live.topic("mask_changed"), live.topic("live_events")})
        pub->publish(t, "", 1, true);   // tidy up: empty retained payloads delete them
    std::this_thread::sleep_for(200ms);
    probe.disconnect();
    pub->disconnect();
}

// Issue #58: a Sleep Bridge night, end to end on a broker, as Home Assistant
// would read it after the fact (retained): the Mask entity's discovery, ON at
// the first therapy write with the session's start as occurred_at, nothing
// said through a delivery stall, OFF at the STR with the end of the last
// record, and the change's attributes beside the state.
TEST(LiveStateBroker, ASleepBridgeNightIsRetainedOnTheMaskEntity) {
    auto pub = std::make_shared<hms::MqttClient>(testMqttConfig("livestate_sb_pub"));
    if (!pub->connect()) GTEST_SKIP() << "no MQTT broker";
    const std::string id = "livestate_sb_" + std::to_string(::getpid());
    TempDir dir;
    const std::string brp = "DATALOG/20261105/20261105_041502_BRP.edf";
    writeEdf(dir.p / brp, 3);
    const auto t0 = localStem(2026, 11, 5, 4, 15, 2);

    std::vector<std::string> mask_said;
    LiveState live(
        [&, pub](const std::string& t, const std::string& p, int q, bool r) {
            if (t == "cpap/" + id + "/realtime/mask") mask_said.push_back(p);
            return pub->isConnected() && pub->publish(t, p, q, r);
        },
        id, "Live test", t0);
    ASSERT_TRUE(live.announce());
    LiveSink sink;
    sink.mask_change = [&](bool on, const std::string& session, Clock::time_point occurred) {
        live.mask({LiveState::kSleepBridge, "TAP-1", on, occurred, session}, t0 + 240s);
    };
    LiveNight night(dir.p.string(), std::move(sink));

    // Hold the probe's view of each topic: the last retained value.
    auto read = [&](const std::vector<std::string>& topics) {
        hms::MqttClient probe(testMqttConfig("livestate_sb_probe"));
        std::map<std::string, std::string> got;
        std::mutex mu;
        EXPECT_TRUE(probe.connect());
        probe.subscribe(topics,
                        [&](const std::string& t, const std::string& p) {
                            std::lock_guard<std::mutex> lk(mu);
                            got[t] = p;
                        },
                        1);
        std::this_thread::sleep_for(800ms);
        probe.disconnect();
        std::lock_guard<std::mutex> lk(mu);
        return got;
    };
    const std::vector<std::string> topics{live.maskConfigTopic(), live.topic("mask"),
                                          live.topic("mask_changed")};

    night.onDelivered(t0 + 58s);
    night.onWritten({brp}, t0 + 60s);
    // A stall: nothing delivered for four and a half minutes, then the backlog.
    night.tick(t0 + 60s + 300s);
    night.onDelivered(t0 + 330s);
    night.onWritten({brp}, t0 + 332s);
    std::this_thread::sleep_for(300ms);
    auto got = read(topics);
    const auto cfg = parse(got[live.maskConfigTopic()]);
    EXPECT_EQ(cfg["unique_id"].asString(), id + "_mask");
    EXPECT_EQ(cfg["state_topic"].asString(), live.topic("mask"));
    EXPECT_EQ(cfg["json_attributes_topic"].asString(), live.topic("mask_changed"));
    EXPECT_EQ(cfg["device_class"].asString(), "running");
    EXPECT_EQ(got[live.topic("mask")], "ON");
    auto c = parse(got[live.topic("mask_changed")]);
    EXPECT_EQ(c["source"].asString(), "sleep_bridge");
    EXPECT_EQ(c["source_id"].asString(), "TAP-1");
    EXPECT_EQ(c["session"].asString(), "20261105_041502");
    EXPECT_EQ(c["occurred_at"].asString(), LiveState::isoUtc(t0));
    EXPECT_EQ(mask_said, std::vector<std::string>{"ON"}) << "the stall said nothing";

    // Mask off: the machine writes the STR.
    night.onWritten({"STR.edf"}, t0 + 400s);
    std::this_thread::sleep_for(300ms);
    got = read(topics);
    EXPECT_EQ(got[live.topic("mask")], "OFF");
    c = parse(got[live.topic("mask_changed")]);
    EXPECT_EQ(c["state"].asString(), "off");
    EXPECT_EQ(c["occurred_at"].asString(), LiveState::isoUtc(t0 + 180s)) << "the last record's end";
    EXPECT_EQ(c["delay_s"].asInt(), 60);

    for (const auto& t : {live.maskConfigTopic(), live.eventsConfigTopic(), live.topic("mask"),
                          live.topic("mask_changed"), live.topic("live_events")})
        pub->publish(t, "", 1, true);
    std::this_thread::sleep_for(200ms);
    pub->disconnect();
}
