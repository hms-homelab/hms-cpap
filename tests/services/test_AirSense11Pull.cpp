// The AirSense 11's daily summary through the bridge: pulled on a pace, its
// days with usage written as STR days, the next pull asked from two days
// before the newest day the last one brought.
#include <gtest/gtest.h>
#include "services/AirSense11Pull.h"
#include "database/SQLiteDatabase.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

// ── a synthetic Summary spool, from the documented field numbers ───────────
// No real night: every number is made up.

using Bytes = std::vector<uint8_t>;

Bytes varint(uint64_t n) {
    Bytes out;
    do {
        uint8_t b = n & 0x7F;
        n >>= 7;
        out.push_back(n ? (b | 0x80) : b);
    } while (n);
    return out;
}
void append(Bytes& to, const Bytes& b) { to.insert(to.end(), b.begin(), b.end()); }
Bytes fieldVarint(uint32_t num, uint64_t v) {
    Bytes out = varint(static_cast<uint64_t>(num) << 3);
    append(out, varint(v));
    return out;
}
Bytes fieldBytes(uint32_t num, const Bytes& v) {
    Bytes out = varint((static_cast<uint64_t>(num) << 3) | 2);
    append(out, varint(v.size()));
    append(out, v);
    return out;
}
Bytes message(const std::map<uint32_t, uint64_t>& m) {
    Bytes out;
    for (const auto& [k, v] : m) append(out, fieldVarint(k, v));
    return out;
}
constexpr uint64_t kDay  = 86'400'000ULL;
constexpr uint64_t kNoon = 1'772'366'400'000ULL;   // 2026-03-01 12:00:00 UTC

/// One day: usage minutes (0 = a day off), one session of that length.
Bytes day(uint64_t start_ms, int usage) {
    Bytes rec;
    append(rec, fieldVarint(2, start_ms));
    append(rec, fieldVarint(3, start_ms + kDay));
    append(rec, fieldVarint(5, static_cast<uint64_t>(usage)));
    Bytes sess;
    if (usage) append(sess, fieldBytes(1, message({{1, start_ms + 36'000'000}, {2, static_cast<uint64_t>(usage)}})));
    append(rec, fieldBytes(6, sess));
    append(rec, fieldVarint(7, 150));    // AHI 1.50
    append(rec, fieldVarint(10, 90));    // OAI 0.90
    append(rec, fieldBytes(14, message({{2, 10}, {3, 14}, {4, 30}, {5, 60}})));
    append(rec, fieldBytes(21, message({{2, 900}, {3, 1050}, {4, 1200}})));
    append(rec, fieldVarint(39, usage ? 1 : 0));
    return fieldBytes(2, rec);
}

class FakeBridge : public IAirSense11Client {
public:
    std::vector<std::string> asked;
    Bytes answer;
    std::vector<uint8_t> fetchSummary(const std::string& from) override {
        asked.push_back(from);
        return answer;
    }
};

long long count(IDatabase& db, const std::string& sql) {
    auto rows = db.executeQuery(sql, {});
    if (!rows.isArray() || rows.empty()) return -1;
    const auto& v = rows[0u]["n"];
    return v.isString() ? std::atoll(v.asCString()) : v.asInt64();
}

/// SQLite hands every cell back as text.
double num(const Json::Value& v) {
    return v.isString() ? std::atof(v.asCString()) : v.asDouble();
}

class AirSense11PullTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = (fs::temp_directory_path() / ("hms_as11_" + std::to_string(::getpid()) + ".db")).string();
        fs::remove(path_);
        auto lite = std::make_shared<SQLiteDatabase>(path_);
        ASSERT_TRUE(lite->connect());
        db_     = lite;
        bridge_ = std::make_shared<FakeBridge>();
    }
    void TearDown() override {
        db_.reset();
        fs::remove(path_);
    }
    long long days() { return count(*db_, "SELECT COUNT(*) AS n FROM cpap_daily_summary"); }

    std::string                   path_;
    std::shared_ptr<IDatabase>    db_;
    std::shared_ptr<FakeBridge>   bridge_;
};

}  // namespace

