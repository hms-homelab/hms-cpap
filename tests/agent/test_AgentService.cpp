#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "agent/AgentService.h"
#include "agent/IAgentLLM.h"
#include "agent/AgentMemory.h"
#include "mqtt_client.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef WITH_POSTGRESQL
#include <pqxx/pqxx>
#endif

using namespace hms_cpap;
using namespace hms;
using json = nlohmann::json;
using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

// ── Mock LLM ────────────────────────────────────────────────────────────────

class MockAgentLLM : public IAgentLLM {
public:
    MOCK_METHOD(LLMToolResponse, generateWithTools,
                (const std::vector<ChatMessage>&, const std::vector<ToolDefinition>&,
                 const std::atomic<bool>*),
                (override));
    MOCK_METHOD(std::vector<float>, embed, (const std::string&), (override));
    MOCK_METHOD(std::vector<float>, embed, (const std::string&, const std::string&), (override));
};

// ── Config / construction tests ─────────────────────────────────────────────

TEST(AgentServiceTest, ConfigDefaults) {
    AgentService::Config config;
    config.device_id = "test_dev";
    config.db_connection_string = "host=localhost dbname=cpap_monitoring";

    EXPECT_EQ(config.embed_model, "nomic-embed-text");
    EXPECT_EQ(config.max_iterations, 5);
    EXPECT_EQ(config.max_context, 20);
    EXPECT_EQ(config.memory_limit, 3);
    EXPECT_DOUBLE_EQ(config.temperature, 0.3);
}

TEST(AgentServiceTest, ConstructsWithoutCrash) {
    AgentService::Config config;
    config.device_id = "test_dev";
    config.db_connection_string = "host=localhost dbname=nonexistent_test_db_999";

    MqttConfig mqtt_cfg;
    mqtt_cfg.broker = "localhost";
    mqtt_cfg.port = 1883;
    mqtt_cfg.client_id = "test_agent";
    auto mqtt = std::make_shared<MqttClient>(mqtt_cfg);
    auto llm = std::make_shared<MockAgentLLM>();

    // Construction should succeed even with invalid DB string
    EXPECT_NO_THROW({
        AgentService service(config, mqtt, llm);
    });
}

TEST(AgentServiceTest, IsNotRunningByDefault) {
    AgentService::Config config;
    config.device_id = "test_dev";
    config.db_connection_string = "host=localhost dbname=test";

    MqttConfig mqtt_cfg;
    mqtt_cfg.client_id = "test";
    auto mqtt = std::make_shared<MqttClient>(mqtt_cfg);
    auto llm = std::make_shared<MockAgentLLM>();

    AgentService service(config, mqtt, llm);
    EXPECT_FALSE(service.isRunning());
}

// start() bails out (returns, never sets running_) if the DB schema setup
// fails. With an unreachable DB host this exercises the start() catch branch.
TEST(AgentServiceTest, StartWithBadDbDoesNotRun) {
    AgentService::Config config;
    config.device_id = "test_dev_baddb";
    // Port 1 is never a postgres server -> connection throws -> start() returns early.
    config.db_connection_string =
        "host=127.0.0.1 port=1 dbname=nope connect_timeout=1";

    MqttConfig mqtt_cfg;
    mqtt_cfg.broker = "localhost";
    mqtt_cfg.port = 1883;
    mqtt_cfg.client_id = "test_agent_baddb";
    auto mqtt = std::make_shared<MqttClient>(mqtt_cfg);
    auto llm = std::make_shared<MockAgentLLM>();

    AgentService service(config, mqtt, llm);
    EXPECT_NO_THROW(service.start());
    EXPECT_FALSE(service.isRunning());
}

// stop() is a no-op when never started (guard: if (!running_) return;).
TEST(AgentServiceTest, StopWhenNotRunningIsNoop) {
    AgentService::Config config;
    config.device_id = "test_dev_stop";
    config.db_connection_string = "host=localhost dbname=test";

    MqttConfig mqtt_cfg;
    mqtt_cfg.client_id = "test_stop";
    auto mqtt = std::make_shared<MqttClient>(mqtt_cfg);
    auto llm = std::make_shared<MockAgentLLM>();

    AgentService service(config, mqtt, llm);
    EXPECT_NO_THROW(service.stop());
    EXPECT_FALSE(service.isRunning());
}

// ── LLM response parsing tests ─────────────────────────────────────────────
// Test that the mock LLM interface works correctly

