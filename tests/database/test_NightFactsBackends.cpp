// SDD-072 D1: a night's facts, read from each engine, add up.
//
// The reporter's night (3 hypopneas, 3 unclassified apneas) reached the model
// as "Total events: 6 (obstructive=0, central=0, hypopnea=3, RERA=0)". The
// facts every AI path renders from now name each type the index counts, and
// their parts equal the night's total_events, on SQLite, MySQL and
// PostgreSQL. The range query a weekly summary and the agent read gives the
// same night as the single-night query the daily summary reads.
#include "AiFactsEngines.h"

#include "services/NightFacts.h"

#include <cpapdash/parser/VLDParser.h>

#include <cmath>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace hms_cpap;
using namespace ai_facts_test;

namespace {

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void expectAddsUp(const NightFacts& f) {
    ASSERT_TRUE(f.graded);
    EXPECT_EQ(f.inIndex(),
              f.obstructive + f.central + f.clear_airway + f.unclassified + f.hypopnea);
    EXPECT_EQ(f.inIndex() + f.outsideIndex(), f.total)
        << "the parts the model is given must equal the total it is given";
}

class NightFactsBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        device_ = "sdd072_facts_" + std::to_string(::getpid());
        path_ = (std::filesystem::temp_directory_path() /
                 ("hms_cpap_sdd072_facts_" + std::to_string(::getpid()) + ".db")).string();
        std::string skip;
        db_ = openEngine(GetParam(), path_, skip);
        if (!db_) GTEST_SKIP() << skip;
        cleanup(*db_, device_, oxi_files_);
    }

    void TearDown() override {
        if (db_) cleanup(*db_, device_, oxi_files_);
        db_.reset();
        if (GetParam() == Engine::SQLite) removeSqlite(path_);
    }

    // 23:00 local on 2099-07-0[day]: the night's sleep day is that date.
    static system_clock::time_point night(int day) { return localAt(2099, 7, day, 23); }
    static std::string dayOf(int day) { return "2099-07-0" + std::to_string(day); }

    std::shared_ptr<IDatabase> db_;
    std::string device_;
    std::string path_;
    std::vector<std::string> oxi_files_{"sdd072_facts_" + std::to_string(::getpid()) + ".vld"};
};

TEST_P(NightFactsBackendTest, TheReportersNightAddsUp) {
    NightSpec n;
    n.hypopneas = 3;
    n.unclassified = 3;
    saveNight(*db_, device_, night(1), n);

    const auto f = night_facts::loadNight(*db_, device_, dayOf(1));
    ASSERT_TRUE(f) << engineName(GetParam());
    expectAddsUp(*f);
    EXPECT_EQ(f->unclassified, 3);
    EXPECT_EQ(f->hypopnea, 3);
    EXPECT_EQ(f->obstructive, 0);
    EXPECT_EQ(f->total, 6);
    EXPECT_NEAR(f->index, 1.0, 0.01);   // 6 events over 6 hours

    const auto text = night_facts::renderNight(*f);
    EXPECT_TRUE(has(text, "counted in the AHI): 6 (obstructive apnea=0, central apnea=0, "
                          "clear airway apnea=0, unclassified apnea=3, hypopnea=3)"))
        << text;
    EXPECT_TRUE(has(text, "Total events recorded: 6")) << text;
}

TEST_P(NightFactsBackendTest, ClearAirwayReraAndOtherEventsAddUp) {
    NightSpec n;
    n.obstructive = 1;
    n.clear_airway = 2;
    n.unclassified = 3;
    n.hypopneas = 3;
    n.reras = 1;
    n.other = 2;   // CSR, flow limitation, ...: in total_events, not in the AHI
    saveNight(*db_, device_, night(2), n);

    const auto f = night_facts::loadNight(*db_, device_, dayOf(2));
    ASSERT_TRUE(f) << engineName(GetParam());
    expectAddsUp(*f);
    EXPECT_EQ(f->inIndex(), 9);
    EXPECT_EQ(f->reras, 1);
    EXPECT_EQ(f->other, 2);
    EXPECT_EQ(f->total, 12);
}

TEST_P(NightFactsBackendTest, AnUngradedNightCarriesNoIndexAndNoPerTypeZeros) {
    NightSpec n;
    n.unclassified = 5;
    n.ungraded = true;   // SDD-024: a machine that detects no hypopneas
    saveNight(*db_, device_, night(3), n);

    const auto f = night_facts::loadNight(*db_, device_, dayOf(3));
    ASSERT_TRUE(f) << engineName(GetParam());
    EXPECT_FALSE(f->graded) << "the range query must carry the index kind";
    EXPECT_EQ(f->total, 5);
    EXPECT_EQ(f->inIndex(), 0);

    const auto text = night_facts::renderNight(*f);
    EXPECT_TRUE(has(text, "AHI: not available")) << text;
    EXPECT_FALSE(has(text, "hypopnea=")) << text;
    EXPECT_FALSE(has(text, "events/hour")) << text;

    const auto j = night_facts::toJson(*f);
    EXPECT_TRUE(j["ahi"].is_string()) << j.dump();
    EXPECT_FALSE(j.contains("events_in_ahi")) << j.dump();
}

TEST_P(NightFactsBackendTest, ANightWithNoStrRowSaysNothingOfOne) {
    saveNight(*db_, device_, night(4), NightSpec{});
    const auto f = night_facts::loadNight(*db_, device_, dayOf(4));
    ASSERT_TRUE(f) << engineName(GetParam());
    EXPECT_FALSE(f->str);
    ASSERT_TRUE(f->usage_hours);
    EXPECT_NEAR(*f->usage_hours, 6.0, 0.01);
    const auto text = night_facts::renderNight(*f);
    EXPECT_FALSE(has(text, "official daily summary")) << text;
    EXPECT_FALSE(has(text, "O2 Ring")) << "no oximetry stored, none said";
}

