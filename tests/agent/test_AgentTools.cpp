#include <gtest/gtest.h>
#include "agent/AgentTools.h"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <unistd.h>

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

TEST_F(AgentToolsTest, HasToolReturnsTrueForValidNames) {
    EXPECT_TRUE(tools.hasTool("get_recent_sessions"));
    EXPECT_TRUE(tools.hasTool("get_session_details"));
    EXPECT_TRUE(tools.hasTool("get_daily_summary"));
    EXPECT_TRUE(tools.hasTool("get_trend_data"));
    EXPECT_TRUE(tools.hasTool("compare_periods"));
    EXPECT_TRUE(tools.hasTool("get_vitals"));
    EXPECT_TRUE(tools.hasTool("get_statistics"));
}

TEST_F(AgentToolsTest, HasToolReturnsFalseForInvalidName) {
    EXPECT_FALSE(tools.hasTool("nonexistent_tool"));
    EXPECT_FALSE(tools.hasTool(""));
    EXPECT_FALSE(tools.hasTool("get_recent_session"));  // singular
    EXPECT_FALSE(tools.hasTool("GET_RECENT_SESSIONS"));  // case-sensitive
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
// execute() dispatch / error branches reachable WITHOUT a usable DB.
//
// execute() catches std::exception and returns {"error": ...}. The "unknown
// tool" branch never touches the connection. get_trend_data validates the
// metric and early-returns BEFORE opening a pqxx::work, so an unknown metric
// is also reachable without a working transaction.
// ============================================================================

namespace {

std::string envOr(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : def;
}

std::string testDbName() { return envOr("PGDATABASE", "cpap_monitoring"); }

std::string makeConnInfo(const std::string& dbname,
                         const std::string& search_path = "") {
    std::string ci = "host=" + envOr("PGHOST", "localhost") +
                     " port=" + envOr("PGPORT", "5432") +
                     " user=" + envOr("PGUSER", "maestro") +
                     " password=" + envOr("PGPASSWORD", "REDACTED") +
                     " dbname=" + dbname +
                     " connect_timeout=3";
    if (!search_path.empty()) {
        ci += " options=-csearch_path=" + search_path;
    }
    return ci;
}

bool serverUsable() {
    try {
        pqxx::connection c(makeConnInfo(testDbName()));
        return c.is_open();
    } catch (...) {
        return false;
    }
}

} // namespace

// Unknown-tool dispatch: returns an error envelope and never touches the conn,
// so we deliberately use a connection that may or may not be open — either way
// the unknown-tool branch fires first.
TEST(AgentToolsDispatchTest, UnknownToolReturnsErrorWithoutTouchingDb) {
    AgentTools tools{"dev"};
    // A broken/unconnectable conninfo: construction may throw, but if the DB
    // happens to be reachable the connection is harmless (never used).
    try {
        pqxx::connection conn(makeConnInfo("definitely_not_a_real_db_zzz"));
        auto j = json::parse(tools.execute("totally_made_up", {}, conn));
        ASSERT_TRUE(j.contains("error"));
        EXPECT_NE(j["error"].get<std::string>().find("Unknown tool: totally_made_up"),
                  std::string::npos);
    } catch (const std::exception&) {
        // Couldn't even open the bogus connection — that's fine; the dispatch
        // branch is also exercised in the DB-backed suite below.
        SUCCEED();
    }
}

// ============================================================================
// DB-backed tool execution. Creates a throwaway schema in cpap_monitoring,
// seeds deterministic rows, executes each tool, and asserts on the JSON shape.
// If no usable PostgreSQL is present, the whole suite GTEST_SKIPs.
// ============================================================================

class AgentToolsDbTest : public ::testing::Test {
protected:
    static constexpr const char* kDevice = "agent_tools_test_dev";

    void SetUp() override {
        if (!serverUsable()) {
            GTEST_SKIP() << "No usable PostgreSQL — skipping AgentTools DB tests.";
        }
        static int counter = 0;
        schema_ = "agt_test_" + std::to_string(::getpid()) + "_" +
                  std::to_string(counter++);

        // Bootstrap connection (no search_path) to create the schema.
        {
            pqxx::connection boot(makeConnInfo(testDbName()));
            pqxx::work txn(boot);
            txn.exec("CREATE SCHEMA IF NOT EXISTS " + txn.esc(schema_));
            txn.commit();
        }

        conn_ = std::make_unique<pqxx::connection>(makeConnInfo(testDbName(), schema_));
        applySchema();
        seed();
    }

    void TearDown() override {
        conn_.reset();
        if (!schema_.empty() && serverUsable()) {
            try {
                pqxx::connection c(makeConnInfo(testDbName()));
                pqxx::work txn(c);
                txn.exec("DROP SCHEMA IF EXISTS " + txn.esc(schema_) + " CASCADE");
                txn.commit();
            } catch (...) { /* best effort */ }
        }
    }

    void applySchema() {
        pqxx::work txn(*conn_);
        txn.exec(R"SQL(
            CREATE TABLE cpap_sessions (
                id SERIAL PRIMARY KEY,
                device_id TEXT NOT NULL,
                session_start TIMESTAMP NOT NULL,
                session_end TIMESTAMP,
                duration_seconds INT DEFAULT 0
            );
            CREATE TABLE cpap_session_metrics (
                id SERIAL PRIMARY KEY,
                session_id INT NOT NULL,
                total_events INT, ahi FLOAT,
                obstructive_apneas INT, central_apneas INT, hypopneas INT,
                reras INT, clear_airway_apneas INT,
                avg_event_duration FLOAT, max_event_duration FLOAT,
                time_in_apnea_percent FLOAT,
                avg_spo2 FLOAT, min_spo2 FLOAT,
                avg_heart_rate INT, min_heart_rate INT, max_heart_rate INT
            );
            CREATE TABLE cpap_events (
                id SERIAL PRIMARY KEY,
                session_id INT NOT NULL,
                event_type TEXT,
                event_timestamp TIMESTAMP NOT NULL,
                duration_seconds FLOAT,
                details TEXT
            );
            CREATE TABLE cpap_vitals (
                id SERIAL PRIMARY KEY,
                session_id INT NOT NULL,
                timestamp TIMESTAMP NOT NULL,
                spo2 FLOAT, heart_rate INT
            );
            CREATE TABLE cpap_daily_summary (
                id SERIAL PRIMARY KEY,
                device_id TEXT NOT NULL,
                record_date DATE NOT NULL,
                duration_minutes FLOAT,
                ahi FLOAT, hi FLOAT, ai FLOAT, oai FLOAT, cai FLOAT, uai FLOAT, rin FLOAT,
                leak_50 FLOAT, leak_95 FLOAT, leak_max FLOAT,
                mask_press_50 FLOAT, mask_press_95 FLOAT, mask_press_max FLOAT,
                spo2_50 FLOAT, spo2_95 FLOAT,
                resp_rate_50 FLOAT, tid_vol_50 FLOAT, min_vent_50 FLOAT,
                mode INT, epr_level FLOAT, pressure_setting FLOAT
            );
        )SQL");
        txn.commit();
    }

    // Deterministic seed. A recent session (NOW()-1day) so day-window queries
    // capture it, plus a session on a fixed sleep-day for date lookups, plus
    // daily_summary rows on fixed dates.
    void seed() {
        pqxx::work txn(*conn_);

        // Recent session for get_recent_sessions (uses NOW() window).
        auto recent = txn.exec_params(
            "INSERT INTO cpap_sessions(device_id, session_start, session_end, duration_seconds)"
            " VALUES ($1, NOW() - INTERVAL '1 day', NOW() - INTERVAL '1 day' + INTERVAL '7 hours', 25200)"
            " RETURNING id", kDevice);
        int recent_id = recent[0][0].as<int>();
        txn.exec_params(
            "INSERT INTO cpap_session_metrics(session_id, total_events, ahi,"
            " obstructive_apneas, central_apneas, hypopneas, reras, clear_airway_apneas,"
            " avg_event_duration, max_event_duration, time_in_apnea_percent,"
            " avg_spo2, min_spo2, avg_heart_rate, min_heart_rate, max_heart_rate)"
            " VALUES ($1, 12, 3.5, 4, 1, 6, 1, 1, 15.0, 30.0, 2.5, 95.0, 88.0, 60, 52, 78)",
            recent_id);

        // Fixed sleep-day session: session_start such that
        // DATE(session_start - INTERVAL '12 hours') = '2025-02-06'.
        // 2025-02-06 22:00:00 -> minus 12h -> 2025-02-06 10:00:00 -> date 2025-02-06.
        auto fixed = txn.exec_params(
            "INSERT INTO cpap_sessions(device_id, session_start, session_end, duration_seconds)"
            " VALUES ($1, TIMESTAMP '2025-02-06 22:00:00', TIMESTAMP '2025-02-07 05:00:00', 25200)"
            " RETURNING id", kDevice);
        int fixed_id = fixed[0][0].as<int>();
        txn.exec_params(
            "INSERT INTO cpap_session_metrics(session_id, total_events, ahi,"
            " obstructive_apneas, central_apneas, hypopneas, reras, clear_airway_apneas,"
            " avg_spo2, min_spo2, avg_heart_rate, min_heart_rate, max_heart_rate)"
            " VALUES ($1, 9, 2.1, 3, 0, 5, 1, 0, 96.0, 90.0, 58, 50, 72)",
            fixed_id);

        // Two events for the fixed session.
        txn.exec_params(
            "INSERT INTO cpap_events(session_id, event_type, event_timestamp, duration_seconds, details)"
            " VALUES ($1, 'Obstructive', TIMESTAMP '2025-02-06 23:10:00', 18.0, 'detail-a'),"
            "        ($1, 'Hypopnea', TIMESTAMP '2025-02-07 01:20:00', 12.0, NULL)",
            fixed_id);

        // Vitals for the fixed session (two distinct minutes).
        txn.exec_params(
            "INSERT INTO cpap_vitals(session_id, timestamp, spo2, heart_rate) VALUES"
            " ($1, TIMESTAMP '2025-02-06 23:00:05', 95, 60),"
            " ($1, TIMESTAMP '2025-02-06 23:00:35', 93, 64),"
            " ($1, TIMESTAMP '2025-02-06 23:01:10', 97, 58)",
            fixed_id);

        // Daily summaries: three fixed dates.
        for (int d = 6; d <= 8; ++d) {
            txn.exec_params(
                "INSERT INTO cpap_daily_summary(device_id, record_date, duration_minutes,"
                " ahi, hi, ai, oai, cai, uai, rin, leak_50, leak_95, leak_max,"
                " mask_press_50, mask_press_95, mask_press_max, spo2_50, spo2_95,"
                " resp_rate_50, tid_vol_50, min_vent_50, mode, epr_level, pressure_setting)"
                " VALUES ($1, $2, $3, 2.5, 1.0, 1.5, 1.0, 0.5, 0.0, 0.5,"
                " 5.0, 12.0, 20.0, 8.0, 10.5, 12.0, 95.0, 91.0, 14.0, 450.0, 6.3,"
                " 11, 2.0, 7.5)",
                kDevice, "2025-02-0" + std::to_string(d), 420.0 + d);
        }

        txn.commit();
    }

    std::unique_ptr<pqxx::connection> conn_;
    std::string schema_;
    AgentTools tools_{kDevice};
};

// ── get_recent_sessions ──────────────────────────────────────────────────────

TEST_F(AgentToolsDbTest, GetRecentSessionsDefaultArgsReturnsRecent) {
    auto out = tools_.execute("get_recent_sessions", json::object(), *conn_);
    auto j = json::parse(out);
    ASSERT_TRUE(j.is_array());
    ASSERT_GE(j.size(), 1u);
    // Recent session has ahi 3.5 and known event counts; values come back as strings.
    bool found = false;
    for (const auto& row : j) {
        ASSERT_TRUE(row.contains("ahi"));
        ASSERT_TRUE(row.contains("duration_hours"));
        if (row["ahi"].is_string() && row["ahi"].get<std::string>().rfind("3.5", 0) == 0)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(AgentToolsDbTest, GetRecentSessionsRespectsLimit) {
    auto j = json::parse(tools_.execute(
        "get_recent_sessions", json{{"days", 3650}, {"limit", 1}}, *conn_));
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 1u);
}

TEST_F(AgentToolsDbTest, GetRecentSessionsNarrowWindowExcludesFixedSession) {
    // days=2 keeps the recent (NOW()-1d) session; the fixed 2025-02-06 session
    // is far in the past and excluded.
    auto j = json::parse(tools_.execute(
        "get_recent_sessions", json{{"days", 2}, {"limit", 50}}, *conn_));
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 1u);
}

// ── get_session_details ──────────────────────────────────────────────────────

TEST_F(AgentToolsDbTest, GetSessionDetailsReturnsSessionWithEvents) {
    auto j = json::parse(tools_.execute(
        "get_session_details", json{{"date", "2025-02-06"}}, *conn_));
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 1u);
    const auto& s = j[0];
    EXPECT_TRUE(s.contains("ahi"));
    ASSERT_TRUE(s.contains("events"));
    ASSERT_TRUE(s["events"].is_array());
    EXPECT_EQ(s["events"].size(), 2u);
    // NULL details column should serialize as JSON null, not the string "null".
    bool sawNull = false;
    for (const auto& ev : s["events"]) {
        ASSERT_TRUE(ev.contains("event_type"));
        if (ev["details"].is_null()) sawNull = true;
    }
    EXPECT_TRUE(sawNull);
}

