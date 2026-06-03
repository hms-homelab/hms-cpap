#include <gtest/gtest.h>
#include "agent/AgentMemory.h"

#ifdef WITH_POSTGRESQL
#include <pqxx/pqxx>
#include <cstdlib>
#include <string>
#include <unistd.h>
#endif

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
                   + " password=" + std::string(pass ? pass : "REDACTED");

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

// ── Isolated-schema integration tests ───────────────────────────────────────
// These mirror tests/database/test_DatabaseService_pg.cpp: connect as
// maestro@localhost (or PG* env), create a UNIQUE throwaway schema selected via
// the connection's search_path so every unqualified table the AgentMemory code
// touches lands in the isolated schema, then DROP it in TearDown. This avoids
// touching production agent_* rows. The whole suite GTEST_SKIPs cleanly when no
// usable PostgreSQL (with the pgvector `vector` type) is reachable.
//
// Determinism: no wall-clock assertions. Expiry is asserted only as NULL vs
// non-NULL, ordering by relative future/past, and round-tripped values.

#ifdef WITH_POSTGRESQL
namespace {

std::string envOr(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : def;
}

std::string testDbName() { return envOr("PGDATABASE", "cpap_monitoring"); }

std::string makeConnInfo(const std::string& search_path = "") {
    std::string ci = "host=" + envOr("PGHOST", "localhost") +
                     " port=" + envOr("PGPORT", "5432") +
                     " user=" + envOr("PGUSER", "maestro") +
                     " password=" + envOr("PGPASSWORD", "REDACTED") +
                     " dbname=" + testDbName() +
                     " connect_timeout=3";
    if (!search_path.empty()) {
        // Put the throwaway schema FIRST (so unqualified CREATE TABLE lands
        // there) but keep public on the path: pgvector's `vector` type and the
        // <=> operator live in public.
        ci += " options=-csearch_path=" + search_path + ",public";
    }
    return ci;
}

// A deterministic 768-dim embedding. The schema declares vector(768), so every
// vector literal must have exactly that dimensionality. `seed` shifts the values
// so we can produce a vector that is "closest" to a chosen target for ANN order
// assertions, without depending on any model.
std::vector<float> embed768(float base) {
    std::vector<float> v(768, 0.0f);
    for (size_t i = 0; i < v.size(); ++i) {
        v[i] = base + static_cast<float>(i) * 0.0001f;
    }
    return v;
}

} // namespace

// Fixture: usable server + pgvector available + we can create a schema. Builds a
// throwaway schema, installs the agent_* tables via AgentMemory::ensureSchema(),
// and drops the schema on teardown.
class AgentMemoryPgTest : public ::testing::Test {
protected:
    static bool serverUsable() {
        try {
            pqxx::connection c(makeConnInfo());
            if (!c.is_open()) return false;
            // Require the pgvector extension type to exist; the schema uses
            // vector(768) and the <=> operator.
            pqxx::work txn(c);
            auto r = txn.exec("SELECT 1 FROM pg_type WHERE typname = 'vector'");
            txn.commit();
            return !r.empty();
        } catch (...) {
            return false;
        }
    }

    void SetUp() override {
        if (!serverUsable()) {
            GTEST_SKIP() << "No usable PostgreSQL+pgvector ("
                         << envOr("PGUSER", "maestro") << "@"
                         << envOr("PGHOST", "localhost") << "/" << testDbName()
                         << ") — skipping AgentMemory (PG) tests.";
        }

        static int counter = 0;
        schema_ = "agentmem_test_" + std::to_string(::getpid()) + "_" +
                  std::to_string(counter++);

        // Create the schema on a plain connection first.
        try {
            pqxx::connection admin(makeConnInfo());
            pqxx::work txn(admin);
            txn.exec("CREATE SCHEMA IF NOT EXISTS " + schema_);
            txn.commit();
        } catch (const std::exception& e) {
            GTEST_SKIP() << "Cannot create schema " << schema_ << ": " << e.what();
        }

        // The working connection has search_path pinned to the throwaway schema,
        // so all unqualified DDL/DML from AgentMemory lands there.
        conn_ = std::make_unique<pqxx::connection>(makeConnInfo(schema_));
        ASSERT_TRUE(conn_->is_open());

        mem_ = std::make_unique<AgentMemory>(device_id_);
        ASSERT_NO_THROW(mem_->ensureSchema(*conn_));
    }

