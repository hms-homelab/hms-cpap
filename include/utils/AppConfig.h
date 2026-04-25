#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace hms_cpap {

struct AppConfig {
    // Device
    std::string device_id = "cpap_resmed_23243570851";
    std::string device_name = "ResMed AirSense 10";

    // Data source
    std::string source = "ezshare";  // "ezshare" (HTTP), "local"
    std::string ezshare_url = "http://192.168.4.1";
    bool ezshare_range = true;
    std::string local_dir;
    int burst_interval = 65;

    // Web server
    int web_port = 8893;
    std::string static_dir = "./static/browser";

    // Database
    struct Database {
        std::string type = "sqlite";  // "sqlite", "mysql", "postgresql"
        std::string sqlite_path;      // defaults to ~/.hms-cpap/cpap.db
        std::string host;
        int port = 0;
        std::string name;
        std::string user;
        std::string password;
    } database;

    // MQTT (optional)
    struct Mqtt {
        bool enabled = false;
        std::string broker;
        int port = 1883;
        std::string username;
        std::string password;
        std::string client_id = "hms_cpap";
    } mqtt;

    // LLM (optional)
    struct Llm {
        bool enabled = false;
        std::string provider = "ollama";
        std::string endpoint;
        std::string model;
        std::string api_key;
        int max_tokens = 1024;
        std::string prompt_file;
    } llm;

    // Agent (optional, requires LLM + PostgreSQL)
    struct Agent {
        bool enabled = false;
        std::string embed_model = "nomic-embed-text";
        double temperature = 0.3;
        int max_iterations = 5;
    } agent;

    // ML Training (optional)
    struct MlTraining {
        bool enabled = false;
        std::string schedule = "weekly";  // "daily", "weekly", "monthly"
        std::string model_dir;            // default: empty (uses dataDir()/models)
        int min_days = 30;                // minimum therapy days before training
        int max_training_days = 0;        // 0 = use all data, >0 = limit lookback
    } ml_training;

    // O2 Ring Oximetry (optional)
    struct O2Ring {
        bool enabled = false;
        std::string mode = "http";      // "http" or "ble"
        std::string mule_url;           // e.g. "http://192.168.2.74"
    } o2ring;

    // Sleep Stage Inference (optional)
    struct SleepStage {
        bool enabled = false;
        bool live_inference = true;
        std::string model_dir;          // default: empty (uses dataDir()/models)
        std::string model_version = "shhs-rf-v1";
    } sleep_stage;

    // Fysetc TCP (raw sector push mode)
    struct Fysetc {
        bool enabled = false;
        int listen_port = 9000;
        std::string listen_bind = "0.0.0.0";
        int connection_timeout_s = 30;
        std::string archive_dir;
        std::string log_dir = "/var/log/maestro_hub";
    } fysetc;

    bool setup_complete = false;

