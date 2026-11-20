#include <gtest/gtest.h>
#include "agent/AgentTools.h"
#include "AiFactsEngines.h"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace hms_cpap;
using json = nlohmann::json;

// ============================================================================
// Definition / registration / validation tests (no DB required).
// ============================================================================

class AgentToolsTest : public ::testing::Test {
protected:
    AgentTools tools{"test_device_123"};
};

// ── Definition tests ────────────────────────────────────────────────────────

TEST_F(AgentToolsTest, HasSevenTools) {
    EXPECT_EQ(tools.definitions().size(), 7u);
}

TEST_F(AgentToolsTest, AllToolsHaveNames) {
    for (const auto& def : tools.definitions()) {
        EXPECT_FALSE(def.name.empty());
        EXPECT_FALSE(def.description.empty());
        EXPECT_TRUE(def.parameters.contains("type"));
        EXPECT_EQ(def.parameters["type"], "object");
    }
}

TEST_F(AgentToolsTest, AllToolsExposePropertiesAndRequired) {
    for (const auto& def : tools.definitions()) {
        EXPECT_TRUE(def.parameters.contains("properties"))
            << def.name << " missing properties";
        EXPECT_TRUE(def.parameters.contains("required"))
            << def.name << " missing required";
        EXPECT_TRUE(def.parameters["required"].is_array());
    }
}

TEST_F(AgentToolsTest, ToolNamesAreUnique) {
    std::set<std::string> names;
    for (const auto& def : tools.definitions()) {
        EXPECT_TRUE(names.insert(def.name).second)
            << "duplicate tool name: " << def.name;
    }
    EXPECT_EQ(names.size(), 7u);
}

TEST_F(AgentToolsTest, ExpectedToolSetPresent) {
    std::set<std::string> names;
    for (const auto& def : tools.definitions()) names.insert(def.name);
    EXPECT_EQ(names, (std::set<std::string>{
        "get_recent_sessions", "get_session_details", "get_daily_summary",
        "get_trend_data", "compare_periods", "get_vitals", "get_statistics"}));
}

// ── Parameter validation tests ──────────────────────────────────────────────

