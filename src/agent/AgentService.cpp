#include "agent/AgentService.h"
#include "services/NightFacts.h"
#include <nlohmann/json.hpp>
#ifdef WITH_POSTGRESQL
#include <pqxx/pqxx>
#endif
#include <chrono>
#include <future>
#include <iostream>
#include <optional>

namespace hms_cpap {

AgentService::AgentService(const Config& config,
                           std::shared_ptr<hms::MqttClient> mqtt,
                           std::shared_ptr<IAgentLLM> llm,
                           std::shared_ptr<IDatabase> db)
    : config_(config)
    , mqtt_(std::move(mqtt))
    , llm_(std::move(llm))
    , db_(std::move(db))
    , tools_(std::make_unique<AgentTools>(config.device_id))
#ifdef WITH_POSTGRESQL
    , memory_(std::make_unique<AgentMemory>(config.device_id))
#endif
{}

AgentService::~AgentService() {
    stop();
}

void AgentService::start() {
    if (running_) return;

    // SDD-072 D4: the tools need the database, on whichever engine it is.
    if (!db_ || !db_->isConnected()) {
        std::cerr << "Agent: no database connection, not started" << std::endl;
        return;
    }

#ifdef WITH_POSTGRESQL
    // The pgvector memory is PostgreSQL's; elsewhere there is none.
    if (db_->dbType() == DbType::POSTGRESQL && !config_.db_connection_string.empty()) {
        try {
            pqxx::connection conn(config_.db_connection_string);
            memory_->ensureSchema(conn);
            memory_enabled_ = true;
            std::cout << "Agent: Memory schema ready" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Agent: Memory schema setup failed, answering without memory: "
                      << e.what() << std::endl;
        }
    }
#endif
    if (!memory_enabled_)
        std::cout << "Agent: no conversation memory on this engine" << std::endl;

    // Subscribe to query topic
    std::string query_topic = "cpap/" + config_.device_id + "/agent/query";
    mqtt_->subscribe(query_topic, [this](const std::string& /*topic*/, const std::string& payload) {
        onQuery(payload);
    });

    running_ = true;
    publishStatus("idle");
    std::cout << "Agent: Started, listening on " << query_topic << std::endl;
}

void AgentService::stop() {
    if (!running_) return;
    running_ = false;
    abort_ = true;
    publishStatus("offline");
}

void AgentService::onQuery(const std::string& payload) {
    // Only one query at a time
    std::unique_lock<std::mutex> lock(processing_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::string resp_topic = "cpap/" + config_.device_id + "/agent/response";
        nlohmann::json err;
        err["text"] = "I'm still processing a previous query. Please wait.";
        err["error"] = true;
        mqtt_->publish(resp_topic, err.dump());
        return;
    }

    auto start_time = std::chrono::steady_clock::now();
    publishStatus("processing");
    abort_ = false;

    try {
        // Parse input
        auto input = nlohmann::json::parse(payload);
        std::string text = input.value("text", "");
        std::string conv_id = input.value("conversation_id", "");

        if (text.empty()) {
            publishStatus("idle");
            return;
        }

        std::vector<ConversationContext> convos;
        std::vector<MemoryFact> facts;
        std::vector<float> embedding;
        std::vector<std::pair<std::string, std::string>> prior_msgs;
#ifdef WITH_POSTGRESQL
        std::optional<pqxx::connection> conn;
        if (memory_enabled_) {
            conn.emplace(config_.db_connection_string);

            // Get or create conversation
            conv_id = memory_->getOrCreateConversation(*conn, conv_id);

            // Parallel phase: embed query + search memory
            auto mem_future = std::async(std::launch::async, [&]() {
                try {
                    embedding = llm_->embed(text);
                    pqxx::connection mem_conn(config_.db_connection_string);
                    convos = memory_->searchSimilar(mem_conn, embedding, config_.memory_limit);
                    facts = memory_->searchFacts(mem_conn, embedding, 5);
                } catch (const std::exception& e) {
                    std::cerr << "Agent: Memory search error: " << e.what() << std::endl;
                }
            });

            // Get prior messages while memory search runs
            prior_msgs = memory_->getMessages(*conn, conv_id, config_.max_context);
            mem_future.get();
        }
#endif
        if (conv_id.empty()) {
            // No memory to issue one: the reply still names its exchange.
            conv_id = "local-" + std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
        }

        // Build messages
        std::string system_prompt = buildSystemPrompt(convos, facts);
        std::vector<hms::ChatMessage> messages;
        messages.push_back({"system", system_prompt, {}, ""});

        for (const auto& [role, content] : prior_msgs) {
            messages.push_back({role, content, {}, ""});
        }
        messages.push_back({"user", text, {}, ""});

        // Run tool-use loop
        std::string response_text = runToolLoop(messages);

#ifdef WITH_POSTGRESQL
        if (memory_enabled_ && conn) {
            // Store conversation
            memory_->storeMessage(*conn, conv_id, "user", text, embedding);
            memory_->storeMessage(*conn, conv_id, "assistant", response_text);

            // Update summary
            std::string summary = text.substr(0, 100) + " -> " + response_text.substr(0, 100);
            std::vector<float> summary_emb;
            try {
                summary_emb = llm_->embed(summary);
            } catch (...) {}
            memory_->updateSummary(*conn, conv_id, summary, summary_emb);
        }
#endif

        // Publish response
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count() / 1000.0;

        nlohmann::json response;
        response["text"] = response_text;
        response["conversation_id"] = conv_id;
        response["elapsed_seconds"] = elapsed;

        std::string resp_topic = "cpap/" + config_.device_id + "/agent/response";
        mqtt_->publish(resp_topic, response.dump());

    } catch (const std::exception& e) {
        std::cerr << "Agent: Query error: " << e.what() << std::endl;
        nlohmann::json err;
        err["text"] = std::string("Sorry, an error occurred: ") + e.what();
        err["error"] = true;
        std::string resp_topic = "cpap/" + config_.device_id + "/agent/response";
        mqtt_->publish(resp_topic, err.dump());
    }

    publishStatus("idle");
}

void AgentService::publishStatus(const std::string& status) {
    std::string topic = "cpap/" + config_.device_id + "/agent/status";
    nlohmann::json j;
    j["status"] = status;
    mqtt_->publish(topic, j.dump(), 1, true);
}

std::string AgentService::basePrompt() {
    // SDD-072 D4: no "AHI > 5" grading for every machine (SDD-024: an index
    // that is not an AHI must not be graded); D2's rules instead, and every
    // number from a tool result.
    std::string prompt = R"(You are a CPAP therapy assistant that helps users understand their sleep apnea treatment data.

You have access to tools that query the user's CPAP therapy database. Use these tools to answer questions with specific data.

Guidelines:
- Always use tools to look up data rather than guessing
- Every number you state must come from a tool result. Never fill a gap: if a figure is not in a result, say it is not available
- Each tool result carries "rules"; follow them
- When a result says it was capped, say that the list is not complete
- Report the AHI only where a result gives one. Where it says the AHI is not available, the machine does not detect hypopneas: do not estimate an AHI or describe the events as mild, normal or severe
- Present numbers with the precision the tool result gives them
- Explain medical metrics in plain language when relevant
- Be encouraging about positive trends
- If data is not available, say so clearly
- Use the get_daily_summary tool for official ResMed stats, get_recent_sessions for detailed session data
- When comparing periods, use the compare_periods tool
)";
    prompt += "\nRules:\n";
    prompt += std::string("- ") + night_facts::kEventLabelRule + "\n";
    prompt += std::string("- ") + night_facts::kUngradedRule + "\n";
    return prompt;
}