    void TearDown() override {
        // Drop via a fresh connection (search_path is inside the schema we drop).
        if (!schema_.empty()) {
            conn_.reset();
            try {
                pqxx::connection admin(makeConnInfo());
                pqxx::work txn(admin);
                txn.exec("DROP SCHEMA IF EXISTS " + schema_ + " CASCADE");
                txn.commit();
            } catch (...) {
                // best effort
            }
        }
    }

    // Scalar helper against the working (search_path-pinned) connection.
    std::string scalar(const std::string& sql) {
        pqxx::work txn(*conn_);
        auto r = txn.exec(sql);
        txn.commit();
        if (r.empty() || r[0][0].is_null()) return std::string();
        return r[0][0].as<std::string>();
    }

    // Run a boolean-returning SELECT and normalize the result. PostgreSQL
    // renders booleans as t/f, so we map to a C++ bool rather than comparing
    // the literal "true"/"false". Accepts either a full "SELECT ..." statement
    // or a bare boolean expression.
    bool boolScalar(const std::string& sql) {
        std::string s = sql;
        if (s.rfind("SELECT", 0) != 0 && s.rfind("select", 0) != 0) {
            s = "SELECT (" + s + ")";
        }
        auto v = scalar(s);
        return v == "t" || v == "true";
    }

    long count(const std::string& table, const std::string& where = "") {
        std::string sql = "SELECT COUNT(*) FROM " + table;
        if (!where.empty()) sql += " WHERE " + where;
        auto v = scalar(sql);
        return v.empty() ? 0 : std::stol(v);
    }

    const std::string device_id_ = "pg_agent_dev";
    std::string schema_;
    std::unique_ptr<pqxx::connection> conn_;
    std::unique_ptr<AgentMemory> mem_;
};

// ── ensureSchema ────────────────────────────────────────────────────────────

TEST_F(AgentMemoryPgTest, EnsureSchemaCreatesAllThreeTables) {
    // Run again to prove idempotency.
    ASSERT_NO_THROW(mem_->ensureSchema(*conn_));
    EXPECT_TRUE(boolScalar("to_regclass('" + schema_ + ".agent_conversations') IS NOT NULL"));
    EXPECT_TRUE(boolScalar("to_regclass('" + schema_ + ".agent_messages') IS NOT NULL"));
    EXPECT_TRUE(boolScalar("to_regclass('" + schema_ + ".agent_memory') IS NOT NULL"));
}

// ── getOrCreateConversation ─────────────────────────────────────────────────

TEST_F(AgentMemoryPgTest, GetOrCreate_NewGeneratesUuidAndPersists) {
    std::string id = mem_->getOrCreateConversation(*conn_);
    EXPECT_EQ(id.size(), 36u);   // canonical UUID
    EXPECT_EQ(count("agent_conversations"), 1);
    EXPECT_EQ(scalar("SELECT device_id FROM agent_conversations"), device_id_);
}

TEST_F(AgentMemoryPgTest, GetOrCreate_ExistingIdReturnsSameNoDuplicate) {
    std::string id = mem_->getOrCreateConversation(*conn_);
    std::string again = mem_->getOrCreateConversation(*conn_, id);
    EXPECT_EQ(id, again);
    EXPECT_EQ(count("agent_conversations"), 1);  // not duplicated
}

