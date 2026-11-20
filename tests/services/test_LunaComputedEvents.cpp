// SDD-058: a Luna's night live (D1), its record while it streams (D2), and the
// events computed from its flow kept apart from the machine's (D3, D4), the
// ring's hypopneas (D5), all behind the gate D6 keeps closed.
//
// Synthetic cards only, in the layout the parser documents: one 256-byte
// packet a second whose words 54-78 are the 25 Hz flow, and a .evt of 32-byte
// records after a 2048-byte region. Invented serial and dates.
#include <gtest/gtest.h>
#include "utils/TimeCompat.h"

#include "database/SQLiteDatabase.h"
#include "services/BmcIngestion.h"
#include "services/BurstCollectorService.h"
#include "services/ComputedEvents.h"
#include "services/DataPublisherService.h"
#include "sleepbridge/LiveNight.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace hms_cpap;
namespace fs = std::filesystem;
namespace bmc = cpapdash::parser;
using hms_cpap::sleepbridge::LiveEvent;
using hms_cpap::sleepbridge::LiveNight;
using hms_cpap::sleepbridge::LiveSink;

namespace {

constexpr const char* kSerial = "11002244";

void putLe16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v & 0xFF);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}
void putLe32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

/// 2025-03-10 23:00:00, the night's first second, as the card counts it.
bmc::BmcCivilSeconds evening() { return *bmc::bmcCivilSeconds(2025, 3, 10, 23, 0, 0); }

/// How the patient breathes in a stretch: 4 s breaths (1.6 s in, 2.4 s out,
/// equal volumes) of `scale` x 30 L/min about a 450-code level; scale 0 is no
/// breathing, a 1 L/min cardiac ripple left, as real flow keeps in an apnea.
struct Stretch {
    int seconds;
    double scale;
};

int flowCode(double scale, long n) {
    const double pi = std::acos(-1.0);
    if (scale <= 0) return static_cast<int>(std::lround(450 + 10 * std::sin(2 * pi * n / 31.25)));
    const double insp = 300 * scale;
    const long phase = n % 100;
    if (phase < 40) return static_cast<int>(std::lround(450 + insp * std::sin(pi * phase / 40.0)));
    return static_cast<int>(std::lround(450 - insp * 40.0 / 60.0 * std::sin(pi * (phase - 40) / 60.0)));
}

/// Session 1's packets from [from] seconds after the evening on, stretch by
/// stretch; the sample count carries on from [from] so the breaths line up.
std::vector<uint8_t> packets(const std::vector<Stretch>& stretches, int from = 0) {
    std::vector<uint8_t> out;
    int sec = from;
    for (const auto& st : stretches) {
        for (int i = 0; i < st.seconds; ++i, ++sec) {
            std::vector<uint8_t> p(bmc::kBmcPacketBytes, 0);
            putLe16(p, 2 * bmc::bmc_word::kSync, bmc::kBmcSync);
            putLe16(p, 2 * bmc::bmc_word::kSessionNumber, 1);
            putLe16(p, 2 * bmc::bmc_word::kEpap, 14);
            putLe16(p, 2 * bmc::bmc_word::kIpap, 14);
            putLe16(p, 2 * bmc::bmc_word::kLeak, 50);
            putLe16(p, 2 * bmc::bmc_word::kRespRate, 15);
            putLe16(p, 2 * bmc::bmc_word::kIeRatio, 15);
            for (int k = 0; k < bmc::kBmcWaveHz; ++k) {
                putLe16(p, 2 * (bmc::bmc_word::kPressure + k), 70);
                putLe16(p, 2 * (bmc::bmc_word::kFlow + k),
                        static_cast<uint16_t>(static_cast<int16_t>(
                            flowCode(st.scale, sec * 25L + k))));
            }
            const std::time_t t = static_cast<std::time_t>(evening() + sec);
            std::tm tm{};
            gmtime_r(&t, &tm);   // civil seconds: the fields, no zone
            putLe16(p, 248, static_cast<uint16_t>(tm.tm_year + 1900));
            p[250] = static_cast<uint8_t>(tm.tm_mon + 1);
            p[251] = static_cast<uint8_t>(tm.tm_mday);
            p[252] = static_cast<uint8_t>(tm.tm_hour);
            p[253] = static_cast<uint8_t>(tm.tm_min);
            p[254] = static_cast<uint8_t>(tm.tm_sec);
            out.insert(out.end(), p.begin(), p.end());
        }
    }
    return out;
}

