#include "mqtt/MqttClient.h"
#include <iostream>
#include <sstream>
#include <algorithm>

namespace hms_cpap {

MqttClient::MqttClient(const std::string& client_id)
    : client_id_(client_id),
      connected_(false),
      auto_reconnect_(true) {
    std::cout << "📡 MQTT: Initialized with client_id: " << client_id << std::endl;
}

MqttClient::~MqttClient() {
    disconnect();
}

bool MqttClient::connect(const std::string& broker_address,
                         const std::string& username,
                         const std::string& password) {
    std::lock_guard<std::recursive_mutex> lock(connection_mutex_);

    broker_address_ = broker_address;
    username_ = username;
    password_ = password;

    std::cout << "📡 MQTT: Connecting to " << broker_address << "..." << std::endl;

    try {
        // Create client with unique ID + timestamp
        std::string full_client_id = client_id_ + "_" + std::to_string(std::time(nullptr));
        client_ = std::make_unique<mqtt::async_client>(broker_address, full_client_id);

        // Set callbacks
        client_->set_message_callback([this](mqtt::const_message_ptr msg) {
            onMessageArrived(msg);
        });

        client_->set_connection_lost_handler([this](const std::string& cause) {
            onConnectionLost(cause);
        });

        // Set connected callback (called when auto-reconnect succeeds)
        client_->set_connected_handler([this](const std::string& cause) {
            std::cerr << "  [connected_handler] CALLBACK ENTER, cause=" << cause << std::flush;
            std::lock_guard<std::recursive_mutex> lock(connection_mutex_);
            std::cerr << " got lock" << std::endl;
            connected_ = true;
            std::cout << "✅ MQTT: Reconnected successfully: " << cause << std::endl;
            std::cerr << "  [connected_handler] CALLBACK EXIT" << std::endl;
        });

        // Connection options
        mqtt::connect_options connOpts;
        connOpts.set_keep_alive_interval(60);  // 60 seconds keep-alive
        connOpts.set_clean_session(true);
        connOpts.set_user_name(username);
        connOpts.set_password(password);

        // Enable auto-reconnect with exponential backoff
        // Min delay: 1 second, Max delay: 64 seconds
        connOpts.set_automatic_reconnect(1, 64);

        // Connect (blocking)
        mqtt::token_ptr conntok = client_->connect(connOpts);
        conntok->wait();  // Wait for connection

        connected_ = true;
        std::cout << "✅ MQTT: Connected successfully" << std::endl;

        return true;

    } catch (const mqtt::exception& e) {
        std::cerr << "❌ MQTT: Connection failed: " << e.what() << std::endl;
        connected_ = false;
        return false;
    }
}

void MqttClient::disconnect() {
    std::lock_guard<std::recursive_mutex> lock(connection_mutex_);

    if (client_ && connected_) {
        try {
            std::cout << "📡 MQTT: Disconnecting..." << std::endl;
            client_->disconnect()->wait();
            connected_ = false;
            std::cout << "📡 MQTT: Disconnected" << std::endl;
        } catch (const mqtt::exception& e) {
            std::cerr << "❌ MQTT: Disconnect error: " << e.what() << std::endl;
        }
    }
}

bool MqttClient::isConnected() const {
    std::cerr << "  [isConnected] entering, trying lock..." << std::flush;
    std::lock_guard<std::recursive_mutex> lock(connection_mutex_);
    std::cerr << " got lock, checking connected_=" << connected_ << std::flush;

    if (!connected_) {
        std::cerr << " -> false (not connected_)" << std::endl;
        return false;
    }
    if (!client_) {
        std::cerr << " -> false (no client)" << std::endl;
        return false;
    }

    std::cerr << ", calling client_->is_connected()..." << std::flush;
    bool paho_connected = client_->is_connected();
    std::cerr << " -> " << paho_connected << std::endl;

    return paho_connected;
}

bool MqttClient::subscribe(const std::string& topic, MessageCallback callback, int qos) {
    if (!isConnected()) {
        std::cerr << "❌ MQTT: Not connected, cannot subscribe" << std::endl;
        return false;
    }

    try {
        std::cout << "📡 MQTT: Subscribing to: " << topic << " (QoS " << qos << ")" << std::endl;

        {
            std::lock_guard<std::recursive_mutex> lock(connection_mutex_);
            client_->subscribe(topic, qos)->wait();  // Wait for subscription
        }

        // Store callback
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            message_callbacks_[topic] = callback;
        }

        std::cout << "✅ MQTT: Subscribed to " << topic << std::endl;
        return true;

    } catch (const mqtt::exception& e) {
        std::cerr << "❌ MQTT: Subscribe failed: " << e.what() << std::endl;
        return false;
    }
}

