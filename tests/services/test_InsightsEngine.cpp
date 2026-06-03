// test_InsightsEngine.cpp — unit tests for InsightsEngine analytics/logic.
//
// InsightsEngine is a pure, static analytics class over STRDailyRecord
// vectors. No I/O, DB, network, or wall-clock dependence. We build input
// records deterministically (record_date derived from a fixed epoch offset
// so date ordering is stable and reproducible) and assert the computed
// insights' categories, metrics, titles, values, and JSON round-trip.

#include <gtest/gtest.h>
#include "services/InsightsEngine.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

using namespace hms_cpap;

namespace {

// Deterministic day epoch: a fixed UTC instant. dayIndex spaces records one
// day apart so sorting by record_date is well-defined and reproducible.
std::chrono::system_clock::time_point dayAt(int dayIndex) {
    // 2024-01-01T12:00:00Z == 1704110400 seconds since epoch.
    constexpr long kBase = 1704110400L;
    return std::chrono::system_clock::time_point(
        std::chrono::seconds(kBase + static_cast<long>(dayIndex) * 86400L));
}

STRDailyRecord makeRecord(int dayIndex, double ahi, double durationMinutes,
                          double leak95, double maskPress50) {
    STRDailyRecord r;
    r.device_id = "dev1";
    r.record_date = dayAt(dayIndex);
    r.ahi = ahi;
    r.duration_minutes = durationMinutes;
    r.leak_95 = leak95;
    r.mask_press_50 = maskPress50;
    return r;
}

// Find first insight whose metric+title matches a predicate; nullptr if none.
const Insight* findByTitlePrefix(const std::vector<Insight>& v,
                                 const std::string& prefix) {
    for (const auto& i : v) {
        if (i.title.rfind(prefix, 0) == 0) return &i;
    }
    return nullptr;
}

const Insight* findByMetric(const std::vector<Insight>& v,
                            const std::string& metric) {
    for (const auto& i : v) {
        if (i.metric == metric) return &i;
    }
    return nullptr;
}

}  // namespace

// ── Guard: insufficient data ────────────────────────────────────────────────

TEST(InsightsEngineTest, FewerThanSevenTherapyDaysReturnsInfoStub) {
    std::vector<STRDailyRecord> recs;
    for (int i = 0; i < 6; ++i) {
        recs.push_back(makeRecord(i, 3.0, 7 * 60.0, 15.0, 9.0));
    }
    auto insights = InsightsEngine::analyze(recs);
    ASSERT_EQ(insights.size(), 1u);
    EXPECT_EQ(insights[0].title, "Not enough data");
    EXPECT_EQ(insights[0].category, "info");
    EXPECT_EQ(insights[0].value, 0);
}

TEST(InsightsEngineTest, NonTherapyDaysAreFilteredOut) {
    std::vector<STRDailyRecord> recs;
    // 6 real therapy days + 5 zero-duration (non-therapy) days.
    for (int i = 0; i < 6; ++i) {
        recs.push_back(makeRecord(i, 3.0, 7 * 60.0, 15.0, 9.0));
    }
    for (int i = 6; i < 11; ++i) {
        recs.push_back(makeRecord(i, 99.0, 0.0, 99.0, 99.0));  // duration 0 -> no therapy
    }
    auto insights = InsightsEngine::analyze(recs);
    // Only 6 therapy days remain -> below threshold of 7.
    ASSERT_EQ(insights.size(), 1u);
    EXPECT_EQ(insights[0].title, "Not enough data");
}

// ── analyze() with exactly 7 days (single-period path) ──────────────────────