/// 180 s of breathing, a 20 s pause at 23:03:00, 120 s of breathing.
std::vector<Stretch> nightWithAPause() { return {{180, 1.0}, {20, 0.0}, {120, 1.0}}; }
/// The same with the 20 s at half the breath instead: a hypopnea's drop.
std::vector<Stretch> nightWithAHalfBreath() { return {{180, 1.0}, {20, 0.5}, {120, 1.0}}; }

/// The machine's own obstructive apnea of session 1 at 23:01:00 (12 s), the
/// onset counted from noon of the sleep day.
std::vector<uint8_t> machineApnea() {
    std::vector<uint8_t> b(bmc::kBmcEventRegionBytes, 0xFF);
    std::vector<uint8_t> r(bmc::kBmcEventRecordBytes, 0);
    putLe16(r, 0, bmc::kBmcSync);
    putLe16(r, 2, 1);
    putLe16(r, 4, static_cast<uint16_t>(bmc::bmc_event::kObstructive));
    putLe32(r, 8, 11 * 3600 + 60);
    putLe32(r, 12, 12);
    b.insert(b.end(), r.begin(), r.end());
    return b;
}

void write(const fs::path& p, const std::vector<uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::chrono::system_clock::time_point at(int seconds_after_evening) {
    return bmc::bmcToTimePoint(evening() + seconds_after_evening);
}

double secondsAfterEvening(std::chrono::system_clock::time_point t) {
    return std::chrono::duration<double>(t - at(0)).count();
}

class LunaComputed : public ::testing::Test {
protected:
    fs::path root_;
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("hms_cpap_luna58_" + std::to_string(::getpid()) + "_" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root_);
        fs::create_directories(root_);
    }
    void TearDown() override { fs::remove_all(root_); }

    void writeData(const std::vector<Stretch>& night) {
        write(root_ / (std::string(kSerial) + ".000"), packets(night));
    }
    void writeEvents() { write(root_ / (std::string(kSerial) + ".evt"), machineApnea()); }
    void writeUser() { write(root_ / (std::string(kSerial) + ".USR"), std::vector<uint8_t>(64, 0x20)); }
};

struct Burst {
    std::shared_ptr<SQLiteDatabase> db;
    BurstCollectorService svc{60};
    explicit Burst(const fs::path& root) {
        db = std::make_shared<SQLiteDatabase>((root / "cpap.db").string());
        EXPECT_TRUE(db->connect());
        svc.injectDependenciesForTest(
            db, nullptr,
            std::make_unique<DataPublisherService>(std::shared_ptr<hms::MqttClient>{}, db));
        svc.useBmcCardForTest(root.string());
    }
    double number(const std::string& sql) {
        const Json::Value rows = db->executeQuery(sql, {svc.deviceIdForTest()});
        if (!rows.isArray() || rows.empty() || rows[0]["v"].isNull()) return -1;
        const Json::Value& v = rows[0]["v"];
        return v.isString() ? std::stod(v.asString()) : v.asDouble();
    }
    std::string text(const std::string& sql) {
        const Json::Value rows = db->executeQuery(sql, {svc.deviceIdForTest()});
        if (!rows.isArray() || rows.empty() || rows[0]["v"].isNull()) return {};
        return rows[0]["v"].asString();
    }
    double computedRows() {
        return number("SELECT COUNT(*) AS v FROM cpap_computed_events c JOIN cpap_sessions s"
                      " ON s.id = c.session_id WHERE s.device_id = ?");
    }
    double machineRows() {
        return number("SELECT COUNT(*) AS v FROM cpap_events e JOIN cpap_sessions s"
                      " ON s.id = e.session_id WHERE s.device_id = ?");
    }
    std::string indexEvents() {
        return text("SELECT m.index_events AS v FROM cpap_session_metrics m JOIN cpap_sessions s"
                    " ON s.id = m.session_id WHERE s.device_id = ?");
    }
    std::string indexKind() {
        return text("SELECT m.index_kind AS v FROM cpap_session_metrics m JOIN cpap_sessions s"
                    " ON s.id = m.session_id WHERE s.device_id = ?");
    }
    double totalEvents() {
        return number("SELECT m.total_events AS v FROM cpap_session_metrics m JOIN cpap_sessions s"
                      " ON s.id = m.session_id WHERE s.device_id = ?");
    }
    double open() {
        return number("SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ?"
                      " AND session_end IS NULL");
    }
};

}  // namespace

