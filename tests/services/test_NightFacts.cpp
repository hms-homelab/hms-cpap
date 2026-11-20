// SDD-072 D1/D2: one NightFacts, three renderings that carry the same numbers.
//
// The daily text, the weekly/monthly text and the agent's tool result used to
// be built by hand in three places and drifted (issue #78). They are now
// renderings of one structure, so a night reads the same in each; the rules
// ride inside them, so they reach a user's own LLM_PROMPT_FILE too.
#include <gtest/gtest.h>

#include "services/NightFacts.h"

#include <string>
#include <vector>

using namespace hms_cpap;

namespace {

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

SessionMetrics aNight() {
    SessionMetrics m;
    m.sleep_day = "2026-11-10";
    m.index_kind = SessionMetrics::IndexKind::AHI;
    m.ahi = 1.234;
    m.usage_hours = 6.2549;
    m.usage_percent = 78.19;
    m.obstructive_apneas = 1;
    m.clear_airway_apneas = 2;
    m.unclassified_apneas = 3;
    m.hypopneas = 2;
    m.reras = 1;
    m.total_events = 11;   // 8 in the AHI, 1 RERA, 2 of other types
    m.avg_leak_rate = 12.3456;
    m.avg_pressure = 9.876;
    return m;
}

}  // namespace

TEST(NightFacts, TheDailyRangeAndAgentRenderingsCarryTheSameNumbers) {
    const auto f = NightFacts::from(aNight());
    const auto daily = night_facts::renderNight(f);
    const auto range = night_facts::renderRange({f}, "Weekly");
    const auto j = night_facts::toJson(f);

    // The AHI.
    EXPECT_TRUE(has(daily, "AHI: 1.23 events/hour")) << daily;
    EXPECT_TRUE(has(range, "| AHI 1.23 |")) << range;
    EXPECT_DOUBLE_EQ(j["ahi"].get<double>(), 1.23);
    // The events behind it, and the rest.
    EXPECT_TRUE(has(daily, "counted in the AHI): 8 (obstructive apnea=1, central apnea=0, "
                           "clear airway apnea=2, unclassified apnea=3, hypopnea=2)"))
        << daily;
    EXPECT_TRUE(has(range, "events 11 (in AHI 8: OA=1 CA=0 CLR=2 UA=3 H=2; not in AHI 3: "
                           "RERA=1 other=2)"))
        << range;
    EXPECT_EQ(j["events_in_ahi"]["total"], 8);
    EXPECT_EQ(j["events_in_ahi"]["unclassified_apnea"], 3);
    EXPECT_EQ(j["events_not_in_ahi"]["total"], 3);
    EXPECT_EQ(j["events_not_in_ahi"]["other_event_types"], 2);
    EXPECT_EQ(j["total_events"], 11);
    // Usage and leak, at the places the text shows.
    EXPECT_TRUE(has(daily, "Usage: 6.25 hours")) << daily;
    EXPECT_TRUE(has(range, "| 6.25h |")) << range;
    EXPECT_DOUBLE_EQ(j["usage_hours"].get<double>(), 6.25);
    EXPECT_TRUE(has(daily, "Leak: avg=12.35 L/min")) << daily;
    EXPECT_TRUE(has(range, "leak avg 12.35 L/min")) << range;
    EXPECT_DOUBLE_EQ(j["leak_avg_l_min"].get<double>(), 12.35);
}

TEST(NightFacts, ThePartsEqualTheTotal) {
    const auto f = NightFacts::from(aNight());
    EXPECT_EQ(f.inIndex() + f.outsideIndex(), f.total);
    EXPECT_EQ(f.other, 2);
}

TEST(NightFacts, AFigureTheNightDoesNotHaveIsAbsentNotZero) {
    SessionMetrics m = aNight();
    m.usage_hours.reset();
    m.usage_percent.reset();
    m.avg_leak_rate.reset();
    const auto f = NightFacts::from(m);
    const auto daily = night_facts::renderNight(f);
    EXPECT_TRUE(has(daily, "Usage: not recorded")) << daily;
    EXPECT_FALSE(has(daily, "0.00 hours")) << daily;
    EXPECT_FALSE(has(daily, "Leak:")) << daily;
    const auto range = night_facts::renderRange({f}, "Weekly");
    EXPECT_TRUE(has(range, "usage not recorded")) << range;
    EXPECT_FALSE(has(range, "Avg usage")) << "no night has a usage to average: " << range;
    const auto j = night_facts::toJson(f);
    EXPECT_FALSE(j.contains("usage_hours"));
    EXPECT_FALSE(j.contains("leak_avg_l_min"));
}