TEST(AgentServiceTest, MockLLMReturnsTextResponse) {
    auto llm = std::make_shared<MockAgentLLM>();

    LLMToolResponse resp;
    resp.text = "Your AHI was 2.3 last night.";
    resp.stop_reason = "end_turn";

    EXPECT_CALL(*llm, generateWithTools(_, _, _))
        .WillOnce(Return(resp));

    auto result = llm->generateWithTools({}, {}, nullptr);
    EXPECT_TRUE(result.text.has_value());
    EXPECT_EQ(result.text.value(), "Your AHI was 2.3 last night.");
    EXPECT_TRUE(result.tool_calls.empty());
}

TEST(AgentServiceTest, MockLLMReturnsToolCalls) {
    auto llm = std::make_shared<MockAgentLLM>();

    LLMToolResponse resp;
    resp.tool_calls = {{
        "call_1",
        "get_recent_sessions",
        {{"days", 7}, {"limit", 5}}
    }};
    resp.stop_reason = "tool_use";

    EXPECT_CALL(*llm, generateWithTools(_, _, _))
        .WillOnce(Return(resp));

    auto result = llm->generateWithTools({}, {}, nullptr);
    EXPECT_FALSE(result.text.has_value());
    EXPECT_EQ(result.tool_calls.size(), 1u);
    EXPECT_EQ(result.tool_calls[0].name, "get_recent_sessions");
    EXPECT_EQ(result.tool_calls[0].arguments["days"], 7);
}

TEST(AgentServiceTest, MockEmbedReturnsVector) {
    auto llm = std::make_shared<MockAgentLLM>();

    std::vector<float> fake_embedding(768, 0.1f);
    EXPECT_CALL(*llm, embed(std::string("test query")))
        .WillOnce(Return(fake_embedding));

    auto result = llm->embed("test query");
    EXPECT_EQ(result.size(), 768u);
    EXPECT_FLOAT_EQ(result[0], 0.1f);
}

// ── MQTT payload parsing tests ──────────────────────────────────────────────

TEST(AgentServiceTest, QueryPayloadParsing) {
    json payload;
    payload["text"] = "What was my AHI last night?";
    payload["conversation_id"] = "abc-123-def";

    auto parsed = json::parse(payload.dump());
    EXPECT_EQ(parsed["text"], "What was my AHI last night?");
    EXPECT_EQ(parsed["conversation_id"], "abc-123-def");
}

TEST(AgentServiceTest, QueryPayloadWithoutConversationId) {
    json payload;
    payload["text"] = "How is my therapy going?";

    auto parsed = json::parse(payload.dump());
    EXPECT_EQ(parsed.value("text", ""), "How is my therapy going?");
    EXPECT_EQ(parsed.value("conversation_id", ""), "");
}

TEST(AgentServiceTest, ResponsePayloadFormat) {
    json response;
    response["text"] = "Your AHI was 2.3.";
    response["conversation_id"] = "uuid-here";
    response["elapsed_seconds"] = 1.5;

    std::string serialized = response.dump();
    auto parsed = json::parse(serialized);

    EXPECT_EQ(parsed["text"], "Your AHI was 2.3.");
    EXPECT_TRUE(parsed.contains("conversation_id"));
    EXPECT_TRUE(parsed.contains("elapsed_seconds"));
}

// ── End-to-end orchestration tests ───────────────────────────────────────────
//
// These drive the full onQuery() pipeline through the real MQTT broker and
// PostgreSQL, while the LLM is mocked. They cover the conversation
// orchestration, prompt/state assembly, tool-use loop branches, status
// publishing, and guard/early returns that are otherwise unreachable because
// onQuery()/runToolLoop()/buildSystemPrompt() are private.
//
// They skip cleanly (GTEST_SKIP) when the broker or DB is not available, so the
// suite stays green in environments without infrastructure.
#ifdef WITH_POSTGRESQL
namespace {

std::atomic<int> g_uniq{0};

std::string uniqueDeviceId(const std::string& tag) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "test_agent_e2e_" + tag + "_" + std::to_string(now) + "_" +
           std::to_string(g_uniq.fetch_add(1));
}