TEST_F(AirSense11PullTest, TheFirstPullAsksForEverythingAndWritesTheDaysWithUsage) {
    bridge_->answer = day(kNoon, 0);                    // a day off
    append(bridge_->answer, day(kNoon + kDay, 412));    // a night
    append(bridge_->answer, day(kNoon + 2 * kDay, 380));
    AirSense11Pull pull(bridge_, db_, "AS11-TEST", std::chrono::hours(6));

    const auto r = pull.run(AirSense11Pull::Clock::now());
    EXPECT_TRUE(r.asked);
    EXPECT_EQ(bridge_->asked, (std::vector<std::string>{"2000-01-01"}));
    EXPECT_EQ(r.days, 3);
    EXPECT_EQ(r.written, 2) << "a day off is not written as zeros";
    EXPECT_TRUE(r.error.empty()) << r.error;
    EXPECT_EQ(days(), 2);
    const auto rows = db_->executeQuery(
        "SELECT record_date, duration_minutes, ahi, oai FROM cpap_daily_summary ORDER BY record_date", {});
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_DOUBLE_EQ(num(rows[0u]["duration_minutes"]), 412);
    EXPECT_DOUBLE_EQ(num(rows[0u]["ahi"]), 1.5);
    EXPECT_DOUBLE_EQ(num(rows[0u]["oai"]), 0.9);

    // The next pull asks from two days before the newest day: the machine
    // revises the current day until it is over.
    const auto newest = AirSense11Pull::fromAfter({});
    EXPECT_EQ(newest, "");
    EXPECT_NE(pull.nextFrom(), "2000-01-01");
    EXPECT_EQ(pull.nextFrom().size(), 10u);
}

TEST_F(AirSense11PullTest, PullsArePacedAndAnEmptyAnswerDoesNotMoveTheWindow) {
    AirSense11Pull pull(bridge_, db_, "AS11-TEST", std::chrono::hours(6));
    const auto t0 = AirSense11Pull::Clock::now();

    // Nothing from the bridge: asked, nothing written, the window stays.
    auto r = pull.run(t0);
    EXPECT_TRUE(r.asked);
    EXPECT_EQ(r.written, 0);
    EXPECT_FALSE(r.error.empty());
    EXPECT_EQ(pull.nextFrom(), "2000-01-01");
    EXPECT_EQ(days(), 0);

    // Too soon: not asked at all.
    bridge_->answer = day(kNoon, 300);
    r = pull.run(t0 + std::chrono::hours(1));
    EXPECT_FALSE(r.asked);
    EXPECT_EQ(bridge_->asked.size(), 1u);

    // Due: asked, written, the window moves.
    r = pull.run(t0 + std::chrono::hours(7));
    EXPECT_TRUE(r.asked);
    EXPECT_EQ(r.written, 1);
    EXPECT_EQ(bridge_->asked.size(), 2u);
    EXPECT_NE(pull.nextFrom(), "2000-01-01");
    EXPECT_EQ(days(), 1);

    // Bytes that are not a summary: refused, the window stays.
    bridge_->answer = Bytes{'n', 'o', 't', ' ', 'a', ' ', 's', 'p', 'o', 'o', 'l'};
    const auto before = pull.nextFrom();
    r = pull.pullNow();
    EXPECT_TRUE(r.asked);
    EXPECT_EQ(r.written, 0);
    EXPECT_NE(r.error.find("not a summary"), std::string::npos);
    EXPECT_EQ(pull.nextFrom(), before);
}

TEST_F(AirSense11PullTest, TheSameDayAgainIsAnUpsertNotASecondRow) {
    bridge_->answer = day(kNoon, 300);
    AirSense11Pull pull(bridge_, db_, "AS11-TEST", std::chrono::hours(6));
    ASSERT_EQ(pull.pullNow().written, 1);
    bridge_->answer = day(kNoon, 420);   // the machine revised the day
    ASSERT_EQ(pull.pullNow().written, 1);
    EXPECT_EQ(days(), 1);
    const auto rows = db_->executeQuery("SELECT duration_minutes FROM cpap_daily_summary", {});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_DOUBLE_EQ(num(rows[0u]["duration_minutes"]), 420);
}

// ── the settings, from the machine's STR, onto the Summary's days ──────────

namespace {
STRDailyRecord dayAt(uint64_t noon_ms) {
    STRDailyRecord r;
    r.device_id = "AS11-TEST";
    r.record_date = std::chrono::system_clock::time_point(std::chrono::milliseconds(noon_ms));
    r.duration_minutes = 400;
    return r;
}
}  // namespace

