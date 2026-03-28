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
    std::string source = "ezshare";  // "ezshare", "local", "fysetc"
    std::string ezshare_url = "http://192.168.4.1";
    std::string local_dir;
    int burst_interval = 300;

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

    bool setup_complete = false;

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
            if (j.contains("ezshare_url"))  config.ezshare_url = j["ezshare_url"];
            if (j.contains("local_dir"))    config.local_dir = j["local_dir"];
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

        return j;
    }
};

} // namespace hms_cpap
