//
// test_ReportFigures.cpp: SDD-082, the clinician and compliance reports'
// arithmetic, and the rules a report request is held to.
//
// The figure cases are CpapDash's own (its clinician and compliance reports'
// unit tests), ported with the namespaces changed: the two copies must agree
// figure for figure (SDD-082 section 2). Two cases are hms-cpap's: the
// machine's pressure (B9) and the ungraded index (B7).
//
#include <gtest/gtest.h>

#include "database/StrSettings.h"
#include "services/reports/ReportRequest.h"
#include "services/reports/figures/ClinicianFigures.h"
#include "services/reports/figures/ComplianceFigures.h"

#include <map>
#include <string>
#include <vector>

using namespace hms_cpap;
using namespace hms_cpap::clinician;
using namespace hms_cpap::compliance;

namespace {

Night night(const std::string& date, double minutes, double ahi = 1.0) {
    Night n;
    n.date = date;
    n.minutes = minutes;
    n.ahi = ahi;
    return n;
}

Json::Value row(const std::string& date, double minutes, double ahi) {
    Json::Value r(Json::objectValue);
    r["record_date"] = date;
    r["duration_minutes"] = minutes;
    r["ahi"] = ahi;
    return r;
}

}  // namespace

// ── Calendar ────────────────────────────────────────────────────────────────

TEST(ReportClinicianCalendar, DaysRoundTripAndBadDatesAreNothing) {
    EXPECT_EQ(*dayNumber("1970-01-01"), 0);
    EXPECT_EQ(dateOf(*dayNumber("2026-02-28") + 1), "2026-03-01");
    EXPECT_EQ(dateOf(*dayNumber("2024-02-28") + 1), "2024-02-29") << "leap year";
    EXPECT_EQ(*dayNumber("2026-09-20 00:00:00+02"), *dayNumber("2026-09-20"));
    EXPECT_FALSE(dayNumber("2026-02-30"));
    EXPECT_FALSE(dayNumber("2026-13-01"));
    EXPECT_FALSE(dayNumber("20260920"));
    EXPECT_FALSE(dayNumber(""));
    EXPECT_EQ(dateOf(-1), "1969-12-31");
}

TEST(ReportClinicianCalendar, MonthsClampToTheLastDay) {
    EXPECT_EQ(addMonths("2026-03-31", -1), "2026-02-28");
    EXPECT_EQ(addMonths("2026-09-20", -12), "2025-09-20");
    EXPECT_EQ(addMonths("2026-01-15", -1), "2025-12-15");
    EXPECT_EQ(addMonths("2025-12-31", 2), "2026-02-28");
    EXPECT_EQ(addMonths("bad", -1), "bad");
}

// ── Rows ────────────────────────────────────────────────────────────────────

TEST(ReportClinicianNights, ARowReadsNumbersOrStringsAndAbsentIsNotZero) {
    Json::Value r(Json::objectValue);
    r["record_date"] = "2026-09-06";
    r["duration_minutes"] = "299";
    r["ahi"] = 3.0;
    r["oai"] = Json::nullValue;
    r["apap_min_pressure"] = "4";
    r["mode"] = "1";
    r["family"] = 2;
    r["leak_over_24_minutes"] = 0;
    const auto n = nightFrom(r);
    ASSERT_TRUE(n);
    EXPECT_EQ(n->date, "2026-09-06");
    EXPECT_DOUBLE_EQ(n->minutes, 299);
    EXPECT_DOUBLE_EQ(*n->ahi, 3.0);
    EXPECT_FALSE(n->oai) << "null is absent";
    EXPECT_FALSE(n->cai) << "missing is absent";
    EXPECT_DOUBLE_EQ(*n->apap_min, 4.0);
    EXPECT_EQ(*n->mode, 1);
    EXPECT_EQ(n->family, 2);
    EXPECT_DOUBLE_EQ(*n->leak_over_24_minutes, 0.0) << "0 is a real figure";
    EXPECT_FALSE(n->ungraded);

    Json::Value bad(Json::objectValue);
    bad["record_date"] = "nope";
    EXPECT_FALSE(nightFrom(bad));
    EXPECT_FALSE(nightFrom(Json::Value("x")));
}

