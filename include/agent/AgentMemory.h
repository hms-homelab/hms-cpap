#pragma once

#include "llm_client.h"
#ifdef WITH_POSTGRESQL
#include <pqxx/pqxx>
#endif
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {

/// A retrieved conversation context for injection into system prompt
struct ConversationContext {
    std::string conversation_id;
    std::string summary;
    std::string category;
    double similarity;
};

/// A stored memory fact
struct MemoryFact {
    int memory_id;
    std::string key;
    std::string value;
    std::string category;
    int importance;
};

#ifdef WITH_POSTGRESQL
/**
 * AgentMemory - pgvector-backed conversation and fact memory.
 *
 * Uses the existing cpap_monitoring database with pgvector extension.
 * Tables: agent_conversations, agent_messages, agent_memory.
 */
class AgentMemory {
public:
    explicit AgentMemory(const std::string& device_id);

    void ensureSchema(pqxx::connection& conn);

    std::string getOrCreateConversation(pqxx::connection& conn,
                                         const std::string& conversation_id = "");

    void storeMessage(pqxx::connection& conn,
                      const std::string& conversation_id,
                      const std::string& role,
                      const std::string& content,
                      const std::vector<float>& embedding = {},
                      const std::string& tool_name = "");

    std::vector<std::pair<std::string, std::string>> getMessages(
        pqxx::connection& conn,
        const std::string& conversation_id,
        int limit = 20);

    void updateSummary(pqxx::connection& conn,
                       const std::string& conversation_id,
                       const std::string& summary,
                       const std::vector<float>& embedding = {});

    std::vector<ConversationContext> searchSimilar(
        pqxx::connection& conn,
        const std::vector<float>& query_embedding,
        int limit = 3);

    void storeFact(pqxx::connection& conn,
                   const std::string& key,
                   const std::string& value,
                   const std::vector<float>& embedding,
                   const std::string& category = "general",
                   int importance = 3);

    std::vector<MemoryFact> searchFacts(pqxx::connection& conn,
                                         const std::vector<float>& query_embedding,
                                         int limit = 5);

    int cleanupExpired(pqxx::connection& conn);

private:
    std::string device_id_;

    static std::string expiryFromImportance(int importance);
};
#endif // WITH_POSTGRESQL

} // namespace hms_cpap
