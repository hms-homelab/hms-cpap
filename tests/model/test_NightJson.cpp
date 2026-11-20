//
// SDD-013: a synthetic night read from JSON renders exactly as the same night
// built in code, so the narrator corpus's inputs are the runtime's inputs.
//
#include "model/NarrationService.h"
#include "model/NightJson.h"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace hms_cpap;
using nlohmann::json;

namespace {

json gradedNight() {
    return json::parse(R"({
      "id": "n1",
      "metrics": {"sleep_day": "2026-03-14", "index_kind": "ahi", "total_events": 14,
                  "ahi": 1.5172, "obstructive_apneas": 4, "central_apneas": 2,
                  "hypopneas": 5, "unclassified_apneas": 0, "clear_airway_apneas": 0,
                  "reras": 1, "other_events": 2, "usage_hours": 7.25,
                  "usage_percent": 90.625, "avg_leak_rate": 2.1, "leak_p95": 9.6,
                  "avg_therapy_pressure": 8.4, "therapy_mode": 0},
      "str": {"family": "autoset", "mode": 1, "ahi": 1.5, "mask_events": 4,
              "leak_95": 8.4, "mask_press_95": 9.1},
      "oximetry": {"avg_spo2": 95.5, "min_spo2": 88, "spo2_baseline": 97,
                   "odi_3pct": 2.1, "time_below_90": 0.4, "time_below_88": 0,
                   "avg_hr": 61.2, "min_hr": 50, "max_hr": 88}
    })");
}

}  // namespace

TEST(NightJson, RendersAsTheSameNightBuiltInCode) {
    SessionMetrics m;
    m.sleep_day = "2026-03-14";
    m.total_events = 14;
    m.ahi = 1.5172;
    m.obstructive_apneas = 4;
    m.central_apneas = 2;
    m.hypopneas = 5;
    m.reras = 1;
    m.other_events = 2;
    m.usage_hours = 7.25;
    m.usage_percent = 90.625;
    m.avg_leak_rate = 2.1;
    m.leak_p95 = 9.6;
    m.avg_therapy_pressure = 8.4;
    m.therapy_mode = 0;
    STRDailyRecord str;
    str.family = STRDailyRecord::Family::AutoSet;
    str.mode = 1;
    str.ahi = 1.5;
    str.mask_events = 4;
    str.leak_95 = 8.4;
    str.mask_press_95 = 9.1;
    NightOximetryFacts o;
    o.avg_spo2 = 95.5;
    o.min_spo2 = 88;
    o.spo2_baseline = 97;
    o.odi_3pct = 2.1;
    o.time_below_90 = 0.4;
    o.time_below_88 = 0;
    o.avg_hr = 61.2;
    o.min_hr = 50;
    o.max_hr = 88;

    const auto expected = model::factBlock(NightFacts::from(m, &str, o));
    const auto got = model::factBlock(model::nightFromJson(gradedNight()).facts());
    EXPECT_EQ(got, expected);
    // The STR's family names the mode, as it does for a stored night.
    EXPECT_NE(got.find("Therapy mode: APAP"), std::string::npos);
    EXPECT_NE(got.find("other event types=2"), std::string::npos);
}

TEST(NightJson, AnAbsentFigureIsAbsentNotZero) {
    auto j = gradedNight();
    j["metrics"].erase("avg_leak_rate");
    j["metrics"].erase("leak_p95");
    j.erase("oximetry");
    j.erase("str");
    const auto block = model::factBlock(model::nightFromJson(j).facts());
    EXPECT_EQ(block.find("Leak:"), std::string::npos);
    EXPECT_EQ(block.find("SpO2"), std::string::npos);
    EXPECT_EQ(block.find("ResMed official daily summary"), std::string::npos);
}

TEST(NightJson, UngradedNightCarriesNoAhi) {
    const auto j = json::parse(R"({"metrics": {"sleep_day": "2026-01-02",
        "index_kind": "ungraded", "total_events": 9, "ahi": 1.2, "usage_hours": 7.5}})");
    const auto block = model::factBlock(model::nightFromJson(j).facts());
    EXPECT_NE(block.find("AHI: not available"), std::string::npos);
    EXPECT_EQ(block.find("AHI: 1.20"), std::string::npos);
}

TEST(NightJson, UnknownKeyIsAnError) {
    auto j = gradedNight();
    j["metrics"]["ahii"] = 2;
    EXPECT_THROW(model::nightFromJson(j), std::invalid_argument);
    auto k = gradedNight();
    k["str"]["family"] = "philips";
    EXPECT_THROW(model::nightFromJson(k), std::invalid_argument);
    auto d = gradedNight();
    d["metrics"]["sleep_day"] = "14/03/2026";
    EXPECT_THROW(model::nightFromJson(d), std::invalid_argument);
}