    /// Fill empty config fields from environment variables (fallback).
    /// Call AFTER loading config.json so file values take precedence.
    void applyEnvFallbacks() {
        auto env = [](const char* name) -> std::string {
            const char* v = std::getenv(name);
            return v ? v : "";
        };
        auto envInt = [&](const char* name, int def) -> int {
            auto s = env(name);
            return s.empty() ? def : std::stoi(s);
        };

        // Device / source
        if (device_id.empty())   device_id   = env("CPAP_DEVICE_ID");
        if (device_name.empty()) device_name = env("CPAP_DEVICE_NAME");
        if (source.empty())      source      = env("CPAP_SOURCE");
        if (ezshare_url.empty()) ezshare_url = env("EZSHARE_BASE_URL");
        {
            auto v = env("EZSHARE_SUPPORTS_RANGE");
            if (v == "false" || v == "0") ezshare_range = false;
        }
        if (local_dir.empty())   local_dir   = env("CPAP_LOCAL_DIR");
        if (burst_interval == 65) {
            int v = envInt("BURST_INTERVAL", 0);
            if (v > 0) burst_interval = v;
        }
        if (web_port == 8893) {
            int v = envInt("HEALTH_CHECK_PORT", 0);
            if (v > 0) web_port = v;
        }

        // Database — env vars imply postgresql
        if (database.host.empty()) database.host = env("DB_HOST");
        if (database.name.empty()) database.name = env("DB_NAME");
        if (database.user.empty()) database.user = env("DB_USER");
        if (database.password.empty()) database.password = env("DB_PASSWORD");
        if (database.port == 0) {
            int v = envInt("DB_PORT", 0);
            if (v > 0) database.port = v;
        }
        // If DB credentials came from env, switch type to postgresql
        if (database.type == "sqlite" && !database.host.empty() && !database.name.empty())
            database.type = "postgresql";

        // MQTT — env vars imply enabled
        if (mqtt.broker.empty())    mqtt.broker    = env("MQTT_BROKER");
        if (mqtt.username.empty())  mqtt.username  = env("MQTT_USER");
        if (mqtt.password.empty())  mqtt.password  = env("MQTT_PASSWORD");
        if (mqtt.client_id == "hms_cpap") {
            auto v = env("MQTT_CLIENT_ID");
            if (!v.empty()) mqtt.client_id = v;
        }
        if (mqtt.port == 1883) {
            int v = envInt("MQTT_PORT", 0);
            if (v > 0) mqtt.port = v;
        }
        if (!mqtt.enabled && !mqtt.broker.empty())
            mqtt.enabled = true;

        // LLM
        if (llm.endpoint.empty()) llm.endpoint = env("LLM_ENDPOINT");
        if (llm.model.empty())    llm.model    = env("LLM_MODEL");
        if (llm.api_key.empty())  llm.api_key  = env("LLM_API_KEY");
        if (llm.provider == "ollama") {
            auto v = env("LLM_PROVIDER");
            if (!v.empty()) llm.provider = v;
        }
        if (llm.prompt_file.empty()) llm.prompt_file = env("LLM_PROMPT_FILE");
        if (llm.max_tokens == 1024) {
            int v = envInt("LLM_MAX_TOKENS", 0);
            if (v > 0) llm.max_tokens = v;
        }
        if (!llm.enabled && env("LLM_ENABLED") == "true")
            llm.enabled = true;

        // Agent
        if (!agent.enabled && env("AGENT_ENABLED") == "true")
            agent.enabled = true;
        if (agent.embed_model == "nomic-embed-text") {
            auto v = env("AGENT_EMBED_MODEL");
            if (!v.empty()) agent.embed_model = v;
        }

        // Fysetc TCP
        if (!fysetc.enabled && env("FYSETC_ENABLED") == "true")
            fysetc.enabled = true;
        if (fysetc.listen_port == 9000) {
            int v = envInt("FYSETC_LISTEN_PORT", 0);
            if (v > 0) fysetc.listen_port = v;
        }
        if (fysetc.archive_dir.empty()) fysetc.archive_dir = env("FYSETC_ARCHIVE_DIR");
        if (fysetc.log_dir == "/var/log/maestro_hub") {
            auto v = env("FYSETC_LOG_DIR");
            if (!v.empty()) fysetc.log_dir = v;
        }

        // ML Training
        if (!ml_training.enabled && env("ML_ENABLED") == "true")
            ml_training.enabled = true;
        if (ml_training.schedule == "weekly") {
            auto v = env("ML_SCHEDULE");
            if (!v.empty()) ml_training.schedule = v;
        }
        if (ml_training.model_dir.empty()) ml_training.model_dir = env("ML_MODEL_DIR");
        if (ml_training.max_training_days == 0) {
            int v = envInt("ML_MAX_TRAINING_DAYS", 0);
            if (v > 0) ml_training.max_training_days = v;
        }
        if (ml_training.min_days == 30) {
            int v = envInt("ML_MIN_DAYS", 0);
            if (v > 0) ml_training.min_days = v;
        }

        // Sleep Stage
        if (!sleep_stage.enabled && env("SLEEP_STAGE_ENABLED") == "true")
            sleep_stage.enabled = true;
        {
            auto v = env("SLEEP_STAGE_LIVE");
            if (v == "false" || v == "0") sleep_stage.live_inference = false;
        }
        if (sleep_stage.model_dir.empty()) sleep_stage.model_dir = env("SLEEP_STAGE_MODEL_DIR");
        if (sleep_stage.model_version == "shhs-rf-v1") {
            auto v = env("SLEEP_STAGE_MODEL_VERSION");
            if (!v.empty()) sleep_stage.model_version = v;
        }
    }