TEST(InsightsEngineTest, SevenNormalDaysProducesExpectedInsightSet) {
    std::vector<STRDailyRecord> recs;
    // 7 days, AHI=3 (normal), 7h, leak 15, pressure 9.
    for (int i = 0; i < 7; ++i) {
        recs.push_back(makeRecord(i, 3.0, 7 * 60.0, 15.0, 9.0));
    }
    auto insights = InsightsEngine::analyze(recs);

    // With exactly 7 days: ahiTrend (single-period), therapyCompliance,
    // bestWorstNights, recentSummary fire. leakCorrelation needs >=14,
    // pressureTrend needs a prior period -> neither fires.
    EXPECT_EQ(findByMetric(insights, "Leak"), nullptr);

    // AHI single-period insight: "Average AHI: ..." normal -> positive.
    const Insight* ahi = findByTitlePrefix(insights, "Average AHI");
    ASSERT_NE(ahi, nullptr);
    EXPECT_EQ(ahi->category, "positive");
    EXPECT_EQ(ahi->metric, "AHI");
    EXPECT_DOUBLE_EQ(ahi->value, 3.0);
    EXPECT_NE(ahi->body.find("Normal range"), std::string::npos);

    // Compliance: 7h >= 4h -> good.
    const Insight* hrs = findByMetric(insights, "Hours");
    ASSERT_NE(hrs, nullptr);
    EXPECT_EQ(hrs->title, "Good therapy duration");
    EXPECT_EQ(hrs->category, "positive");
    EXPECT_DOUBLE_EQ(hrs->value, 7.0);
    EXPECT_NE(hrs->body.find("100%"), std::string::npos);

    // Best vs worst: all identical -> diff 0.
    const Insight* bw = findByTitlePrefix(insights, "Best vs worst");
    ASSERT_NE(bw, nullptr);
    EXPECT_EQ(bw->category, "info");
    EXPECT_DOUBLE_EQ(bw->value, 0.0);

    // Recent summary over last 7.
    const Insight* sum = findByMetric(insights, "Summary");
    ASSERT_NE(sum, nullptr);
    EXPECT_EQ(sum->title, "Last 7 nights");
    EXPECT_DOUBLE_EQ(sum->value, 3.0);
}

// ── ahiTrend single-period category boundaries ──────────────────────────────

TEST(InsightsEngineTest, AhiSinglePeriodModerateRangeIsWarning) {
    std::vector<STRDailyRecord> recs;
    for (int i = 0; i < 7; ++i) recs.push_back(makeRecord(i, 10.0, 7 * 60.0, 15.0, 9.0));
    auto insights = InsightsEngine::analyze(recs);
    const Insight* ahi = findByTitlePrefix(insights, "Average AHI");
    ASSERT_NE(ahi, nullptr);
    EXPECT_EQ(ahi->category, "warning");  // 5 <= 10 <= 15
    EXPECT_NE(ahi->body.find("Moderate range"), std::string::npos);
}

TEST(InsightsEngineTest, AhiSinglePeriodElevatedRangeIsAlert) {
    std::vector<STRDailyRecord> recs;
    for (int i = 0; i < 7; ++i) recs.push_back(makeRecord(i, 25.0, 7 * 60.0, 15.0, 9.0));
    auto insights = InsightsEngine::analyze(recs);
    const Insight* ahi = findByTitlePrefix(insights, "Average AHI");
    ASSERT_NE(ahi, nullptr);
    EXPECT_EQ(ahi->category, "alert");  // > 15
    EXPECT_NE(ahi->body.find("Elevated"), std::string::npos);
}

// ── ahiTrend with prior period: improved / worsened / stable ────────────────

TEST(InsightsEngineTest, AhiImprovedTrendIsPositive) {
    std::vector<STRDailyRecord> recs;
    // 10 prior days high AHI (~12), then 30 recent days low AHI (~3).
    for (int i = 0; i < 10; ++i) recs.push_back(makeRecord(i, 12.0, 7 * 60.0, 15.0, 9.0));
    for (int i = 10; i < 40; ++i) recs.push_back(makeRecord(i, 3.0, 7 * 60.0, 15.0, 9.0));
    auto insights = InsightsEngine::analyze(recs);

    const Insight* ahi = findByMetric(insights, "AHI");
    ASSERT_NE(ahi, nullptr);
    EXPECT_EQ(ahi->title, "AHI trend: improved");
    EXPECT_EQ(ahi->category, "positive");
    EXPECT_DOUBLE_EQ(ahi->value, 3.0);  // last-30 avg
    EXPECT_NE(ahi->body.find("right direction"), std::string::npos);
    EXPECT_NE(ahi->body.find("down"), std::string::npos);
}