TEST(ReportClinicianNights, ThePressureIsTheMachinesThenTheMasksAndZeroIsNone) {
    // B9 (issue #56): the delivered pressure where the night has it.
    Json::Value r(Json::objectValue);
    r["record_date"] = "2026-09-06";
    r["duration_minutes"] = 400;
    r["therapy_press_50"] = 8.0;
    r["mask_press_50"] = 6.4;
    r["therapy_press_95"] = 0.0;   // no PLD minutes
    r["mask_press_95"] = 9.9;
    r["mask_press_max"] = 0.0;     // a machine without the channel
    r["index_kind"] = "ungraded";
    const auto n = nightFrom(r);
    ASSERT_TRUE(n);
    EXPECT_DOUBLE_EQ(*n->press_50, 8.0);
    EXPECT_DOUBLE_EQ(*n->press_95, 9.9) << "the mask's when the machine's is 0";
    EXPECT_FALSE(n->press_max) << "0 is no reading (issue 15)";
    EXPECT_TRUE(n->ungraded) << "B7: SDD-024's kind rides along";
}

TEST(ReportClinicianNights, TheAnswerIsSortedAndOneRowPerDate) {
    Json::Value rows(Json::arrayValue);
    rows.append(row("2026-09-07", 100, 1));
    rows.append(row("2026-09-05", 200, 2));
    rows.append(row("2026-09-07", 300, 3));
    const auto ns = nightsFrom(rows);
    ASSERT_EQ(ns.size(), 2u);
    EXPECT_EQ(ns[0].date, "2026-09-05");
    EXPECT_DOUBLE_EQ(ns[1].minutes, 300) << "the later row for a date wins";
    EXPECT_TRUE(nightsFrom(Json::Value(Json::objectValue)).empty());
}

// ── One window ──────────────────────────────────────────────────────────────

TEST(ReportClinicianWindow, UsageCountsEveryDayAndIndicesAreWeightedByHours) {
    std::vector<Night> ns = {night("2026-09-01", 480, 1.0), night("2026-09-02", 60, 9.0),
                             night("2026-09-04", 300, 2.0)};
    ns[0].oai = 0.5;
    ns[1].oai = 3.0;
    ns.push_back(night("2026-08-31", 600, 50));   // outside the window
    const auto f = figuresFor(ns, {"w", "2026-09-01", "2026-09-05"});
    EXPECT_EQ(f.total_days, 5);
    EXPECT_EQ(f.used_days, 3);
    EXPECT_EQ(f.days_4h, 2);
    EXPECT_EQ(f.days_under_4h, 1);
    EXPECT_NEAR(f.pct_total_4h, 40.0, 1e-9) << "of all five days";
    EXPECT_NEAR(f.pct_used_4h, 66.6667, 1e-3) << "of the three used";
    EXPECT_NEAR(f.avg_hours, (8 + 1 + 5) / 3.0, 1e-9);
    EXPECT_NEAR(f.median_hours, 5.0, 1e-9);
    EXPECT_NEAR(*f.ahi, (1.0 * 480 + 9.0 * 60 + 2.0 * 300) / 840.0, 1e-9)
        << "events over hours, not the mean of nightly AHIs";
    EXPECT_NEAR(*f.ahi_median, 2.0, 1e-9);
    EXPECT_NEAR(*f.oai, (0.5 * 480 + 3.0 * 60) / 540.0, 1e-9);
    EXPECT_FALSE(f.cai) << "no night carries it";
    EXPECT_FALSE(f.large_leak_pct);
}

TEST(ReportClinicianWindow, LargeLeakUsesTheMachinesFlagsElseThe24LineAndCsrIsAShare) {
    std::vector<Night> ns = {night("2026-09-01", 400), night("2026-09-02", 200),
                             night("2026-09-03", 100)};
    ns[0].large_leak_minutes = 20;
    ns[0].leak_over_24_minutes = 99;   // ignored where flags exist
    ns[1].leak_over_24_minutes = 10;
    ns[0].csr_minutes = 6;
    ns[1].csr_minutes = 0;
    const auto f = figuresFor(ns, {"w", "2026-09-01", "2026-09-03"});
    ASSERT_TRUE(f.large_leak_pct);
    EXPECT_NEAR(*f.large_leak_pct, 100.0 * 30 / 600, 1e-9);
    EXPECT_TRUE(f.large_leak_uses_flags);
    EXPECT_TRUE(f.large_leak_uses_line);
    ASSERT_TRUE(f.csr_pct);
    EXPECT_NEAR(*f.csr_pct, 1.0, 1e-9);
}

