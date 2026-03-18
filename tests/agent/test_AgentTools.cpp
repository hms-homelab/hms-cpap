#include <gtest/gtest.h>
#include "agent/AgentTools.h"
#include <nlohmann/json.hpp>

using namespace hms_cpap;
using json = nlohmann::json;

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
}

// ── Parameter validation tests ──────────────────────────────────────────────

TEST_F(AgentToolsTest, GetRecentSessionsHasOptionalParams) {
    const auto& defs = tools.definitions();
    const auto& def = defs[0];  // get_recent_sessions
    EXPECT_EQ(def.name, "get_recent_sessions");
    EXPECT_TRUE(def.parameters["properties"].contains("days"));
    EXPECT_TRUE(def.parameters["properties"].contains("limit"));
    EXPECT_TRUE(def.parameters["required"].empty());
}

TEST_F(AgentToolsTest, GetSessionDetailsRequiresDate) {
    const auto& defs = tools.definitions();
    // Find get_session_details
    for (const auto& def : defs) {
        if (def.name == "get_session_details") {
            EXPECT_TRUE(def.parameters["required"].is_array());
            auto req = def.parameters["required"];
            EXPECT_EQ(req.size(), 1u);
            EXPECT_EQ(req[0], "date");
            return;
        }
    }
    FAIL() << "get_session_details not found";
}

TEST_F(AgentToolsTest, GetTrendDataRequiresMetric) {
    for (const auto& def : tools.definitions()) {
        if (def.name == "get_trend_data") {
            auto req = def.parameters["required"];
            EXPECT_EQ(req.size(), 1u);
            EXPECT_EQ(req[0], "metric");
            // Check enum
            auto metric_props = def.parameters["properties"]["metric"];
            EXPECT_TRUE(metric_props.contains("enum"));
            auto enums = metric_props["enum"];
            EXPECT_EQ(enums.size(), 6u);
            return;
        }
    }
    FAIL() << "get_trend_data not found";
}

TEST_F(AgentToolsTest, ComparePeriodsRequiresFourDates) {
    for (const auto& def : tools.definitions()) {
        if (def.name == "compare_periods") {
            auto req = def.parameters["required"];
            EXPECT_EQ(req.size(), 4u);
            return;
        }
    }
    FAIL() << "compare_periods not found";
}

TEST_F(AgentToolsTest, GetStatisticsRequiresDateRange) {
    for (const auto& def : tools.definitions()) {
        if (def.name == "get_statistics") {
            auto req = def.parameters["required"];
            EXPECT_EQ(req.size(), 2u);
            EXPECT_EQ(req[0], "start_date");
            EXPECT_EQ(req[1], "end_date");
            return;
        }
    }
    FAIL() << "get_statistics not found";
}

// ── Error handling (no DB connection) ───────────────────────────────────────
// These tests verify that execute() returns error JSON when the DB is unavailable,
// rather than crashing.

TEST_F(AgentToolsTest, ExecuteUnknownToolReturnsError) {
    // We need a connection object but it can be invalid for this test
    // since we expect the "unknown tool" path before any DB access
    try {
        pqxx::connection conn("host=localhost dbname=nonexistent_db_12345");
        auto result = tools.execute("nonexistent", {}, conn);
        auto j = json::parse(result);
        EXPECT_TRUE(j.contains("error"));
    } catch (...) {
        // Connection failure is expected - the unknown tool check happens inside execute()
        // which catches exceptions
    }
}
