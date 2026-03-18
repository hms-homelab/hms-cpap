#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "agent/AgentService.h"
#include "agent/IAgentLLM.h"
#include "mqtt_client.h"
#include <nlohmann/json.hpp>

using namespace hms_cpap;
using namespace hms;
using json = nlohmann::json;
using ::testing::_;
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
