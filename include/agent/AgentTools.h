#pragma once

#include "llm_client.h"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <functional>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * AgentTools - 7 read-only CPAP data tools for LLM function calling.
 *
 * Each tool maps to a hardcoded parameterized SQL query.
 * Device ID is injected at construction, never exposed to LLM.
 */
class AgentTools {
public:
    explicit AgentTools(const std::string& device_id);

    /// Get tool definitions for LLM (JSON Schema format)
    const std::vector<hms::ToolDefinition>& definitions() const;

    /// Execute a tool by name with JSON arguments, using provided DB connection
    std::string execute(const std::string& name,
                        const nlohmann::json& arguments,
                        pqxx::connection& conn);

    /// Validate tool name exists
    bool hasTool(const std::string& name) const;

private:
    std::string device_id_;
    std::vector<hms::ToolDefinition> definitions_;

    void buildDefinitions();

    // Tool implementations
    std::string getRecentSessions(const nlohmann::json& args, pqxx::connection& conn);
    std::string getSessionDetails(const nlohmann::json& args, pqxx::connection& conn);
    std::string getDailySummary(const nlohmann::json& args, pqxx::connection& conn);
    std::string getTrendData(const nlohmann::json& args, pqxx::connection& conn);
    std::string comparePeriods(const nlohmann::json& args, pqxx::connection& conn);
    std::string getVitals(const nlohmann::json& args, pqxx::connection& conn);
    std::string getStatistics(const nlohmann::json& args, pqxx::connection& conn);

    // Helper to format pqxx::result as JSON string
    static std::string resultToJson(const pqxx::result& result);
};

} // namespace hms_cpap