TEST(ReportClinicianWindow, PressuresTakeTheExtremesAndAnEmptyWindowHasOnlyItsDays) {
    std::vector<Night> ns = {night("2026-09-01", 400), night("2026-09-02", 400)};
    ns[0].press_max = 12; ns[1].press_max = 16;
    ns[0].press_min = 4.5; ns[1].press_min = 4.0;
    ns[0].epap_max = 9; ns[1].epap_min = 4;
    const auto f = figuresFor(ns, {"w", "2026-09-01", "2026-09-02"});
    EXPECT_DOUBLE_EQ(*f.press_max, 16);
    EXPECT_DOUBLE_EQ(*f.press_min, 4.0);
    EXPECT_DOUBLE_EQ(*f.epap_max, 9);
    EXPECT_DOUBLE_EQ(*f.epap_min, 4);
    const auto none = figuresFor(ns, {"w", "2026-10-01", "2026-10-03"});
    EXPECT_EQ(none.total_days, 3);
    EXPECT_EQ(none.used_days, 0);
    EXPECT_FALSE(none.ahi);
    EXPECT_EQ(figuresFor(ns, {"w", "2026-10-03", "2026-10-01"}).total_days, 0) << "backwards";
}

TEST(ReportClinicianWindow, TheStandardWindowsDropWhatPredatesTheFirstNight) {
    std::vector<Night> ns = {night("2026-09-20", 4), night("2026-09-12", 338)};
    auto ws = standardWindows(ns, "2026-09-01", "2026-09-20", "2026-01-01");
    std::vector<std::string> labels;
    for (const auto& w : ws) labels.push_back(w.label);
    EXPECT_EQ(labels, (std::vector<std::string>{"Last night", "Last 7 days", "Last 30 days",
                                                "Last 6 months", "Your range"}));
    EXPECT_EQ(ws[0].start, "2026-09-20") << "the 4 minute stub is last night";
    EXPECT_EQ(ws[1].start, "2026-09-14");
    EXPECT_EQ(ws[2].start, "2026-08-22");
    EXPECT_EQ(ws[3].start, "2026-03-21");
    ws = standardWindows(ns, "2026-08-22", "2026-09-20", "2025-01-01");
    EXPECT_EQ(ws.back().label, "Last year") << "the user's range IS the 30 days: not twice";
    EXPECT_EQ(ws.back().start, "2025-09-21");
    EXPECT_TRUE(standardWindows(ns, "x", "bad", "").empty());
}

TEST(ReportClinicianWindow, MonthsAreNewestFirstClippedAndStopAtTheFirstNight) {
    const auto ms = monthlyWindows("2026-09-20", "2026-05-15");
    ASSERT_EQ(ms.size(), 5u);
    EXPECT_EQ(ms[0].label, "2026-09");
    EXPECT_EQ(ms[0].start, "2026-09-01");
    EXPECT_EQ(ms[0].end, "2026-09-20");
    EXPECT_EQ(ms[1].end, "2026-08-31");
    EXPECT_EQ(ms[4].label, "2026-05");
    EXPECT_EQ(monthlyWindows("2026-09-20", "").size(), 12u);
    EXPECT_EQ(monthlyWindows("2026-03-31", "")[1].end, "2026-02-28");
}

// ── Settings periods ────────────────────────────────────────────────────────

TEST(ReportClinicianSettings, ATherapyIsNamedOnlyWhenFamilyAndModeAgree) {
    EXPECT_EQ(therapyLabel(2, 1), "APAP");
    EXPECT_EQ(therapyLabel(2, 0), "CPAP");
    EXPECT_EQ(therapyLabel(3, 8), "VAuto");
    EXPECT_EQ(therapyLabel(4, 8), "ASVAuto");
    EXPECT_EQ(therapyLabel(3, 7), "") << "never from the number alone";
    EXPECT_EQ(therapyLabel(0, 1), "");
    EXPECT_EQ(therapyLabel(2, std::nullopt), "");
}