std::string dbConnString() {
    auto env = [](const char* k, const char* dflt) {
        const char* v = std::getenv(k);
        return std::string(v ? v : dflt);
    };
    return "host=" + env("DB_HOST", "localhost") +
           " port=" + env("DB_PORT", "5432") +
           " dbname=" + env("DB_NAME", "cpap_monitoring") +
           " user=" + env("DB_USER", "maestro") +
           " password=" + env("DB_PASSWORD", "REDACTED") +
           " connect_timeout=3";
}

// Collects messages arriving on a topic; lets a test block until one shows up.
struct Collector {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> payloads;

    void push(const std::string& p) {
        {
            std::lock_guard<std::mutex> lk(m);
            payloads.push_back(p);
        }
        cv.notify_all();
    }

    // Wait until at least `n` payloads have arrived (or timeout).
    bool waitFor(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, timeout, [&] { return payloads.size() >= n; });
    }

    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lk(m);
        return payloads;
    }
};

} // namespace

class AgentServiceE2ETest : public ::testing::Test {
protected:
    bool infra_ok_ = false;
    std::string conn_str_;
    std::string device_id_;
    std::string query_topic_;
    std::string resp_topic_;
    std::string status_topic_;

    std::shared_ptr<MqttClient> svc_mqtt_;   // used by AgentService
    std::shared_ptr<MqttClient> test_mqtt_;  // test publisher/subscriber
    std::shared_ptr<MockAgentLLM> llm_;
    std::unique_ptr<AgentService> service_;

    Collector resp_collector_;
    Collector status_collector_;

    void SetUp() override {
        conn_str_ = dbConnString();

        // DB reachable?
        try {
            pqxx::connection c(conn_str_);
            if (!c.is_open()) return;
        } catch (...) {
            return;
        }

        device_id_ = uniqueDeviceId("svc");
        query_topic_ = "cpap/" + device_id_ + "/agent/query";
        resp_topic_ = "cpap/" + device_id_ + "/agent/response";
        status_topic_ = "cpap/" + device_id_ + "/agent/status";

        MqttConfig svc_cfg;
        svc_cfg.broker = "localhost";
        svc_cfg.port = 1883;
        svc_cfg.client_id = device_id_ + "_svc";
        svc_mqtt_ = std::make_shared<MqttClient>(svc_cfg);

        MqttConfig test_cfg;
        test_cfg.broker = "localhost";
        test_cfg.port = 1883;
        test_cfg.client_id = device_id_ + "_test";
        test_mqtt_ = std::make_shared<MqttClient>(test_cfg);

        if (!svc_mqtt_->connect() || !test_mqtt_->connect()) {
            return;  // broker not available
        }

        // Test client subscribes to what the service will publish.
        test_mqtt_->subscribe(resp_topic_,
            [this](const std::string&, const std::string& p) { resp_collector_.push(p); });
        test_mqtt_->subscribe(status_topic_,
            [this](const std::string&, const std::string& p) { status_collector_.push(p); });

        // Give the subscriptions a brief moment to register on the broker.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        llm_ = std::make_shared<MockAgentLLM>();

        AgentService::Config cfg;
        cfg.device_id = device_id_;
        cfg.db_connection_string = conn_str_;
        cfg.max_iterations = 3;
        service_ = std::make_unique<AgentService>(cfg, svc_mqtt_, llm_);

        infra_ok_ = true;
    }

    void TearDown() override {
        if (service_) service_->stop();
        if (svc_mqtt_) svc_mqtt_->disconnect();
        if (test_mqtt_) test_mqtt_->disconnect();
        // Best-effort DB cleanup of any conversations created for this device.
        if (!conn_str_.empty() && !device_id_.empty()) {
            try {
                pqxx::connection c(conn_str_);
                pqxx::work txn(c);
                txn.exec_params(
                    "DELETE FROM agent_conversations WHERE device_id = $1", device_id_);
                txn.commit();
            } catch (...) {}
        }
    }

    void publishQuery(const std::string& payload) {
        ASSERT_TRUE(test_mqtt_->publish(query_topic_, payload, 1, false));
    }

    // Returns the parsed response JSON, or fails if none arrived.
    json awaitResponse(size_t which = 1) {
        EXPECT_TRUE(resp_collector_.waitFor(which, std::chrono::seconds(10)))
            << "No agent response received";
        auto snap = resp_collector_.snapshot();
        EXPECT_GE(snap.size(), which);
        return json::parse(snap[which - 1]);
    }
};

