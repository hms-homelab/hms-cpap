#pragma once

#include "database/IDatabase.h"
#include "llm_client.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * AgentTools - 7 read-only CPAP data tools for LLM function calling.
 *
 * SDD-072 D4: every tool reads through IDatabase, so the agent runs on SQLite,
 * MySQL and PostgreSQL. A night is a NightFacts (the same facts the daily and
 * range summaries render); the daily rows, vitals and events come from
 * agent_reads. The tools hold no SQL and no connection of their own. Every
 * result carries the rules (D2), and a list that was cut says so.
 *
 * Device ID is injected at construction, never exposed to LLM.
 */
class AgentTools {
public:
    explicit AgentTools(const std::string& device_id);

    /// Get tool definitions for LLM (JSON Schema format)
    const std::vector<hms::ToolDefinition>& definitions() const;

    /// Execute a tool by name with JSON arguments, against [db]. Errors come
    /// back as {"error": "..."}, never as an exception.
    std::string execute(const std::string& name, const nlohmann::json& arguments, IDatabase& db);

    /// Validate tool name exists
    bool hasTool(const std::string& name) const;

    /// The most nights get_recent_sessions returns, whatever limit is asked.
    static constexpr int kMaxNights = 31;
    /// The most events get_session_details returns per night.
    static constexpr int kMaxEvents = 300;

private:
    std::string device_id_;
    std::vector<hms::ToolDefinition> definitions_;

    void buildDefinitions();

    nlohmann::json getRecentSessions(const nlohmann::json& args, IDatabase& db);
    nlohmann::json getSessionDetails(const nlohmann::json& args, IDatabase& db);
    nlohmann::json getDailySummary(const nlohmann::json& args, IDatabase& db);
    nlohmann::json getTrendData(const nlohmann::json& args, IDatabase& db);
    nlohmann::json comparePeriods(const nlohmann::json& args, IDatabase& db);
    nlohmann::json getVitals(const nlohmann::json& args, IDatabase& db);
    nlohmann::json getStatistics(const nlohmann::json& args, IDatabase& db);
};

} // namespace hms_cpap