TEST_F(AgentMemoryPgTest, GetOrCreate_SuppliedUnknownIdInsertsThatId) {
    // A valid UUID that doesn't exist yet — code inserts it verbatim.
    const std::string fixed = "11111111-2222-3333-4444-555555555555";
    std::string got = mem_->getOrCreateConversation(*conn_, fixed);
    EXPECT_EQ(got, fixed);
    EXPECT_EQ(count("agent_conversations", "conversation_id = '" + fixed + "'::uuid"), 1);
    EXPECT_EQ(scalar("SELECT device_id FROM agent_conversations"), device_id_);
}

// ── storeMessage / getMessages ──────────────────────────────────────────────

TEST_F(AgentMemoryPgTest, StoreMessage_NoEmbeddingBranch) {
    std::string conv = mem_->getOrCreateConversation(*conn_);
    mem_->storeMessage(*conn_, conv, "user", "no embedding here");
    EXPECT_EQ(count("agent_messages"), 1);
    EXPECT_TRUE(boolScalar("SELECT embedding IS NULL FROM agent_messages"));
    EXPECT_TRUE(boolScalar("SELECT tool_name IS NULL FROM agent_messages"));
}

TEST_F(AgentMemoryPgTest, StoreMessage_WithEmbeddingAndToolName) {
    std::string conv = mem_->getOrCreateConversation(*conn_);
    mem_->storeMessage(*conn_, conv, "tool", "ran a query", embed768(0.1f), "query_ahi");
    EXPECT_TRUE(boolScalar("SELECT embedding IS NOT NULL FROM agent_messages"));
    EXPECT_EQ(scalar("SELECT tool_name FROM agent_messages"), "query_ahi");
    // Embedding stored with full dimensionality.
    EXPECT_EQ(scalar("SELECT vector_dims(embedding) FROM agent_messages"), "768");
}

TEST_F(AgentMemoryPgTest, StoreMessage_BumpsConversationUpdatedAt) {
    std::string conv = mem_->getOrCreateConversation(*conn_);
    // Force updated_at into the past so the storeMessage bump is observable.
    {
        pqxx::work txn(*conn_);
        txn.exec_params(
            "UPDATE agent_conversations SET updated_at = NOW() - INTERVAL '1 hour' "
            "WHERE conversation_id = $1::uuid", conv);
        txn.commit();
    }
    mem_->storeMessage(*conn_, conv, "user", "hello");
    // After the bump, updated_at should be very recent (within last minute).
    EXPECT_TRUE(boolScalar("SELECT updated_at > NOW() - INTERVAL '1 minute' "
                           "FROM agent_conversations WHERE conversation_id = '" + conv + "'::uuid"));
}

TEST_F(AgentMemoryPgTest, GetMessages_ReturnsChronologicalOrder) {
    std::string conv = mem_->getOrCreateConversation(*conn_);
    mem_->storeMessage(*conn_, conv, "user", "first");
    mem_->storeMessage(*conn_, conv, "assistant", "second");
    mem_->storeMessage(*conn_, conv, "user", "third");

    auto msgs = mem_->getMessages(*conn_, conv, 10);
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(msgs[0].second, "first");
    EXPECT_EQ(msgs[1].second, "second");
    EXPECT_EQ(msgs[2].second, "third");
    EXPECT_EQ(msgs[0].first, "user");
    EXPECT_EQ(msgs[1].first, "assistant");
}

TEST_F(AgentMemoryPgTest, GetMessages_LimitTakesMostRecentButReturnsChronological) {
    std::string conv = mem_->getOrCreateConversation(*conn_);
    mem_->storeMessage(*conn_, conv, "user", "m1");
    mem_->storeMessage(*conn_, conv, "user", "m2");
    mem_->storeMessage(*conn_, conv, "user", "m3");

    // LIMIT 2 selects the two newest (m2, m3) by DESC, then reverses to ascending.
    auto msgs = mem_->getMessages(*conn_, conv, 2);
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0].second, "m2");
    EXPECT_EQ(msgs[1].second, "m3");
}

TEST_F(AgentMemoryPgTest, GetMessages_EmptyForUnknownConversation) {
    const std::string fixed = "99999999-8888-7777-6666-555555555555";
    auto msgs = mem_->getMessages(*conn_, fixed, 10);
    EXPECT_TRUE(msgs.empty());
}

