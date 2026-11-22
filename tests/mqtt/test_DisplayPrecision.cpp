#include <gtest/gtest.h>

#include "mqtt/DisplayPrecision.h"

#include <tuple>

using hms_cpap::displayPrecision;

namespace {
// (name, unit, device_class) -> decimals; -1 for "no hint".
int p(const char* name, const char* unit = "", const char* dc = "") {
    auto v = displayPrecision(name, unit, dc);
    return v ? *v : -1;
}
}  // namespace

TEST(DisplayPrecision, PressureLeakAndFlowShowOneDecimal) {
    EXPECT_EQ(p("avg_mask_pressure", "cmH2O"), 1);   // was 9.947559
    EXPECT_EQ(p("avg_leak_rate", "L/min"), 1);        // was 3.897860
    EXPECT_EQ(p("avg_minute_ventilation", "L/min"), 1);
    EXPECT_EQ(p("avg_respiratory_rate", "breaths/min"), 1);
    EXPECT_EQ(p("usage_percent", "%"), 1);
    EXPECT_EQ(p("str_csr", "min"), 1);
}

TEST(DisplayPrecision, TheIndexKeepsTwoDecimals) {
    EXPECT_EQ(p("ahi", "events/h"), 2);
    EXPECT_EQ(p("str_oai", "events/h"), 2);
    EXPECT_EQ(p("ahi_delta", "events/h"), 2);
}

TEST(DisplayPrecision, WholeNumbersForVolumesRatesAndSpo2) {
    EXPECT_EQ(p("avg_tidal_volume", "mL"), 0);   // was 518.171402
    EXPECT_EQ(p("heart_rate", "bpm"), 0);
    EXPECT_EQ(p("avg_spo2", "%"), 0);
    EXPECT_EQ(p("last_spo2", "%"), 0);
    EXPECT_EQ(p("str_spo2_50", "%"), 0);
    EXPECT_EQ(p("mask_days_left", "d"), 0);
}

TEST(DisplayPrecision, UnitlessNumbersAreToldByWhatTheyAre) {
    EXPECT_EQ(p("avg_snore"), 2);
    EXPECT_EQ(p("current_snore"), 2);
    EXPECT_EQ(p("avg_flow_limitation"), 2);
    EXPECT_EQ(p("avg_ie_ratio"), 2);
    EXPECT_EQ(p("total_events"), 0);
    EXPECT_EQ(p("obstructive_apneas"), 0);
    EXPECT_EQ(p("central_apneas"), 0);
    EXPECT_EQ(p("clear_airway_apneas"), 0);
    EXPECT_EQ(p("hypopneas"), 0);
    EXPECT_EQ(p("reras"), 0);
    EXPECT_EQ(p("str_mask_events"), 0);
    EXPECT_EQ(p("str_sleep_index"), 0);
    EXPECT_EQ(p("motion"), 0);
}

TEST(DisplayPrecision, TimesAreLeftToHomeAssistant) {
    EXPECT_EQ(p("usage_hours", "h", "duration"), -1);
    EXPECT_EQ(p("avg_event_duration", "s", "duration"), -1);
    EXPECT_EQ(p("last_session_time", "", "timestamp"), -1);
}

TEST(DisplayPrecision, TextAndModesGetNoHint) {
    EXPECT_EQ(p("session_status"), -1);
    EXPECT_EQ(p("session_summary"), -1);
    EXPECT_EQ(p("str_sleep_index_band"), -1);   // the band is a word, not the index
    EXPECT_EQ(p("therapy_mode"), -1);
}

TEST(DisplayPrecision, AUnitWeHaveNoViewOnIsLeftAlone) {
    EXPECT_EQ(p("something", "furlongs"), -1);
}

// The frontend formats a state as a number only with a unit or a state class.
TEST(DisplayHints, AUnitlessNumberGetsAStateClassSoItIsRounded) {
    Json::Value c(Json::objectValue);
    hms_cpap::addDisplayHints(c, "current_snore", "", "");
    EXPECT_EQ(c["suggested_display_precision"].asInt(), 2);
    EXPECT_EQ(c["state_class"].asString(), "measurement")
        << "without a unit or a state class the frontend shows the raw text, 0.000000";
}

TEST(DisplayHints, ANumberWithAUnitNeedsNoStateClass) {
    Json::Value c(Json::objectValue);
    hms_cpap::addDisplayHints(c, "avg_leak_rate", "L/min", "");
    EXPECT_EQ(c["suggested_display_precision"].asInt(), 1);
    EXPECT_FALSE(c.isMember("state_class"));
}

TEST(DisplayHints, AStateClassAlreadyThereIsKept) {
    Json::Value c(Json::objectValue);
    c["state_class"] = "total";
    hms_cpap::addDisplayHints(c, "total_events", "", "");
    EXPECT_EQ(c["state_class"].asString(), "total");
}

TEST(DisplayHints, TextAndTimesGetNothing) {
    for (auto [name, unit, dc] : {std::tuple<const char*, const char*, const char*>{"session_summary", "", ""},
                                  {"session_status", "", ""},
                                  {"last_session_time", "", "timestamp"},
                                  {"usage_hours", "h", "duration"}}) {
        Json::Value c(Json::objectValue);
        hms_cpap::addDisplayHints(c, name, unit, dc);
        EXPECT_TRUE(c.empty()) << name;
    }
}