// Happy path: a single text answer, no tool calls. Covers onQuery() success
// branch, runToolLoop() final-text branch, embed() calls, message storage, and
// the response publish.
TEST_F(AgentServiceE2ETest, TextOnlyResponseRoundTrip) {
    if (!infra_ok_) GTEST_SKIP() << "MQTT broker or DB not available";

    // embed() called for the query and again for the summary.
    EXPECT_CALL(*llm_, embed(_))
        .WillRepeatedly(Return(std::vector<float>(768, 0.01f)));

    LLMToolResponse resp;
    resp.text = "Your average AHI this week is 2.10.";
    resp.stop_reason = "end_turn";
    EXPECT_CALL(*llm_, generateWithTools(_, _, _))
        .WillOnce(Return(resp));

    service_->start();
    ASSERT_TRUE(service_->isRunning());

    json q;
    q["text"] = "What is my AHI this week?";
    publishQuery(q.dump());

    auto response = awaitResponse();
    EXPECT_EQ(response["text"], "Your average AHI this week is 2.10.");
    EXPECT_TRUE(response.contains("conversation_id"));
    EXPECT_FALSE(response["conversation_id"].get<std::string>().empty());
    EXPECT_TRUE(response.contains("elapsed_seconds"));
}

// Tool-use path: first turn returns a tool call, second turn returns text.
// Covers runToolLoop() tool-call branch (execute tools, push tool messages)
// followed by the final-text branch.
TEST_F(AgentServiceE2ETest, ToolCallThenTextResponse) {
    if (!infra_ok_) GTEST_SKIP() << "MQTT broker or DB not available";

    EXPECT_CALL(*llm_, embed(_))
        .WillRepeatedly(Return(std::vector<float>(768, 0.02f)));

    LLMToolResponse turn1;
    turn1.tool_calls = {{"call_a", "get_recent_sessions",
                         {{"days", 7}, {"limit", 5}}}};
    turn1.stop_reason = "tool_use";

    LLMToolResponse turn2;
    turn2.text = "Over the last 7 days you averaged 6.2 hours of use.";
    turn2.stop_reason = "end_turn";

    EXPECT_CALL(*llm_, generateWithTools(_, _, _))
        .WillOnce(Return(turn1))
        .WillOnce(Return(turn2));

    service_->start();
    ASSERT_TRUE(service_->isRunning());

    json q;
    q["text"] = "How many hours did I use my machine last week?";
    publishQuery(q.dump());

    auto response = awaitResponse();
    EXPECT_EQ(response["text"], "Over the last 7 days you averaged 6.2 hours of use.");
}

// Max-iterations guard: the model keeps requesting tools and never produces a
// final answer, so the loop exits with the fallback message after
// max_iterations (3) calls.
TEST_F(AgentServiceE2ETest, ToolLoopHitsMaxIterations) {
    if (!infra_ok_) GTEST_SKIP() << "MQTT broker or DB not available";

    EXPECT_CALL(*llm_, embed(_))
        .WillRepeatedly(Return(std::vector<float>(768, 0.03f)));

    LLMToolResponse tool_turn;
    tool_turn.tool_calls = {{"call_x", "get_daily_summary", {{"days", 1}}}};
    tool_turn.stop_reason = "tool_use";

    // max_iterations == 3 -> exactly 3 calls, all tool calls, no final text.
    EXPECT_CALL(*llm_, generateWithTools(_, _, _))
        .Times(3)
        .WillRepeatedly(Return(tool_turn));

    service_->start();

    json q;
    q["text"] = "Loop forever please";
    publishQuery(q.dump());

    auto response = awaitResponse();
    EXPECT_THAT(response["text"].get<std::string>(),
                ::testing::HasSubstr("maximum number of tool calls"));
}

// Degenerate model response: neither text nor tool calls. Covers the
// runToolLoop() "couldn't generate a response" branch.
TEST_F(AgentServiceE2ETest, EmptyModelResponseFallback) {
    if (!infra_ok_) GTEST_SKIP() << "MQTT broker or DB not available";

    EXPECT_CALL(*llm_, embed(_))
        .WillRepeatedly(Return(std::vector<float>(768, 0.04f)));

    LLMToolResponse empty;  // no text, no tool calls
    empty.stop_reason = "stop";
    EXPECT_CALL(*llm_, generateWithTools(_, _, _))
        .WillOnce(Return(empty));

    service_->start();

    json q;
    q["text"] = "Say nothing useful";
    publishQuery(q.dump());

    auto response = awaitResponse();
    EXPECT_THAT(response["text"].get<std::string>(),
                ::testing::HasSubstr("wasn't able to generate a response"));
}