TEST_F(AgentToolsDbTest, GetSessionDetailsUnknownDateReturnsEmptyArray) {
    auto j = json::parse(tools_.execute(
        "get_session_details", json{{"date", "1999-01-01"}}, *conn_));
    ASSERT_TRUE(j.is_array());
    EXPECT_TRUE(j.empty());
}

TEST_F(AgentToolsDbTest, GetSessionDetailsMissingDateArgReturnsError) {
    auto j = json::parse(tools_.execute("get_session_details", json::object(), *conn_));
    ASSERT_TRUE(j.contains("error"));
}

// ── get_daily_summary ────────────────────────────────────────────────────────

TEST_F(AgentToolsDbTest, GetDailySummaryReturnsRangeOrdered) {
    auto j = json::parse(tools_.execute(
        "get_daily_summary",
        json{{"start_date", "2025-02-06"}, {"end_date", "2025-02-08"}}, *conn_));
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 3u);
    EXPECT_TRUE(j[0].contains("record_date"));
    EXPECT_TRUE(j[0].contains("mode"));
    EXPECT_TRUE(j[0].contains("pressure_setting"));
}

TEST_F(AgentToolsDbTest, GetDailySummaryNarrowedRange) {
    auto j = json::parse(tools_.execute(
        "get_daily_summary",
        json{{"start_date", "2025-02-07"}, {"end_date", "2025-02-07"}}, *conn_));
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 1u);
}

