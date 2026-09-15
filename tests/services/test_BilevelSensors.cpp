// SDD-030 (#33): the rules for what a bi-level publishes, without a broker,
// as IndexSensorRouting tests indexSensorsFor.
#include <gtest/gtest.h>

#include "services/BilevelSensors.h"
#include "services/BurstCollectorService.h"

#include <map>

using namespace hms_cpap;

namespace {

std::map<std::string, double> asMap(const NamedValues& v) {
    return {v.begin(), v.end()};
}

SessionMetrics nightWith(std::optional<double> ipap, std::optional<double> epap) {
    SessionMetrics m;
    m.avg_therapy_pressure = ipap;
    m.avg_epr_pressure = epap;
    return m;
}

}  // namespace

TEST(BilevelNightSensors, ABilevelNightHasIpapEpapAndTheirDifference) {
    // The reporter's first session: Press 9.02, EprPress 5.02, PS 4.
    const auto s = asMap(bilevelNightSensors(MachineFamily::BiLevel, nightWith(9.02, 5.02)));
    ASSERT_EQ(s.size(), 3u);
    EXPECT_DOUBLE_EQ(s.at("ipap"), 9.02);
    EXPECT_DOUBLE_EQ(s.at("epap"), 5.02);
    EXPECT_NEAR(s.at("pressure_support"), 4.0, 1e-9);
}

TEST(BilevelNightSensors, AnAirSenseGetsNone) {
    // On an AirSense the same two channels are the delivered pressure and the
    // EPR set-point; calling them IPAP and EPAP would be wrong.
    for (auto f : {MachineFamily::AutoSet, MachineFamily::Cpap, MachineFamily::Asv,
                   MachineFamily::Unknown})
        EXPECT_TRUE(bilevelNightSensors(f, nightWith(9.0, 5.0)).empty());
}

TEST(BilevelNightSensors, OneChannelAloneGivesNothing) {
    EXPECT_TRUE(bilevelNightSensors(MachineFamily::BiLevel, nightWith(9.0, std::nullopt)).empty());
    EXPECT_TRUE(bilevelNightSensors(MachineFamily::BiLevel, nightWith(std::nullopt, 5.0)).empty());
}

TEST(BilevelStrSensors, AVautoPublishesItsCeilingFloorSupportAndTargets) {
    STRDailyRecord r;
    r.family = MachineFamily::BiLevel;
    r.bl_max_ipap = 20;  // S.VA.MaxIPAP
    r.bl_min_epap = 5;   // S.VA.MinEPAP
    r.bl_ps = 4;         // S.VA.PS
    r.bl_ipap = 15;      // S.S.IPAP is on the reporter's card too; S.VA wins
    r.bl_epap = 10;
    r.tgt_ipap_95 = 11.2;
    r.tgt_epap_95 = 7.3;
    const auto s = asMap(bilevelStrSensors(r));
    EXPECT_DOUBLE_EQ(s.at("str_max_ipap"), 20);
    EXPECT_DOUBLE_EQ(s.at("str_min_epap"), 5);
    EXPECT_DOUBLE_EQ(s.at("str_pressure_support"), 4);
    EXPECT_DOUBLE_EQ(s.at("str_tgt_ipap_95"), 11.2);
    EXPECT_DOUBLE_EQ(s.at("str_tgt_epap_95"), 7.3);
}

TEST(BilevelStrSensors, AFixedBilevelTakesItsSupportFromItsTwoPressures) {
    STRDailyRecord r;
    r.family = MachineFamily::BiLevel;
    r.bl_ipap = 15;
    r.bl_epap = 10;
    const auto s = asMap(bilevelStrSensors(r));
    EXPECT_DOUBLE_EQ(s.at("str_max_ipap"), 15);
    EXPECT_DOUBLE_EQ(s.at("str_min_epap"), 10);
    EXPECT_DOUBLE_EQ(s.at("str_pressure_support"), 5);
    EXPECT_EQ(s.count("str_tgt_ipap_95"), 0u) << "only what the card wrote";
}

TEST(BilevelStrSensors, NotABilevelNothing) {
    STRDailyRecord r;
    r.family = MachineFamily::AutoSet;
    r.tgt_ipap_95 = 11;  // even with a value lying around
    EXPECT_TRUE(bilevelStrSensors(r).empty());
}

TEST(TherapyModeName, EightIsVautoOnABilevelAndAsvOtherwise) {
    EXPECT_EQ(therapyModeName(8, MachineFamily::BiLevel), "VAuto");   // AirCurve 11 VAuto
    EXPECT_EQ(therapyModeName(6, MachineFamily::BiLevel), "VAuto");   // AirCurve 10 VAuto
    EXPECT_EQ(therapyModeName(8, MachineFamily::Asv), "ASV (Variable EPAP)");
    EXPECT_EQ(therapyModeName(8, MachineFamily::Unknown), "ASV (Variable EPAP)");
    EXPECT_EQ(therapyModeName(1, MachineFamily::AutoSet), "APAP");
}

TEST(TherapyModeName, AnUnseenBilevelModeIsNamedByNumberNotGuessed) {
    EXPECT_EQ(therapyModeName(3, MachineFamily::BiLevel), "Bi-level (mode 3)");
}

TEST(TherapyModeFor, TheSessionWinsUnlessItIsZero) {
    EXPECT_EQ(therapyModeFor(1, 8), 1);                          // session has one
    EXPECT_EQ(therapyModeFor(0, 8), 8);                          // stored "none" -> STR
    EXPECT_EQ(therapyModeFor(std::nullopt, 6), 6);               // no session value
    EXPECT_EQ(therapyModeFor(0, std::nullopt), 0);               // nothing better
    EXPECT_EQ(therapyModeFor(std::nullopt, std::nullopt), std::nullopt);
}

TEST(TherapyModeName, TheMetricsStringTakesTheStrModeOverTheStoredZero) {
    // What a stored ResMed session really carries: therapy_mode 0, the value
    // every engine writes for "none". The AirCurve 10's STR says 6.
    BurstCollectorService svc(60);
    SessionMetrics m;
    m.therapy_mode = 0;
    STRDailyRecord r;
    r.family = MachineFamily::BiLevel;
    r.mode = 6;
    const auto out = svc.buildMetricsStringForTest(m, &r);
    EXPECT_NE(out.find("Therapy mode: VAuto"), std::string::npos) << out;
    EXPECT_EQ(out.find("Therapy mode: CPAP"), std::string::npos) << out;
}

TEST(StrSpo2, NoOximeterIsAbsentNotZero) {
    EXPECT_FALSE(strSpo2Present(0.0));
    EXPECT_FALSE(strSpo2Present(-1.0));
    EXPECT_TRUE(strSpo2Present(95.0));
}