// ── D6: the gate ─────────────────────────────────────────────────────────────

TEST(ComputedEventsGate, IsClosed) {
    EXPECT_FALSE(kComputedEventsOpen)
        << "SDD-058 D6: no computed index until it agrees with a reference on real nights";
}

TEST_F(LunaComputed, WithTheGateClosedNothingIsComputedOrStored) {
    writeData(nightWithAPause());
    writeEvents();
    writeUser();
    Burst b(root_);
    ASSERT_TRUE(b.svc.runBurstCycleForTest());
    EXPECT_EQ(b.computedRows(), 0) << "the pause in the flow is not stored";
    EXPECT_EQ(b.machineRows(), 1);
    EXPECT_EQ(b.indexEvents(), "machine");
    EXPECT_EQ(b.totalEvents(), 1);
    EXPECT_EQ(b.indexKind(), "ahi");
}

// ── D4: which set counts ─────────────────────────────────────────────────────

TEST(ComputedEventsChoice, TheMachinesEventsWinOnceTheirFileIsThereAndTheSessionStopped) {
    EXPECT_EQ(chooseIndexEvents(true, false), IndexEvents::Machine);
    EXPECT_EQ(chooseIndexEvents(true, true), IndexEvents::Computed) << "before the stop";
    EXPECT_EQ(chooseIndexEvents(false, false), IndexEvents::Computed) << "a card with no .evt";
    EXPECT_EQ(chooseIndexEvents(false, true), IndexEvents::Computed);
    EXPECT_STREQ(indexEventsName(IndexEvents::Machine), "machine");
    EXPECT_STREQ(indexEventsName(IndexEvents::Computed), "computed");
}

TEST(ComputedEventsChoice, AnIndexFromTheComputedSetIsNeverASum) {
    CPAPSession s;
    s.duration_seconds = 3600;
    s.events = {CPAPEvent(EventType::OBSTRUCTIVE, at(60), 12)};
    s.computed_events = {CPAPEvent(EventType::APNEA, at(180), 20),
                         CPAPEvent(EventType::APNEA, at(900), 15)};
    for (auto& e : s.computed_events) e.source = bmc::EventSource::Computed;
    s.calculateMetrics();
    s.metrics->avg_pressure = 7.0;
    ASSERT_EQ(s.metrics->total_events, 1);

    indexFromComputed(s);
    EXPECT_EQ(s.events.size(), 1u) << "the machine's events are kept as they are";
    EXPECT_EQ(s.computed_events.size(), 2u);
    EXPECT_EQ(s.metrics->total_events, 2) << "two, not three";
    EXPECT_EQ(s.metrics->obstructive_apneas, 0);
    EXPECT_EQ(s.metrics->unclassified_apneas, 2);
    EXPECT_DOUBLE_EQ(s.metrics->ahi, 2.0);
    EXPECT_EQ(s.metrics->index_kind, SessionMetrics::IndexKind::Ungraded) << "apneas only (D5)";
    EXPECT_DOUBLE_EQ(s.metrics->avg_pressure.value_or(0), 7.0) << "every other figure kept";

    s.computed_events.push_back(CPAPEvent(EventType::HYPOPNEA, at(1500), 12));
    indexFromComputed(s);
    EXPECT_EQ(s.metrics->index_kind, SessionMetrics::IndexKind::AHI)
        << "with the ring's hypopneas it is an AHI";
    EXPECT_DOUBLE_EQ(s.metrics->ahi, 3.0);
}