TEST_F(AgentToolsDbTest, GetDailySummaryMissingArgReturnsError) {
    auto j = json::parse(tools_.execute(
        "get_daily_summary", json{{"start_date", "2025-02-06"}}, *conn_));
    ASSERT_TRUE(j.contains("error"));
}

// ── get_trend_data ───────────────────────────────────────────────────────────

TEST_F(AgentToolsDbTest, GetTrendDataAhiColumns) {
    auto j = json::parse(tools_.execute(
        "get_trend_data", json{{"metric", "ahi"}, {"days", 3650}}, *conn_));
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 3u);
    EXPECT_TRUE(j[0].contains("ahi"));
    EXPECT_TRUE(j[0].contains("oai"));
    EXPECT_TRUE(j[0].contains("cai"));
}

TEST_F(AgentToolsDbTest, GetTrendDataPressureColumns) {
    auto j = json::parse(tools_.execute(
        "get_trend_data", json{{"metric", "pressure"}, {"days", 3650}}, *conn_));
    ASSERT_TRUE(j.is_array());
    ASSERT_GE(j.size(), 1u);
    EXPECT_TRUE(j[0].contains("mask_press_95"));
    EXPECT_TRUE(j[0].contains("pressure_setting"));
}

TEST_F(AgentToolsDbTest, GetTrendDataLeakColumns) {
    auto j = json::parse(tools_.execute(
        "get_trend_data", json{{"metric", "leak"}, {"days", 3650}}, *conn_));
    ASSERT_GE(j.size(), 1u);
    EXPECT_TRUE(j[0].contains("leak_50"));
    EXPECT_TRUE(j[0].contains("leak_max"));
}

