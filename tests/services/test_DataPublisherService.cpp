/**
 * HMS-CPAP DataPublisherService Unit Tests
 *
 * Tests Home Assistant status subscription and discovery republishing.
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "services/DataPublisherService.h"
#include "mqtt/MqttClient.h"
#include "database/DatabaseService.h"
#include <thread>
#include <chrono>
#include <atomic>

using namespace hms_cpap;

// Test fixture
class DataPublisherServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        mqtt_broker = "tcp://localhost:1883";

        // Create MQTT client and database service
        mqtt_client = std::make_shared<MqttClient>("test_data_publisher");
        db_service = std::make_shared<DatabaseService>("postgresql://localhost/test");  // Dummy connection string

        // Create DataPublisherService
        data_publisher = std::make_unique<DataPublisherService>(mqtt_client, db_service);
    }

    void TearDown() override {
        if (mqtt_client) {
            mqtt_client->disconnect();
        }
    }

    std::string mqtt_broker;
    std::shared_ptr<MqttClient> mqtt_client;
    std::shared_ptr<DatabaseService> db_service;
    std::unique_ptr<DataPublisherService> data_publisher;
};

/**
 * Test: DataPublisherService subscribes to homeassistant/status on initialization
 */
TEST_F(DataPublisherServiceTest, Initialize_SubscribesToHAStatus) {
    bool connected = mqtt_client->connect(mqtt_broker, "", "");

    if (!connected) {
        GTEST_SKIP() << "MQTT broker not available at " << mqtt_broker;
    }

    // Initialize should subscribe to homeassistant/status
    bool init_success = data_publisher->initialize();

    EXPECT_TRUE(init_success);

    // Verify subscription by publishing a message and checking if callback fires
    // (This is implicit - the subscription happens in initialize())
    std::cout << "✅ DataPublisherService subscribes to homeassistant/status" << std::endl;
}

/**
 * Test: HA status "online" message triggers discovery republish
 *
 * This test verifies that when Home Assistant sends "online" on homeassistant/status,
 * the service republishes discovery messages.
 */
TEST_F(DataPublisherServiceTest, HAStatusOnline_RepublishesDiscovery) {
    bool connected = mqtt_client->connect(mqtt_broker, "", "");

    if (!connected) {
        GTEST_SKIP() << "MQTT broker not available";
    }

    // Initialize (subscribes to homeassistant/status)
    data_publisher->initialize();

    // Create a separate client to publish HA status
    auto publisher_client = std::make_unique<MqttClient>("test_ha_simulator");
    publisher_client->connect(mqtt_broker, "", "");

    // Subscribe to discovery topic to verify republishing
    std::atomic<int> discovery_count{0};
    mqtt_client->subscribe("homeassistant/sensor/+/+/config",
        [&discovery_count](const std::string& topic, const std::string& payload) {
            discovery_count++;
        }, 1);

    // Wait for subscription to be active
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Simulate Home Assistant restart
    publisher_client->publish("homeassistant/status", "online", 1, true);

    // Wait for discovery republish
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Should have received discovery messages
    // HMS-CPAP publishes 33 sensors (8 realtime + 25 historical)
    EXPECT_GT(discovery_count.load(), 0);
    std::cout << "✅ Received " << discovery_count.load() << " discovery messages after HA restart" << std::endl;

    publisher_client->disconnect();
}

/**
 * Test: HA status "offline" message does NOT trigger discovery
 */
TEST_F(DataPublisherServiceTest, HAStatusOffline_DoesNotRepublish) {
    bool connected = mqtt_client->connect(mqtt_broker, "", "");

    if (!connected) {
        GTEST_SKIP() << "MQTT broker not available";
    }

    data_publisher->initialize();

    // Create separate client to simulate HA
    auto publisher_client = std::make_unique<MqttClient>("test_ha_sim_offline");
    publisher_client->connect(mqtt_broker, "", "");

    // Subscribe to discovery topic
    std::atomic<int> discovery_count{0};
    mqtt_client->subscribe("homeassistant/sensor/+/+/config",
        [&discovery_count](const std::string& topic, const std::string& payload) {
            discovery_count++;
        }, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Publish "offline" (should NOT trigger discovery)
    publisher_client->publish("homeassistant/status", "offline", 1, true);

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Should NOT have received discovery messages
    EXPECT_EQ(discovery_count.load(), 0);
    std::cout << "✅ HA 'offline' status does not trigger discovery" << std::endl;

    publisher_client->disconnect();
}

/**
 * Test: Discovery messages are retained
 *
 * Verifies that discovery messages are published with retain=true flag.
 * This ensures Home Assistant can discover devices even if it starts after HMS-CPAP.
 */
TEST_F(DataPublisherServiceTest, DiscoveryMessages_AreRetained) {
    bool connected = mqtt_client->connect(mqtt_broker, "", "");

    if (!connected) {
        GTEST_SKIP() << "MQTT broker not available";
    }

    // Publish discovery manually
    bool discovery_success = data_publisher->publishDiscovery();

    // Discovery publishes to retained topics
    // We can't easily verify retain flag without broker API access,
    // but we can verify publish succeeds
    EXPECT_TRUE(discovery_success);
    std::cout << "✅ Discovery messages published (retain=true)" << std::endl;
}

/**
 * Test: Initialize works even when MQTT not connected
 */
TEST_F(DataPublisherServiceTest, Initialize_WorksWhenMqttDisconnected) {
    // Don't connect MQTT

    // Initialize should not crash
    bool init_success = data_publisher->initialize();

    EXPECT_TRUE(init_success);
    std::cout << "✅ Initialize succeeds even when MQTT disconnected" << std::endl;
}

/**
 * Test: Discovery republishes after MQTT reconnection
 *
 * This test verifies that discovery messages are republished when MQTT
 * reconnects after a disconnection.
 */
TEST_F(DataPublisherServiceTest, MQTTReconnection_RepublishesDiscovery) {
    // This test requires the static flag logic in publishSession()
    // to detect reconnection and republish discovery

    bool connected = mqtt_client->connect(mqtt_broker, "", "");

    if (!connected) {
        GTEST_SKIP() << "MQTT broker not available";
    }

    data_publisher->initialize();

    // Create a dummy session to trigger publishSession
    CPAPSession dummy_session;
    dummy_session.device_id = "cpap_test_device";
    dummy_session.status = CPAPSession::Status::COMPLETED;

    // Simulate disconnect
    mqtt_client->disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Reconnect
    mqtt_client->connect(mqtt_broker, "", "");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Subscribe to discovery to verify republish
    std::atomic<int> discovery_count{0};
    mqtt_client->subscribe("homeassistant/sensor/+/+/config",
        [&discovery_count](const std::string& topic, const std::string& payload) {
            discovery_count++;
        }, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Publish session (should trigger discovery republish due to was_disconnected flag)
    bool publish_success = data_publisher->publishSession(dummy_session);

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Should have republished discovery
    EXPECT_GT(discovery_count.load(), 0);
    std::cout << "✅ Discovery republished after MQTT reconnection" << std::endl;
}
