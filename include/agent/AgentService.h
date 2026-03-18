#pragma once

#include "agent/IAgentLLM.h"
#include "agent/AgentMemory.h"
#include "agent/AgentTools.h"
#include "mqtt_client.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace hms_cpap {

/**
 * AgentService - Agentic AI orchestrator for CPAP data queries.
 *
 * Subscribes to MQTT query topic, runs LLM tool-use loop with
 * parallel memory search, publishes response.
 *
 * MQTT Topics:
 *   cpap/{device_id}/agent/query      <- user publishes {"text": "...", "conversation_id": "..."}
 *   cpap/{device_id}/agent/response   -> agent publishes {"text": "...", "conversation_id": "...", ...}
 *   cpap/{device_id}/agent/status     -> retained: {"status": "idle|processing|error"}
 */
class AgentService {
public:
    struct Config {
        std::string device_id;
        std::string db_connection_string;
        std::string embed_model = "nomic-embed-text";
        double temperature = 0.3;
        int max_iterations = 5;
        int max_context = 20;
        int memory_limit = 3;
    };

    AgentService(const Config& config,
                 std::shared_ptr<hms::MqttClient> mqtt,
                 std::shared_ptr<IAgentLLM> llm);
    ~AgentService();

    /// Start: subscribe to MQTT, ensure DB schema
    void start();

    /// Stop: unsubscribe, clean up
    void stop();

    bool isRunning() const { return running_; }

private:
    Config config_;
    std::shared_ptr<hms::MqttClient> mqtt_;
    std::shared_ptr<IAgentLLM> llm_;
    std::unique_ptr<AgentTools> tools_;
    std::unique_ptr<AgentMemory> memory_;
    std::atomic<bool> running_{false};
    std::atomic<bool> abort_{false};
    std::mutex processing_mutex_;

    /// Handle incoming query message
    void onQuery(const std::string& payload);

    /// Publish status (retained)
    void publishStatus(const std::string& status);

    /// Build system prompt with optional memory context
    std::string buildSystemPrompt(const std::vector<ConversationContext>& similar_convos,
                                   const std::vector<MemoryFact>& facts);

    /// Run the agentic tool-use loop, return final response text
    std::string runToolLoop(std::vector<hms::ChatMessage>& messages);
};

} // namespace hms_cpap