TEST(ReportClinicianSettings, NightsGroupIntoPeriodsByWhatTheMachineWasSetTo) {
    std::vector<Night> ns;
    for (const char* d : {"2026-09-01", "2026-09-02", "2026-09-04"}) {
        Night n = night(d, 420, 2.0);
        n.family = 2; n.mode = 1; n.apap_min = 4; n.apap_max = 20; n.epr_level = 3;
        n.pressure_setting = 10;
        ns.push_back(n);
    }
    Night changed = night("2026-09-05", 300, 1.0);
    changed.family = 2; changed.mode = 1; changed.apap_min = 6; changed.apap_max = 15;
    changed.epr_level = 0;
    ns.push_back(changed);
    Night cpap = night("2026-09-06", 360, 1.5);
    cpap.family = 2; cpap.mode = 0; cpap.pressure_setting = 9;
    ns.push_back(cpap);
    ns.push_back(night("2026-09-07", 0, 0));
    ns.push_back(night("2026-09-03", 400, 5.0));   // settings unknown: no split

    const auto ps = settingsPeriods(ns);
    ASSERT_EQ(ps.size(), 3u);
    EXPECT_EQ(ps[0].first, "2026-09-01");
    EXPECT_EQ(ps[0].last, "2026-09-04");
    EXPECT_EQ(ps[0].nights, 3);
    EXPECT_EQ(ps[0].mode_label, "APAP");
    EXPECT_EQ(ps[0].pressure, "4.0 - 20.0 cmH2O") << "the AutoSet limits, not S.C.Press";
    EXPECT_EQ(ps[0].relief, "EPR 3");
    EXPECT_NEAR(*ps[0].ahi, 2.0, 1e-9);
    EXPECT_NEAR(ps[0].avg_hours, 7.0, 1e-9);
    EXPECT_EQ(ps[1].pressure, "6.0 - 15.0 cmH2O");
    EXPECT_EQ(ps[1].relief, "EPR off");
    EXPECT_EQ(ps[2].mode_label, "CPAP");
    EXPECT_EQ(ps[2].pressure, "9.0 cmH2O");
    EXPECT_EQ(ps[2].relief, "");

    Night bl = night("2026-09-08", 400);
    bl.family = 3; bl.mode = 8; bl.bl_min_epap = 5; bl.bl_max_ipap = 14; bl.bl_ps = 3;
    const auto b = settingsPeriods({bl});
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].mode_label, "VAuto");
    EXPECT_EQ(b[0].pressure, "EPAP 5.0, IPAP 14.0 cmH2O, PS 3.0");
}

// ── CMS ─────────────────────────────────────────────────────────────────────

TEST(ReportClinicianCms, TwentyOneGoodNightsInAnyThirtyMeetTheRule) {
    std::vector<Night> ns;
    const long f = *dayNumber("2026-01-01");
    for (long d = 0; d < 90; ++d)
        ns.push_back(night(dateOf(f + d), (d >= 20 && d < 41) ? 250 : 120));
    const auto c = cmsAdherence(ns, "2026-01-01", "2026-06-01");
    ASSERT_TRUE(c.known);
    EXPECT_TRUE(c.complete);
    EXPECT_EQ(c.day_90, "2026-03-31");
    EXPECT_EQ(c.best_nights_4h, 21);
    EXPECT_EQ(c.best_start, "2026-01-12");
    EXPECT_EQ(c.best_end, "2026-02-10");
    EXPECT_NEAR(c.best_pct, 70.0, 1e-9);
    EXPECT_TRUE(c.met);
}

TEST(ReportClinicianCms, TwentyIsNotEnoughAMissingDayIsZeroAndTheClockBoundsTheWindows) {
    std::vector<Night> ns;
    const long f = *dayNumber("2026-01-01");
    for (long d = 0; d < 20; ++d) ns.push_back(night(dateOf(f + 2 * d), 300));
    ns.push_back(night(dateOf(f + 95), 600));
    auto c = cmsAdherence(ns, "2026-01-01", "2026-12-31");
    EXPECT_EQ(c.best_nights_4h, 15);
    EXPECT_FALSE(c.met);
    EXPECT_TRUE(c.complete);
    c = cmsAdherence(ns, "2026-01-01", "2026-01-10");
    EXPECT_FALSE(c.complete);
    EXPECT_EQ(c.best_start, "2026-01-01");
    EXPECT_FALSE(cmsAdherence(ns, "", "2026-01-10").known);
}

// ── One night ───────────────────────────────────────────────────────────────

TEST(ReportClinicianNight, TheLastAndTheWorstNight) {
    std::vector<Night> ns = {night("2026-09-05", 358, 2.0), night("2026-09-06", 299, 3.0),
                             night("2026-09-07", 184, 3.0), night("2026-09-20", 4, 0.0),
                             night("2026-09-21", 400, 9.0)};
    EXPECT_EQ(lastNight(ns, "2026-09-20")->date, "2026-09-20");
    EXPECT_EQ(worstNight(ns, "2026-09-01", "2026-09-20")->date, "2026-09-07");
    EXPECT_FALSE(worstNight(ns, "2026-10-01", "2026-10-02"));
    EXPECT_FALSE(lastNight(ns, "2026-09-01"));
}