    // Get default data directory (~/.hms-cpap/)
    static std::string dataDir() {
        std::string home;
        #ifdef _WIN32
        home = std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "C:\\";
        return home + "\\.hms-cpap";
        #else
        home = std::getenv("HOME") ? std::getenv("HOME") : "/tmp";
        return home + "/.hms-cpap";
        #endif
    }

    // Load from JSON file. Returns false if file doesn't exist (first run).
    static bool load(const std::string& path, AppConfig& config) {
        if (!std::filesystem::exists(path)) return false;
        try {
            std::ifstream f(path);
            nlohmann::json j;
            f >> j;

            if (j.contains("device_id"))    config.device_id = j["device_id"];
            if (j.contains("device_name"))  config.device_name = j["device_name"];
            if (j.contains("source"))       config.source = j["source"];
            if (j.contains("ezshare_url"))    config.ezshare_url = j["ezshare_url"];
            if (j.contains("ezshare_range")) config.ezshare_range = j["ezshare_range"];
            if (j.contains("local_dir"))     config.local_dir = j["local_dir"];
            if (j.contains("burst_interval")) config.burst_interval = j["burst_interval"];
            if (j.contains("web_port"))     config.web_port = j["web_port"];
            if (j.contains("static_dir"))   config.static_dir = j["static_dir"];
            if (j.contains("setup_complete")) config.setup_complete = j["setup_complete"];

            if (j.contains("database")) {
                auto& d = j["database"];
                if (d.contains("type"))        config.database.type = d["type"];
                if (d.contains("sqlite_path")) config.database.sqlite_path = d["sqlite_path"];
                if (d.contains("host"))        config.database.host = d["host"];
                if (d.contains("port"))        config.database.port = d["port"];
                if (d.contains("name"))        config.database.name = d["name"];
                if (d.contains("user"))        config.database.user = d["user"];
                if (d.contains("password"))    config.database.password = d["password"];
            }

            if (j.contains("mqtt")) {
                auto& m = j["mqtt"];
                if (m.contains("enabled"))   config.mqtt.enabled = m["enabled"];
                if (m.contains("broker"))    config.mqtt.broker = m["broker"];
                if (m.contains("port"))      config.mqtt.port = m["port"];
                if (m.contains("username"))  config.mqtt.username = m["username"];
                if (m.contains("password"))  config.mqtt.password = m["password"];
                if (m.contains("client_id")) config.mqtt.client_id = m["client_id"];
            }

            if (j.contains("llm")) {
                auto& l = j["llm"];
                if (l.contains("enabled"))     config.llm.enabled = l["enabled"];
                if (l.contains("provider"))    config.llm.provider = l["provider"];
                if (l.contains("endpoint"))    config.llm.endpoint = l["endpoint"];
                if (l.contains("model"))       config.llm.model = l["model"];
                if (l.contains("api_key"))     config.llm.api_key = l["api_key"];
                if (l.contains("max_tokens"))  config.llm.max_tokens = l["max_tokens"];
                if (l.contains("prompt_file")) config.llm.prompt_file = l["prompt_file"];
            }

            if (j.contains("agent")) {
                auto& a = j["agent"];
                if (a.contains("enabled"))        config.agent.enabled = a["enabled"];
                if (a.contains("embed_model"))    config.agent.embed_model = a["embed_model"];
                if (a.contains("temperature"))    config.agent.temperature = a["temperature"];
                if (a.contains("max_iterations")) config.agent.max_iterations = a["max_iterations"];
            }

            if (j.contains("ml_training")) {
                auto& ml = j["ml_training"];
                if (ml.contains("enabled"))   config.ml_training.enabled = ml["enabled"];
                if (ml.contains("schedule"))  config.ml_training.schedule = ml["schedule"];
                if (ml.contains("model_dir")) config.ml_training.model_dir = ml["model_dir"];
                if (ml.contains("min_days"))  config.ml_training.min_days = ml["min_days"];
                if (ml.contains("max_training_days")) config.ml_training.max_training_days = ml["max_training_days"];
            }

            if (j.contains("fysetc")) {
                auto& f = j["fysetc"];
                if (f.contains("enabled"))            config.fysetc.enabled = f["enabled"];
                if (f.contains("listen_port"))        config.fysetc.listen_port = f["listen_port"];
                if (f.contains("listen_bind"))        config.fysetc.listen_bind = f["listen_bind"];
                if (f.contains("connection_timeout_s")) config.fysetc.connection_timeout_s = f["connection_timeout_s"];
                if (f.contains("archive_dir"))        config.fysetc.archive_dir = f["archive_dir"];
                if (f.contains("log_dir"))            config.fysetc.log_dir = f["log_dir"];
            }

            if (j.contains("o2ring")) {
                auto& o = j["o2ring"];
                if (o.contains("enabled"))              config.o2ring.enabled = o["enabled"];
                if (o.contains("mode"))                 config.o2ring.mode = o["mode"];
                if (o.contains("mule_url"))             config.o2ring.mule_url = o["mule_url"];
            }

            if (j.contains("sleep_stage")) {
                auto& ss = j["sleep_stage"];
                if (ss.contains("enabled"))        config.sleep_stage.enabled = ss["enabled"];
                if (ss.contains("live_inference"))  config.sleep_stage.live_inference = ss["live_inference"];
                if (ss.contains("model_dir"))      config.sleep_stage.model_dir = ss["model_dir"];
                if (ss.contains("model_version"))  config.sleep_stage.model_version = ss["model_version"];
            }

            return true;
        } catch (const std::exception& e) {
            std::cerr << "Config load error: " << e.what() << std::endl;
            return false;
        }
    }

