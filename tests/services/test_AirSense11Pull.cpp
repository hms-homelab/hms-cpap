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