// ── updateSummary ───────────────────────────────────────────────────────────

TEST_F(AgentMemoryPgTest, UpdateSummary_NoEmbeddingBranch) {
    std::string conv = mem_->getOrCreateConversation(*conn_);
    mem_->updateSummary(*conn_, conv, "Discussed leak rate");
    EXPECT_EQ(scalar("SELECT summary FROM agent_conversations"), "Discussed leak rate");
    EXPECT_TRUE(boolScalar("SELECT summary_embedding IS NULL FROM agent_conversations"));
}

TEST_F(AgentMemoryPgTest, UpdateSummary_WithEmbeddingBranch) {
    std::string conv = mem_->getOrCreateConversation(*conn_);
    mem_->updateSummary(*conn_, conv, "Discussed AHI trend", embed768(0.2f));
    EXPECT_EQ(scalar("SELECT summary FROM agent_conversations"), "Discussed AHI trend");
    EXPECT_TRUE(boolScalar("SELECT summary_embedding IS NOT NULL FROM agent_conversations"));
    EXPECT_EQ(scalar("SELECT vector_dims(summary_embedding) FROM agent_conversations"), "768");
}

// ── searchSimilar ───────────────────────────────────────────────────────────

TEST_F(AgentMemoryPgTest, SearchSimilar_OrdersByEmbeddingDistance) {
    // Two summarized conversations; query nearest the second one's vector.
    std::string near = mem_->getOrCreateConversation(*conn_);
    mem_->updateSummary(*conn_, near, "near summary", embed768(0.5f));
    std::string far = mem_->getOrCreateConversation(*conn_);
    mem_->updateSummary(*conn_, far, "far summary", embed768(50.0f));

    auto ctxs = mem_->searchSimilar(*conn_, embed768(0.5f), 3);
    ASSERT_GE(ctxs.size(), 2u);
    // The closest must be the "near" conversation.
    EXPECT_EQ(ctxs[0].conversation_id, near);
    EXPECT_EQ(ctxs[0].summary, "near summary");
    EXPECT_EQ(ctxs[0].category, "general");        // default category column
    EXPECT_GE(ctxs[0].similarity, ctxs[1].similarity);  // ordered nearest-first
}

TEST_F(AgentMemoryPgTest, SearchSimilar_ExcludesSummariesWithoutEmbedding) {
    std::string with = mem_->getOrCreateConversation(*conn_);
    mem_->updateSummary(*conn_, with, "has embedding", embed768(1.0f));
    std::string without = mem_->getOrCreateConversation(*conn_);
    mem_->updateSummary(*conn_, without, "no embedding");  // summary but null vector

    auto ctxs = mem_->searchSimilar(*conn_, embed768(1.0f), 10);
    ASSERT_EQ(ctxs.size(), 1u);
    EXPECT_EQ(ctxs[0].conversation_id, with);
}

TEST_F(AgentMemoryPgTest, SearchSimilar_ExcludesExpiredConversations) {
    std::string live = mem_->getOrCreateConversation(*conn_);
    mem_->updateSummary(*conn_, live, "live", embed768(2.0f));
    std::string expired = mem_->getOrCreateConversation(*conn_);
    mem_->updateSummary(*conn_, expired, "expired", embed768(2.0f));
    {
        pqxx::work txn(*conn_);
        txn.exec_params("UPDATE agent_conversations SET expires_at = NOW() - INTERVAL '1 day' "
                        "WHERE conversation_id = $1::uuid", expired);
        txn.commit();
    }
    auto ctxs = mem_->searchSimilar(*conn_, embed768(2.0f), 10);
    ASSERT_EQ(ctxs.size(), 1u);
    EXPECT_EQ(ctxs[0].conversation_id, live);
}

TEST_F(AgentMemoryPgTest, SearchSimilar_EmptyWhenNoConversations) {
    EXPECT_TRUE(mem_->searchSimilar(*conn_, embed768(3.0f), 5).empty());
}