TEST_F(AgentToolsDbTest, GetTrendDataSpo2Columns) {
    auto j = json::parse(tools_.execute(
        "get_trend_data", json{{"metric", "spo2"}, {"days", 3650}}, *conn_));
    ASSERT_GE(j.size(), 1u);
    EXPECT_TRUE(j[0].contains("spo2_50"));
    EXPECT_TRUE(j[0].contains("spo2_95"));
}

TEST_F(AgentToolsDbTest, GetTrendDataHrColumns) {
    auto j = json::parse(tools_.execute(
        "get_trend_data", json{{"metric", "hr"}, {"days", 3650}}, *conn_));
    ASSERT_GE(j.size(), 1u);
    EXPECT_TRUE(j[0].contains("resp_rate_50"));
}

TEST_F(AgentToolsDbTest, GetTrendDataDurationColumns) {
    auto j = json::parse(tools_.execute(
        "get_trend_data", json{{"metric", "duration"}, {"days", 3650}}, *conn_));
    ASSERT_GE(j.size(), 1u);
    EXPECT_TRUE(j[0].contains("duration_minutes"));
}

TEST_F(AgentToolsDbTest, GetTrendDataUnknownMetricReturnsError) {
    auto j = json::parse(tools_.execute(
        "get_trend_data", json{{"metric", "bogus"}, {"days", 30}}, *conn_));
    ASSERT_TRUE(j.contains("error"));
    EXPECT_NE(j["error"].get<std::string>().find("Unknown metric"), std::string::npos);
}

