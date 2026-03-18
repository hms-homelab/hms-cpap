#pragma once

#include "llm_client.h"
#include <pqxx/pqxx>
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

/**
 * AgentMemory - pgvector-backed conversation and fact memory.
 *
 * Uses the existing cpap_monitoring database with pgvector extension.
 * Tables: agent_conversations, agent_messages, agent_memory.
 */
class AgentMemory {
public:
    explicit AgentMemory(const std::string& device_id);

    /// Ensure memory tables exist (runs CREATE IF NOT EXISTS)
    void ensureSchema(pqxx::connection& conn);

    /// Create or retrieve a conversation by ID
    std::string getOrCreateConversation(pqxx::connection& conn,
                                         const std::string& conversation_id = "");

    /// Store a message in a conversation
    void storeMessage(pqxx::connection& conn,
                      const std::string& conversation_id,
                      const std::string& role,
                      const std::string& content,
                      const std::vector<float>& embedding = {},
                      const std::string& tool_name = "");

    /// Get recent messages for a conversation (ordered by created_at)
    std::vector<std::pair<std::string, std::string>> getMessages(
        pqxx::connection& conn,
        const std::string& conversation_id,
        int limit = 20);

    /// Update conversation summary and embedding
    void updateSummary(pqxx::connection& conn,
                       const std::string& conversation_id,
                       const std::string& summary,
                       const std::vector<float>& embedding = {});

    /// Search for similar past conversations by embedding
    std::vector<ConversationContext> searchSimilar(
        pqxx::connection& conn,
        const std::vector<float>& query_embedding,
        int limit = 3);

    /// Store or update a memory fact
    void storeFact(pqxx::connection& conn,
                   const std::string& key,
                   const std::string& value,
                   const std::vector<float>& embedding,
                   const std::string& category = "general",
                   int importance = 3);

    /// Search memory facts by embedding
    std::vector<MemoryFact> searchFacts(pqxx::connection& conn,
                                         const std::vector<float>& query_embedding,
                                         int limit = 5);

    /// Cleanup expired conversations and memories
    int cleanupExpired(pqxx::connection& conn);

private:
    std::string device_id_;

    /// Compute expiry timestamp from importance level
    static std::string expiryFromImportance(int importance);
};

} // namespace hms_cpap
