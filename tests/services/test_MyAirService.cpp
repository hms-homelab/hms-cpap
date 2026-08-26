/**
 * SDD-020: storing myAir nights, and the comparison read that puts them beside
 * ours.
 *
 * The fetch is injected, so everything below the network is exercised here with
 * no myAir account and no internet. That matters more than usual, because the
 * one thing these tests cannot cover is the real service, which is exactly why
 * everything else has to be covered.
 */

#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "services/MyAirService.h"
#include "utils/AppConfig.h"
#include "web/QueryService.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

constexpr const char* kDevice = "myair-test";

MyAirSleepRecord night(const std::string& date, double usage_min, int score,
                       double ahi, double leak) {
    MyAirSleepRecord r;
    r.start_date = date;
    r.total_usage_min = usage_min;
    r.sleep_score = score;
    r.usage_score = score / 2;
    r.ahi_score = 5;
    r.mask_score = 16;
    r.leak_score = 20;
    r.ahi = ahi;
    r.mask_pair_count = 2;
    r.leak_percentile = leak;
    return r;
}

/// A night ResMed has no data for: everything zero, which is what the real
/// account returns for most dates.
MyAirSleepRecord emptyNight(const std::string& date) {
    MyAirSleepRecord r;
    r.start_date = date;
    return r;
}

double asNumber(const Json::Value& v) {
    if (v.isNull()) return -999;
    if (v.isString()) return std::stod(v.asString());
    return v.asDouble();
}

class MyAirServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        static int counter = 0;
        path_ = (fs::temp_directory_path() /
                 ("hms_myair_" + std::to_string(::getpid()) + "_" +
                  std::to_string(counter++) + ".db")).string();
        std::error_code ec;
        fs::remove(path_, ec);

        db_ = std::make_shared<SQLiteDatabase>(path_);
        ASSERT_TRUE(db_->connect());

        config_.myair.enabled = true;
        config_.myair.username = "someone@example.com";
        config_.myair.password = "secret";
        config_.myair.region = "NA";

        service_ = std::make_unique<MyAirService>(db_, config_);
    }

    void TearDown() override {
        service_.reset();
        db_.reset();
        for (const auto* suffix : {"", "-wal", "-shm"}) {
            std::error_code ec;
            fs::remove(path_ + suffix, ec);
        }
    }

    Json::Value storedRows() {
        return db_->executeQuery(
            "SELECT record_date, total_usage_min, sleep_score, ahi, leak_percentile,"
            " has_data FROM cpap_myair_records ORDER BY record_date");
    }

    /// A night as OUR parser recorded it, so the comparison has both sides.
    void addOurNight(const std::string& ymd, double minutes, double ahi, double leak) {
        db_->executeQuery(
            "INSERT INTO cpap_daily_summary (device_id, record_date, duration_minutes,"
            " patient_hours, ahi, leak_95, mask_events) VALUES (?, ?, ?, ?, ?, ?, 2)",
            {kDevice, ymd, std::to_string(minutes), std::to_string(minutes / 60.0),
             std::to_string(ahi), std::to_string(leak)});
    }

    std::string path_;
    AppConfig config_;
    std::shared_ptr<SQLiteDatabase> db_;
    std::unique_ptr<MyAirService> service_;
};

}  // namespace

TEST_F(MyAirServiceTest, DoesNothingWhenNotConfigured) {
    config_.myair.enabled = false;
    EXPECT_FALSE(service_->enabled());

    std::string err;
    EXPECT_EQ(service_->syncNow(err), -1);
    EXPECT_FALSE(err.empty());

    // And a sweep is silent rather than an error path.
    EXPECT_NO_THROW(service_->sweep());
}

TEST_F(MyAirServiceTest, HalfACredentialIsNotConfigured) {
    // A toggle switched on with an empty password is not a working integration
    // and must not behave like one.
    config_.myair.password.clear();
    EXPECT_FALSE(service_->enabled());
}

TEST_F(MyAirServiceTest, StoresNightsAndMarksTheOnesResMedHasNoDataFor) {
    std::string err;
    const int stored = service_->store(
        {night("2026-08-24", 432, 91, 1.4, 18.5), emptyNight("2026-08-25"),
         night("2026-08-26", 80, 38, 0.0, 1.2)},
        err);
    ASSERT_EQ(stored, 3) << err;

    const auto rows = storedRows();
    ASSERT_EQ(rows.size(), 3u);

    EXPECT_DOUBLE_EQ(asNumber(rows[0]["total_usage_min"]), 432.0);
    EXPECT_DOUBLE_EQ(asNumber(rows[0]["has_data"]), 1);

    // The middle night is the one that matters: ResMed returned nothing for it,
    // which is not the same as a night with no therapy, and storing it as an
    // ordinary zero would invent a bad night.
    EXPECT_DOUBLE_EQ(asNumber(rows[1]["has_data"]), 0)
        << "an all-zero night from ResMed must be marked as absent data";
    EXPECT_DOUBLE_EQ(asNumber(rows[2]["has_data"]), 1);
}

TEST_F(MyAirServiceTest, RefetchingTheSameWindowReplacesRatherThanDuplicates) {
    std::string err;
    ASSERT_GE(service_->store({night("2026-08-24", 432, 91, 1.4, 18.5)}, err), 0) << err;
    ASSERT_EQ(storedRows().size(), 1u);

    // The same night, rescored by ResMed after reprocessing. The whole window is
    // refetched every poll, so this has to land as an update, not a second row.
    ASSERT_GE(service_->store({night("2026-08-24", 440, 93, 1.1, 17.0)}, err), 0) << err;

    const auto rows = storedRows();
    ASSERT_EQ(rows.size(), 1u) << "the refetch duplicated the night";
    EXPECT_DOUBLE_EQ(asNumber(rows[0]["total_usage_min"]), 440.0) << "the newer score did not win";
    EXPECT_DOUBLE_EQ(asNumber(rows[0]["sleep_score"]), 93);
}

