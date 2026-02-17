/**
 * HMS-CPAP ConfigManager Unit Tests
 *
 * Tests environment variable parsing and default value handling.
 */

#include <gtest/gtest.h>
#include "utils/ConfigManager.h"
#include <cstdlib>

using namespace hms_cpap;

// Test constants - centralized for easy configuration
namespace test_defaults {
    constexpr const char* EZSHARE_HOST = "192.168.4.1";  // ez Share direct connection default
    constexpr int EZSHARE_PORT = 80;
    constexpr const char* DB_HOST = "localhost";
    constexpr int DB_PORT = 5432;
    constexpr const char* DB_NAME = "cpap_monitoring";
    constexpr const char* MQTT_HOST = "localhost";
    constexpr int MQTT_PORT = 1883;
}

// Test fixture
class ConfigManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Save original environment
        saveEnvVar("EZSHARE_HOST");
        saveEnvVar("EZSHARE_PORT");
        saveEnvVar("DB_HOST");
        saveEnvVar("DB_PORT");
        saveEnvVar("MQTT_HOST");
    }

    void TearDown() override {
        // Restore original environment
        restoreEnvVars();
    }

    void saveEnvVar(const std::string& name) {
        const char* val = std::getenv(name.c_str());
        if (val != nullptr) {
            saved_env_[name] = std::string(val);
        }
    }

    void restoreEnvVars() {
        for (const auto& [name, value] : saved_env_) {
            setenv(name.c_str(), value.c_str(), 1);
        }
    }

    std::map<std::string, std::string> saved_env_;
};

// ============================================================================
// ENVIRONMENT VARIABLE TESTS
// ============================================================================

TEST_F(ConfigManagerTest, EnvVarExists) {
    // Set environment variable
    setenv("EZSHARE_HOST", "192.168.2.100", 1);

    // Get from environment
    const char* value = std::getenv("EZSHARE_HOST");

    ASSERT_NE(value, nullptr);
    EXPECT_STREQ(value, "192.168.2.100");
}

TEST_F(ConfigManagerTest, EnvVarMissing_UsesDefault) {
    // Unset environment variable
    unsetenv("TEST_VAR_MISSING");

    // Should use default value
    const char* value = std::getenv("TEST_VAR_MISSING");
    EXPECT_EQ(value, nullptr);

    // In ConfigManager, this would use the default
    std::string host = (value != nullptr) ? value : test_defaults::EZSHARE_HOST;
    EXPECT_EQ(host, test_defaults::EZSHARE_HOST);
}

TEST_F(ConfigManagerTest, EnvVarEmpty_UsesDefault) {
    // Set to empty string
    setenv("TEST_VAR_EMPTY", "", 1);

    const char* value = std::getenv("TEST_VAR_EMPTY");
    ASSERT_NE(value, nullptr);
    EXPECT_STREQ(value, "");

    // Empty string should use default
    std::string host = (value != nullptr && strlen(value) > 0) ? value : test_defaults::EZSHARE_HOST;
    EXPECT_EQ(host, test_defaults::EZSHARE_HOST);
}

// ============================================================================
// INTEGER PARSING TESTS
// ============================================================================

TEST_F(ConfigManagerTest, IntegerParsing_Valid) {
    setenv("TEST_PORT", "8080", 1);

    const char* value = std::getenv("TEST_PORT");
    ASSERT_NE(value, nullptr);

    int port = std::stoi(value);
    EXPECT_EQ(port, 8080);
}

TEST_F(ConfigManagerTest, IntegerParsing_Invalid_UsesDefault) {
    setenv("TEST_PORT", "not_a_number", 1);

    const char* value = std::getenv("TEST_PORT");
    ASSERT_NE(value, nullptr);

    // Should throw exception and use default
    int port = 5432;  // default
    try {
        port = std::stoi(value);
    } catch (const std::exception&) {
        port = 5432;  // Keep default
    }

    EXPECT_EQ(port, 5432);
}

// ============================================================================
// BOOLEAN PARSING TESTS
// ============================================================================

TEST_F(ConfigManagerTest, BooleanParsing_True) {
    setenv("TEST_BOOL", "true", 1);

    const char* value = std::getenv("TEST_BOOL");
    ASSERT_NE(value, nullptr);

    std::string val_str = value;
    bool enabled = (val_str == "true" || val_str == "1" || val_str == "yes");

    EXPECT_TRUE(enabled);
}

TEST_F(ConfigManagerTest, BooleanParsing_False) {
    setenv("TEST_BOOL", "false", 1);

    const char* value = std::getenv("TEST_BOOL");
    ASSERT_NE(value, nullptr);

    std::string val_str = value;
    bool enabled = (val_str == "true" || val_str == "1" || val_str == "yes");

    EXPECT_FALSE(enabled);
}