TEST_F(AgentToolsDbTest, GetTrendDataMissingMetricReturnsError) {
    auto j = json::parse(tools_.execute("get_trend_data", json::object(), *conn_));
    ASSERT_TRUE(j.contains("error"));
}

// ── compare_periods ──────────────────────────────────────────────────────────

TEST_F(AgentToolsDbTest, ComparePeriodsReturnsTwoLabeledPeriods) {
    auto j = json::parse(tools_.execute("compare_periods", json{
        {"period1_start", "2025-02-06"}, {"period1_end", "2025-02-07"},
        {"period2_start", "2025-02-08"}, {"period2_end", "2025-02-08"}}, *conn_));
    ASSERT_TRUE(j.contains("period1"));
    ASSERT_TRUE(j.contains("period2"));
    EXPECT_EQ(j["period1"]["period"], "2025-02-06 to 2025-02-07");
    EXPECT_EQ(j["period2"]["period"], "2025-02-08 to 2025-02-08");
    // period1 covers 2 nights, period2 covers 1.
    EXPECT_EQ(j["period1"]["nights"].get<std::string>(), "2");
    EXPECT_EQ(j["period2"]["nights"].get<std::string>(), "1");
    EXPECT_TRUE(j["period1"].contains("avg_ahi"));
    EXPECT_TRUE(j["period1"].contains("compliance_pct"));
}

TEST_F(AgentToolsDbTest, ComparePeriodsEmptyPeriodHasZeroNightsNullAverages) {
    auto j = json::parse(tools_.execute("compare_periods", json{
        {"period1_start", "1999-01-01"}, {"period1_end", "1999-01-02"},
        {"period2_start", "2025-02-06"}, {"period2_end", "2025-02-08"}}, *conn_));
    // Empty range still yields a row: COUNT=0, AVG=NULL, compliance NULL.
    EXPECT_EQ(j["period1"]["nights"].get<std::string>(), "0");
    EXPECT_TRUE(j["period1"]["avg_ahi"].is_null());
    EXPECT_TRUE(j["period1"]["compliance_pct"].is_null());
    EXPECT_EQ(j["period2"]["nights"].get<std::string>(), "3");
}