TEST(AirSense11PullSettings, EverySettingsFieldComesFromTheStrRecordOfTheSameDay) {
    // A bi-level ASV card record, so every settings field is carried.
    STRDailyRecord card = dayAt(kNoon);
    card.mode = 8;
    card.epr_level = 2;
    card.pressure_setting = 9.5;
    card.max_pressure = 20;
    card.min_pressure = 5;
    card.family = STRDailyRecord::Family::Asv;
    card.bl_max_ipap = 18; card.bl_min_epap = 6; card.bl_ps = 4; card.bl_ipap = 14; card.bl_epap = 7;
    card.asv_start_press = 6; card.asv_epap = 7; card.asv_max_ps = 12; card.asv_min_ps = 3;
    card.asvauto_min_epap = 5; card.asvauto_max_epap = 11;
    card.duration_minutes = 999;   // not a setting: the Summary's own stays

    std::vector<STRDailyRecord> days{dayAt(kNoon)};
    EXPECT_EQ(AirSense11Pull::withStrSettings(days, {card}), 1);
    const auto& d = days[0];
    EXPECT_EQ(d.mode, card.mode);
    EXPECT_EQ(d.epr_level, card.epr_level);
    EXPECT_EQ(d.pressure_setting, card.pressure_setting);
    EXPECT_EQ(d.max_pressure, card.max_pressure);
    EXPECT_EQ(d.min_pressure, card.min_pressure);
    EXPECT_EQ(d.family, card.family);
    EXPECT_EQ(d.bl_max_ipap, card.bl_max_ipap);
    EXPECT_EQ(d.bl_min_epap, card.bl_min_epap);
    EXPECT_EQ(d.bl_ps, card.bl_ps);
    EXPECT_EQ(d.bl_ipap, card.bl_ipap);
    EXPECT_EQ(d.bl_epap, card.bl_epap);
    EXPECT_EQ(d.asv_start_press, card.asv_start_press);
    EXPECT_EQ(d.asv_epap, card.asv_epap);
    EXPECT_EQ(d.asv_max_ps, card.asv_max_ps);
    EXPECT_EQ(d.asv_min_ps, card.asv_min_ps);
    EXPECT_EQ(d.asvauto_min_epap, card.asvauto_min_epap);
    EXPECT_EQ(d.asvauto_max_epap, card.asvauto_max_epap);
    EXPECT_EQ(d.duration_minutes, 400) << "only settings are taken";
}

TEST(AirSense11PullSettings, ADayBeforeTheSettingsWereReadKeepsNone) {
    STRDailyRecord before = dayAt(kNoon);
    before.mode = -1;   // the STR's "not set": before the settings were read
    STRDailyRecord after = dayAt(kNoon + kDay);
    after.mode = 1;
    after.epr_level = 3;
    std::vector<STRDailyRecord> days{dayAt(kNoon), dayAt(kNoon + kDay), dayAt(kNoon + 2 * kDay)};
    EXPECT_EQ(AirSense11Pull::withStrSettings(days, {before, after}), 1);
    EXPECT_EQ(days[0].mode, 0);
    EXPECT_EQ(days[0].epr_level, 0);
    EXPECT_EQ(days[1].mode, 1);
    EXPECT_EQ(days[1].epr_level, 3);
    EXPECT_EQ(days[2].mode, 0) << "no STR record for that day";
}

TEST_F(AirSense11PullTest, WithoutAnStrThePullStoresTheDaysAsBefore) {
    bridge_->answer = day(kNoon + kDay, 412);
    AirSense11Pull pull(bridge_, db_, "AS11-TEST", std::chrono::hours(6));
    pull.setStrPathProvider([] { return std::string(); });
    EXPECT_EQ(pull.pullNow().written, 1);
    pull.setStrPathProvider([] { return std::string("/nonexistent/STR.edf"); });
    EXPECT_EQ(pull.pullNow().written, 1);
    EXPECT_EQ(days(), 1);
}

TEST(AirSense11PullFrom, TwoDaysBeforeTheNewestDay) {
    STRDailyRecord a, b;
    a.record_date = std::chrono::system_clock::time_point(std::chrono::milliseconds(kNoon));
    b.record_date = std::chrono::system_clock::time_point(std::chrono::milliseconds(kNoon + 5 * kDay));
    const auto from = AirSense11Pull::fromAfter({a, b});
    // 2026-03-06 noon UTC minus 48 h, in the local zone: the 4th anywhere
    // within twelve hours of UTC.
    EXPECT_EQ(from.substr(0, 8), "2026-03-");
    EXPECT_EQ(from.substr(8), "04");
}