TEST_F(LunaComputed, WithTheGateOpenBothSetsAreStoredApartAndTheLiveNightCountsTheComputed) {
    // The first look: the card has its .evt and nothing is being written.
    writeData({{180, 1.0}, {20, 0.0}, {60, 1.0}});
    writeEvents();
    writeUser();
    Burst b(root_);
    b.svc.setComputedEventsOpenForTest(true);
    ASSERT_TRUE(b.svc.runBurstCycleForTest());
    EXPECT_EQ(b.machineRows(), 1);
    EXPECT_EQ(b.computedRows(), 1) << "the pause, stored beside the machine's apnea";
    EXPECT_EQ(b.indexEvents(), "machine") << "the .evt is there and the session stopped";
    EXPECT_EQ(b.totalEvents(), 1);

    // The machine writes on: until the stop, the computed set is the night's.
    writeData(nightWithAPause());
    ASSERT_TRUE(b.svc.runBurstCycleForTest());
    EXPECT_EQ(b.open(), 1) << "growing, so live";
    EXPECT_EQ(b.indexEvents(), "computed");
    EXPECT_EQ(b.indexKind(), "ungraded") << "apneas only: no ring";
    EXPECT_EQ(b.totalEvents(), 1) << "the computed apnea alone, never 1 + 1";
    EXPECT_EQ(b.machineRows(), 1) << "the machine's events are still stored";
    EXPECT_EQ(b.computedRows(), 1) << "replaced, not added to";

    // Inside the quiet window, nothing new: still the computed set.
    b.svc.setCardMaskQuietForTest(std::chrono::hours(1));
    ASSERT_TRUE(b.svc.runBurstCycleForTest());
    EXPECT_EQ(b.open(), 1);
    EXPECT_EQ(b.indexEvents(), "computed");

    // Quiet: closed, and the machine's set wins its night again.
    b.svc.setCardMaskQuietForTest(std::chrono::seconds(0));
    ASSERT_TRUE(b.svc.runBurstCycleForTest());
    EXPECT_EQ(b.open(), 0);
    EXPECT_EQ(b.indexEvents(), "machine");
    EXPECT_EQ(b.indexKind(), "ahi");
    EXPECT_EQ(b.totalEvents(), 1) << "the machine's apnea alone";
    EXPECT_EQ(b.computedRows(), 1) << "the computed one stays stored, apart";
}

// ── D2, D4: a card the machine has not stopped on ────────────────────────────

TEST_F(LunaComputed, ACardWithNoEventFileYetIsReadAndItsEventsArriveAtTheStop) {
    writeData(nightWithAPause());
    writeUser();   // no .evt: the machine writes it at the stop
    {
        const auto read = BmcIngestion(root_.string()).readSessions("dev", "Luna");
        ASSERT_TRUE(read.ok) << read.error;
        EXPECT_FALSE(read.has_event_file);
        ASSERT_EQ(read.sessions.size(), 1u);
        EXPECT_EQ(read.sessions[0]->duration_seconds.value_or(0), 320);
        EXPECT_TRUE(read.sessions[0]->events.empty());
    }
    Burst b(root_);
    ASSERT_TRUE(b.svc.runBurstCycleForTest());
    EXPECT_EQ(b.machineRows(), 0);

    writeEvents();   // the stop: the .evt, the same seconds
    ASSERT_TRUE(b.svc.runBurstCycleForTest());
    EXPECT_EQ(b.machineRows(), 1) << "the night takes the events the machine wrote at the stop";
    EXPECT_EQ(b.totalEvents(), 1);
    EXPECT_EQ(b.open(), 0) << "events alone are not the machine writing";
}