TEST(ReportClinicianNight, TimestampsReadEveryOffsetFormAndAStoredLabelAsItStands) {
    EXPECT_EQ(*epochOf("1970-01-01 00:00:00+00"), 0);
    EXPECT_EQ(*epochOf("1970-01-01 02:00:00+02"), 0);
    EXPECT_EQ(*epochOf("1970-01-01T00:00:00Z"), 0);
    EXPECT_EQ(*epochOf("1969-12-31 19:00:00-05:00"), 0);
    EXPECT_EQ(*epochOf("1970-01-01 00:00:10.250+00"), 10);
    EXPECT_EQ(*epochOf("1970-01-01 05:30:00+05:30"), 0);
    EXPECT_EQ(*epochOf("1970-01-01 00:00:05"), 5) << "a stored label: the machine's clock";
    EXPECT_FALSE(epochOf("1970-01-01"));
    EXPECT_FALSE(epochOf("1970-01-01 xx:00:00+00"));
    EXPECT_FALSE(epochOf("1970-01-01 00:00:00?00"));
}

TEST(ReportClinicianNight, EventsCountIntoTheirFlagRowsAndTheRunningAhiWaitsTenMinutes) {
    Json::Value rows(Json::arrayValue);
    const auto ev = [&](const char* type, const char* ts) {
        Json::Value e(Json::objectValue);
        e["event_type"] = type;
        e["event_timestamp"] = ts;
        e["duration_seconds"] = "12";
        rows.append(e);
    };
    ev("Obstructive", "2026-09-07 01:00:00");
    ev("Hypopnea", "2026-09-07 00:30:00");
    ev("Clear Airway", "2026-09-07 02:00:00");
    ev("RERA", "2026-09-07 02:10:00");
    ev("Large Leak", "2026-09-07 02:20:00");
    ev("Something Else", "2026-09-07 02:30:00");
    ev("Obstructive", "garbage");
    const auto events = eventsFrom(rows);
    ASSERT_EQ(events.size(), 6u);
    EXPECT_EQ(events[0].type, "Hypopnea");
    EXPECT_DOUBLE_EQ(events[0].duration, 12);

    const auto counts = flagCounts(events);
    const auto& fr = flagRows();
    std::map<std::string, int> byLabel;
    for (std::size_t i = 0; i < fr.size(); ++i) byLabel[fr[i].label] = counts[i];
    EXPECT_EQ(byLabel["OA"], 1);
    EXPECT_EQ(byLabel["H"], 1);
    EXPECT_EQ(byLabel["CA"], 1);
    EXPECT_EQ(byLabel["RE"], 1);
    EXPECT_EQ(byLabel["LL"], 1);
    EXPECT_EQ(byLabel["UA"], 0);

    const long t0 = *epochOf("2026-09-07 00:00:00");
    const auto ahi = runningAhi(events, t0);
    ASSERT_EQ(ahi.size(), 3u);
    EXPECT_NEAR(ahi[0].second, 1 / 0.5, 1e-9);
    EXPECT_NEAR(ahi[2].second, 3 / 2.0, 1e-9);
    const auto early = runningAhi({{"Hypopnea", t0 + 60, 10}}, t0);
    ASSERT_EQ(early.size(), 1u);
    EXPECT_NEAR(early[0].second, 6.0, 1e-9);
}

TEST(ReportClinicianNight, TimeAtPressureBinsByHalfCentimetreAndSkipsTheMachineOff) {
    const auto bins = timeAtPressure({4.0, 4.2, 4.6, 0.0, -1, 10.0, 10.49});
    ASSERT_EQ(bins.size(), 3u);
    EXPECT_DOUBLE_EQ(bins[0].first, 4.0);
    EXPECT_DOUBLE_EQ(bins[0].second, 2);
    EXPECT_DOUBLE_EQ(bins[1].first, 4.5);
    EXPECT_DOUBLE_EQ(bins[2].first, 10.0);
    EXPECT_DOUBLE_EQ(bins[2].second, 2);
}