TEST_F(AgentToolsDbTest, ComparePeriodsMissingArgReturnsError) {
    auto j = json::parse(tools_.execute("compare_periods", json{
        {"period1_start", "2025-02-06"}, {"period1_end", "2025-02-07"}}, *conn_));
    ASSERT_TRUE(j.contains("error"));
}

// ── get_vitals ───────────────────────────────────────────────────────────────

TEST_F(AgentToolsDbTest, GetVitalsReturnsPerMinuteAggregates) {
    auto j = json::parse(tools_.execute(
        "get_vitals", json{{"date", "2025-02-06"}}, *conn_));
    ASSERT_TRUE(j.is_array());
    // Two distinct minutes (23:00 and 23:01).
    EXPECT_EQ(j.size(), 2u);
    EXPECT_TRUE(j[0].contains("avg_spo2"));
    EXPECT_TRUE(j[0].contains("min_hr"));
    EXPECT_TRUE(j[0].contains("max_hr"));
}

TEST_F(AgentToolsDbTest, GetVitalsUnknownDateEmpty) {
    auto j = json::parse(tools_.execute(
        "get_vitals", json{{"date", "1990-05-05"}}, *conn_));
    ASSERT_TRUE(j.is_array());
    EXPECT_TRUE(j.empty());
}

TEST_F(AgentToolsDbTest, GetVitalsMissingArgReturnsError) {
    auto j = json::parse(tools_.execute("get_vitals", json::object(), *conn_));
    ASSERT_TRUE(j.contains("error"));
}

// ── get_statistics ───────────────────────────────────────────────────────────

TEST_F(AgentToolsDbTest, GetStatisticsReturnsAggregateRow) {
    auto j = json::parse(tools_.execute("get_statistics", json{
        {"start_date", "2025-02-06"}, {"end_date", "2025-02-08"}}, *conn_));
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 1u);
    const auto& r = j[0];
    EXPECT_EQ(r["total_nights"].get<std::string>(), "3");
    EXPECT_TRUE(r.contains("avg_ahi"));
    EXPECT_TRUE(r.contains("stddev_ahi"));
    EXPECT_TRUE(r.contains("compliance_pct"));
    EXPECT_TRUE(r.contains("total_therapy_minutes"));
}

TEST_F(AgentToolsDbTest, GetStatisticsEmptyRangeZeroNights) {
    auto j = json::parse(tools_.execute("get_statistics", json{
        {"start_date", "1980-01-01"}, {"end_date", "1980-01-02"}}, *conn_));
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["total_nights"].get<std::string>(), "0");
    // Averages over no rows are NULL; compliance uses NULLIF -> NULL.
    EXPECT_TRUE(j[0]["avg_ahi"].is_null());
    EXPECT_TRUE(j[0]["compliance_pct"].is_null());
}

TEST_F(AgentToolsDbTest, GetStatisticsMissingArgReturnsError) {
    auto j = json::parse(tools_.execute(
        "get_statistics", json{{"start_date", "2025-02-06"}}, *conn_));
    ASSERT_TRUE(j.contains("error"));
}

// ── dispatch via execute(), DB-backed ────────────────────────────────────────

TEST_F(AgentToolsDbTest, ExecuteUnknownToolReturnsErrorEnvelope) {
    auto j = json::parse(tools_.execute("no_such_tool", json::object(), *conn_));
    ASSERT_TRUE(j.contains("error"));
    EXPECT_NE(j["error"].get<std::string>().find("Unknown tool: no_such_tool"),
              std::string::npos);
}

TEST_F(AgentToolsDbTest, DeviceIsolationOtherDeviceSeesNothing) {
    AgentTools other{"some_other_device"};
    auto j = json::parse(other.execute("get_statistics", json{
        {"start_date", "2025-02-06"}, {"end_date", "2025-02-08"}}, *conn_));
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["total_nights"].get<std::string>(), "0");
}
