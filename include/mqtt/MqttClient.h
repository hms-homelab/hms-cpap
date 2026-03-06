#pragma once

#include <mqtt/async_client.h>
#include <string>
#include <functional>
#include <mutex>
#include <memory>
#include <map>
#include <atomic>

namespace hms_cpap {

/**
 * MqttClient - Thread-safe MQTT client wrapper
 *
 * Wraps Eclipse Paho MQTT C++ client with simplified interface for:
 * - Publishing UPS state/discovery messages
 * - Subscribing to multi-device UPS topics
 * - Auto-reconnect on connection loss
 * - Thread-safe operations (shared by multiple service threads)
 */
class MqttClient {
public:
    /**
     * Message callback type
     *
     * @param topic MQTT topic
     * @param payload Message payload (string)
     */
    using MessageCallback = std::function<void(const std::string& topic, const std::string& payload)>;

    /**
     * Constructor
     *
     * @param client_id MQTT client identifier (must be unique)
     */
    explicit MqttClient(const std::string& client_id);

    /**
     * Destructor - cleanup and disconnect
     */
    ~MqttClient();

    // Disable copy
    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    /**
     * Connect to MQTT broker
     *
     * @param broker_address Broker address (e.g., "tcp://192.168.2.15:1883")
     * @param username MQTT username
     * @param password MQTT password
     * @return true if connected successfully
     */
    bool connect(const std::string& broker_address,
                 const std::string& username,
                 const std::string& password);

    /**
     * Disconnect from broker
     */
    void disconnect();

    /**
     * Check if connected
     *
     * @return true if connected to broker
     */
    bool isConnected() const;

    /**
     * Subscribe to MQTT topic with callback
     *
     * Supports wildcards (+, #)
     *
     * @param topic MQTT topic pattern (e.g., "homeassistant/sensor/+/battery_charge/state")
     * @param callback Function to call when message received
     * @param qos Quality of Service (default: 1)
     * @return true if subscribed successfully
     */
    bool subscribe(const std::string& topic, MessageCallback callback, int qos = 1);

    /**
     * Subscribe to multiple topics with same callback
     *
     * @param topics Vector of MQTT topic patterns
     * @param callback Function to call when message received
     * @param qos Quality of Service (default: 1)
     * @return true if all subscriptions succeeded
     */
    bool subscribeMultiple(const std::vector<std::string>& topics,
                           MessageCallback callback,
                           int qos = 1);

    /**
     * Unsubscribe from topic
     *
     * @param topic MQTT topic pattern
     * @return true if unsubscribed successfully
     */
    bool unsubscribe(const std::string& topic);

    /**
     * Publish to MQTT topic
     *
     * @param topic MQTT topic
     * @param payload Message payload (string)
     * @param qos Quality of Service (0, 1, or 2)
     * @param retain Retain message flag
     * @return true if published successfully
     */
    bool publish(const std::string& topic, const std::string& payload, int qos = 1, bool retain = false);

    /**
     * Get broker address
     *
     * @return Broker address string
     */
    std::string getBrokerAddress() const { return broker_address_; }

private:
    /**
     * Message arrived callback (internal)
     */
    void onMessageArrived(mqtt::const_message_ptr msg);

    /**
     * Connection lost callback (internal)
     */
    void onConnectionLost(const std::string& cause);

    /**
     * Check if topic matches pattern (supports wildcards)
     *
     * @param topic Actual topic
     * @param pattern Pattern with wildcards
     * @return true if topic matches pattern
     */
    bool topicMatches(const std::string& topic, const std::string& pattern) const;

    // MQTT client
    std::unique_ptr<mqtt::async_client> client_;
    std::string client_id_;

    // Message callbacks (map: topic_pattern -> callback)
    std::map<std::string, MessageCallback> message_callbacks_;
    mutable std::mutex callbacks_mutex_;

    // Connection state
    std::string broker_address_;
    std::string username_;
    std::string password_;
    std::atomic<bool> connected_;  // Atomic for lock-free reads
    bool initial_connect_done_ = false;
    mutable std::recursive_mutex connection_mutex_;  // For client_ access only

    // Auto-reconnect enabled
    bool auto_reconnect_;
};

}  // namespace hms_cpap
