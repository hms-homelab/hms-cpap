/**
 * HMS-CPAP AppConfig Unit Tests
 *
 * Tests config.json loading, env var fallback, and precedence rules.
 */

#include <gtest/gtest.h>
#include "utils/AppConfig.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace hms_cpap;

class AppConfigTest : public ::testing::Test {
protected:
    std::string tmp_dir_;
    std::string config_path_;
    std::vector<std::pair<std::string, std::string>> saved_env_;

    void SetUp() override {
        tmp_dir_ = "/tmp/hms_cpap_test_" + std::to_string(getpid());
        std::filesystem::create_directories(tmp_dir_);
        config_path_ = tmp_dir_ + "/config.json";
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir_);
        // Restore saved env vars
        for (const auto& [name, val] : saved_env_) {
            if (val.empty())
                unsetenv(name.c_str());
            else
                setenv(name.c_str(), val.c_str(), 1);
        }
    }

    void saveAndSetEnv(const std::string& name, const std::string& value) {
        const char* old = std::getenv(name.c_str());
        saved_env_.push_back({name, old ? old : ""});
        setenv(name.c_str(), value.c_str(), 1);
    }

    void saveAndUnsetEnv(const std::string& name) {
        const char* old = std::getenv(name.c_str());
        saved_env_.push_back({name, old ? old : ""});
        unsetenv(name.c_str());
    }

    void writeConfig(const std::string& json) {
        std::ofstream f(config_path_);
        f << json;
    }
};

// --- Default values ---

TEST_F(AppConfigTest, DefaultBurstInterval) {
    AppConfig config;
    EXPECT_EQ(config.burst_interval, 65);
}

TEST_F(AppConfigTest, DefaultDbType) {
    AppConfig config;
    EXPECT_EQ(config.database.type, "sqlite");
}

TEST_F(AppConfigTest, DefaultMqttDisabled) {
    AppConfig config;
    EXPECT_FALSE(config.mqtt.enabled);
}

TEST_F(AppConfigTest, DefaultLlmDisabled) {
    AppConfig config;
    EXPECT_FALSE(config.llm.enabled);
}

// --- Config file takes precedence ---

TEST_F(AppConfigTest, ConfigFileTakesPrecedenceOverEnvVars) {
    writeConfig(R"({
        "burst_interval": 90,
        "database": {"type": "postgresql", "host": "10.0.0.1", "port": 5432, "name": "mydb", "user": "myuser", "password": "mypass"}
    })");

    saveAndSetEnv("BURST_INTERVAL", "120");
    saveAndSetEnv("DB_HOST", "10.0.0.2");
    saveAndSetEnv("DB_NAME", "otherdb");

    AppConfig config;
    AppConfig::load(config_path_, config);
    config.applyEnvFallbacks();

    EXPECT_EQ(config.burst_interval, 90);
    EXPECT_EQ(config.database.host, "10.0.0.1");
    EXPECT_EQ(config.database.name, "mydb");
    EXPECT_EQ(config.database.type, "postgresql");
}

// --- Env var fallback ---

TEST_F(AppConfigTest, EnvVarFallbackForEmptyDbFields) {
    // Config has sqlite defaults (empty host/name)
    writeConfig(R"({"database": {"type": "sqlite", "host": "", "name": "", "user": "", "password": ""}})");

    saveAndSetEnv("DB_HOST", "192.168.2.15");
    saveAndSetEnv("DB_PORT", "5432");
    saveAndSetEnv("DB_NAME", "cpap_monitoring");
    saveAndSetEnv("DB_USER", "maestro");
    saveAndSetEnv("DB_PASSWORD", "secret");

    AppConfig config;
    AppConfig::load(config_path_, config);
    config.applyEnvFallbacks();

    EXPECT_EQ(config.database.type, "postgresql");  // auto-switched
    EXPECT_EQ(config.database.host, "192.168.2.15");
    EXPECT_EQ(config.database.port, 5432);
    EXPECT_EQ(config.database.name, "cpap_monitoring");
    EXPECT_EQ(config.database.user, "maestro");
    EXPECT_EQ(config.database.password, "secret");
}

TEST_F(AppConfigTest, EnvVarFallbackForMqtt) {
    saveAndSetEnv("MQTT_BROKER", "192.168.2.15");
    saveAndSetEnv("MQTT_PORT", "1883");
    saveAndSetEnv("MQTT_USER", "aamat");
    saveAndSetEnv("MQTT_PASSWORD", "pass");
    saveAndSetEnv("MQTT_CLIENT_ID", "hms_cpap_pi");

    AppConfig config;
    config.applyEnvFallbacks();

    EXPECT_TRUE(config.mqtt.enabled);  // auto-enabled
    EXPECT_EQ(config.mqtt.broker, "192.168.2.15");
    EXPECT_EQ(config.mqtt.username, "aamat");
    EXPECT_EQ(config.mqtt.client_id, "hms_cpap_pi");
}

TEST_F(AppConfigTest, EnvVarFallbackForLlm) {
    saveAndSetEnv("LLM_ENABLED", "true");
    saveAndSetEnv("LLM_PROVIDER", "gemini");
    saveAndSetEnv("LLM_ENDPOINT", "https://api.example.com");
    saveAndSetEnv("LLM_MODEL", "gemini-pro");
    saveAndSetEnv("LLM_API_KEY", "key123");
    saveAndSetEnv("LLM_MAX_TOKENS", "4096");

    AppConfig config;
    config.applyEnvFallbacks();

    EXPECT_TRUE(config.llm.enabled);
    EXPECT_EQ(config.llm.provider, "gemini");
    EXPECT_EQ(config.llm.endpoint, "https://api.example.com");
    EXPECT_EQ(config.llm.model, "gemini-pro");
    EXPECT_EQ(config.llm.api_key, "key123");
    EXPECT_EQ(config.llm.max_tokens, 4096);
}