// Aborted generation: the LLM reports was_aborted -> runToolLoop returns the
// cancellation message.
TEST_F(AgentServiceE2ETest, AbortedGenerationReturnsCancelled) {
    if (!infra_ok_) GTEST_SKIP() << "MQTT broker or DB not available";

    EXPECT_CALL(*llm_, embed(_))
        .WillRepeatedly(Return(std::vector<float>(768, 0.05f)));

    LLMToolResponse aborted;
    aborted.was_aborted = true;
    EXPECT_CALL(*llm_, generateWithTools(_, _, _))
        .WillOnce(Return(aborted));

    service_->start();

    json q;
    q["text"] = "This will be cancelled";
    publishQuery(q.dump());

    auto response = awaitResponse();
    EXPECT_THAT(response["text"].get<std::string>(),
                ::testing::HasSubstr("cancelled"));
}

// Empty text guard: onQuery() returns early (status back to idle) and never
// calls the LLM or publishes a response.
TEST_F(AgentServiceE2ETest, EmptyTextQueryProducesNoResponse) {
    if (!infra_ok_) GTEST_SKIP() << "MQTT broker or DB not available";

    // LLM must never be invoked for an empty query.
    EXPECT_CALL(*llm_, generateWithTools(_, _, _)).Times(0);
    EXPECT_CALL(*llm_, embed(_)).Times(0);

    service_->start();

    json q;
    q["text"] = "";  // empty -> early return
    publishQuery(q.dump());

    // No response should arrive on the response topic.
    EXPECT_FALSE(resp_collector_.waitFor(1, std::chrono::seconds(2)))
        << "Empty query should not yield a response";
}

// Malformed JSON payload: onQuery() catch block publishes an error response.
TEST_F(AgentServiceE2ETest, MalformedPayloadPublishesError) {
    if (!infra_ok_) GTEST_SKIP() << "MQTT broker or DB not available";

    EXPECT_CALL(*llm_, generateWithTools(_, _, _)).Times(0);

    service_->start();

    publishQuery("this is not json{{{");

    auto response = awaitResponse();
    EXPECT_TRUE(response.contains("error"));
    EXPECT_TRUE(response["error"].get<bool>());
    EXPECT_THAT(response["text"].get<std::string>(),
                ::testing::HasSubstr("error occurred"));
}

// Conversation continuity: a returned conversation_id can be fed back in and is
// preserved (getOrCreateConversation existing-id branch + prior message
// loading into the prompt).
TEST_F(AgentServiceE2ETest, ConversationIdIsReused) {
    if (!infra_ok_) GTEST_SKIP() << "MQTT broker or DB not available";

    EXPECT_CALL(*llm_, embed(_))
        .WillRepeatedly(Return(std::vector<float>(768, 0.06f)));

    LLMToolResponse a;
    a.text = "First answer.";
    a.stop_reason = "end_turn";
    LLMToolResponse b;
    b.text = "Second answer.";
    b.stop_reason = "end_turn";
    EXPECT_CALL(*llm_, generateWithTools(_, _, _))
        .WillOnce(Return(a))
        .WillOnce(Return(b));

    service_->start();

    json q1;
    q1["text"] = "First question";
    publishQuery(q1.dump());
    auto r1 = awaitResponse(1);
    std::string conv_id = r1["conversation_id"].get<std::string>();
    ASSERT_FALSE(conv_id.empty());

    json q2;
    q2["text"] = "Second question";
    q2["conversation_id"] = conv_id;
    publishQuery(q2.dump());
    auto r2 = awaitResponse(2);
    EXPECT_EQ(r2["conversation_id"].get<std::string>(), conv_id);
    EXPECT_EQ(r2["text"], "Second answer.");
}