static const hms::ToolDefinition* findDef(const AgentTools& t, const std::string& name) {
    for (const auto& d : t.definitions()) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

TEST_F(AgentToolsTest, GetRecentSessionsHasOptionalParams) {
    const auto* def = findDef(tools, "get_recent_sessions");
    ASSERT_NE(def, nullptr);
    EXPECT_TRUE(def->parameters["properties"].contains("days"));
    EXPECT_TRUE(def->parameters["properties"].contains("limit"));
    EXPECT_TRUE(def->parameters["required"].empty());
    EXPECT_EQ(def->parameters["properties"]["days"]["type"], "integer");
    EXPECT_EQ(def->parameters["properties"]["limit"]["type"], "integer");
}

TEST_F(AgentToolsTest, GetSessionDetailsRequiresDate) {
    const auto* def = findDef(tools, "get_session_details");
    ASSERT_NE(def, nullptr);
    auto req = def->parameters["required"];
    ASSERT_EQ(req.size(), 1u);
    EXPECT_EQ(req[0], "date");
    EXPECT_EQ(def->parameters["properties"]["date"]["type"], "string");
}

TEST_F(AgentToolsTest, GetDailySummaryRequiresDateRange) {
    const auto* def = findDef(tools, "get_daily_summary");
    ASSERT_NE(def, nullptr);
    auto req = def->parameters["required"];
    ASSERT_EQ(req.size(), 2u);
    EXPECT_EQ(req[0], "start_date");
    EXPECT_EQ(req[1], "end_date");
}

TEST_F(AgentToolsTest, GetTrendDataRequiresMetric) {
    const auto* def = findDef(tools, "get_trend_data");
    ASSERT_NE(def, nullptr);
    auto req = def->parameters["required"];
    ASSERT_EQ(req.size(), 1u);
    EXPECT_EQ(req[0], "metric");
    auto metric_props = def->parameters["properties"]["metric"];
    ASSERT_TRUE(metric_props.contains("enum"));
    auto enums = metric_props["enum"];
    EXPECT_EQ(enums.size(), 6u);
    std::set<std::string> e(enums.begin(), enums.end());
    EXPECT_EQ(e, (std::set<std::string>{"ahi", "pressure", "leak", "spo2", "hr", "duration"}));
}

TEST_F(AgentToolsTest, ComparePeriodsRequiresFourDates) {
    const auto* def = findDef(tools, "compare_periods");
    ASSERT_NE(def, nullptr);
    auto req = def->parameters["required"];
    ASSERT_EQ(req.size(), 4u);
    std::set<std::string> r(req.begin(), req.end());
    EXPECT_EQ(r, (std::set<std::string>{
        "period1_start", "period1_end", "period2_start", "period2_end"}));
}

TEST_F(AgentToolsTest, GetVitalsRequiresDate) {
    const auto* def = findDef(tools, "get_vitals");
    ASSERT_NE(def, nullptr);
    auto req = def->parameters["required"];
    ASSERT_EQ(req.size(), 1u);
    EXPECT_EQ(req[0], "date");
}

TEST_F(AgentToolsTest, GetStatisticsRequiresDateRange) {
    const auto* def = findDef(tools, "get_statistics");
    ASSERT_NE(def, nullptr);
    auto req = def->parameters["required"];
    ASSERT_EQ(req.size(), 2u);
    EXPECT_EQ(req[0], "start_date");
    EXPECT_EQ(req[1], "end_date");
}

// ============================================================================
// SDD-072 D4: every tool on every engine, through IDatabase.
//
// Seeds through the engines' own writers (saveSession, saveSTRDailyRecords,
// aggregateDailySummaryFromSessions), so the schema is the real one. SQLite
// always runs; MySQL and PostgreSQL run when MYSQL_TEST_HOST / PGHOST are set.
// ============================================================================

namespace {

using namespace ai_facts_test;

class AgentToolsEngineTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        device_ = "sdd072_agent_" + std::to_string(::getpid());
        path_ = (std::filesystem::temp_directory_path() /
                 ("hms_cpap_sdd072_agent_" + std::to_string(::getpid()) + ".db")).string();
        std::string skip;
        db_ = openEngine(GetParam(), path_, skip);
        if (!db_) GTEST_SKIP() << skip;
        cleanup(*db_, device_);
        tools_ = std::make_unique<AgentTools>(device_);
        seed();
    }

    void TearDown() override {
        if (db_) cleanup(*db_, device_);
        db_.reset();
        if (GetParam() == Engine::SQLite) removeSqlite(path_);
    }

    json run(const std::string& tool, const json& args = json::object()) {
        return json::parse(tools_->execute(tool, args, *db_));
    }

    void seed() {
        // Three recent nights, 24 h apart, the newest the reporter's (#78).
        const auto now = system_clock::now();
        NightSpec reporter;
        reporter.hypopneas = 3;
        reporter.unclassified = 3;
        saveNight(*db_, device_, now - std::chrono::hours(30), reporter);
        NightSpec mid;
        mid.obstructive = 2;
        mid.reras = 1;
        mid.other = 1;
        saveNight(*db_, device_, now - std::chrono::hours(54), mid);
        saveNight(*db_, device_, now - std::chrono::hours(78), NightSpec{300, 1});

        // A fixed night with its events and vitals: sleep day 2099-07-01.
        const auto start = localAt(2099, 7, 1, 23);
        NightSpec fixed;
        fixed.obstructive = 1;
        fixed.hypopneas = 1;
        std::vector<SleepEvent> events = {
            SleepEvent(EventType::OBSTRUCTIVE, start + minutes(10), 18.0),
            SleepEvent(EventType::HYPOPNEA, start + minutes(80), 12.0)};
        std::vector<VitalSample> vitals;
        for (auto [sec, spo2, hr] : std::vector<std::tuple<int, double, int>>{
                 {5, 95, 60}, {35, 93, 64}, {70, 97, 58}}) {
            VitalSample v(start + std::chrono::seconds(sec));
            v.spo2 = spo2;
            v.heart_rate = hr;
            vitals.push_back(v);
        }
        saveNight(*db_, device_, start, fixed, events, vitals);

        // An ungraded night (SDD-024), and the daily rows of every night.
        NightSpec ungraded;
        ungraded.unclassified = 5;
        ungraded.ungraded = true;
        saveNight(*db_, device_, localAt(2099, 9, 1, 23), ungraded);
        ASSERT_TRUE(db_->aggregateDailySummaryFromSessions(device_));

        // Three STR days with no sessions: the machine's own figures.
        std::vector<STRDailyRecord> days;
        for (int d = 6; d <= 8; ++d) {
            STRDailyRecord r;
            r.device_id = device_;
            r.record_date = localAt(2099, 8, d, 12);
            r.duration_minutes = 420.0 + d;
            r.ahi = 2.0 + d * 0.25;   // 3.5, 3.75, 4.0
            r.hi = 1.0;
            r.leak_95 = 12.0;
            r.mask_press_95 = 10.5;
            r.spo2_50 = 95.0;
            r.resp_rate_50 = 14.0;
            days.push_back(r);
        }
        ASSERT_TRUE(db_->saveSTRDailyRecords(days));
    }

    std::shared_ptr<IDatabase> db_;
    std::unique_ptr<AgentTools> tools_;
    std::string device_;
    std::string path_;
};