TEST_F(AppConfigTest, EnvVarFallbackForBurstInterval) {
    saveAndSetEnv("BURST_INTERVAL", "45");

    AppConfig config;  // default 65
    config.applyEnvFallbacks();

    EXPECT_EQ(config.burst_interval, 45);
}

TEST_F(AppConfigTest, ConfigBurstIntervalNotOverriddenByEnv) {
    writeConfig(R"({"burst_interval": 90})");

    saveAndSetEnv("BURST_INTERVAL", "45");

    AppConfig config;
    AppConfig::load(config_path_, config);
    config.applyEnvFallbacks();

    // Config had non-default value 90, env should not override
    EXPECT_EQ(config.burst_interval, 90);
}

// --- No env vars = pure defaults ---

TEST_F(AppConfigTest, NoEnvVarsUsesDefaults) {
    saveAndUnsetEnv("DB_HOST");
    saveAndUnsetEnv("DB_NAME");
    saveAndUnsetEnv("MQTT_BROKER");
    saveAndUnsetEnv("LLM_ENABLED");
    saveAndUnsetEnv("BURST_INTERVAL");

    AppConfig config;
    config.applyEnvFallbacks();

    EXPECT_EQ(config.burst_interval, 65);
    EXPECT_EQ(config.database.type, "sqlite");
    EXPECT_FALSE(config.mqtt.enabled);
    EXPECT_FALSE(config.llm.enabled);
}

// --- Auto-detect postgresql from env vars ---

TEST_F(AppConfigTest, AutoDetectPostgresqlFromEnv) {
    saveAndSetEnv("DB_HOST", "dbserver");
    saveAndSetEnv("DB_NAME", "mydb");

    AppConfig config;
    EXPECT_EQ(config.database.type, "sqlite");  // default

    config.applyEnvFallbacks();
    EXPECT_EQ(config.database.type, "postgresql");  // auto-switched
}

TEST_F(AppConfigTest, NoAutoSwitchWhenConfigExplicitlySqlite) {
    // Config explicitly sets sqlite with host filled (edge case)
    writeConfig(R"({"database": {"type": "sqlite", "host": "somehost", "name": "somedb"}})");

    AppConfig config;
    AppConfig::load(config_path_, config);
    // host is already filled from config, type is sqlite
    // applyEnvFallbacks should still switch since host+name are present
    config.applyEnvFallbacks();
    EXPECT_EQ(config.database.type, "postgresql");
}

// --- Save and reload round-trip ---

TEST_F(AppConfigTest, SaveAndReloadPreservesValues) {
    AppConfig original;
    original.burst_interval = 65;
    original.database.type = "postgresql";
    original.database.host = "192.168.2.15";
    original.database.port = 5432;
    original.database.name = "cpap_monitoring";
    original.mqtt.enabled = true;
    original.mqtt.broker = "192.168.2.15";
    original.save(config_path_);

    AppConfig loaded;
    EXPECT_TRUE(AppConfig::load(config_path_, loaded));
    EXPECT_EQ(loaded.burst_interval, 65);
    EXPECT_EQ(loaded.database.type, "postgresql");
    EXPECT_EQ(loaded.database.host, "192.168.2.15");
    EXPECT_EQ(loaded.database.port, 5432);
    EXPECT_TRUE(loaded.mqtt.enabled);
    EXPECT_EQ(loaded.mqtt.broker, "192.168.2.15");
}

// --- First run scenario (no config file) ---

TEST_F(AppConfigTest, FirstRunWithEnvVarsCreatesCorrectConfig) {
    saveAndSetEnv("DB_HOST", "192.168.2.15");
    saveAndSetEnv("DB_PORT", "5432");
    saveAndSetEnv("DB_NAME", "cpap_monitoring");
    saveAndSetEnv("DB_USER", "maestro");
    saveAndSetEnv("DB_PASSWORD", "secret");
    saveAndSetEnv("MQTT_BROKER", "192.168.2.15");
    saveAndSetEnv("MQTT_USER", "aamat");
    saveAndSetEnv("MQTT_PASSWORD", "pass");
    saveAndSetEnv("BURST_INTERVAL", "65");

    // No config file exists
    AppConfig config;
    bool existed = AppConfig::load(config_path_, config);
    EXPECT_FALSE(existed);

    config.applyEnvFallbacks();

    EXPECT_EQ(config.burst_interval, 65);
    EXPECT_EQ(config.database.type, "postgresql");
    EXPECT_EQ(config.database.host, "192.168.2.15");
    EXPECT_TRUE(config.mqtt.enabled);
    EXPECT_EQ(config.mqtt.broker, "192.168.2.15");

    // Save and verify it persists
    config.save(config_path_);

    AppConfig reloaded;
    EXPECT_TRUE(AppConfig::load(config_path_, reloaded));
    EXPECT_EQ(reloaded.database.type, "postgresql");
    EXPECT_TRUE(reloaded.mqtt.enabled);
}