// ── storeFact / searchFacts (importance -> expiry) ──────────────────────────

TEST_F(AgentMemoryPgTest, StoreFact_Importance5IsPermanentNullExpiry) {
    mem_->storeFact(*conn_, "fav_metric", "AHI", embed768(0.3f), "preference", 5);
    EXPECT_EQ(count("agent_memory"), 1);
    EXPECT_TRUE(boolScalar("SELECT expires_at IS NULL FROM agent_memory WHERE key='fav_metric'"));
    EXPECT_EQ(scalar("SELECT category FROM agent_memory WHERE key='fav_metric'"), "preference");
    EXPECT_EQ(scalar("SELECT importance FROM agent_memory WHERE key='fav_metric'"), "5");
}

TEST_F(AgentMemoryPgTest, StoreFact_LowImportanceHasNearExpiry) {
    // importance 1 -> "7 days"; importance 4 -> "365 days". Assert relative
    // ordering of expiry without depending on wall-clock absolutes.
    mem_->storeFact(*conn_, "k1", "v1", embed768(0.4f), "general", 1);
    mem_->storeFact(*conn_, "k4", "v4", embed768(0.4f), "general", 4);

    EXPECT_TRUE(boolScalar("SELECT expires_at IS NOT NULL FROM agent_memory WHERE key='k1'"));
    EXPECT_TRUE(boolScalar("SELECT expires_at IS NOT NULL FROM agent_memory WHERE key='k4'"));
    // 7-day expiry must be earlier than 365-day expiry.
    EXPECT_TRUE(boolScalar("SELECT (SELECT expires_at FROM agent_memory WHERE key='k1') < "
                           "(SELECT expires_at FROM agent_memory WHERE key='k4')"));
    // 7-day expiry is in the future but well under a year.
    EXPECT_TRUE(boolScalar("SELECT expires_at < NOW() + INTERVAL '10 days' "
                           "FROM agent_memory WHERE key='k1'"));
}

TEST_F(AgentMemoryPgTest, StoreFact_UpsertOnConflictUpdatesValueAndExpiry) {
    mem_->storeFact(*conn_, "dup", "first", embed768(0.6f), "general", 1);   // 7 days
    mem_->storeFact(*conn_, "dup", "second", embed768(0.6f), "therapy", 5);  // permanent

    EXPECT_EQ(count("agent_memory", "key='dup'"), 1);  // upsert, not a 2nd row
    EXPECT_EQ(scalar("SELECT value FROM agent_memory WHERE key='dup'"), "second");
    EXPECT_EQ(scalar("SELECT category FROM agent_memory WHERE key='dup'"), "therapy");
    EXPECT_EQ(scalar("SELECT importance FROM agent_memory WHERE key='dup'"), "5");
    // Re-store with importance 5 flips expiry back to NULL (permanent branch).
    EXPECT_TRUE(boolScalar("SELECT expires_at IS NULL FROM agent_memory WHERE key='dup'"));
}

TEST_F(AgentMemoryPgTest, StoreFact_UpsertFromPermanentToExpiringSetsExpiry) {
    mem_->storeFact(*conn_, "p2e", "perm", embed768(0.7f), "general", 5);    // NULL expiry
    EXPECT_TRUE(boolScalar("SELECT expires_at IS NULL FROM agent_memory WHERE key='p2e'"));
    mem_->storeFact(*conn_, "p2e", "now-expiring", embed768(0.7f), "general", 2);  // 30 days
    EXPECT_TRUE(boolScalar("SELECT expires_at IS NOT NULL FROM agent_memory WHERE key='p2e'"));
    EXPECT_EQ(scalar("SELECT value FROM agent_memory WHERE key='p2e'"), "now-expiring");
}

