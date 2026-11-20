// The AI agent's MQTT connection: no user unless one is set.
//
// The broker cases follow TestBroker.h: HMS_TEST_MQTT ("host:port" or
// "host") names a throwaway broker without auth. HMS_TEST_MQTT_AUTH names
// one that requires HMS_TEST_MQTT_AUTH_USER / HMS_TEST_MQTT_AUTH_PASSWORD.
// Each case skips when its broker is unset.
#include <gtest/gtest.h>
#include "PosixCompat.h"

#include "agent/AgentMqttConfig.h"
#include "mqtt_client.h"

#include <cstdlib>
#include <map>
#include <optional>
#include <string>

using namespace hms_cpap;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

/// "host:port" or "host" into MQTT_BROKER / MQTT_PORT.
void pointAt(const std::string& broker) {
    const auto colon = broker.rfind(':');
    setenv("MQTT_BROKER", broker.substr(0, colon).c_str(), 1);
    setenv("MQTT_PORT", colon == std::string::npos ? "1883" : broker.substr(colon + 1).c_str(), 1);
}

class AgentMqttConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        for (const char* name : {"MQTT_BROKER", "MQTT_PORT", "MQTT_USER", "MQTT_PASSWORD",
                                 "MQTT_CLIENT_ID"}) {
            const char* v = std::getenv(name);
            saved_[name] = v ? std::optional<std::string>(v) : std::nullopt;
            unsetenv(name);
        }
    }
    void TearDown() override {
        for (const auto& [name, value] : saved_) {
            if (value) setenv(name.c_str(), value->c_str(), 1);
            else unsetenv(name.c_str());
        }
    }
    std::map<std::string, std::optional<std::string>> saved_;
};

}  // namespace

TEST_F(AgentMqttConfigTest, NoUserIsSetUnlessOneIsGiven) {
    const auto c = agentMqttConfig();
    EXPECT_EQ(c.username, "") << "an empty user connects anonymously";
    EXPECT_EQ(c.password, "");
    EXPECT_EQ(c.broker, "127.0.0.1");
    EXPECT_EQ(c.port, 1883);
    EXPECT_EQ(c.client_id, "hms_cpap_agent");
}

TEST_F(AgentMqttConfigTest, AConfiguredUserIsUsedAsItIs) {
    setenv("MQTT_BROKER", "broker.example", 1);
    setenv("MQTT_PORT", "8883", 1);
    setenv("MQTT_USER", "hms", 1);
    setenv("MQTT_PASSWORD", "secret", 1);
    setenv("MQTT_CLIENT_ID", "kitchen", 1);
    const auto c = agentMqttConfig();
    EXPECT_EQ(c.broker, "broker.example");
    EXPECT_EQ(c.port, 8883);
    EXPECT_EQ(c.username, "hms");
    EXPECT_EQ(c.password, "secret");
    EXPECT_EQ(c.client_id, "kitchen_agent");
}

TEST_F(AgentMqttConfigTest, AnEmptyUserConnectsToABrokerWithoutAuth) {
    const std::string broker = envOr("HMS_TEST_MQTT", "");
    if (broker.empty()) GTEST_SKIP() << "HMS_TEST_MQTT unset, skipping.";
    pointAt(broker);
    setenv("MQTT_CLIENT_ID", "hms_cpap_test_anon", 1);
    hms::MqttClient client(agentMqttConfig());
    EXPECT_TRUE(client.connect());
    client.disconnect();
}

TEST_F(AgentMqttConfigTest, AConfiguredUserConnectsToABrokerThatRequiresIt) {
    const std::string broker = envOr("HMS_TEST_MQTT_AUTH", "");
    const std::string user = envOr("HMS_TEST_MQTT_AUTH_USER", "");
    if (broker.empty() || user.empty())
        GTEST_SKIP() << "HMS_TEST_MQTT_AUTH / HMS_TEST_MQTT_AUTH_USER unset, skipping.";
    pointAt(broker);
    setenv("MQTT_CLIENT_ID", "hms_cpap_test_auth", 1);
    {
        hms::MqttClient anonymous(agentMqttConfig());
        EXPECT_FALSE(anonymous.connect()) << "the broker refuses no user";
    }
    setenv("MQTT_USER", user.c_str(), 1);
    setenv("MQTT_PASSWORD", envOr("HMS_TEST_MQTT_AUTH_PASSWORD", "").c_str(), 1);
    hms::MqttClient client(agentMqttConfig());
    EXPECT_TRUE(client.connect());
    client.disconnect();
}
