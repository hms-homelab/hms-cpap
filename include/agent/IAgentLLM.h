#pragma once

#include "llm_client.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * IAgentLLM - Interface wrapping hms::LLMClient for testability.
 *
 * Production implementation delegates to LLMClient.
 * Tests can mock this to control LLM responses.
 */
class IAgentLLM {
public:
    virtual ~IAgentLLM() = default;

    virtual hms::LLMToolResponse generateWithTools(
        const std::vector<hms::ChatMessage>& messages,
        const std::vector<hms::ToolDefinition>& tools,
        const std::atomic<bool>* abort_flag = nullptr) = 0;

    virtual std::vector<float> embed(const std::string& text) = 0;
    virtual std::vector<float> embed(const std::string& text, const std::string& model) = 0;
};

/**
 * Production implementation delegating to hms::LLMClient.
 *
 * Supports separate LLM clients for chat (tool-use) and embeddings,
 * since the chat model may be on a different provider (e.g., OpenAI)
 * than the embedding model (e.g., Ollama nomic-embed-text).
 */
class AgentLLM : public IAgentLLM {
public:
    /// Single client for both chat and embed (same provider)
    explicit AgentLLM(std::shared_ptr<hms::LLMClient> llm, const std::string& embed_model = "nomic-embed-text")
        : llm_(std::move(llm)), embed_llm_(nullptr), embed_model_(embed_model) {}

    /// Separate clients: llm for chat/tools, embed_llm for embeddings
    AgentLLM(std::shared_ptr<hms::LLMClient> llm,
             std::shared_ptr<hms::LLMClient> embed_llm,
             const std::string& embed_model = "nomic-embed-text")
        : llm_(std::move(llm)), embed_llm_(std::move(embed_llm)), embed_model_(embed_model) {}

    hms::LLMToolResponse generateWithTools(
        const std::vector<hms::ChatMessage>& messages,
        const std::vector<hms::ToolDefinition>& tools,
        const std::atomic<bool>* abort_flag = nullptr) override {
        return llm_->generateWithTools(messages, tools, abort_flag);
    }

    std::vector<float> embed(const std::string& text) override {
        auto& client = embed_llm_ ? embed_llm_ : llm_;
        return client->embed(text, embed_model_);
    }

    std::vector<float> embed(const std::string& text, const std::string& model) override {
        auto& client = embed_llm_ ? embed_llm_ : llm_;
        return client->embed(text, model);
    }

private:
    std::shared_ptr<hms::LLMClient> llm_;
    std::shared_ptr<hms::LLMClient> embed_llm_;  // optional: separate client for embeddings
    std::string embed_model_;
};

} // namespace hms_cpap