TEST(ReportClinicianNight, ThePercentileIsPostgresPercentileCont) {
    // B8: linear between the ranks around (n - 1) x p, as percentile_cont.
    EXPECT_FALSE(percentileCont({}, 0.95));
    EXPECT_DOUBLE_EQ(*percentileCont({0.3}, 0.95), 0.3);
    // n = 21 values 0.00..0.20: (21 - 1) x 0.95 = 19 exactly -> 0.19.
    std::vector<double> v;
    for (int i = 20; i >= 0; --i) v.push_back(i / 100.0);
    EXPECT_NEAR(*percentileCont(v, 0.95), 0.19, 1e-12);
    // n = 4 {0, 0, 0.1, 0.5}: pos 2.85 -> 0.1 + 0.85 x 0.4 = 0.44.
    EXPECT_NEAR(*percentileCont({0.5, 0.0, 0.1, 0.0}, 0.95), 0.44, 1e-12);
}

// ── Compliance ──────────────────────────────────────────────────────────────

TEST(ReportCompliance, EveryCalendarNightCountsAndANoUseNightIsAMiss) {
    std::vector<Night> ns = {night("2026-09-01", 300), night("2026-09-02", 240),
                             night("2026-09-03", 239), night("2026-09-05", 480),
                             night("2026-09-07", 0),   night("2026-09-09", 600),
                             night("2026-08-31", 600), night("2026-09-11", 600)};
    const auto j = judge(ns, "2026-09-01", "2026-09-10", Rule{4.0, 70});
    EXPECT_EQ(j.nights, 10);
    EXPECT_EQ(j.nights_used, 5);
    EXPECT_EQ(j.nights_met, 4) << "240 is at the line; 239 is under";
    EXPECT_NEAR(j.pct, 40.0, 1e-9) << "of all ten nights, not of the used ones";
    EXPECT_EQ(j.nights_needed, 7);
    EXPECT_FALSE(j.meets);
    EXPECT_EQ(j.margin, -3);
}

TEST(ReportCompliance, TheBoundaryMeetsAndTheMarginIsWhatWasToSpare) {
    std::vector<Night> ns;
    const long s = *dayNumber("2026-09-01");
    for (long d = 0; d < 30; ++d) ns.push_back(night(dateOf(s + d), d < 21 ? 250 : 100));
    auto j = judge(ns, "2026-09-01", "2026-09-30", Rule{4.0, 70});
    EXPECT_EQ(j.nights_met, 21);
    EXPECT_NEAR(j.pct, 70.0, 1e-9);
    EXPECT_TRUE(j.meets) << "pct == share meets";
    EXPECT_EQ(j.margin, 0);
    j = judge(ns, "2026-09-01", "2026-09-30", Rule{4.0, 71});
    EXPECT_FALSE(j.meets);
    EXPECT_EQ(j.nights_needed, 22);
    EXPECT_EQ(j.margin, -1);
    j = judge(ns, "2026-09-01", "2026-09-30", Rule{1.5, 50});
    EXPECT_EQ(j.nights_met, 30);
    EXPECT_TRUE(j.meets);
    EXPECT_EQ(j.margin, 15);
    j = judge(ns, "2026-09-01", "2026-09-30", Rule{6.0, 1});
    EXPECT_EQ(j.nights_met, 0);
    EXPECT_FALSE(j.meets) << "1 % of 30 is still one night";
    EXPECT_EQ(j.nights_needed, 1);
}

TEST(ReportCompliance, ABadRangeHasNoNightsAndDoesNotMeet) {
    const std::vector<Night> ns = {night("2026-09-01", 300)};
    EXPECT_EQ(judge(ns, "2026-09-10", "2026-09-01", Rule{}).nights, 0);
    EXPECT_FALSE(judge(ns, "2026-09-10", "2026-09-01", Rule{}).meets);
    EXPECT_EQ(judge(ns, "nope", "2026-09-01", Rule{}).nights, 0);
    const auto one = judge(ns, "2026-09-01", "2026-09-01", Rule{4.0, 70});
    EXPECT_EQ(one.nights, 1);
    EXPECT_TRUE(one.meets);
    EXPECT_TRUE(calendarMinutes(ns, "2026-09-02", "2026-09-01").empty());
}

TEST(ReportCompliance, TheVerdictCarriesTheRuleAndTheCounts) {
    const std::vector<Night> ns = {night("2026-09-01", 300), night("2026-09-03", 100)};
    const auto j = judge(ns, "2026-09-01", "2026-09-04", Rule{4.0, 25});
    const auto v = verdictOf(j, Rule{4.0, 25});
    EXPECT_TRUE(v["meets"].asBool());
    EXPECT_DOUBLE_EQ(v["hours"].asDouble(), 4.0);
    EXPECT_EQ(v["share"].asInt(), 25);
    EXPECT_EQ(v["nights"].asInt(), 4);
    EXPECT_EQ(v["nights_used"].asInt(), 2);
    EXPECT_EQ(v["nights_met"].asInt(), 1);
    EXPECT_NEAR(v["pct"].asDouble(), 25.0, 1e-9);
}