std::string AgentService::buildSystemPrompt(
    const std::vector<ConversationContext>& similar_convos,
    const std::vector<MemoryFact>& facts) {

    std::string prompt = basePrompt();

    if (!facts.empty()) {
        prompt += "\nRelevant facts about this user:\n";
        for (const auto& f : facts) {
            prompt += "- " + f.key + ": " + f.value + "\n";
        }
    }

    if (!similar_convos.empty()) {
        prompt += "\nRecent related conversations:\n";
        for (const auto& c : similar_convos) {
            if (c.similarity > 0.5) {
                prompt += "- " + c.summary + "\n";
            }
        }
    }

    return prompt;
}

std::string AgentService::runToolLoop(std::vector<hms::ChatMessage>& messages) {
    for (int i = 0; i < config_.max_iterations; ++i) {
        if (abort_) return "Query was cancelled.";

        auto resp = llm_->generateWithTools(messages, tools_->definitions(), &abort_);

        if (resp.was_aborted) return "Query was cancelled.";

        if (!resp.tool_calls.empty()) {
            // Assistant made tool calls
            hms::ChatMessage assistant_msg;
            assistant_msg.role = "assistant";
            assistant_msg.content = resp.text.value_or("");
            assistant_msg.tool_calls = resp.tool_calls;
            messages.push_back(assistant_msg);

            // Execute each tool call, through the database layer (SDD-072 D4)
            for (const auto& tc : resp.tool_calls) {
                hms::ChatMessage tool_msg;
                tool_msg.role = "tool";
                tool_msg.content = tools_->execute(tc.name, tc.arguments, *db_);
                tool_msg.tool_call_id = tc.id;
                messages.push_back(tool_msg);
            }
        } else if (resp.text.has_value()) {
            // Final text response
            return resp.text.value();
        } else {
            return "I wasn't able to generate a response. Please try rephrasing your question.";
        }
    }

    return "I reached the maximum number of tool calls. Here's what I found so far based on the data retrieved.";
}

} // namespace hms_cpap
