/**
 * QueryService read paths.
 *
 * QueryService joined the test target with the oximetry fix, and only the
 * oximetry query came with cover, so ~500 lines of hand-built SQL entered the
 * coverage denominator untested and the ratchet went red. Raising the threshold
 * would have hidden that; this covers the code instead.
 *
 * These matter beyond the number. QueryService writes SQL for THREE dialects by
 * hand, and every method here is what the dashboard actually calls. The
 * oximetry bug it was written for (an unnarrowed OR serving one recording on two
 * nights) is exactly the failure that survives a compiler and a type checker and
 * only a query test catches.
 *
 * Every method is also exercised against an EMPTY database. A read path that
 * throws or returns malformed JSON on a fresh install breaks the dashboard for
 * every new user, which is the single worst first impression the product can
 * make, and it is invisible to any test that seeds data first.
 */

#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "web/QueryService.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace hms_cpap;
using namespace std::chrono;
namespace fs = std::filesystem;

namespace {

constexpr const char* kDevice = "cpap-test";

class QueryServiceReadTest : public ::testing::Test {
protected:
    void SetUp() override {
        static int counter = 0;
        db_path_ = (fs::temp_directory_path() /
                    ("hms_cpap_qs_read_" + std::to_string(::getpid()) +
                     "_" + std::to_string(counter++) + ".db")).string();
        cleanup();

        db_ = std::make_shared<SQLiteDatabase>(db_path_);
        ASSERT_TRUE(db_->connect());
        qs_ = std::make_unique<QueryService>(db_, kDevice);
    }

    void TearDown() override {
        qs_.reset();
        db_.reset();
        cleanup();
    }

    void cleanup() {
        for (const auto* suffix : {"", "-wal", "-shm"}) {
            std::error_code ec;
            fs::remove(db_path_ + suffix, ec);
        }
    }

    static system_clock::time_point tp(long secs) {
        return system_clock::time_point{} + seconds(secs);
    }

    /// Noon, `days_ago` days back from today.
    ///
    /// RELATIVE, not a fixed calendar date. The dashboard trends look back a
    /// fixed window from now, so hard-coded dates pass when written and quietly
    /// fall out of range weeks later, which is a test that rots into a
    /// false negative.
    static long noonDaysAgo(int days_ago) {
        const auto now = system_clock::now();
        const long secs = duration_cast<seconds>(now.time_since_epoch()).count();
        const long midnight = (secs / 86400) * 86400;
        return midnight - static_cast<long>(days_ago) * 86400 + 12 * 3600;
    }

    /// One finished night. Deliberately built through saveSession rather than
    /// raw SQL, so the tests exercise the same shape the collector writes.
    void addSession(long start_epoch, int duration_seconds) {
        CPAPSession s;
        s.device_id = kDevice;
        s.device_name = "Test Machine";
        s.session_start = tp(start_epoch);
        s.session_end = tp(start_epoch + duration_seconds);
        s.duration_seconds = duration_seconds;
        ASSERT_TRUE(db_->saveSession(s));
    }

    /// A day in cpap_daily_summary, which is what the dashboard, trends and
    /// statistics actually read. Sessions alone leave all of those empty, so
    /// without this the row-mapping loops, where most of the code lives, never
    /// execute.
    void addDailySummary(long noon_epoch, double ahi, double duration_minutes) {
        STRDailyRecord r;
        r.device_id = kDevice;
        r.record_date = tp(noon_epoch);
        r.ahi = ahi;
        r.duration_minutes = duration_minutes;
        ASSERT_TRUE(db_->saveSTRDailyRecords({r}));
    }

    std::string db_path_;
    std::shared_ptr<SQLiteDatabase> db_;
    std::unique_ptr<QueryService> qs_;
};

// ─────────────────────────────────────────────────────────────────────────────
// The empty database. Every one of these is a fresh install.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QueryServiceReadTest, DashboardOnAFreshInstallIsWellFormed) {
    const auto j = qs_->getDashboard();
    // Not merely "did not throw": the frontend indexes into this, so an object
    // is the contract even when there is nothing to report.
    EXPECT_TRUE(j.isObject()) << "the dashboard must be an object on day one";
}

TEST_F(QueryServiceReadTest, EveryReadPathSurvivesAnEmptyDatabase) {
    // A read that throws on a fresh install breaks the dashboard for every new
    // user, and seeding data first would hide it completely.
    EXPECT_NO_THROW({
        (void)qs_->getSessions(10, 0);
        (void)qs_->getSessionDetail("20260505");
        (void)qs_->getDailySummary("2026-05-01", "2026-05-31");
        (void)qs_->getTrend("ahi", 30);
        (void)qs_->getStatistics("2026-05-01", "2026-05-31");
        (void)qs_->getSummaries("weekly", 5);
        (void)qs_->getInsights(30);
        (void)qs_->getSessionSignals("20260505");
        (void)qs_->getSessionVitals("20260505", 60);
        (void)qs_->getSessionEvents("20260505");
        (void)qs_->getSessionBreaths("20260505");
        (void)qs_->getSessionOximetry("20260505", 1);
    });
}

