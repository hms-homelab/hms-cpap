// Issue #78: what the AI summaries are told about a night's events.
//
// The reporter's AirCurve 11 ASV night had 3 hypopneas and 3 unclassified
// apneas. The prompt said "Total events: 6 (obstructive=0, central=0,
// hypopnea=3, RERA=0)" and Gemini called all six hypopneas. Every type the
// AHI counts is now named, and what is outside the AHI is its own figure, so
// the parts the model is given add up to the total it is given.
#include <gtest/gtest.h>

#include "services/BurstCollectorService.h"

#include <string>
#include <vector>

using namespace hms_cpap;

namespace {

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

SessionMetrics reportersNight() {
    SessionMetrics m;
    m.index_kind = SessionMetrics::IndexKind::AHI;
    m.usage_hours = 6.0;
    m.hypopneas = 3;
    m.unclassified_apneas = 3;
    m.total_events = 6;
    m.ahi = 1.0;
    return m;
}

}  // namespace

TEST(AiSummaryEvents, TheReportersNightNamesTheUnclassifiedApneas) {
    BurstCollectorService svc(60);
    const auto out = svc.buildMetricsStringForTest(reportersNight());
    EXPECT_TRUE(has(out, "Apneas and hypopneas (the events counted in the AHI): 6 ("))
        << out;
    EXPECT_TRUE(has(out, "unclassified apnea=3")) << out;
    EXPECT_TRUE(has(out, "hypopnea=3)")) << out;
    EXPECT_TRUE(has(out, "Other recorded events (not counted in the AHI): 0 (RERA=0, "
                         "other event types=0)"))
        << out;
    EXPECT_TRUE(has(out, "Total events recorded: 6")) << out;
    EXPECT_FALSE(has(out, "Total events: 6 (obstructive=0")) << out;
}

TEST(AiSummaryEvents, TheModelIsToldNotToReclassify) {
    BurstCollectorService svc(60);
    const auto out = svc.buildMetricsStringForTest(reportersNight());
    EXPECT_TRUE(has(out, "never describe it as obstructive, central or a hypopnea")) << out;
}

TEST(AiSummaryEvents, ClearAirwayAndOtherEventsMakeTheTotalAddUp) {
    // 1 OA + 2 clear airway + 3 unclassified + 3 H = 9 in the AHI; 1 RERA and
    // 2 events of other types (CSR, flow limitation, ...) outside it; 12 in all.
    BurstCollectorService svc(60);
    SessionMetrics m = reportersNight();
    m.obstructive_apneas = 1;
    m.clear_airway_apneas = 2;
    m.reras = 1;
    m.total_events = 12;
    const auto out = svc.buildMetricsStringForTest(m);
    EXPECT_TRUE(has(out, "counted in the AHI): 9 (obstructive apnea=1, central apnea=0, "
                         "clear airway apnea=2, unclassified apnea=3, hypopnea=3)"))
        << out;
    EXPECT_TRUE(has(out, "not counted in the AHI): 3 (RERA=1, other event types=2)")) << out;
    EXPECT_TRUE(has(out, "Total events recorded: 12")) << out;
}

TEST(AiSummaryEvents, AMachineThatDoesNotClassifyStillGetsNoBreakdown) {
    // SDD-024: on a machine with no per-type data every type is 0; naming
    // them would tell the model "no hypopneas were recorded".
    BurstCollectorService svc(60);
    SessionMetrics m;
    m.index_kind = SessionMetrics::IndexKind::Ungraded;
    m.total_events = 5;
    const auto out = svc.buildMetricsStringForTest(m);
    EXPECT_TRUE(has(out, "Total events: 5 (all apneas")) << out;
    EXPECT_FALSE(has(out, "unclassified apnea=")) << out;
    EXPECT_FALSE(has(out, "counted in the AHI")) << out;
}

TEST(AiSummaryEvents, TheWeeklyLineCarriesEveryTypeAndAddsUp) {
    BurstCollectorService svc(60);
    SessionMetrics n = reportersNight();
    n.sleep_day = "2026-11-10";
    n.clear_airway_apneas = 2;
    n.total_events = 9;   // 3 H + 3 UA + 2 CLR in the AHI, 1 other outside it
    const auto out =
        svc.buildRangeMetricsStringForTest({n}, SummaryPeriod::WEEKLY);
    EXPECT_TRUE(has(out, "events 9 (in AHI 8: OA=0 CA=0 CLR=2 UA=3 H=3; "
                         "not in AHI 1: RERA=0 other=1)"))
        << out;
    EXPECT_TRUE(has(out, "UA unclassified apnea")) << out;
    EXPECT_TRUE(has(out, "never describe it as obstructive, central or a hypopnea")) << out;
}