// ── D5: the ring's hypopneas ─────────────────────────────────────────────────

namespace {

/// The ring's night on its own clock, `behind` seconds behind the machine's:
/// 96% throughout, but 92% from [dip_from] to [dip_to] (machine seconds).
std::vector<CPAPVitals> ring(int dip_from, int dip_to, std::chrono::seconds behind) {
    std::vector<CPAPVitals> out;
    for (int sec = 0; sec < 320; ++sec) {
        CPAPVitals v(at(sec) - behind);
        v.spo2 = (sec >= dip_from && sec < dip_to) ? 92.0 : 96.0;
        v.heart_rate = 60;
        out.push_back(v);
    }
    return out;
}

}  // namespace

TEST(RingHypopneas, AHalfBreathWithTheRingsDesaturationAfterItIsAHypopnea) {
    const auto card = bmc::buildBmcCard(kSerial, {packets(nightWithAHalfBreath())}, {});
    ASSERT_EQ(card.sessions.size(), 1u);
    const auto behind = std::chrono::seconds(90);
    const auto found = ringHypopneas(card, card.sessions[0], ring(212, 235, behind), behind);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].event_type, EventType::HYPOPNEA);
    EXPECT_EQ(found[0].source, bmc::EventSource::Computed);
    EXPECT_NEAR(secondsAfterEvening(found[0].timestamp), 180.0, 4.0);
    EXPECT_GE(found[0].duration_seconds, 10.0);
}

TEST(RingHypopneas, NoRingOrNoDesaturationOrTheRingsOwnClockIsNothing) {
    const auto card = bmc::buildBmcCard(kSerial, {packets(nightWithAHalfBreath())}, {});
    const auto& span = card.sessions[0];
    const auto behind = std::chrono::seconds(90);
    EXPECT_TRUE(ringHypopneas(card, span, {}, behind).empty()) << "ringless: never flow-only";
    EXPECT_TRUE(ringHypopneas(card, span, ring(0, 0, behind), behind).empty())
        << "a ring that never desaturates";
    EXPECT_TRUE(ringHypopneas(card, span, ring(212, 235, behind), std::chrono::seconds(0)).empty())
        << "left on the ring's clock, the dip falls 90 s early, before the drop";
}

TEST(RingHypopneas, AnApneaIsNotAlsoAHypopnea) {
    const auto card = bmc::buildBmcCard(kSerial, {packets(nightWithAPause())}, {});
    const auto behind = std::chrono::seconds(0);
    EXPECT_EQ(bmc::BmcParser::flowApneas(card, card.sessions[0]).size(), 1u);
    EXPECT_TRUE(ringHypopneas(card, card.sessions[0], ring(205, 230, behind), behind).empty());
}

// ── D1: the live view ────────────────────────────────────────────────────────

namespace {

struct LiveRecorder {
    std::vector<LiveEvent> events;
    std::vector<std::tuple<bool, std::string, std::chrono::system_clock::time_point>> masks;
    std::vector<int> totals;
    LiveSink sink() {
        LiveSink s;
        s.event = [this](const LiveEvent& e) { events.push_back(e); };
        s.counts = [this](const std::map<std::string, int>&, int t) { totals.push_back(t); };
        s.mask_change = [this](bool on, const std::string& session,
                               std::chrono::system_clock::time_point occurred) {
            masks.emplace_back(on, session, occurred);
        };
        return s;
    }
};

using namespace std::chrono_literals;
const auto t0 = std::chrono::system_clock::from_time_t(1800000000);

}  // namespace