    // Save to JSON file
    bool save(const std::string& path) const {
        try {
            std::filesystem::create_directories(std::filesystem::path(path).parent_path());
            nlohmann::json j;
            j["device_id"] = device_id;
            j["device_name"] = device_name;
            j["source"] = source;
            j["ezshare_url"] = ezshare_url;
            j["ezshare_range"] = ezshare_range;
            j["local_dir"] = local_dir;
            j["burst_interval"] = burst_interval;
            j["web_port"] = web_port;
            j["static_dir"] = static_dir;
            j["setup_complete"] = setup_complete;

            j["database"]["type"] = database.type;
            j["database"]["sqlite_path"] = database.sqlite_path;
            j["database"]["host"] = database.host;
            j["database"]["port"] = database.port;
            j["database"]["name"] = database.name;
            j["database"]["user"] = database.user;
            j["database"]["password"] = database.password;

            j["mqtt"]["enabled"] = mqtt.enabled;
            j["mqtt"]["broker"] = mqtt.broker;
            j["mqtt"]["port"] = mqtt.port;
            j["mqtt"]["username"] = mqtt.username;
            j["mqtt"]["password"] = mqtt.password;
            j["mqtt"]["client_id"] = mqtt.client_id;

            j["llm"]["enabled"] = llm.enabled;
            j["llm"]["provider"] = llm.provider;
            j["llm"]["endpoint"] = llm.endpoint;
            j["llm"]["model"] = llm.model;
            j["llm"]["api_key"] = llm.api_key;
            j["llm"]["max_tokens"] = llm.max_tokens;
            j["llm"]["prompt_file"] = llm.prompt_file;

            j["agent"]["enabled"] = agent.enabled;
            j["agent"]["embed_model"] = agent.embed_model;
            j["agent"]["temperature"] = agent.temperature;
            j["agent"]["max_iterations"] = agent.max_iterations;

            j["ml_training"]["enabled"] = ml_training.enabled;
            j["ml_training"]["schedule"] = ml_training.schedule;
            j["ml_training"]["model_dir"] = ml_training.model_dir;
            j["ml_training"]["min_days"] = ml_training.min_days;
            j["ml_training"]["max_training_days"] = ml_training.max_training_days;

            j["o2ring"]["enabled"] = o2ring.enabled;
            j["o2ring"]["mode"] = o2ring.mode;
            j["o2ring"]["mule_url"] = o2ring.mule_url;

            j["sleep_stage"]["enabled"] = sleep_stage.enabled;
            j["sleep_stage"]["live_inference"] = sleep_stage.live_inference;
            j["sleep_stage"]["model_dir"] = sleep_stage.model_dir;
            j["sleep_stage"]["model_version"] = sleep_stage.model_version;

            std::ofstream f(path);
            f << j.dump(2);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Config save error: " << e.what() << std::endl;
            return false;
        }
    }