TEST(InsightsEngineTest, AhiWorsenedTrendIsWarning) {
    std::vector<STRDailyRecord> recs;
    for (int i = 0; i < 10; ++i) recs.push_back(makeRecord(i, 3.0, 7 * 60.0, 15.0, 9.0));
    for (int i = 10; i < 40; ++i) recs.push_back(makeRecord(i, 12.0, 7 * 60.0, 15.0, 9.0));
    auto insights = InsightsEngine::analyze(recs);

    const Insight* ahi = findByMetric(insights, "AHI");
    ASSERT_NE(ahi, nullptr);
    EXPECT_EQ(ahi->title, "AHI trend: worsened");
    EXPECT_EQ(ahi->category, "warning");
    EXPECT_DOUBLE_EQ(ahi->value, 12.0);
    EXPECT_NE(ahi->body.find("mask fit"), std::string::npos);
    EXPECT_NE(ahi->body.find("up"), std::string::npos);
}

TEST(InsightsEngineTest, AhiStableWhenChangeSmall) {
    std::vector<STRDailyRecord> recs;
    // Prior avg 5.0, recent avg 5.2 -> |change| = 0.2 <= 0.5 -> stable.
    for (int i = 0; i < 10; ++i) recs.push_back(makeRecord(i, 5.0, 7 * 60.0, 15.0, 9.0));
    for (int i = 10; i < 40; ++i) recs.push_back(makeRecord(i, 5.2, 7 * 60.0, 15.0, 9.0));
    auto insights = InsightsEngine::analyze(recs);

    const Insight* ahi = findByMetric(insights, "AHI");
    ASSERT_NE(ahi, nullptr);
    EXPECT_EQ(ahi->title, "AHI stable");
    EXPECT_EQ(ahi->category, "positive");
    EXPECT_DOUBLE_EQ(ahi->value, 5.2);
    EXPECT_NE(ahi->body.find("Consistent therapy"), std::string::npos);
}

// ── leakCorrelation: leak affects AHI vs not ────────────────────────────────

TEST(InsightsEngineTest, LeakCorrelationFlagsHighLeakImpact) {
    std::vector<STRDailyRecord> recs;
    // 20 days. Low-leak (10) nights have AHI 2; high-leak (40) nights AHI 8.
    // Median leak will sit between, splitting cleanly into the two groups.
    for (int i = 0; i < 10; ++i) recs.push_back(makeRecord(i, 2.0, 7 * 60.0, 10.0, 9.0));
    for (int i = 10; i < 20; ++i) recs.push_back(makeRecord(i, 8.0, 7 * 60.0, 40.0, 9.0));
    auto insights = InsightsEngine::analyze(recs);

    const Insight* leak = findByMetric(insights, "Leak");
    ASSERT_NE(leak, nullptr);
    EXPECT_EQ(leak->title, "Mask leak is affecting your AHI");
    EXPECT_EQ(leak->category, "actionable");
    // median of sorted leaks (ten 10s then ten 40s); index 20/2=10 -> 40.
    EXPECT_DOUBLE_EQ(leak->value, 40.0);
    EXPECT_NE(leak->body.find("Improving mask seal"), std::string::npos);
}

TEST(InsightsEngineTest, LeakCorrelationReportsAdequateSeal) {
    std::vector<STRDailyRecord> recs;
    // 14 days, AHI flat regardless of leak -> diff ~0 -> positive.
    for (int i = 0; i < 7; ++i) recs.push_back(makeRecord(i, 4.0, 7 * 60.0, 10.0, 9.0));
    for (int i = 7; i < 14; ++i) recs.push_back(makeRecord(i, 4.0, 7 * 60.0, 40.0, 9.0));
    auto insights = InsightsEngine::analyze(recs);

    const Insight* leak = findByMetric(insights, "Leak");
    ASSERT_NE(leak, nullptr);
    EXPECT_EQ(leak->title, "Leak is not a major factor");
    EXPECT_EQ(leak->category, "positive");
    EXPECT_NE(leak->body.find("mask seal is adequate"), std::string::npos);
}

// ── pressureTrend ───────────────────────────────────────────────────────────