void expectNightAddsUp(const json& n) {
    ASSERT_TRUE(n.contains("events_in_ahi")) << n.dump();
    const auto& in = n["events_in_ahi"];
    EXPECT_EQ(in["total"].get<int>(),
              in["obstructive_apnea"].get<int>() + in["central_apnea"].get<int>() +
                  in["clear_airway_apnea"].get<int>() + in["unclassified_apnea"].get<int>() +
                  in["hypopnea"].get<int>());
    EXPECT_EQ(in["total"].get<int>() + n["events_not_in_ahi"]["total"].get<int>(),
              n["total_events"].get<int>())
        << n.dump();
}

TEST_P(AgentToolsEngineTest, RecentSessionsAreNightFactsThatAddUp) {
    const auto j = run("get_recent_sessions");
    ASSERT_TRUE(j.contains("nights")) << j.dump();
    ASSERT_EQ(j["nights"].size(), 3u) << j.dump();
    EXPECT_FALSE(j["capped"].get<bool>());
    for (const auto& n : j["nights"]) expectNightAddsUp(n);
    // Newest first: the reporter's night, its unclassified apneas named.
    EXPECT_EQ(j["nights"][0]["events_in_ahi"]["unclassified_apnea"], 3);
    EXPECT_EQ(j["nights"][0]["events_in_ahi"]["hypopnea"], 3);
    EXPECT_EQ(j["nights"][1]["events_not_in_ahi"]["other_event_types"], 1);
    ASSERT_TRUE(j["rules"].is_array());
    EXPECT_NE(j["rules"].dump().find("never describe it as obstructive"), std::string::npos);
}

TEST_P(AgentToolsEngineTest, ACappedListSaysSo) {
    const auto j = run("get_recent_sessions", {{"days", 7}, {"limit", 2}});
    ASSERT_EQ(j["nights"].size(), 2u) << j.dump();
    EXPECT_TRUE(j["capped"].get<bool>());
    EXPECT_EQ(j["available"], 3);
    EXPECT_EQ(j["returned"], 2);
    EXPECT_TRUE(j.contains("capped_note"));
}

TEST_P(AgentToolsEngineTest, SessionDetailsCarryTheNightAndItsEvents) {
    const auto j = run("get_session_details", {{"date", "2099-07-01"}});
    ASSERT_TRUE(j["found"].get<bool>()) << j.dump();
    expectNightAddsUp(j);
    EXPECT_EQ(j["total_events"], 2);
    ASSERT_EQ(j["event_list"].size(), 2u) << j.dump();
    EXPECT_FALSE(j["event_list_capped"].get<bool>());
    EXPECT_NEAR(j["event_list"][0]["duration_s"].get<double>(), 18.0, 1e-9);
    EXPECT_NE(j["event_list"][0]["time"].get<std::string>().find("2099-07-01"),
              std::string::npos);
}

TEST_P(AgentToolsEngineTest, SessionDetailsSayWhenNoNightIsStored) {
    const auto j = run("get_session_details", {{"date", "2099-01-15"}});
    EXPECT_FALSE(j["found"].get<bool>()) << j.dump();
    EXPECT_FALSE(j.contains("ahi"));
}

TEST_P(AgentToolsEngineTest, AnUngradedNightIsNotGradedByAnyTool) {
    const auto night = run("get_session_details", {{"date", "2099-09-01"}});
    ASSERT_TRUE(night["found"].get<bool>()) << night.dump();
    EXPECT_TRUE(night["ahi"].is_string()) << night.dump();
    EXPECT_FALSE(night.contains("events_in_ahi"));
    EXPECT_EQ(night["total_events"], 5);

    const auto day = run("get_daily_summary", {{"start_date", "2099-09-01"},
                                               {"end_date", "2099-09-01"}});
    ASSERT_EQ(day["days"].size(), 1u) << day.dump();
    EXPECT_TRUE(day["days"][0]["ahi"].is_string())
        << "the daily row carries the night's kind on every engine: " << day.dump();
    EXPECT_FALSE(day["days"][0].contains("hypopnea_index"));
    EXPECT_NE(day["rules"].dump().find("does not detect hypopneas"), std::string::npos);

    const auto stats = run("get_statistics", {{"start_date", "2099-09-01"},
                                              {"end_date", "2099-09-01"}});
    EXPECT_TRUE(stats["avg_ahi"].is_string()) << stats.dump();
}