TEST_P(NightFactsBackendTest, ANightWithOximetryCarriesItsOwn) {
    saveNight(*db_, device_, night(5), NightSpec{});

    cpapdash::parser::OximetrySession o;
    o.filename = oxi_files_.front();
    std::tm tm{};
    tm.tm_year = 2099 - 1900;
    tm.tm_mon = 6;
    tm.tm_mday = 5;
    tm.tm_hour = 12;
#ifdef _WIN32
    o.start_time = system_clock::from_time_t(_mkgmtime(&tm));
#else
    o.start_time = system_clock::from_time_t(timegm(&tm));
#endif
    o.end_time = o.start_time + std::chrono::hours(6);
    o.duration_seconds = 6 * 3600;
    o.sample_interval = 4.0;
    o.metrics.avg_spo2 = 94.5;
    o.metrics.min_spo2 = 88.0;
    o.metrics.spo2_baseline = 96.0;
    o.metrics.odi_3pct = 3.25;
    o.metrics.time_below_90_pct = 1.5;
    o.metrics.time_below_88_pct = 0.5;
    o.metrics.avg_hr = 61.0;
    o.metrics.min_hr = 52;
    o.metrics.max_hr = 88;
    o.metrics.valid_samples = 3;
    o.metrics.total_samples = 3;
    for (int i = 0; i < 3; ++i) {
        cpapdash::parser::OximetrySample smp{};
        smp.timestamp = o.start_time + std::chrono::seconds(i * 4);
        smp.spo2 = 95;
        smp.heart_rate = 60;
        o.samples.push_back(smp);
    }
    ASSERT_TRUE(db_->saveOximetrySession(kOximetryDeviceId, o));

    const auto f = night_facts::loadNight(*db_, device_, dayOf(5));
    ASSERT_TRUE(f) << engineName(GetParam());
    ASSERT_TRUE(f->oximetry) << "the night's own ring summary";
    EXPECT_NEAR(f->oximetry->avg_spo2, 94.5, 0.05);
    EXPECT_NEAR(f->oximetry->odi_3pct, 3.25, 0.05);
    const auto text = night_facts::renderNight(*f);
    EXPECT_TRUE(has(text, "Avg SpO2: 94.50%")) << text;
    EXPECT_TRUE(has(text, "ODI (3%): 3.25 events/hour")) << text;
    EXPECT_NEAR(night_facts::toJson(*f)["oximetry"]["avg_spo2_pct"].get<double>(), 94.5, 1e-9);
}

TEST_P(NightFactsBackendTest, TheRangeQueryGivesTheNightTheDailyQueryGives) {
    NightSpec n;
    n.obstructive = 2;
    n.unclassified = 1;
    n.hypopneas = 4;
    n.reras = 1;
    saveNight(*db_, device_, night(6), n);
    saveNight(*db_, device_, night(6) + std::chrono::hours(7), NightSpec{30, 1});

    const auto nightly = db_->getNightlyMetrics(device_, night(6));
    ASSERT_TRUE(nightly) << engineName(GetParam());
    const auto daily = NightFacts::from(*nightly);
    const auto ranged = night_facts::loadNight(*db_, device_, dayOf(6));
    ASSERT_TRUE(ranged);

    EXPECT_EQ(ranged->graded, daily.graded);
    EXPECT_EQ(ranged->total, daily.total);
    EXPECT_EQ(ranged->inIndex(), daily.inIndex());
    EXPECT_EQ(ranged->unclassified, daily.unclassified);
    EXPECT_EQ(ranged->reras, daily.reras);
    EXPECT_EQ(ranged->other, daily.other);
    EXPECT_NEAR(ranged->index, daily.index, 1e-6);
    ASSERT_TRUE(ranged->usage_hours && daily.usage_hours);
    EXPECT_NEAR(*ranged->usage_hours, *daily.usage_hours, 1e-6);
    EXPECT_EQ(ranged->therapy_pressure.has_value(), daily.therapy_pressure.has_value());
    expectAddsUp(*ranged);
    EXPECT_EQ(ranged->total, 8 + 1);   // both sessions of the night
}

TEST_P(NightFactsBackendTest, ARangeIsItsNightsOldestFirstEachAddingUp) {
    NightSpec a;
    a.hypopneas = 2;
    a.unclassified = 1;
    NightSpec b;
    b.central = 3;
    b.other = 1;
    saveNight(*db_, device_, night(7), a);
    saveNight(*db_, device_, night(8), b);

    const auto nights = night_facts::loadRange(*db_, device_, dayOf(7), dayOf(8));
    ASSERT_EQ(nights.size(), 2u) << engineName(GetParam());
    EXPECT_EQ(nights[0].sleep_day, dayOf(7));
    EXPECT_EQ(nights[1].sleep_day, dayOf(8));
    for (const auto& f : nights) expectAddsUp(f);
    EXPECT_EQ(nights[1].other, 1);

    const auto text = night_facts::renderRange(nights, "Weekly");
    EXPECT_TRUE(has(text, dayOf(7) + " | 6.00h | AHI 0.50 | events 3 (in AHI 3: OA=0 CA=0 "
                                     "CLR=0 UA=1 H=2; not in AHI 0: RERA=0 other=0)"))
        << text;
}

INSTANTIATE_TEST_SUITE_P(Engines, NightFactsBackendTest,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
                         engineParamName);

}  // namespace