TEST_F(MyAirServiceTest, ReplacingAWindowLeavesNightsOutsideItAlone) {
    std::string err;
    ASSERT_GE(service_->store({night("2026-07-01", 400, 80, 1.0, 10.0)}, err), 0);
    ASSERT_GE(service_->store({night("2026-08-24", 432, 91, 1.4, 18.5)}, err), 0);
    ASSERT_EQ(storedRows().size(), 2u);

    // A poll covering only August must not delete July.
    ASSERT_GE(service_->store({night("2026-08-24", 440, 93, 1.1, 17.0)}, err), 0);
    EXPECT_EQ(storedRows().size(), 2u) << "the window delete reached outside its own dates";
}

TEST_F(MyAirServiceTest, AnEmptyFetchStoresNothingAndIsNotAnError) {
    std::string err;
    EXPECT_EQ(service_->store({}, err), 0);
    EXPECT_TRUE(storedRows().empty());
}

TEST_F(MyAirServiceTest, SyncNowUsesTheInjectedFetch) {
    service_->setFetch([](std::vector<MyAirSleepRecord>& out, std::string&) {
        out.push_back(night("2026-08-24", 432, 91, 1.4, 18.5));
        return true;
    });
    std::string err;
    EXPECT_EQ(service_->syncNow(err), 1) << err;
    EXPECT_EQ(storedRows().size(), 1u);
}

TEST_F(MyAirServiceTest, AFailedFetchChangesNothing) {
    std::string err;
    ASSERT_GE(service_->store({night("2026-08-24", 432, 91, 1.4, 18.5)}, err), 0);

    service_->setFetch([](std::vector<MyAirSleepRecord>&, std::string& e) {
        e = "myAir is having a day";
        return false;
    });
    EXPECT_EQ(service_->syncNow(err), -1);
    EXPECT_EQ(err, "myAir is having a day");
    EXPECT_EQ(service_->lastError(), "myAir is having a day");

    // The stored night survives a failed poll untouched. A transient outage must
    // not empty the comparison.
    EXPECT_EQ(storedRows().size(), 1u);
}

TEST_F(MyAirServiceTest, SweepPollsOnceAndThenWaits) {
    int calls = 0;
    service_->setFetch([&calls](std::vector<MyAirSleepRecord>& out, std::string&) {
        ++calls;
        out.push_back(night("2026-08-24", 432, 91, 1.4, 18.5));
        return true;
    });
    config_.myair.poll_minutes = 60;

    service_->sweep();
    service_->sweep();
    service_->sweep();
    EXPECT_EQ(calls, 1) << "the burst loop runs constantly; myAir must not be polled every time";
}

// ─────────────────────────────────────────────────────────────────────────────
// The comparison
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(MyAirServiceTest, ComparisonPutsBothSidesOnOneRowWithDeltas) {
    // The real shape from Albin's own account on 2026-08-24: ours said 100
    // minutes / AHI 1.8 / leak 32.4, myAir said 116 / 1.5 / 31.2.
    addOurNight("2026-08-24", 100, 1.8, 32.4);
    std::string err;
    ASSERT_GE(service_->store({night("2026-08-24", 116, 41, 1.5, 31.2)}, err), 0) << err;

    QueryService qs(db_, kDevice);
    const auto rows = qs.getMyAirComparison("2000-01-01", "2099-12-31");
    ASSERT_EQ(rows.size(), 1u);

    EXPECT_TRUE(rows[0]["myair_present"].asBool());
    EXPECT_DOUBLE_EQ(asNumber(rows[0]["duration_minutes"]), 100.0);
    EXPECT_DOUBLE_EQ(asNumber(rows[0]["total_usage_min"]), 116.0);

    // Ours minus theirs, so a positive number always means we report more.
    EXPECT_NEAR(asNumber(rows[0]["usage_delta_min"]), -16.0, 0.001);
    EXPECT_NEAR(asNumber(rows[0]["ahi_delta"]), 0.3, 0.001);
    EXPECT_NEAR(asNumber(rows[0]["leak_delta"]), 1.2, 0.001);
}

TEST_F(MyAirServiceTest, ANightWeHaveAndMyAirDoesNotStillAppears) {
    // The single most useful thing this view can show: the card holds a night
    // ResMed never received. An inner join would hide exactly this.
    addOurNight("2026-08-24", 100, 1.8, 32.4);

    QueryService qs(db_, kDevice);
    const auto rows = qs.getMyAirComparison("2000-01-01", "2099-12-31");
    ASSERT_EQ(rows.size(), 1u) << "our night vanished because myAir had no row for it";
    EXPECT_FALSE(rows[0]["myair_present"].asBool());
    EXPECT_TRUE(rows[0]["usage_delta_min"].isNull()) << "a delta against nothing is not zero";
}

TEST_F(MyAirServiceTest, ANightResMedHasNoDataForIsNotComparedAgainstZero) {
    addOurNight("2026-08-25", 400, 1.0, 20.0);
    std::string err;
    ASSERT_GE(service_->store({emptyNight("2026-08-25")}, err), 0) << err;

    QueryService qs(db_, kDevice);
    const auto rows = qs.getMyAirComparison("2000-01-01", "2099-12-31");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0]["myair_present"].asBool())
        << "an all-zero myAir night means they have no data, not that usage was zero";
    EXPECT_TRUE(rows[0]["usage_delta_min"].isNull())
        << "comparing 400 minutes against 'no data' would report a 400 minute discrepancy";
}