TEST_P(AgentToolsEngineTest, DailySummaryIsTheMachinesDays) {
    const auto j = run("get_daily_summary", {{"start_date", "2099-08-06"},
                                             {"end_date", "2099-08-08"}});
    ASSERT_EQ(j["days"].size(), 3u) << j.dump();
    EXPECT_EQ(j["days"][0]["date"], "2099-08-06");
    EXPECT_NEAR(j["days"][0]["ahi"].get<double>(), 3.5, 1e-9);
    EXPECT_NEAR(j["days"][2]["duration_minutes"].get<double>(), 428.0, 1e-9);
    EXPECT_NEAR(j["days"][0]["spo2_median_pct"].get<double>(), 95.0, 1e-9);
}

TEST_P(AgentToolsEngineTest, TrendDataReadsTheDailyRows) {
    const auto j = run("get_trend_data", {{"metric", "duration"}, {"days", 7}});
    ASSERT_TRUE(j["points"].is_array()) << j.dump();
    // The last 7 days: the three recent nights' rows, none of the 2099 ones.
    ASSERT_EQ(j["points"].size(), 3u) << j.dump();
    EXPECT_NEAR(j["points"][0]["duration_minutes"].get<double>(), 300.0, 1e-9);
    EXPECT_NEAR(j["points"][2]["duration_minutes"].get<double>(), 360.0, 1e-9);

    const auto bad = run("get_trend_data", {{"metric", "bogus"}});
    ASSERT_TRUE(bad.contains("error"));
    EXPECT_NE(bad["error"].get<std::string>().find("Unknown metric"), std::string::npos);
}

TEST_P(AgentToolsEngineTest, ComparePeriodsAndStatisticsAreComputedFromTheRows) {
    const auto c = run("compare_periods", {{"period1_start", "2099-08-06"},
                                           {"period1_end", "2099-08-07"},
                                           {"period2_start", "2099-08-08"},
                                           {"period2_end", "2099-08-08"}});
    EXPECT_EQ(c["period1"]["nights"], 2) << c.dump();
    EXPECT_NEAR(c["period1"]["avg_ahi"].get<double>(), 3.63, 1e-9);   // (3.5 + 3.75) / 2
    EXPECT_NEAR(c["period1"]["avg_duration_min"].get<double>(), 426.5, 1e-9);
    EXPECT_NEAR(c["period1"]["compliance_pct"].get<double>(), 100.0, 1e-9);
    EXPECT_NEAR(c["period1"]["avg_pressure_95"].get<double>(), 10.5, 1e-9);
    EXPECT_EQ(c["period2"]["nights"], 1);
    EXPECT_EQ(c["period1"]["period"], "2099-08-06 to 2099-08-07");

    const auto s = run("get_statistics", {{"start_date", "2099-08-06"},
                                          {"end_date", "2099-08-08"}});
    EXPECT_EQ(s["total_nights"], 3) << s.dump();
    EXPECT_NEAR(s["avg_ahi"].get<double>(), 3.75, 1e-9);
    EXPECT_NEAR(s["min_ahi"].get<double>(), 3.5, 1e-9);
    EXPECT_NEAR(s["max_ahi"].get<double>(), 4.0, 1e-9);
    EXPECT_NEAR(s["stddev_ahi"].get<double>(), 0.25, 1e-9);
    EXPECT_NEAR(s["total_therapy_minutes"].get<double>(), 1281.0, 1e-9);
}

TEST_P(AgentToolsEngineTest, VitalsAreOneRowPerMinute) {
    const auto j = run("get_vitals", {{"date", "2099-07-01"}});
    ASSERT_EQ(j["minutes"].size(), 2u) << j.dump();
    EXPECT_NEAR(j["minutes"][0]["avg_spo2"].get<double>(), 94.0, 1e-9);
    EXPECT_NEAR(j["minutes"][0]["max_hr"].get<double>(), 64.0, 1e-9);
    EXPECT_NE(j["minutes"][0]["minute"].get<std::string>().find(":00:00"), std::string::npos)
        << j.dump();
}

TEST_P(AgentToolsEngineTest, BadArgumentsAndUnknownToolsComeBackAsErrors) {
    EXPECT_TRUE(run("get_vitals", {{"date", "07/01/2099"}}).contains("error"));
    EXPECT_TRUE(run("get_session_details").contains("error"));
    const auto u = run("totally_made_up");
    ASSERT_TRUE(u.contains("error"));
    EXPECT_NE(u["error"].get<std::string>().find("Unknown tool: totally_made_up"),
              std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(Engines, AgentToolsEngineTest,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
                         engineParamName);

}  // namespace