bool MqttClient::subscribeMultiple(const std::vector<std::string>& topics,
                                    MessageCallback callback,
                                    int qos) {
    bool all_success = true;

    for (const auto& topic : topics) {
        if (!subscribe(topic, callback, qos)) {
            all_success = false;
            std::cerr << "⚠️  MQTT: Failed to subscribe to: " << topic << std::endl;
        }
    }

    return all_success;
}

bool MqttClient::unsubscribe(const std::string& topic) {
    if (!isConnected()) {
        std::cerr << "❌ MQTT: Not connected, cannot unsubscribe" << std::endl;
        return false;
    }

    try {
        {
            std::lock_guard<std::recursive_mutex> lock(connection_mutex_);
            client_->unsubscribe(topic)->wait();
        }

        // Remove callback
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            message_callbacks_.erase(topic);
        }

        std::cout << "📡 MQTT: Unsubscribed from " << topic << std::endl;
        return true;

    } catch (const mqtt::exception& e) {
        std::cerr << "❌ MQTT: Unsubscribe failed: " << e.what() << std::endl;
        return false;
    }
}

bool MqttClient::publish(const std::string& topic,
                         const std::string& payload,
                         int qos,
                         bool retain) {
    if (!isConnected()) {
        std::cerr << "❌ MQTT: Not connected, cannot publish" << std::endl;
        return false;
    }

    try {
        mqtt::message_ptr pubmsg = mqtt::make_message(topic, payload);
        pubmsg->set_qos(qos);
        pubmsg->set_retained(retain);

        {
            std::lock_guard<std::recursive_mutex> lock(connection_mutex_);
            client_->publish(pubmsg)->wait();
        }

        // Simplified logging for state messages (too verbose otherwise)
        if (topic.find("/state") != std::string::npos) {
            // Only log occasionally for state messages
            static int log_counter = 0;
            if (++log_counter % 50 == 0) {  // Log every 50th message
                std::cout << "📤 MQTT: Published " << log_counter << " messages..." << std::endl;
            }
        } else {
            std::cout << "📤 MQTT: Published to " << topic
                      << " (" << payload.length() << " bytes)"
                      << (retain ? " [retained]" : "") << std::endl;
        }

        return true;

    } catch (const mqtt::exception& e) {
        std::cerr << "❌ MQTT: Publish failed: " << e.what() << std::endl;
        return false;
    }
}

bool MqttClient::topicMatches(const std::string& topic, const std::string& pattern) const {
    // Split topic and pattern by '/'
    auto split = [](const std::string& str) -> std::vector<std::string> {
        std::vector<std::string> parts;
        std::stringstream ss(str);
        std::string part;
        while (std::getline(ss, part, '/')) {
            parts.push_back(part);
        }
        return parts;
    };

    auto topic_parts = split(topic);
    auto pattern_parts = split(pattern);

    // Check if pattern has '#' wildcard (multi-level)
    bool has_multilevel = !pattern_parts.empty() && pattern_parts.back() == "#";

    if (has_multilevel) {
        // Match up to '#'
        if (topic_parts.size() < pattern_parts.size() - 1) {
            return false;
        }
        pattern_parts.pop_back();  // Remove '#'
    } else {
        // Exact level count match required
        if (topic_parts.size() != pattern_parts.size()) {
            return false;
        }
    }

    // Match each level
    for (size_t i = 0; i < pattern_parts.size(); ++i) {
        const auto& pattern_level = pattern_parts[i];
        const auto& topic_level = topic_parts[i];

        if (pattern_level == "+") {
            // Single-level wildcard - matches any value
            continue;
        } else if (pattern_level != topic_level) {
            // Exact match required
            return false;
        }
    }

    return true;
}

void MqttClient::onMessageArrived(mqtt::const_message_ptr msg) {
    std::string topic = msg->get_topic();
    std::string payload = msg->to_string();

    // Find matching callbacks
    std::lock_guard<std::mutex> lock(callbacks_mutex_);

    for (const auto& [pattern, callback] : message_callbacks_) {
        if (topicMatches(topic, pattern)) {
            try {
                callback(topic, payload);
            } catch (const std::exception& e) {
                std::cerr << "❌ MQTT: Callback error for topic " << topic
                          << ": " << e.what() << std::endl;
            }
        }
    }
}

void MqttClient::onConnectionLost(const std::string& cause) {
    std::cerr << "  [onConnectionLost] CALLBACK ENTER, cause=" << cause << std::flush;
    std::lock_guard<std::recursive_mutex> lock(connection_mutex_);
    std::cerr << " got lock" << std::endl;
    connected_ = false;

    std::cerr << "⚠️  MQTT: Connection lost: " << cause << std::endl;

    if (auto_reconnect_) {
        std::cout << "🔄 MQTT: Auto-reconnect enabled (handled by paho-mqtt)" << std::endl;
    }
    std::cerr << "  [onConnectionLost] CALLBACK EXIT" << std::endl;
}

}  // namespace hms_cpap