// start() publishes an "idle" status and processing transitions publish
// "processing" then "idle". Covers publishStatus() and the status lifecycle.
TEST_F(AgentServiceE2ETest, PublishesStatusLifecycle) {
    if (!infra_ok_) GTEST_SKIP() << "MQTT broker or DB not available";

    EXPECT_CALL(*llm_, embed(_))
        .WillRepeatedly(Return(std::vector<float>(768, 0.07f)));
    LLMToolResponse resp;
    resp.text = "Status test answer.";
    resp.stop_reason = "end_turn";
    EXPECT_CALL(*llm_, generateWithTools(_, _, _))
        .WillOnce(Return(resp));

    service_->start();

    // Initial "idle" from start().
    ASSERT_TRUE(status_collector_.waitFor(1, std::chrono::seconds(5)));

    json q;
    q["text"] = "Trigger processing";
    publishQuery(q.dump());

    // Wait for the response so the full lifecycle has run.
    awaitResponse();

    // Among all statuses we should have seen "processing" and "idle".
    auto statuses = status_collector_.snapshot();
    bool saw_processing = false, saw_idle = false;
    for (const auto& s : statuses) {
        auto j = json::parse(s);
        if (j["status"] == "processing") saw_processing = true;
        if (j["status"] == "idle") saw_idle = true;
    }
    EXPECT_TRUE(saw_processing);
    EXPECT_TRUE(saw_idle);
}

// buildSystemPrompt() non-empty facts branch: seed a memory fact for this
// device, then query. The fact is retrieved by the memory search and folded
// into the system prompt that the (mocked) LLM receives. We assert the prompt
// content via a captured system message.
TEST_F(AgentServiceE2ETest, SystemPromptIncludesStoredFact) {
    if (!infra_ok_) GTEST_SKIP() << "MQTT broker or DB not available";

    // Seed a fact with a known embedding so cosine search returns it.
    std::vector<float> emb(768, 0.5f);
    {
        pqxx::connection c(conn_str_);
        AgentMemory mem(device_id_);
        mem.ensureSchema(c);
        mem.storeFact(c, "preferred_metric", "AHI", emb, "general", 5);
    }

    // The query embedding matches the seeded fact embedding exactly.
    EXPECT_CALL(*llm_, embed(_))
        .WillRepeatedly(Return(emb));

    std::string captured_system;
    LLMToolResponse resp;
    resp.text = "Noted.";
    resp.stop_reason = "end_turn";
    EXPECT_CALL(*llm_, generateWithTools(_, _, _))
        .WillOnce(Invoke([&](const std::vector<ChatMessage>& msgs,
                             const std::vector<ToolDefinition>&,
                             const std::atomic<bool>*) {
            if (!msgs.empty() && msgs.front().role == "system") {
                captured_system = msgs.front().content;
            }
            return resp;
        }));

    service_->start();

    json q;
    q["text"] = "What metric do I care about?";
    publishQuery(q.dump());

    awaitResponse();

    // The base prompt is always present; the fact section should appear too.
    EXPECT_THAT(captured_system, ::testing::HasSubstr("CPAP therapy assistant"));
    EXPECT_THAT(captured_system, ::testing::HasSubstr("preferred_metric"));
    EXPECT_THAT(captured_system, ::testing::HasSubstr("AHI"));

    // Cleanup the fact.
    try {
        pqxx::connection c(conn_str_);
        pqxx::work txn(c);
        txn.exec_params("DELETE FROM agent_memory WHERE device_id = $1", device_id_);
        txn.commit();
    } catch (...) {}
}

// Default (empty memory) system prompt: a fresh device has no facts/convos, so
// buildSystemPrompt() takes the empty branches and emits only the base prompt.
TEST_F(AgentServiceE2ETest, SystemPromptBaseOnlyWhenNoMemory) {
    if (!infra_ok_) GTEST_SKIP() << "MQTT broker or DB not available";

    EXPECT_CALL(*llm_, embed(_))
        .WillRepeatedly(Return(std::vector<float>(768, 0.0f)));

    std::string captured_system;
    LLMToolResponse resp;
    resp.text = "Hello.";
    resp.stop_reason = "end_turn";
    EXPECT_CALL(*llm_, generateWithTools(_, _, _))
        .WillOnce(Invoke([&](const std::vector<ChatMessage>& msgs,
                             const std::vector<ToolDefinition>&,
                             const std::atomic<bool>*) {
            if (!msgs.empty() && msgs.front().role == "system") {
                captured_system = msgs.front().content;
            }
            return resp;
        }));

    service_->start();

    json q;
    q["text"] = "Hi there";
    publishQuery(q.dump());

    awaitResponse();

    EXPECT_THAT(captured_system, ::testing::HasSubstr("CPAP therapy assistant"));
    // No facts seeded -> no "Relevant facts" section.
    EXPECT_EQ(captured_system.find("Relevant facts about this user"),
              std::string::npos);
}

#endif // WITH_POSTGRESQL