    // Convert to JSON for API (passwords redacted)
    nlohmann::json toJson() const {
        nlohmann::json j;
        j["device_id"] = device_id;
        j["device_name"] = device_name;
        j["source"] = source;
        j["ezshare_url"] = ezshare_url;
        j["ezshare_range"] = ezshare_range;
        j["local_dir"] = local_dir;
        j["burst_interval"] = burst_interval;
        j["web_port"] = web_port;
        j["setup_complete"] = setup_complete;

        j["database"]["type"] = database.type;
        j["database"]["sqlite_path"] = database.sqlite_path;
        j["database"]["host"] = database.host;
        j["database"]["port"] = database.port;
        j["database"]["name"] = database.name;
        j["database"]["user"] = database.user;
        j["database"]["password"] = database.password.empty() ? "" : "********";

        j["mqtt"]["enabled"] = mqtt.enabled;
        j["mqtt"]["broker"] = mqtt.broker;
        j["mqtt"]["port"] = mqtt.port;
        j["mqtt"]["username"] = mqtt.username;
        j["mqtt"]["password"] = mqtt.password.empty() ? "" : "********";

        j["llm"]["enabled"] = llm.enabled;
        j["llm"]["provider"] = llm.provider;
        j["llm"]["endpoint"] = llm.endpoint;
        j["llm"]["model"] = llm.model;
        j["llm"]["api_key"] = llm.api_key.empty() ? "" : "********";
        j["llm"]["max_tokens"] = llm.max_tokens;

        j["agent"]["enabled"] = agent.enabled;
        j["agent"]["embed_model"] = agent.embed_model;
        j["agent"]["temperature"] = agent.temperature;
        j["agent"]["max_iterations"] = agent.max_iterations;

        j["ml_training"]["enabled"] = ml_training.enabled;
        j["ml_training"]["schedule"] = ml_training.schedule;
        j["ml_training"]["model_dir"] = ml_training.model_dir;
        j["ml_training"]["min_days"] = ml_training.min_days;
        j["ml_training"]["max_training_days"] = ml_training.max_training_days;

        j["o2ring"]["enabled"] = o2ring.enabled;
        j["o2ring"]["mode"] = o2ring.mode;
        j["o2ring"]["mule_url"] = o2ring.mule_url;

        j["sleep_stage"]["enabled"] = sleep_stage.enabled;
        j["sleep_stage"]["live_inference"] = sleep_stage.live_inference;
        j["sleep_stage"]["model_dir"] = sleep_stage.model_dir;
        j["sleep_stage"]["model_version"] = sleep_stage.model_version;

        return j;
    }
};

} // namespace hms_cpap
