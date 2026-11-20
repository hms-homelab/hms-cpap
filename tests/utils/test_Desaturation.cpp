// SDD-001: a session's desaturations at the threshold set in Settings.
//
// The parser counts them at its own 3 % default; the owner may choose another
// drop (4 % is the CMS rule). desaturationsAt() is what every engine's
// saveSession stores, so the events, the count and the ODI follow the setting.
#include <gtest/gtest.h>

#include "utils/AppConfig.h"
#include "utils/Desaturation.h"

#include <chrono>
#include <cstdlib>
#include <string>

using namespace hms_cpap;
using std::chrono::seconds;
using std::chrono::system_clock;

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

/// Thirty minutes of 1 Hz SpO2 at 97 %, with a 4.5-point dip at minute 5 and a
/// 3.5-point dip at minute 15, each held 20 s. At the 3 % rule both count; at
/// 4 % only the first.
CPAPSession sessionWithTwoDips() {
    CPAPSession s;
    s.device_id = "desat-test";
    const auto start = system_clock::time_point{} + seconds(1'800'000'000);
    s.session_start = start;
    s.duration_seconds = 30 * 60;
    for (int i = 0; i < 30 * 60; ++i) {
        CPAPVitals v(start + seconds(i));
        double spo2 = 97.0;
        if (i >= 300 && i < 320) spo2 = 92.5;
        if (i >= 900 && i < 920) spo2 = 93.5;
        v.spo2 = spo2;
        v.heart_rate = 60;
        s.vitals.push_back(v);
    }
    SessionMetrics m;
    m.usage_hours = 0.5;
    s.metrics = m;
    return s;
}

class DesatEnvTest : public ::testing::Test {
protected:
    void TearDown() override { setEnv("DESAT_DROP_PCT", nullptr); }
};

}  // namespace

TEST(Desaturation, TheDefaultIsTheParsersThreePercent) {
    EXPECT_DOUBLE_EQ(defaultDesatDropPct(), 3.0);
    // AppConfig cannot include the parser, so it repeats the number; they must agree.
    EXPECT_DOUBLE_EQ(AppConfig{}.desat_drop_pct, defaultDesatDropPct());
}

TEST_F(DesatEnvTest, TheThresholdComesFromTheEnvironment) {
    setEnv("DESAT_DROP_PCT", nullptr);
    EXPECT_DOUBLE_EQ(desatDropPct(), 3.0);
    setEnv("DESAT_DROP_PCT", "4.000000");
    EXPECT_DOUBLE_EQ(desatDropPct(), 4.0);
}

TEST_F(DesatEnvTest, AValueSettingsWouldRefuseFallsBackToTheDefault) {
    for (const char* bad : {"abc", "0", "0.5", "50", "-3", ""}) {
        setEnv("DESAT_DROP_PCT", bad);
        EXPECT_DOUBLE_EQ(desatDropPct(), 3.0) << "'" << bad << "'";
    }
}

TEST(Desaturation, AtThreePercentBothDipsCount) {
    const auto d = desaturationsAt(sessionWithTwoDips(), 3.0);
    EXPECT_TRUE(d.from_vitals);
    ASSERT_EQ(d.events.size(), 2u);
    EXPECT_EQ(d.spo2_drops, 2);
    ASSERT_TRUE(d.odi.has_value());
    EXPECT_NEAR(*d.odi, 4.0, 1e-9) << "two in half an hour of therapy";
    EXPECT_NEAR(d.events[0].nadir, 92.5, 1e-9);
    EXPECT_NEAR(d.events[0].depth, 4.5, 1e-9);
}

TEST(Desaturation, AtFourPercentOnlyTheDeeperDipCounts) {
    const auto d = desaturationsAt(sessionWithTwoDips(), 4.0);
    ASSERT_EQ(d.events.size(), 1u);
    EXPECT_EQ(d.spo2_drops, 1);
    EXPECT_NEAR(*d.odi, 2.0, 1e-9);
    EXPECT_NEAR(d.events[0].nadir, 92.5, 1e-9);
}

TEST(Desaturation, WithoutTherapyHoursTheOdiIsOverTheSamples) {
    auto s = sessionWithTwoDips();
    s.metrics->usage_hours.reset();
    const auto d = desaturationsAt(s, 3.0);
    EXPECT_NEAR(*d.odi, 4.0, 1e-9) << "1,800 samples at 1 Hz are half an hour";
}

TEST(Desaturation, ASessionWithoutSpo2KeepsWhatItCarries) {
    CPAPSession s;
    SessionMetrics m;
    m.spo2_drops = 5;
    m.odi = 1.5;
    s.metrics = m;
    DesatEvent e;
    e.duration_seconds = 12;
    s.desaturations = {e};
    CPAPVitals hr_only(system_clock::now());
    hr_only.heart_rate = 55;
    s.vitals = {hr_only};

    const auto d = desaturationsAt(s, 4.0);
    EXPECT_FALSE(d.from_vitals) << "nothing to work out, so nothing stored is replaced";
    EXPECT_EQ(d.events.size(), 1u);
    EXPECT_EQ(d.spo2_drops, 5);
    EXPECT_EQ(d.odi, 1.5);

    const auto kept = withDesaturations(m, d);
    EXPECT_EQ(kept.spo2_drops, 5);
    EXPECT_EQ(kept.odi, 1.5);
}

TEST(Desaturation, TheMetricsTakeTheCountAndOdiWorkedOut) {
    const auto s = sessionWithTwoDips();
    auto m = *s.metrics;
    m.spo2_drops = 9;   // the parser's, at its own default
    m.odi = 18.0;
    const auto out = withDesaturations(m, desaturationsAt(s, 4.0));
    EXPECT_EQ(out.spo2_drops, 1);
    EXPECT_NEAR(*out.odi, 2.0, 1e-9);
    EXPECT_EQ(out.usage_hours, 0.5) << "nothing else changes";
}
