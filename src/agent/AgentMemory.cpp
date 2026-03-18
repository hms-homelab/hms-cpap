#include "agent/AgentMemory.h"
#include <iostream>
#include <optional>
#include <sstream>
#include <chrono>

namespace hms_cpap {

AgentMemory::AgentMemory(const std::string& device_id)
    : device_id_(device_id) {}

void AgentMemory::ensureSchema(pqxx::connection& conn) {
    pqxx::work txn(conn);
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS agent_conversations (
            conversation_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
            device_id VARCHAR(255) NOT NULL,
            category VARCHAR(50) DEFAULT 'general',
            summary TEXT,
            summary_embedding vector(768),
            created_at TIMESTAMP DEFAULT NOW(),
            updated_at TIMESTAMP DEFAULT NOW(),
            expires_at TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS agent_messages (
            message_id SERIAL PRIMARY KEY,
            conversation_id UUID REFERENCES agent_conversations(conversation_id) ON DELETE CASCADE,
            role VARCHAR(20) NOT NULL,
            content TEXT NOT NULL,
            tool_name VARCHAR(100),
            embedding vector(768),
            created_at TIMESTAMP DEFAULT NOW()
        );

        CREATE TABLE IF NOT EXISTS agent_memory (
            memory_id SERIAL PRIMARY KEY,
            device_id VARCHAR(255) NOT NULL,
            key VARCHAR(255) NOT NULL,
            value TEXT NOT NULL,
            embedding vector(768) NOT NULL,
            category VARCHAR(50) DEFAULT 'general',
            importance INT DEFAULT 3 CHECK (importance BETWEEN 1 AND 5),
            created_at TIMESTAMP DEFAULT NOW(),
            expires_at TIMESTAMP,
            UNIQUE(device_id, key)
        );
    )");
    txn.commit();
}

std::string AgentMemory::getOrCreateConversation(pqxx::connection& conn,
                                                   const std::string& conversation_id) {
    pqxx::work txn(conn);

    if (!conversation_id.empty()) {
        // Check if exists
        auto check = txn.exec_params(
            "SELECT conversation_id FROM agent_conversations WHERE conversation_id = $1::uuid",
            conversation_id);

        if (!check.empty()) {
            txn.commit();
            return conversation_id;
        }

        // Create with given ID
        txn.exec_params(R"(
            INSERT INTO agent_conversations (conversation_id, device_id)
            VALUES ($1::uuid, $2)
        )", conversation_id, device_id_);
        txn.commit();
        return conversation_id;
    }

    // Create new conversation with auto-generated UUID
    auto result = txn.exec_params(R"(
        INSERT INTO agent_conversations (device_id)
        VALUES ($1)
        RETURNING conversation_id::text
    )", device_id_);
    txn.commit();

    return result[0][0].c_str();
}

void AgentMemory::storeMessage(pqxx::connection& conn,
                                const std::string& conversation_id,
                                const std::string& role,
                                const std::string& content,
                                const std::vector<float>& embedding,
                                const std::string& tool_name) {
    pqxx::work txn(conn);

    std::optional<std::string> tool_opt = tool_name.empty() ? std::nullopt : std::optional<std::string>(tool_name);

    if (embedding.empty()) {
        txn.exec_params(R"(
            INSERT INTO agent_messages (conversation_id, role, content, tool_name)
            VALUES ($1::uuid, $2, $3, $4)
        )", conversation_id, role, content, tool_opt);
    } else {
        std::string vec_literal = hms::LLMClient::toVectorLiteral(embedding);
        txn.exec_params(R"(
            INSERT INTO agent_messages (conversation_id, role, content, tool_name, embedding)
            VALUES ($1::uuid, $2, $3, $4, $5::vector)
        )", conversation_id, role, content, tool_opt, vec_literal);
    }

    // Update conversation timestamp
    txn.exec_params(R"(
        UPDATE agent_conversations SET updated_at = NOW()
        WHERE conversation_id = $1::uuid
    )", conversation_id);

    txn.commit();
}

std::vector<std::pair<std::string, std::string>> AgentMemory::getMessages(
    pqxx::connection& conn,
    const std::string& conversation_id,
    int limit) {
    pqxx::work txn(conn);
    auto result = txn.exec_params(R"(
        SELECT role, content FROM agent_messages
        WHERE conversation_id = $1::uuid
        ORDER BY created_at DESC
        LIMIT $2
    )", conversation_id, limit);
    txn.commit();

    // Reverse to get chronological order
    std::vector<std::pair<std::string, std::string>> messages;
    for (int i = static_cast<int>(result.size()) - 1; i >= 0; --i) {
        messages.emplace_back(result[i][0].c_str(), result[i][1].c_str());
    }
    return messages;
}

void AgentMemory::updateSummary(pqxx::connection& conn,
                                 const std::string& conversation_id,
                                 const std::string& summary,
                                 const std::vector<float>& embedding) {
    pqxx::work txn(conn);

    if (embedding.empty()) {
        txn.exec_params(R"(
            UPDATE agent_conversations
            SET summary = $2, updated_at = NOW()
            WHERE conversation_id = $1::uuid
        )", conversation_id, summary);
    } else {
        std::string vec_literal = hms::LLMClient::toVectorLiteral(embedding);
        txn.exec_params(R"(
            UPDATE agent_conversations
            SET summary = $2, summary_embedding = $3::vector, updated_at = NOW()
            WHERE conversation_id = $1::uuid
        )", conversation_id, summary, vec_literal);
    }
    txn.commit();
}

std::vector<ConversationContext> AgentMemory::searchSimilar(
    pqxx::connection& conn,
    const std::vector<float>& query_embedding,
    int limit) {
    std::string vec_literal = hms::LLMClient::toVectorLiteral(query_embedding);

    pqxx::work txn(conn);
    auto result = txn.exec_params(R"(
        SELECT conversation_id::text, summary, category,
               1 - (summary_embedding <=> $1::vector) as similarity
        FROM agent_conversations
        WHERE device_id = $2
          AND summary_embedding IS NOT NULL
          AND summary IS NOT NULL
          AND (expires_at IS NULL OR expires_at > NOW())
        ORDER BY summary_embedding <=> $1::vector
        LIMIT $3
    )", vec_literal, device_id_, limit);
    txn.commit();

    std::vector<ConversationContext> contexts;
    for (const auto& row : result) {
        contexts.push_back({
            row[0].c_str(),
            row[1].c_str(),
            row[2].c_str(),
            row[3].as<double>()
        });
    }
    return contexts;
}

void AgentMemory::storeFact(pqxx::connection& conn,
                             const std::string& key,
                             const std::string& value,
                             const std::vector<float>& embedding,
                             const std::string& category,
                             int importance) {
    std::string vec_literal = hms::LLMClient::toVectorLiteral(embedding);
    std::string expiry = expiryFromImportance(importance);

    pqxx::work txn(conn);
    if (expiry.empty()) {
        txn.exec_params(R"(
            INSERT INTO agent_memory (device_id, key, value, embedding, category, importance, expires_at)
            VALUES ($1, $2, $3, $4::vector, $5, $6, NULL)
            ON CONFLICT (device_id, key) DO UPDATE
            SET value = EXCLUDED.value, embedding = EXCLUDED.embedding,
                category = EXCLUDED.category, importance = EXCLUDED.importance,
                expires_at = NULL
        )", device_id_, key, value, vec_literal, category, importance);
    } else {
        txn.exec_params(R"(
            INSERT INTO agent_memory (device_id, key, value, embedding, category, importance, expires_at)
            VALUES ($1, $2, $3, $4::vector, $5, $6, NOW() + $7::interval)
            ON CONFLICT (device_id, key) DO UPDATE
            SET value = EXCLUDED.value, embedding = EXCLUDED.embedding,
                category = EXCLUDED.category, importance = EXCLUDED.importance,
                expires_at = NOW() + $7::interval
        )", device_id_, key, value, vec_literal, category, importance, expiry);
    }
    txn.commit();
}

std::vector<MemoryFact> AgentMemory::searchFacts(pqxx::connection& conn,
                                                   const std::vector<float>& query_embedding,
                                                   int limit) {
    std::string vec_literal = hms::LLMClient::toVectorLiteral(query_embedding);

    pqxx::work txn(conn);
    auto result = txn.exec_params(R"(
        SELECT memory_id, key, value, category, importance
        FROM agent_memory
        WHERE device_id = $1
          AND (expires_at IS NULL OR expires_at > NOW())
        ORDER BY embedding <=> $2::vector
        LIMIT $3
    )", device_id_, vec_literal, limit);
    txn.commit();

    std::vector<MemoryFact> facts;
    for (const auto& row : result) {
        facts.push_back({
            row[0].as<int>(),
            row[1].c_str(),
            row[2].c_str(),
            row[3].c_str(),
            row[4].as<int>()
        });
    }
    return facts;
}

int AgentMemory::cleanupExpired(pqxx::connection& conn) {
    pqxx::work txn(conn);
    auto r1 = txn.exec("DELETE FROM agent_conversations WHERE expires_at IS NOT NULL AND expires_at < NOW()");
    auto r2 = txn.exec("DELETE FROM agent_memory WHERE expires_at IS NOT NULL AND expires_at < NOW()");
    txn.commit();
    return static_cast<int>(r1.affected_rows() + r2.affected_rows());
}

std::string AgentMemory::expiryFromImportance(int importance) {
    switch (importance) {
        case 1: return "7 days";
        case 2: return "30 days";
        case 3: return "90 days";
        case 4: return "365 days";
        case 5: return "";  // permanent (NULL expiry)
        default: return "90 days";
    }
}

} // namespace hms_cpap