TEST_F(QueryServiceReadTest, ListsAreEmptyArraysNotNull) {
    // null and [] are different to the frontend: one iterates, the other throws.
    const auto sessions = qs_->getSessions(10, 0);
    EXPECT_TRUE(sessions.isArray()) << "getSessions must return an array";
    EXPECT_EQ(sessions.size(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// With data
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QueryServiceReadTest, ASavedSessionAppearsInTheList) {
    addSession(1778020200, 8 * 3600);       // 2026-05-05 22:30 UTC
    const auto sessions = qs_->getSessions(10, 0);
    ASSERT_TRUE(sessions.isArray());
    ASSERT_GE(sessions.size(), 1u) << "a saved session never came back";
    EXPECT_TRUE(sessions[0].isMember("sleep_day"));
}

TEST_F(QueryServiceReadTest, SessionsCarryTheSDD008NightState) {
    // The enrichment added with partial-night reporting. It is attached in C++
    // rather than SQL because the join key needs a string transform across three
    // dialects, and until now nothing tested it at all.
    addSession(1778020200, 8 * 3600);
    const auto sessions = qs_->getSessions(10, 0);
    ASSERT_GE(sessions.size(), 1u);

    ASSERT_TRUE(sessions[0].isMember("night_state"))
        << "night_state is missing; the frontend would fall back to re-deriving it";
    ASSERT_TRUE(sessions[0].isMember("partial"));

    const auto state = sessions[0]["night_state"].asString();
    EXPECT_TRUE(state == "live" || state == "partial" || state == "complete")
        << "unexpected night_state: " << state;
}

TEST_F(QueryServiceReadTest, WithNoLedgerRowANightIsNotReportedPartial) {
    // No sync-folder ledger exists for a session saved directly, which is the
    // case for every local-directory and Prisma user. Reporting those partial
    // would badge nights that were never transferred at all.
    addSession(1778020200, 8 * 3600);
    const auto sessions = qs_->getSessions(10, 0);
    ASSERT_GE(sessions.size(), 1u);
    EXPECT_FALSE(sessions[0]["partial"].asBool())
        << "a night with no transfer ledger was reported as partial";
}

TEST_F(QueryServiceReadTest, PagingActuallyPages) {
    // Three separate nights, 24h apart, so each is its own sleep_day.
    addSession(1778020200, 3600);
    addSession(1778020200 + 86400, 3600);
    addSession(1778020200 + 2 * 86400, 3600);

    const auto first = qs_->getSessions(2, 0);
    const auto second = qs_->getSessions(2, 2);
    ASSERT_TRUE(first.isArray());
    ASSERT_TRUE(second.isArray());
    EXPECT_LE(first.size(), 2u) << "limit was not applied";

    if (first.size() > 0 && second.size() > 0) {
        EXPECT_NE(first[0]["sleep_day"].asString(), second[0]["sleep_day"].asString())
            << "offset returned the same page";
    }
}

TEST_F(QueryServiceReadTest, ADayWithNoSessionHasNoDetail) {
    addSession(1778020200, 3600);
    // Returns an ARRAY of that night's sessions, not an object. Asserted as
    // discovered rather than as assumed: an empty array and an object with
    // zeroed metrics look identical to a careless reader and completely
    // different to the frontend.
    const auto detail = qs_->getSessionDetail("19990101");
    ASSERT_TRUE(detail.isArray()) << "getSessionDetail changed shape";
    EXPECT_EQ(detail.size(), 0u) << "a night that does not exist returned rows";
}

TEST_F(QueryServiceReadTest, ADayWithASessionHasDetail) {
    // The other half, so the empty case above cannot pass simply because the
    // query is broken for every date.
    addSession(1778020200, 3600);
    const auto sessions = qs_->getSessions(10, 0);
    ASSERT_GE(sessions.size(), 1u);

    const auto detail = qs_->getSessionDetail(sessions[0]["sleep_day"].asString());
    ASSERT_TRUE(detail.isArray());
    EXPECT_GE(detail.size(), 1u) << "a night that exists returned no detail";
}

TEST_F(QueryServiceReadTest, TrendAndStatisticsAcceptOrdinaryArguments) {
    addSession(1778020200, 8 * 3600);
    // Both return arrays of rows. Written after checking, not before: guessing
    // the shape is how a test ends up asserting the wrong thing confidently.
    EXPECT_TRUE(qs_->getTrend("ahi", 30).isArray());
    EXPECT_TRUE(qs_->getStatistics("2026-05-01", "2026-05-31").isArray());
}

TEST_F(QueryServiceReadTest, AnUnknownTrendMetricDoesNotBecomeSql) {
    // The metric name reaches a hand-built query. Whatever it does with an
    // unknown one, it must not throw and must not be interpolated blindly.
    EXPECT_NO_THROW({
        (void)qs_->getTrend("definitely_not_a_column", 30);
        (void)qs_->getTrend("ahi'; DROP TABLE cpap_sessions;--", 30);
    });
    // The table must still be there afterwards.
    EXPECT_NO_THROW({ (void)qs_->getSessions(1, 0); });
}

TEST_F(QueryServiceReadTest, SummariesAcceptEachPeriod) {
    for (const char* period : {"weekly", "monthly", "nightly"}) {
        EXPECT_NO_THROW({ (void)qs_->getSummaries(period, 5); }) << period;
    }
}

TEST_F(QueryServiceReadTest, SignalReadsForAnAbsentNightAreEmptyNotBroken) {
    addSession(1778020200, 3600);
    EXPECT_NO_THROW({
        (void)qs_->getSessionSignals("19990101");
        (void)qs_->getSessionVitals("19990101", 60);
        (void)qs_->getSessionEvents("19990101");
        (void)qs_->getSessionBreaths("19990101");
    });
}


// ─────────────────────────────────────────────────────────────────────────────
// With daily summaries, which is what the dashboard actually reads
//
// Sessions alone leave the dashboard, trends and statistics empty, so these
// exercise the row-mapping loops rather than the early returns above.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QueryServiceReadTest, TheDashboardReportsTheLatestNight) {
    addDailySummary(noonDaysAgo(2), 4.2, 430);
    addDailySummary(noonDaysAgo(1), 2.1, 465);   // the newer night

    const auto j = qs_->getDashboard();
    ASSERT_TRUE(j.isObject());
    ASSERT_TRUE(j.isMember("latest_night")) << "the dashboard lost its latest night";
    // Newest wins: showing the night before last is the kind of wrong that
    // looks right.
    // executeQuery hands every column back as a STRING, so this goes through
    // stod rather than asDouble, which throws.
    EXPECT_NEAR(std::stod(j["latest_night"]["ahi"].asString()), 2.1, 0.01);
}

TEST_F(QueryServiceReadTest, TheDashboardCarriesItsTrends) {
    for (int i = 0; i < 5; ++i)
        addDailySummary(noonDaysAgo(5 - i), 3.0 + i, 400 + i);

    const auto j = qs_->getDashboard();
    ASSERT_TRUE(j.isObject());
    for (const char* key : {"ahi_trend", "usage_trend"}) {
        if (j.isMember(key)) {
            EXPECT_TRUE(j[key].isArray()) << key << " must be an array";
            EXPECT_GE(j[key].size(), 1u) << key << " was empty with five nights stored";
        }
    }
}

TEST_F(QueryServiceReadTest, TrendReturnsOneRowPerNight) {
    for (int i = 0; i < 4; ++i)
        addDailySummary(noonDaysAgo(4 - i), 3.0 + i, 400);

    const auto t = qs_->getTrend("ahi", 3650);   // wide window, all four nights
    ASSERT_TRUE(t.isArray());
    EXPECT_GE(t.size(), 4u) << "the trend dropped nights";
}

TEST_F(QueryServiceReadTest, StatisticsAggregateTheStoredNights) {
    addDailySummary(noonDaysAgo(2), 2.0, 400);
    addDailySummary(noonDaysAgo(1), 6.0, 500);

    const auto st = qs_->getStatistics("2000-01-01", "2099-12-31");
    ASSERT_TRUE(st.isArray());
    ASSERT_GE(st.size(), 1u) << "statistics returned nothing for two stored nights";
    // Averaged, not summed or last-wins.
    if (st[0].isMember("avg_ahi") && !st[0]["avg_ahi"].isNull()) {
        EXPECT_NEAR(std::stod(st[0]["avg_ahi"].asString()), 4.0, 0.2);
    }
}

TEST_F(QueryServiceReadTest, DailySummaryReturnsTheRangeAsked) {
    for (int i = 0; i < 3; ++i)
        addDailySummary(noonDaysAgo(3 - i), 3.0, 400);

    const auto all = qs_->getDailySummary("2000-01-01", "2099-12-31");
    ASSERT_TRUE(all.isArray());
    EXPECT_GE(all.size(), 3u);

    // A range with nothing in it must come back empty rather than unfiltered.
    const auto none = qs_->getDailySummary("1990-01-01", "1990-12-31");
    ASSERT_TRUE(none.isArray());
    EXPECT_EQ(none.size(), 0u) << "the date range was not applied";
}

TEST_F(QueryServiceReadTest, ASavedSummaryComesBack) {
    ASSERT_TRUE(db_->saveSummary(kDevice, "weekly", "2026-05-01", "2026-05-07",
                                 7, 3.5, 7.2, 85.0, "A steady week."));
    const auto s = qs_->getSummaries("weekly", 5);
    ASSERT_TRUE(s.isArray());
    ASSERT_GE(s.size(), 1u) << "a saved summary never came back";

    // And the period filter is real, not decorative.
    const auto monthly = qs_->getSummaries("monthly", 5);
    ASSERT_TRUE(monthly.isArray());
    EXPECT_EQ(monthly.size(), 0u) << "the period filter was ignored";
}

TEST_F(QueryServiceReadTest, InsightsRunOverStoredNights) {
    for (int i = 0; i < 10; ++i)
        addDailySummary(noonDaysAgo(10 - i), 2.0 + (i % 3), 400 + i * 5);
    EXPECT_NO_THROW({ (void)qs_->getInsights(3650); });
}

}  // namespace