TEST(NightFacts, AnUngradedNightIsNeitherGradedNorAveragedIntoAnAhi) {
    SessionMetrics u;
    u.sleep_day = "2026-11-11";
    u.index_kind = SessionMetrics::IndexKind::Ungraded;
    u.ahi = 7.5;   // an apnea count per hour, not an AHI
    u.total_events = 30;
    u.usage_hours = 4.0;
    const auto graded = NightFacts::from(aNight());
    const auto ungraded = NightFacts::from(u);

    const auto daily = night_facts::renderNight(ungraded);
    EXPECT_FALSE(has(daily, "7.50")) << "the index is not handed over: " << daily;
    EXPECT_FALSE(has(daily, "never describe it as obstructive"))
        << "no per-type counts, so no per-type rule: " << daily;
    EXPECT_TRUE(has(daily, "do not report any event type as zero")) << daily;

    const auto range = night_facts::renderRange({graded, ungraded}, "Weekly");
    EXPECT_TRUE(has(range, "2026-11-11 | 4.00h | AHI not available | events 30")) << range;
    EXPECT_TRUE(has(range, "Avg AHI: 1.23 events/hour (over the 1 nights that have an AHI)"))
        << range;
    EXPECT_FALSE(has(range, "7.50")) << range;
    EXPECT_TRUE(has(range, "Worst AHI: 1.23 (2026-11-10)")) << range;

    const auto only = night_facts::renderRange({ungraded}, "Monthly");
    EXPECT_TRUE(has(only, "Avg AHI: not available")) << only;
    EXPECT_FALSE(has(only, "Best AHI")) << only;
}

TEST(NightFacts, TheRulesRideWithTheFacts) {
    const auto graded = NightFacts::from(aNight());
    const auto daily = night_facts::renderNight(graded);
    EXPECT_TRUE(has(daily, night_facts::kEventLabelRule)) << daily;
    EXPECT_TRUE(has(daily, night_facts::kNoGapRule)) << daily;
    EXPECT_FALSE(has(daily, night_facts::kUngradedRule)) << daily;

    const auto r = night_facts::rules({graded});
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0], night_facts::kEventLabelRule);
    EXPECT_EQ(r[1], night_facts::kNoGapRule);

    // A user's own LLM_PROMPT_FILE gets {metrics} only: the rules are in it.
    SessionMetrics u;
    u.index_kind = SessionMetrics::IndexKind::Ungraded;
    const auto ungraded = night_facts::renderNight(NightFacts::from(u));
    EXPECT_TRUE(has(ungraded, night_facts::kUngradedRule)) << ungraded;
}

TEST(NightFacts, AnUngradedNightsStrIndexIsNotAnAhiEither) {
    SessionMetrics u;
    u.index_kind = SessionMetrics::IndexKind::Ungraded;
    u.total_events = 4;
    STRDailyRecord str;
    str.ahi = 3.3;
    str.mask_events = 4;
    str.leak_95 = 20.0;
    str.mask_press_95 = 10.0;
    const auto f = NightFacts::from(u, &str);
    const auto daily = night_facts::renderNight(f);
    EXPECT_TRUE(has(daily, "Mask events: 2 (on/off pairs)")) << daily;
    EXPECT_FALSE(has(daily, "STR AHI")) << daily;
    EXPECT_FALSE(night_facts::toJson(f)["machine_daily_summary"].contains("ahi"));
}

TEST(NightFacts, TheModeIsTheStrsWhenTheSessionHasNone) {
    SessionMetrics m = aNight();
    m.therapy_mode = 0;
    STRDailyRecord str;
    str.mode = 1;
    const auto f = NightFacts::from(m, &str);
    ASSERT_TRUE(f.therapy_mode);
    EXPECT_EQ(*f.therapy_mode, "APAP");
    EXPECT_TRUE(has(night_facts::renderNight(f), "Therapy mode: APAP"));
}