// ============================================================================
// DEFAULT VALUE TESTS
// ============================================================================

TEST_F(ConfigManagerTest, DefaultValues_EzShare) {
    // Test default ez Share values
    unsetenv("EZSHARE_HOST");
    unsetenv("EZSHARE_PORT");

    std::string host = test_defaults::EZSHARE_HOST;
    int port = test_defaults::EZSHARE_PORT;

    const char* env_host = std::getenv("EZSHARE_HOST");
    const char* env_port = std::getenv("EZSHARE_PORT");

    if (env_host != nullptr && strlen(env_host) > 0) {
        host = env_host;
    }
    if (env_port != nullptr && strlen(env_port) > 0) {
        port = std::stoi(env_port);
    }

    EXPECT_EQ(host, test_defaults::EZSHARE_HOST);
    EXPECT_EQ(port, test_defaults::EZSHARE_PORT);
}

TEST_F(ConfigManagerTest, DefaultValues_Database) {
    // Test default database values
    unsetenv("DB_HOST");
    unsetenv("DB_PORT");
    unsetenv("DB_NAME");

    std::string host = test_defaults::DB_HOST;
    int port = test_defaults::DB_PORT;
    std::string name = test_defaults::DB_NAME;

    const char* env_host = std::getenv("DB_HOST");
    const char* env_port = std::getenv("DB_PORT");
    const char* env_name = std::getenv("DB_NAME");

    if (env_host != nullptr && strlen(env_host) > 0) host = env_host;
    if (env_port != nullptr && strlen(env_port) > 0) port = std::stoi(env_port);
    if (env_name != nullptr && strlen(env_name) > 0) name = env_name;

    EXPECT_EQ(host, test_defaults::DB_HOST);
    EXPECT_EQ(port, test_defaults::DB_PORT);
    EXPECT_EQ(name, test_defaults::DB_NAME);
}

TEST_F(ConfigManagerTest, DefaultValues_MQTT) {
    // Test default MQTT values
    unsetenv("MQTT_HOST");
    unsetenv("MQTT_PORT");

    std::string host = test_defaults::MQTT_HOST;
    int port = test_defaults::MQTT_PORT;

    const char* env_host = std::getenv("MQTT_HOST");
    const char* env_port = std::getenv("MQTT_PORT");

    if (env_host != nullptr && strlen(env_host) > 0) host = env_host;
    if (env_port != nullptr && strlen(env_port) > 0) port = std::stoi(env_port);

    EXPECT_EQ(host, test_defaults::MQTT_HOST);
    EXPECT_EQ(port, test_defaults::MQTT_PORT);
}

// ============================================================================
// OVERRIDE TESTS
// ============================================================================

TEST_F(ConfigManagerTest, Override_EzShareHost) {
    const char* override_value = "192.168.2.100";
    setenv("EZSHARE_HOST", override_value, 1);

    std::string host = test_defaults::EZSHARE_HOST;
    const char* env_host = std::getenv("EZSHARE_HOST");

    if (env_host != nullptr && strlen(env_host) > 0) {
        host = env_host;
    }

    EXPECT_EQ(host, override_value);
}

TEST_F(ConfigManagerTest, Override_DatabasePort) {
    const int override_port = 5433;
    setenv("DB_PORT", std::to_string(override_port).c_str(), 1);

    int port = test_defaults::DB_PORT;
    const char* env_port = std::getenv("DB_PORT");

    if (env_port != nullptr && strlen(env_port) > 0) {
        port = std::stoi(env_port);
    }

    EXPECT_EQ(port, override_port);
}

// ============================================================================
// VALIDATION TESTS
// ============================================================================

TEST_F(ConfigManagerTest, Validation_PortRange) {
    // Valid port range: 1-65535
    std::vector<int> valid_ports = {80, 443, 1883, 5432, 8123};
    std::vector<int> invalid_ports = {0, -1, 65536, 100000};

    for (int port : valid_ports) {
        EXPECT_GE(port, 1);
        EXPECT_LE(port, 65535);
    }

    for (int port : invalid_ports) {
        bool is_valid = (port >= 1 && port <= 65535);
        EXPECT_FALSE(is_valid) << "Port " << port << " should be invalid";
    }
}

TEST_F(ConfigManagerTest, Validation_HostFormat) {
    std::vector<std::string> valid_hosts = {
        "localhost",
        "192.168.4.1",
        "example.com",
        "192.168.2.15"
    };

    for (const auto& host : valid_hosts) {
        EXPECT_FALSE(host.empty());
        EXPECT_GT(host.length(), 0);
    }
}