TEST(InsightsEngineTest, PressureDecreasingIsPositive) {
    std::vector<STRDailyRecord> recs;
    // Prior pressure 12, recent 9 -> change -3 -> "decreasing" positive.
    for (int i = 0; i < 10; ++i) recs.push_back(makeRecord(i, 4.0, 7 * 60.0, 15.0, 12.0));
    for (int i = 10; i < 40; ++i) recs.push_back(makeRecord(i, 4.0, 7 * 60.0, 15.0, 9.0));
    auto insights = InsightsEngine::analyze(recs);

    const Insight* p = findByMetric(insights, "Pressure");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->title, "Pressure is decreasing");
    EXPECT_EQ(p->category, "positive");
    EXPECT_DOUBLE_EQ(p->value, 9.0);
    EXPECT_NE(p->body.find("less pressure"), std::string::npos);
}

TEST(InsightsEngineTest, PressureIncreasingIsInfo) {
    std::vector<STRDailyRecord> recs;
    for (int i = 0; i < 10; ++i) recs.push_back(makeRecord(i, 4.0, 7 * 60.0, 15.0, 9.0));
    for (int i = 10; i < 40; ++i) recs.push_back(makeRecord(i, 4.0, 7 * 60.0, 15.0, 12.0));
    auto insights = InsightsEngine::analyze(recs);

    const Insight* p = findByMetric(insights, "Pressure");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->title, "Pressure is increasing");
    EXPECT_EQ(p->category, "info");
    EXPECT_DOUBLE_EQ(p->value, 12.0);
    EXPECT_NE(p->body.find("working harder"), std::string::npos);
}

TEST(InsightsEngineTest, PressureStableProducesNoPressureInsight) {
    std::vector<STRDailyRecord> recs;
    // Prior 9.0, recent 9.2 -> |change| 0.2 <= 0.5 -> no pressure insight.
    for (int i = 0; i < 10; ++i) recs.push_back(makeRecord(i, 4.0, 7 * 60.0, 15.0, 9.0));
    for (int i = 10; i < 40; ++i) recs.push_back(makeRecord(i, 4.0, 7 * 60.0, 15.0, 9.2));
    auto insights = InsightsEngine::analyze(recs);
    EXPECT_EQ(findByMetric(insights, "Pressure"), nullptr);
}

// ── therapyCompliance low-hours branch ──────────────────────────────────────

TEST(InsightsEngineTest, LowTherapyHoursIsWarning) {
    std::vector<STRDailyRecord> recs;
    // 8 days at 3h (< 4h) -> low therapy, 0% met threshold.
    for (int i = 0; i < 8; ++i) recs.push_back(makeRecord(i, 4.0, 3 * 60.0, 15.0, 9.0));
    auto insights = InsightsEngine::analyze(recs);

    const Insight* hrs = findByMetric(insights, "Hours");
    ASSERT_NE(hrs, nullptr);
    EXPECT_EQ(hrs->title, "Low therapy hours");
    EXPECT_EQ(hrs->category, "warning");
    EXPECT_DOUBLE_EQ(hrs->value, 3.0);
    EXPECT_NE(hrs->body.find("0%"), std::string::npos);
}

// ── bestWorstNights diff math ───────────────────────────────────────────────

TEST(InsightsEngineTest, BestWorstNightsComputesAhiSpread) {
    std::vector<STRDailyRecord> recs;
    // 7 days with distinct AHIs; best=1, worst=20 -> diff 19.
    double ahis[] = {5, 1, 12, 8, 20, 3, 7};
    for (int i = 0; i < 7; ++i) recs.push_back(makeRecord(i, ahis[i], 7 * 60.0, 15.0, 9.0));
    auto insights = InsightsEngine::analyze(recs);

    const Insight* bw = findByTitlePrefix(insights, "Best vs worst");
    ASSERT_NE(bw, nullptr);
    EXPECT_EQ(bw->category, "info");
    EXPECT_DOUBLE_EQ(bw->value, 19.0);  // 20 - 1
    EXPECT_NE(bw->body.find("Best:"), std::string::npos);
    EXPECT_NE(bw->body.find("Worst:"), std::string::npos);
}

// ── recentSummary averages last 7 ───────────────────────────────────────────