TEST_F(AgentMemoryPgTest, SearchFacts_ReturnsNearestFirst) {
    mem_->storeFact(*conn_, "near", "close value", embed768(5.0f), "general", 3);
    mem_->storeFact(*conn_, "far", "distant value", embed768(500.0f), "general", 3);

    auto facts = mem_->searchFacts(*conn_, embed768(5.0f), 5);
    ASSERT_GE(facts.size(), 2u);
    EXPECT_EQ(facts[0].key, "near");
    EXPECT_EQ(facts[0].value, "close value");
    EXPECT_EQ(facts[0].category, "general");
    EXPECT_EQ(facts[0].importance, 3);
    EXPECT_GT(facts[0].memory_id, 0);
}

TEST_F(AgentMemoryPgTest, SearchFacts_RespectsLimit) {
    mem_->storeFact(*conn_, "a", "va", embed768(1.0f), "general", 3);
    mem_->storeFact(*conn_, "b", "vb", embed768(1.1f), "general", 3);
    mem_->storeFact(*conn_, "c", "vc", embed768(1.2f), "general", 3);
    auto facts = mem_->searchFacts(*conn_, embed768(1.0f), 2);
    EXPECT_EQ(facts.size(), 2u);
}

TEST_F(AgentMemoryPgTest, SearchFacts_ExcludesExpiredFacts) {
    mem_->storeFact(*conn_, "fresh", "vf", embed768(7.0f), "general", 5);  // permanent
    mem_->storeFact(*conn_, "stale", "vs", embed768(7.0f), "general", 1);
    {
        pqxx::work txn(*conn_);
        txn.exec("UPDATE agent_memory SET expires_at = NOW() - INTERVAL '1 day' WHERE key='stale'");
        txn.commit();
    }
    auto facts = mem_->searchFacts(*conn_, embed768(7.0f), 10);
    ASSERT_EQ(facts.size(), 1u);
    EXPECT_EQ(facts[0].key, "fresh");
}

TEST_F(AgentMemoryPgTest, SearchFacts_EmptyWhenNone) {
    EXPECT_TRUE(mem_->searchFacts(*conn_, embed768(9.0f), 5).empty());
}

// ── cleanupExpired (both tables) ────────────────────────────────────────────

TEST_F(AgentMemoryPgTest, CleanupExpired_RemovesExpiredConversationsAndFacts) {
    // One expired conversation, one live conversation.
    std::string live = mem_->getOrCreateConversation(*conn_);
    std::string deadConv = mem_->getOrCreateConversation(*conn_);
    // One expired fact, one permanent fact.
    mem_->storeFact(*conn_, "perm", "v", embed768(0.1f), "general", 5);
    mem_->storeFact(*conn_, "dead", "v", embed768(0.1f), "general", 1);
    {
        pqxx::work txn(*conn_);
        txn.exec_params("UPDATE agent_conversations SET expires_at = NOW() - INTERVAL '2 days' "
                        "WHERE conversation_id = $1::uuid", deadConv);
        txn.exec("UPDATE agent_memory SET expires_at = NOW() - INTERVAL '2 days' WHERE key='dead'");
        txn.commit();
    }

    int cleaned = mem_->cleanupExpired(*conn_);
    EXPECT_EQ(cleaned, 2);  // 1 conversation + 1 fact
    EXPECT_EQ(count("agent_conversations"), 1);
    EXPECT_EQ(scalar("SELECT conversation_id::text FROM agent_conversations"), live);
    EXPECT_EQ(count("agent_memory"), 1);
    EXPECT_EQ(scalar("SELECT key FROM agent_memory"), "perm");
}

TEST_F(AgentMemoryPgTest, CleanupExpired_NothingExpiredReturnsZero) {
    mem_->getOrCreateConversation(*conn_);                       // no expires_at
    mem_->storeFact(*conn_, "keep", "v", embed768(0.1f), "general", 5);  // NULL expiry
    EXPECT_EQ(mem_->cleanupExpired(*conn_), 0);
    EXPECT_EQ(count("agent_conversations"), 1);
    EXPECT_EQ(count("agent_memory"), 1);
}

#endif // WITH_POSTGRESQL