TEST(ReportCompliance, TheCalendarListsEveryNightWithZeroForAMissingRow) {
    const std::vector<Night> ns = {night("2026-09-03", 300), night("2026-09-01", 60),
                                   night("2026-09-03", 200)};
    const auto cal = calendarMinutes(ns, "2026-09-01", "2026-09-04");
    ASSERT_EQ(cal.size(), 4u);
    EXPECT_DOUBLE_EQ(cal[0].second, 60);
    EXPECT_DOUBLE_EQ(cal[1].second, 0) << "no row is no use";
    EXPECT_DOUBLE_EQ(cal[2].second, 300) << "a repeated date keeps its largest figure";
    EXPECT_EQ(weekdayOf(*dayNumber("1970-01-01")), 3) << "a Thursday";
    EXPECT_EQ(weekdayOf(*dayNumber("2026-09-07")), 0) << "a Monday";
    EXPECT_EQ(weekdayOf(*dayNumber("2026-09-06")), 6) << "a Sunday";
    EXPECT_EQ(weekdayOf(*dayNumber("1969-12-28")), 6);
}

TEST(ReportCompliance, EveryThirtyNightWindowGetsItsShareAndAShortRangeNone) {
    std::vector<Night> ns;
    const long s = *dayNumber("2026-09-01");
    for (long d = 0; d < 30; ++d) ns.push_back(night(dateOf(s + d), 300));
    const auto roll = rollingShare(ns, "2026-09-01", "2026-10-10", 4.0);
    ASSERT_EQ(roll.size(), 11u);
    EXPECT_EQ(roll[0].first, "2026-09-30");
    EXPECT_NEAR(roll[0].second, 100.0, 1e-9);
    EXPECT_NEAR(roll[10].second, 100.0 * 20 / 30, 1e-9);
    EXPECT_TRUE(rollingShare(ns, "2026-09-01", "2026-09-29", 4.0).empty());
    EXPECT_EQ(rollingShare(ns, "2026-09-01", "2026-09-30", 4.0).size(), 1u);
}

TEST(ReportCompliance, TheClinicianWindowCountsAgainstTheThresholdItIsGiven) {
    const std::vector<Night> ns = {night("2026-09-01", 300), night("2026-09-02", 200),
                                   night("2026-09-03", 100)};
    const Window w{"w", "2026-09-01", "2026-09-04"};
    EXPECT_EQ(figuresFor(ns, w, 240).days_4h, 1);
    EXPECT_EQ(figuresFor(ns, w, 90).days_4h, 3);
    EXPECT_NEAR(figuresFor(ns, w, 150).pct_total_4h, 50.0, 1e-9);
    EXPECT_EQ(figuresFor(ns, w).days_4h, 1) << "the default is still 4 h";
}

// ── The request (POST /api/reports/generate) ────────────────────────────────

namespace {
Json::Value body(const std::string& kind) {
    Json::Value b(Json::objectValue);
    b["start"] = "2026-09-01";
    b["end"] = "2026-09-30";
    if (!kind.empty()) b["kind"] = kind;
    return b;
}
}  // namespace

TEST(ReportRequests, NoKindIsTheStandardReportAsBefore) {
    ReportRequest r;
    EXPECT_FALSE(parseReportRequest(body(""), 4.0, r));
    EXPECT_EQ(r.kind, "standard");
    EXPECT_EQ(r.language, "en");
    EXPECT_EQ(reportFilename("standard", "2026-09-01", "2026-09-01"),
              "cpap_report_2026-09-01_to_2026-09-01.pdf") << "the name it always had";
}

TEST(ReportRequests, AnUnknownKindIsRefused) {
    ReportRequest r;
    EXPECT_TRUE(parseReportRequest(body("agent_report"), 4.0, r));
    Json::Value b = body("");
    b["kind"] = 3;
    EXPECT_TRUE(parseReportRequest(b, 4.0, r));
}

TEST(ReportRequests, AnAbsentRuleIsTheUsageTargetAndSeventyPercent) {
    ReportRequest r;
    EXPECT_FALSE(parseReportRequest(body("compliance"), 6.0, r));
    EXPECT_EQ(r.kind, "compliance");
    EXPECT_DOUBLE_EQ(r.rule_hours, 6.0) << "B5: the usage target from Settings";
    EXPECT_EQ(r.rule_share, 70);
    Json::Value b = body("compliance");
    b["rule"]["hours"] = 4.5;
    EXPECT_FALSE(parseReportRequest(b, 6.0, r));
    EXPECT_DOUBLE_EQ(r.rule_hours, 4.5);
    EXPECT_EQ(r.rule_share, 70) << "each half by itself";
}

