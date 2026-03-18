#include <gtest/gtest.h>
#include "agent/AgentMemory.h"

using namespace hms_cpap;

class AgentMemoryTest : public ::testing::Test {
protected:
    AgentMemory memory{"test_device_456"};
};

// ── Unit tests (no DB required) ─────────────────────────────────────────────

TEST(AgentMemoryUnit, ConstructsWithDeviceId) {
    AgentMemory mem("my_device");
    // No crash = success. Device ID is private, verified via DB operations.
    SUCCEED();
}

TEST(AgentMemoryUnit, ConversationContextStruct) {
    ConversationContext ctx;
    ctx.conversation_id = "abc-123";
    ctx.summary = "Asked about AHI trends";
    ctx.category = "therapy";
    ctx.similarity = 0.85;

    EXPECT_EQ(ctx.conversation_id, "abc-123");
    EXPECT_EQ(ctx.summary, "Asked about AHI trends");
    EXPECT_EQ(ctx.category, "therapy");
    EXPECT_DOUBLE_EQ(ctx.similarity, 0.85);
}

TEST(AgentMemoryUnit, MemoryFactStruct) {
    MemoryFact fact;
    fact.memory_id = 1;
    fact.key = "preferred_metric";
    fact.value = "AHI";
    fact.category = "general";
    fact.importance = 3;

    EXPECT_EQ(fact.memory_id, 1);
    EXPECT_EQ(fact.key, "preferred_metric");
    EXPECT_EQ(fact.value, "AHI");
    EXPECT_EQ(fact.category, "general");
    EXPECT_EQ(fact.importance, 3);
}

// ── Integration tests (require DB) ─────────────────────────────────────────
// These tests connect to the actual cpap_monitoring database.
// They are skipped if the DB is unavailable.

class AgentMemoryDBTest : public ::testing::Test {
protected:
    std::string conn_str_;
    bool db_available_ = false;

    void SetUp() override {
        const char* host = std::getenv("DB_HOST");
        const char* port = std::getenv("DB_PORT");
        const char* name = std::getenv("DB_NAME");
        const char* user = std::getenv("DB_USER");
        const char* pass = std::getenv("DB_PASSWORD");

        conn_str_ = "host=" + std::string(host ? host : "localhost")
                   + " port=" + std::string(port ? port : "5432")
                   + " dbname=" + std::string(name ? name : "cpap_monitoring")
                   + " user=" + std::string(user ? user : "maestro")
                   + " password=" + std::string(pass ? pass : "maestro_postgres_2026_secure");

        try {
            pqxx::connection conn(conn_str_);
            db_available_ = conn.is_open();
        } catch (...) {
            db_available_ = false;
        }
    }
};

TEST_F(AgentMemoryDBTest, EnsureSchemaCreatesTablesIdempotent) {
    if (!db_available_) GTEST_SKIP() << "Database not available";

    AgentMemory mem("test_agent_mem");
    pqxx::connection conn(conn_str_);

    // Should not throw on first or repeated calls
    EXPECT_NO_THROW(mem.ensureSchema(conn));
    EXPECT_NO_THROW(mem.ensureSchema(conn));
}

TEST_F(AgentMemoryDBTest, CreateAndRetrieveConversation) {
    if (!db_available_) GTEST_SKIP() << "Database not available";

    AgentMemory mem("test_agent_conv");
    pqxx::connection conn(conn_str_);
    mem.ensureSchema(conn);

    // Create new conversation
    std::string id = mem.getOrCreateConversation(conn);
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(id.size(), 36u);  // UUID format

    // Retrieve existing conversation
    std::string id2 = mem.getOrCreateConversation(conn, id);
    EXPECT_EQ(id, id2);

    // Cleanup
    pqxx::work txn(conn);
    txn.exec_params("DELETE FROM agent_conversations WHERE conversation_id = $1::uuid", id);
    txn.commit();
}

TEST_F(AgentMemoryDBTest, StoreAndGetMessages) {
    if (!db_available_) GTEST_SKIP() << "Database not available";

    AgentMemory mem("test_agent_msg");
    pqxx::connection conn(conn_str_);
    mem.ensureSchema(conn);

    std::string conv_id = mem.getOrCreateConversation(conn);

    mem.storeMessage(conn, conv_id, "user", "What was my AHI last night?");
    mem.storeMessage(conn, conv_id, "assistant", "Your AHI was 2.5 last night.");

    auto messages = mem.getMessages(conn, conv_id, 10);
    EXPECT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].first, "user");
    EXPECT_EQ(messages[0].second, "What was my AHI last night?");
    EXPECT_EQ(messages[1].first, "assistant");
    EXPECT_EQ(messages[1].second, "Your AHI was 2.5 last night.");

    // Cleanup
    pqxx::work txn(conn);
    txn.exec_params("DELETE FROM agent_conversations WHERE conversation_id = $1::uuid", conv_id);
    txn.commit();
}

TEST_F(AgentMemoryDBTest, CleanupExpiredRemovesOldEntries) {
    if (!db_available_) GTEST_SKIP() << "Database not available";

    AgentMemory mem("test_agent_cleanup");
    pqxx::connection conn(conn_str_);
    mem.ensureSchema(conn);

    // Insert an already-expired conversation
    pqxx::work txn(conn);
    txn.exec_params(R"(
        INSERT INTO agent_conversations (device_id, expires_at)
        VALUES ($1, NOW() - INTERVAL '1 day')
    )", "test_agent_cleanup");
    txn.commit();

    int cleaned = mem.cleanupExpired(conn);
    EXPECT_GE(cleaned, 1);
}