TEST_F(LunaComputed, TheGrowingDataFileIsTheMaskOnAndItsQuietTheMaskOff) {
    writeUser();
    write(root_ / (std::string(kSerial) + ".000"), packets({{120, 1.0}}));
    LiveRecorder rec;
    LiveNight live(root_.string(), rec.sink(), 300s);
    const std::string data = std::string(kSerial) + ".000";

    live.onWritten({data}, t0);
    EXPECT_TRUE(live.maskOn());
    ASSERT_EQ(rec.masks.size(), 1u);
    EXPECT_TRUE(std::get<0>(rec.masks[0]));
    EXPECT_EQ(std::get<1>(rec.masks[0]), "20250310_230000") << "named like a ResMed session";
    EXPECT_EQ(std::get<2>(rec.masks[0]), at(0)) << "the session's first second";

    write(root_ / data, packets({{320, 1.0}}));
    live.onWritten({data}, t0 + 60s);
    live.onDelivered(t0 + 100s);
    live.tick(t0 + 400s);
    EXPECT_FALSE(live.maskOn()) << "the stream delivered and the file did not grow";
    ASSERT_EQ(rec.masks.size(), 2u);
    EXPECT_EQ(std::get<2>(rec.masks[1]), at(320)) << "the second after the last packet";
    EXPECT_TRUE(rec.events.empty()) << "no computed events with the gate closed";
}

TEST_F(LunaComputed, TheEventFileWrittenGivesTheMachinesNewApneasOnce) {
    writeUser();
    writeData(nightWithAPause());
    LiveRecorder rec;
    LiveNight live(root_.string(), rec.sink(), 300s);
    const std::string data = std::string(kSerial) + ".000";
    const std::string evt = std::string(kSerial) + ".evt";

    live.onWritten({data}, t0);
    writeEvents();
    live.onWritten({evt}, t0 + 30s);
    ASSERT_EQ(rec.events.size(), 1u);
    EXPECT_EQ(rec.events[0].type, "OA");
    EXPECT_EQ(rec.events[0].at, at(60));
    EXPECT_DOUBLE_EQ(rec.events[0].duration_s, 12);
    EXPECT_FALSE(rec.events[0].computed);
    EXPECT_EQ(live.total(), 1);

    live.onWritten({evt}, t0 + 60s);   // written again, nothing new
    EXPECT_EQ(rec.events.size(), 1u);
}

TEST_F(LunaComputed, ACatchUpMarksTheCardsEventsAsKnown) {
    writeUser();
    writeData(nightWithAPause());
    writeEvents();
    LiveRecorder rec;
    LiveNight live(root_.string(), rec.sink(), 300s);
    const std::string data = std::string(kSerial) + ".000";
    const std::string evt = std::string(kSerial) + ".evt";
    live.onWritten({data, evt}, t0, /*catch_up=*/true);
    EXPECT_FALSE(live.maskOn());
    live.onWritten({evt}, t0 + 30s);
    EXPECT_TRUE(rec.events.empty()) << "already on the card when the bridge caught up";
}

TEST_F(LunaComputed, WithComputedEventsOnTheFlowsApneaIsALiveEventSaidAsComputed) {
    writeUser();
    const std::string data = std::string(kSerial) + ".000";
    write(root_ / data, packets({{180, 1.0}}));
    LiveRecorder rec;
    LiveNight live(root_.string(), rec.sink(), 300s, 900s, /*computed_events=*/true);
    live.onWritten({data}, t0);
    EXPECT_TRUE(rec.events.empty());

    writeData(nightWithAPause());
    live.onWritten({data}, t0 + 60s);
    ASSERT_EQ(rec.events.size(), 1u);
    EXPECT_EQ(rec.events[0].type, "UA");
    EXPECT_TRUE(rec.events[0].computed);
    EXPECT_NEAR(secondsAfterEvening(rec.events[0].at), 180.0, 4.0);

    live.onWritten({data}, t0 + 120s);
    EXPECT_EQ(rec.events.size(), 1u) << "said once";
}