TEST(InsightsEngineTest, RecentSummaryAveragesLastSevenNights) {
    std::vector<STRDailyRecord> recs;
    // 10 therapy days; last 7 should drive the summary. Make the last 7 have
    // AHI 6 and the first 3 AHI 100 to prove only last 7 are used.
    for (int i = 0; i < 3; ++i) recs.push_back(makeRecord(i, 100.0, 7 * 60.0, 15.0, 9.0));
    for (int i = 3; i < 10; ++i) recs.push_back(makeRecord(i, 6.0, 7 * 60.0, 15.0, 9.0));
    auto insights = InsightsEngine::analyze(recs);

    const Insight* sum = findByMetric(insights, "Summary");
    ASSERT_NE(sum, nullptr);
    EXPECT_EQ(sum->title, "Last 7 nights");
    EXPECT_DOUBLE_EQ(sum->value, 6.0);
}

// ── ordering independence: shuffled input is sorted by date internally ──────

TEST(InsightsEngineTest, UnsortedInputIsSortedByDateBeforeAnalysis) {
    std::vector<STRDailyRecord> ordered;
    for (int i = 0; i < 10; ++i) ordered.push_back(makeRecord(i, 3.0, 7 * 60.0, 15.0, 9.0));
    for (int i = 10; i < 40; ++i) ordered.push_back(makeRecord(i, 12.0, 7 * 60.0, 15.0, 9.0));

    auto shuffled = ordered;
    std::reverse(shuffled.begin(), shuffled.end());

    auto a = InsightsEngine::analyze(ordered);
    auto b = InsightsEngine::analyze(shuffled);

    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].title, b[i].title);
        EXPECT_EQ(a[i].body, b[i].body);
        EXPECT_DOUBLE_EQ(a[i].value, b[i].value);
    }
}

// ── toJson round-trip ───────────────────────────────────────────────────────

TEST(InsightsEngineTest, ToJsonSerializesAllFields) {
    std::vector<Insight> insights = {
        {"Title A", "Body A", "positive", "AHI", 3.5},
        {"Title B", "Body B", "warning", "Leak", 42.0},
    };
    Json::Value arr = InsightsEngine::toJson(insights);

    ASSERT_TRUE(arr.isArray());
    ASSERT_EQ(arr.size(), 2u);

    EXPECT_EQ(arr[0]["title"].asString(), "Title A");
    EXPECT_EQ(arr[0]["body"].asString(), "Body A");
    EXPECT_EQ(arr[0]["category"].asString(), "positive");
    EXPECT_EQ(arr[0]["metric"].asString(), "AHI");
    EXPECT_DOUBLE_EQ(arr[0]["value"].asDouble(), 3.5);

    EXPECT_EQ(arr[1]["title"].asString(), "Title B");
    EXPECT_EQ(arr[1]["category"].asString(), "warning");
    EXPECT_EQ(arr[1]["metric"].asString(), "Leak");
    EXPECT_DOUBLE_EQ(arr[1]["value"].asDouble(), 42.0);
}

TEST(InsightsEngineTest, ToJsonEmptyYieldsEmptyArray) {
    Json::Value arr = InsightsEngine::toJson({});
    ASSERT_TRUE(arr.isArray());
    EXPECT_EQ(arr.size(), 0u);
}

// ── analyze() -> toJson end-to-end stays consistent ─────────────────────────

TEST(InsightsEngineTest, AnalyzeThenToJsonMatchesInsightCount) {
    std::vector<STRDailyRecord> recs;
    for (int i = 0; i < 10; ++i) recs.push_back(makeRecord(i, 3.0, 7 * 60.0, 15.0, 9.0));
    for (int i = 10; i < 40; ++i) recs.push_back(makeRecord(i, 12.0, 7 * 60.0, 25.0, 11.0));

    auto insights = InsightsEngine::analyze(recs);
    Json::Value arr = InsightsEngine::toJson(insights);

    ASSERT_TRUE(arr.isArray());
    EXPECT_EQ(arr.size(), insights.size());
    EXPECT_GT(insights.size(), 1u);  // produced real insights, not the stub
    for (Json::ArrayIndex i = 0; i < arr.size(); ++i) {
        EXPECT_FALSE(arr[i]["title"].asString().empty());
        EXPECT_FALSE(arr[i]["category"].asString().empty());
    }
}