TEST(ReportRequests, ARuleOutsideTheBoundsIsRefused) {
    ReportRequest r;
    const auto with = [&](double h, double s) {
        Json::Value b = body("compliance");
        b["rule"]["hours"] = h;
        b["rule"]["share"] = s;
        return parseReportRequest(b, 4.0, r);
    };
    EXPECT_FALSE(with(0.5, 1));
    EXPECT_FALSE(with(24, 100));
    EXPECT_TRUE(with(0, 70)) << "0 hours";
    EXPECT_TRUE(with(24.5, 70));
    EXPECT_TRUE(with(4, 0));
    EXPECT_TRUE(with(4, 101));
    EXPECT_TRUE(with(4, 70.5)) << "a whole percent";
    Json::Value b = body("compliance");
    b["rule"] = "4h";
    EXPECT_TRUE(parseReportRequest(b, 4.0, r));
}

TEST(ReportRequests, ARuleOnAnotherKindIsIgnoredAndTheLanguageIsOneOfFive) {
    ReportRequest r;
    Json::Value b = body("clinician");
    b["rule"]["hours"] = 0;   // not judged: not a compliance report
    b["language"] = "hu";
    EXPECT_FALSE(parseReportRequest(b, 4.0, r));
    EXPECT_EQ(r.language, "hu");
    b["language"] = "de";
    EXPECT_FALSE(parseReportRequest(b, 4.0, r));
    EXPECT_EQ(r.language, "en") << "anything else is English";
}

TEST(ReportRequests, TheKindRidesOnTheFilenameAsCpapDashNamesIt) {
    EXPECT_EQ(reportFilename("clinician", "2026-09-01", "2026-09-30"),
              "cpap_clinician_report_2026-09-01_to_2026-09-30.pdf");
    EXPECT_EQ(reportFilename("compliance", "2026-09-01", "2026-09-01"),
              "cpap_compliance_report_2026-09-01.pdf");
    EXPECT_EQ(reportKindOfFilename("cpap_clinician_report_2026-09-01_to_2026-09-30.pdf"), "clinician");
    EXPECT_EQ(reportKindOfFilename("cpap_compliance_report_2026-09-01.pdf"), "compliance");
    EXPECT_EQ(reportKindOfFilename("cpap_report_2026-09-01_to_2026-09-30.pdf"), "standard");
    EXPECT_EQ(ruleJsonOf(4.0, 70), "{\"hours\":4,\"share\":70}");
    EXPECT_EQ(ruleJsonOf(4.5, 90), "{\"hours\":4.5,\"share\":90}");
}

// ── The STR's settings (B10) ────────────────────────────────────────────────

TEST(ReportStrSettings, TheFamilyAndThePrescriptionMapAndZeroIsNone) {
    STRDailyRecord r;
    r.family = STRDailyRecord::Family::AutoSet;
    r.min_pressure = 4.0;
    r.max_pressure = 20.0;
    auto s = strSettingsOf(r);
    EXPECT_EQ(s.family, 2);
    EXPECT_DOUBLE_EQ(*s.apap_min, 4.0);
    EXPECT_DOUBLE_EQ(*s.apap_max, 20.0);
    EXPECT_FALSE(s.bl_min_epap);

    STRDailyRecord b;
    b.family = STRDailyRecord::Family::BiLevel;
    b.bl_min_epap = 5.0;
    b.bl_max_ipap = 14.0;
    b.bl_ps = 0.0;
    s = strSettingsOf(b);
    EXPECT_EQ(s.family, 3);
    EXPECT_FALSE(s.apap_min) << "an unset limit is 0 on the record: none";
    EXPECT_DOUBLE_EQ(*s.bl_min_epap, 5.0);
    EXPECT_DOUBLE_EQ(*s.bl_ps, 0.0) << "a pressure support of 0 is a setting";
    EXPECT_EQ(strSettingsOf(STRDailyRecord{}).family, 0);
    EXPECT_EQ(strFamilyCode(STRDailyRecord::Family::Cpap), 1);
    EXPECT_EQ(strFamilyCode(STRDailyRecord::Family::Asv), 4);
}
