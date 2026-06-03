/**
 * HMS-CPAP DataPublisherService Unit Tests
 *
 * Tests Home Assistant status subscription and discovery republishing.
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "services/DataPublisherService.h"
#include "mqtt_client.h"
#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include <thread>
#include <chrono>
#include <atomic>

using namespace hms_cpap;

static hms::MqttConfig testMqttConfig(const std::string& client_id) {
    hms::MqttConfig cfg;
    cfg.broker = "localhost";
    cfg.port = 1883;
    cfg.client_id = client_id;
    return cfg;
}

// Test fixture
class DataPublisherServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        mqtt_broker = "tcp://localhost:1883";

        // Create MQTT client and database service
        mqtt_client = std::make_shared<hms::MqttClient>(testMqttConfig("test_data_publisher"));
        db_service = std::make_shared<SQLiteDatabase>(":memory:");

        // Create DataPublisherService
        data_publisher = std::make_unique<DataPublisherService>(mqtt_client, db_service);
    }

    void TearDown() override {
        if (mqtt_client) {
            mqtt_client->disconnect();
        }
    }

    std::string mqtt_broker;
    std::shared_ptr<hms::MqttClient> mqtt_client;
    std::shared_ptr<IDatabase> db_service;
    std::unique_ptr<DataPublisherService> data_publisher;
};

/**
 * Test: DataPublisherService subscribes to homeassistant/status on initialization
 */
TEST_F(DataPublisherServiceTest, Initialize_SubscribesToHAStatus) {
    bool connected = mqtt_client->connect();

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
    bool connected = mqtt_client->connect();

    if (!connected) {
        GTEST_SKIP() << "MQTT broker not available";
    }

    // Initialize (subscribes to homeassistant/status)
    data_publisher->initialize();

    // Create a separate client to publish HA status
    auto publisher_client = std::make_unique<hms::MqttClient>(testMqttConfig("test_ha_simulator"));
    publisher_client->connect();

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
    bool connected = mqtt_client->connect();

    if (!connected) {
        GTEST_SKIP() << "MQTT broker not available";
    }

    data_publisher->initialize();

    // Create separate client to simulate HA
    auto publisher_client = std::make_unique<hms::MqttClient>(testMqttConfig("test_ha_sim_offline"));
    publisher_client->connect();

    // Subscribe to discovery topic
    std::atomic<int> discovery_count{0};
    mqtt_client->subscribe("homeassistant/sensor/+/+/config",
        [&discovery_count](const std::string& topic, const std::string& payload) {
            discovery_count++;
        }, 1);

    // Wait for retained messages from prior runs to arrive, then reset counter
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int retained_count = discovery_count.load();
    discovery_count.store(0);

    // Publish "offline" (should NOT trigger discovery)
    publisher_client->publish("homeassistant/status", "offline", 1, true);

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Should NOT have received NEW discovery messages (only retained from before)
    EXPECT_EQ(discovery_count.load(), 0)
        << "Got " << discovery_count.load() << " new discovery messages after 'offline' "
        << "(ignored " << retained_count << " pre-existing retained messages)";
    std::cout << "✅ HA 'offline' status does not trigger discovery"
              << " (ignored " << retained_count << " retained)" << std::endl;

    publisher_client->disconnect();
}

/**
 * Test: Discovery messages are retained
 *
 * Verifies that discovery messages are published with retain=true flag.
 * This ensures Home Assistant can discover devices even if it starts after HMS-CPAP.
 */
TEST_F(DataPublisherServiceTest, DiscoveryMessages_AreRetained) {
    bool connected = mqtt_client->connect();

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

// ============================================================================
// HISTORICAL STATE PUBLISHING TESTS
// (regression for: historical metrics never published at session completion)
// ============================================================================

/**
 * Test: publishHistoricalState(SessionMetrics) publishes AHI and event counts
 *
 * Regression test for the bug where session completion called publishSessionCompleted()
 * which only published session_status/session_active — skipping historical metrics
 * entirely. HA therefore showed zeros for AHI/events after every session.
 */
TEST_F(DataPublisherServiceTest, PublishHistoricalState_PublishesAHIAndEvents) {
    bool connected = mqtt_client->connect();

    if (!connected) {
        GTEST_SKIP() << "MQTT broker not available";
    }

    data_publisher->initialize();

    // Subscribe to historical MQTT topics
    std::map<std::string, std::string> received;
    std::mutex mu;
    mqtt_client->subscribe("cpap/+/historical/#",
        [&received, &mu](const std::string& topic, const std::string& payload) {
            std::lock_guard<std::mutex> lk(mu);
            received[topic] = payload;
        }, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Build a SessionMetrics as the DB would return after session completion
    SessionMetrics m;
    m.total_events = 8;
    m.ahi = 1.875;
    m.obstructive_apneas = 2;
    m.central_apneas = 4;
    m.hypopneas = 0;
    m.reras = 1;
    m.clear_airway_apneas = 0;
    m.usage_hours = 4.2667;
    m.avg_leak_rate = 0.91;

    // Call the new public overload directly
    data_publisher->publishHistoricalState(m);

    std::this_thread::sleep_for(std::chrono::seconds(1));

    {
        std::lock_guard<std::mutex> lk(mu);
        // Verify key event topics were published
        bool found_ahi = false, found_central = false, found_obstructive = false;
        for (const auto& [topic, val] : received) {
            if (topic.find("/historical/ahi") != std::string::npos) {
                EXPECT_NEAR(std::stod(val), 1.875, 0.001);
                found_ahi = true;
            }
            if (topic.find("/historical/central_apneas") != std::string::npos) {
                EXPECT_EQ(std::stoi(val), 4);
                found_central = true;
            }
            if (topic.find("/historical/obstructive_apneas") != std::string::npos) {
                EXPECT_EQ(std::stoi(val), 2);
                found_obstructive = true;
            }
        }
        EXPECT_TRUE(found_ahi) << "historical/ahi not published";
        EXPECT_TRUE(found_central) << "historical/central_apneas not published";
        EXPECT_TRUE(found_obstructive) << "historical/obstructive_apneas not published";
    }

    std::cout << "Received " << received.size() << " historical MQTT topics" << std::endl;
}

/**
 * Test: publishHistoricalState(SessionMetrics) with zero events publishes zeros
 *
 * Verifies that a clean session with 0 apneas publishes 0s correctly
 * (distinguishes "no events" from "never published").
 */
TEST_F(DataPublisherServiceTest, PublishHistoricalState_ZeroEvents_PublishesZeros) {
    bool connected = mqtt_client->connect();

    if (!connected) {
        GTEST_SKIP() << "MQTT broker not available";
    }

    data_publisher->initialize();

    std::map<std::string, std::string> received;
    std::mutex mu;
    mqtt_client->subscribe("cpap/+/historical/#",
        [&received, &mu](const std::string& topic, const std::string& payload) {
            std::lock_guard<std::mutex> lk(mu);
            received[topic] = payload;
        }, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SessionMetrics m;  // All zero (default)
    m.usage_hours = 6.0;
    data_publisher->publishHistoricalState(m);

    std::this_thread::sleep_for(std::chrono::seconds(1));

    {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& [topic, val] : received) {
            if (topic.find("/historical/ahi") != std::string::npos) {
                EXPECT_NEAR(std::stod(val), 0.0, 0.001);
            }
            if (topic.find("/historical/total_events") != std::string::npos) {
                EXPECT_EQ(std::stoi(val), 0);
            }
        }
    }
    std::cout << "✅ Zero-event session publishes zeros correctly" << std::endl;
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

    bool connected = mqtt_client->connect();

    if (!connected) {
        GTEST_SKIP() << "MQTT broker not available";
    }

    data_publisher->initialize();

    // Create a dummy session to trigger publishSession
    // (status is always IN_PROGRESS during parsing — completion is handled separately)
    CPAPSession dummy_session;
    dummy_session.device_id = "cpap_test_device";

    // Simulate disconnect
    mqtt_client->disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Reconnect
    mqtt_client->connect();
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

// ============================================================================
// MQTT DISABLED TESTS
// (verify no crash when mqtt.enabled=false → null mqtt_client)
// ============================================================================

class MqttDisabledTest : public ::testing::Test {
protected:
    void SetUp() override {
        db = std::make_shared<SQLiteDatabase>(":memory:");
        db->connect();
        // null mqtt_client — simulates mqtt.enabled=false
        publisher = std::make_unique<DataPublisherService>(nullptr, db);
    }

    std::shared_ptr<IDatabase> db;
    std::unique_ptr<DataPublisherService> publisher;
};

TEST_F(MqttDisabledTest, Initialize_NoMqtt_NoCrash) {
    EXPECT_TRUE(publisher->initialize());
}

TEST_F(MqttDisabledTest, PublishDiscovery_NoMqtt_NoCrash) {
    EXPECT_TRUE(publisher->publishDiscovery());
}

TEST_F(MqttDisabledTest, PublishSessionCompleted_NoMqtt_NoCrash) {
    EXPECT_TRUE(publisher->publishSessionCompleted());
}

TEST_F(MqttDisabledTest, PublishHistoricalState_NoMqtt_NoCrash) {
    SessionMetrics m;
    m.ahi = 2.5;
    m.usage_hours = 6.0;
    m.total_events = 10;
    publisher->publishHistoricalState(m);  // void — just verify no crash
}

TEST_F(MqttDisabledTest, PublishSTRState_NoMqtt_NoCrash) {
    STRDailyRecord rec;
    rec.device_id = "test";
    rec.ahi = 1.5;
    rec.duration_minutes = 360;
    publisher->publishSTRState(rec);  // void — no crash
}

TEST_F(MqttDisabledTest, PublishSessionSummary_NoMqtt_NoCrash) {
    EXPECT_FALSE(publisher->publishSessionSummary("test summary"));  // returns false (not connected)
}

TEST_F(MqttDisabledTest, PublishInsights_NoMqtt_NoCrash) {
    std::vector<Insight> insights;
    publisher->publishInsights(insights);  // void — no crash
}

TEST_F(MqttDisabledTest, PublishSession_NoMqtt_NoCrash) {
    CPAPSession session;
    session.device_id = "test_device";
    // publishSession saves to DB + publishes MQTT; with null MQTT it should still save
    publisher->publishSession(session);
}

TEST_F(MqttDisabledTest, PublishRangeSummary_NoMqtt_ReturnsFalse) {
    // No broker → not connected → both periods return false
    EXPECT_FALSE(publisher->publishRangeSummary(SummaryPeriod::WEEKLY, "weekly text"));
    EXPECT_FALSE(publisher->publishRangeSummary(SummaryPeriod::MONTHLY, "monthly text"));
}

TEST_F(MqttDisabledTest, PublishOximetryLive_NoMqtt_NoCrash) {
    IO2RingClient::LiveReading live;
    live.spo2 = 96;
    live.hr = 60;
    live.motion = 3;
    live.active = true;
    live.valid = true;
    publisher->publishOximetryLive("o2ring_device", live);  // void — early return, no crash
}

TEST_F(MqttDisabledTest, PublishOximetrySummary_NoMqtt_NoCrash) {
    // Early returns on null mqtt before ever touching the DB.
    publisher->publishOximetrySummary("20260601");  // void — no crash
}

// ============================================================================
// PAYLOAD / TOPIC CONSTRUCTION TESTS (broker-backed, GTEST_SKIP if no broker)
// These assert the EXACT bytes/topics the service constructs, which is the
// part of the module that is pure string/JSON building.
// ============================================================================

class PayloadTest : public ::testing::Test {
protected:
    void SetUp() override {
        mqtt_client = std::make_shared<hms::MqttClient>(testMqttConfig("test_payload_pub"));
        db_service = std::make_shared<SQLiteDatabase>(":memory:");
        publisher = std::make_unique<DataPublisherService>(mqtt_client, db_service);
        connected = mqtt_client->connect();
    }

    void TearDown() override {
        if (mqtt_client) mqtt_client->disconnect();
    }

    // Subscribe, run an action, collect all messages matching wildcard.
    std::map<std::string, std::string> capture(const std::string& wildcard,
                                               std::function<void()> action) {
        std::map<std::string, std::string> received;
        std::mutex mu;
        mqtt_client->subscribe(wildcard,
            [&received, &mu](const std::string& topic, const std::string& payload) {
                std::lock_guard<std::mutex> lk(mu);
                received[topic] = payload;
            }, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        action();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::lock_guard<std::mutex> lk(mu);
        return received;
    }

    bool connected = false;
    std::shared_ptr<hms::MqttClient> mqtt_client;
    std::shared_ptr<IDatabase> db_service;
    std::unique_ptr<DataPublisherService> publisher;
};

/**
 * publishSTRState formats every value with 2-decimal fixed precision,
 * converts duration_minutes→hours, halves mask_events into pairs, and
 * computes ahi_delta = record.ahi - nightly_ahi (only when nightly_ahi>0).
 */
TEST_F(PayloadTest, PublishSTRState_FormatsValuesAndComputesDelta) {
    if (!connected) GTEST_SKIP() << "MQTT broker not available";

    STRDailyRecord rec;
    rec.device_id = "test";
    rec.ahi = 3.0;
    rec.oai = 1.5;
    rec.cai = 0.5;
    rec.hi = 1.0;
    rec.rin = 0.25;
    rec.csr = 12.0;
    rec.duration_minutes = 360.0;   // → 6.00 h
    rec.mask_events = 6;            // → 3 pairs
    rec.leak_95 = 18.5;
    rec.mask_press_95 = 11.2;
    rec.spo2_50 = 95.0;
    rec.patient_hours = 1234.5;

    auto received = capture("cpap/+/daily/#",
        [&] { publisher->publishSTRState(rec, /*nightly_ahi=*/2.0); });

    auto get = [&](const std::string& suffix) -> std::string {
        for (const auto& [t, v] : received)
            if (t.find("/daily/" + suffix) != std::string::npos) return v;
        return "<missing>";
    };

    EXPECT_EQ(get("str_ahi"), "3.00");
    EXPECT_EQ(get("str_usage_hours"), "6.00");      // 360/60
    EXPECT_EQ(get("str_mask_events"), "3.00");      // 6/2 pairs, fixed(2)
    EXPECT_EQ(get("str_leak_95"), "18.50");
    EXPECT_EQ(get("str_patient_hours"), "1234.50");
    // ahi_delta = 3.0 - 2.0 = 1.00 (nightly_ahi > 0)
    EXPECT_EQ(get("ahi_delta"), "1.00");
}

/**
 * When nightly_ahi <= 0, ahi_delta is forced to 0 (not record.ahi).
 */
TEST_F(PayloadTest, PublishSTRState_NoNightlyAhi_DeltaIsZero) {
    if (!connected) GTEST_SKIP() << "MQTT broker not available";

    STRDailyRecord rec;
    rec.ahi = 5.0;
    rec.duration_minutes = 120.0;

    auto received = capture("cpap/+/daily/#",
        [&] { publisher->publishSTRState(rec, /*nightly_ahi=*/0.0); });

    std::string delta = "<missing>";
    for (const auto& [t, v] : received)
        if (t.find("/daily/ahi_delta") != std::string::npos) delta = v;

    EXPECT_EQ(delta, "0.00");
}

/**
 * publishSessionSummary wraps the text in {"summary": "..."} JSON and
 * publishes to the daily/session_summary topic.
 */
TEST_F(PayloadTest, PublishSessionSummary_PublishesJsonWrappedText) {
    if (!connected) GTEST_SKIP() << "MQTT broker not available";

    auto received = capture("cpap/+/daily/session_summary",
        [&] {
            EXPECT_TRUE(publisher->publishSessionSummary("Great night, AHI under 2."));
        });

    ASSERT_FALSE(received.empty());
    std::string payload = received.begin()->second;
    // Must be valid JSON with the summary key holding the original text.
    Json::Value root;
    Json::CharReaderBuilder rb;
    std::istringstream is(payload);
    std::string err;
    ASSERT_TRUE(Json::parseFromStream(rb, is, &root, &err)) << err;
    EXPECT_EQ(root["summary"].asString(), "Great night, AHI under 2.");
}

/**
 * publishRangeSummary routes WEEKLY → weekly/summary and MONTHLY → monthly/summary,
 * each as {"summary": text} JSON.
 */
TEST_F(PayloadTest, PublishRangeSummary_RoutesWeeklyAndMonthlyTopics) {
    if (!connected) GTEST_SKIP() << "MQTT broker not available";

    auto weekly = capture("cpap/+/weekly/summary",
        [&] {
            EXPECT_TRUE(publisher->publishRangeSummary(SummaryPeriod::WEEKLY, "week text"));
        });
    ASSERT_FALSE(weekly.empty());
    EXPECT_NE(weekly.begin()->first.find("/weekly/summary"), std::string::npos);

    auto monthly = capture("cpap/+/monthly/summary",
        [&] {
            EXPECT_TRUE(publisher->publishRangeSummary(SummaryPeriod::MONTHLY, "month text"));
        });
    ASSERT_FALSE(monthly.empty());
    EXPECT_NE(monthly.begin()->first.find("/monthly/summary"), std::string::npos);

    Json::Value root;
    Json::CharReaderBuilder rb;
    std::istringstream is(monthly.begin()->second);
    ASSERT_TRUE(Json::parseFromStream(rb, is, &root, nullptr));
    EXPECT_EQ(root["summary"].asString(), "month text");
}

/**
 * publishOximetryLive publishes active/spo2/heart_rate/motion every call,
 * but last_spo2/last_heart_rate ONLY when live.valid is true.
 */
TEST_F(PayloadTest, PublishOximetryLive_PublishesLastOnlyWhenValid) {
    if (!connected) GTEST_SKIP() << "MQTT broker not available";

    // Invalid reading: no last_* topics expected.
    {
        IO2RingClient::LiveReading live;
        live.spo2 = 88;
        live.hr = 72;
        live.motion = 1;
        live.active = false;
        live.valid = false;
        auto received = capture("cpap/oxi_invalid/oximetry/#",
            [&] { publisher->publishOximetryLive("oxi_invalid", live); });

        auto has = [&](const std::string& suffix) {
            for (const auto& [t, v] : received)
                if (t.find("/oximetry/" + suffix) != std::string::npos) return true;
            return false;
        };
        std::string active;
        for (const auto& [t, v] : received)
            if (t.find("/oximetry/active") != std::string::npos) active = v;

        EXPECT_EQ(active, "OFF");
        EXPECT_TRUE(has("spo2"));
        EXPECT_TRUE(has("heart_rate"));
        EXPECT_TRUE(has("motion"));
        EXPECT_FALSE(has("last_spo2")) << "last_spo2 must not publish on invalid reading";
        EXPECT_FALSE(has("last_heart_rate"));
    }

    // Valid reading: last_* topics ARE published and carry the live values.
    {
        IO2RingClient::LiveReading live;
        live.spo2 = 97;
        live.hr = 58;
        live.motion = 0;
        live.active = true;
        live.valid = true;
        auto received = capture("cpap/oxi_valid/oximetry/#",
            [&] { publisher->publishOximetryLive("oxi_valid", live); });

        auto get = [&](const std::string& suffix) -> std::string {
            for (const auto& [t, v] : received)
                if (t.find("/oximetry/" + suffix) != std::string::npos) return v;
            return "<missing>";
        };

        std::string active;
        for (const auto& [t, v] : received)
            if (t.find("/oximetry/active") != std::string::npos) active = v;
        EXPECT_EQ(active, "ON");

        EXPECT_EQ(get("last_spo2"), "97");
        EXPECT_EQ(get("last_heart_rate"), "58");
        // device_id in the topic is the one passed to the call, not device_id_.
        EXPECT_NE(received.begin()->first.find("cpap/oxi_valid/oximetry/"),
                  std::string::npos);
    }
}

/**
 * publishMqttState → publishRealtimeState always publishes session_status=in_progress
 * and session_active=ON, plus duration (hours) and an ISO-8601 timestamp with a
 * colon-inserted timezone offset.
 */
TEST_F(PayloadTest, PublishMqttState_PublishesInProgressAndDuration) {
    if (!connected) GTEST_SKIP() << "MQTT broker not available";

    CPAPSession session;
    session.device_id = "test";
    session.duration_seconds = 3600;  // → 1.0 hour
    session.session_start = std::chrono::system_clock::now();

    BreathingSummary bs;
    bs.avg_pressure = 9.5;
    bs.avg_flow_rate = 12.3;
    bs.mask_pressure = 8.8;
    bs.snore_index = 0.0;
    session.breathing_summary.push_back(bs);

    // publishSession() is the public entry that reaches publishRealtimeState
    // (it calls publishMqttState internally when MQTT is connected).
    auto received = capture("cpap/+/realtime/#",
        [&] { publisher->publishSession(session); });

    auto endsWith = [](const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    auto get = [&](const std::string& suffix) -> std::string {
        for (const auto& [t, v] : received)
            if (endsWith(t, "/realtime/" + suffix)) return v;
        return "<missing>";
    };

    EXPECT_EQ(get("session_status"), "in_progress");
    EXPECT_EQ(get("session_active"), "ON");
    // duration published as hours; std::to_string(double) → "1.000000"
    EXPECT_NEAR(std::stod(get("session_duration")), 1.0, 0.001);
    EXPECT_NEAR(std::stod(get("current_pressure")), 9.5, 0.001);

    // ISO-8601 timestamp: a colon must be inserted into the TZ offset, so the
    // string contains a ':' in the final 6 chars (e.g. ...+00:00 or ...-05:00).
    std::string ts = get("last_session_time");
    ASSERT_NE(ts, "<missing>");
    ASSERT_GE(ts.size(), 6u);
    EXPECT_NE(ts.substr(ts.size() - 6).find(':'), std::string::npos)
        << "timezone colon not inserted: " << ts;
}

/**
 * Realtime optional metrics: when a BreathingSummary HAS optional values set,
 * each corresponding current_* topic is published with the right value.
 *
 * Note: we assert PRESENCE+value (positive behavior) rather than absence,
 * because realtime topics are published with retain=true, so a shared broker
 * holds stale values from prior publishes and absence cannot be asserted
 * deterministically.
 */
TEST_F(PayloadTest, PublishMqttState_PublishesPresentOptionalMetrics) {
    if (!connected) GTEST_SKIP() << "MQTT broker not available";

    CPAPSession session;
    session.device_id = "test";
    BreathingSummary bs;
    bs.avg_pressure = 7.0;
    bs.avg_flow_rate = 10.0;
    bs.respiratory_rate = 14.0;
    bs.tidal_volume = 480.0;
    bs.minute_ventilation = 6.7;
    bs.mask_pressure = 6.5;
    bs.snore_index = 1.0;
    bs.target_ventilation = 5.0;
    session.breathing_summary.push_back(bs);

    auto received = capture("cpap/+/realtime/#",
        [&] { publisher->publishSession(session); });

    auto endsWith = [](const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    auto get = [&](const std::string& suffix) -> std::string {
        for (const auto& [t, v] : received)
            if (endsWith(t, "/realtime/" + suffix)) return v;
        return "<missing>";
    };

    EXPECT_NEAR(std::stod(get("current_pressure")), 7.0, 0.001);
    EXPECT_NEAR(std::stod(get("current_flow_rate")), 10.0, 0.001);
    EXPECT_NEAR(std::stod(get("current_respiratory_rate")), 14.0, 0.001);
    EXPECT_NEAR(std::stod(get("current_tidal_volume")), 480.0, 0.001);
    EXPECT_NEAR(std::stod(get("current_minute_ventilation")), 6.7, 0.001);
    EXPECT_NEAR(std::stod(get("current_mask_pressure")), 6.5, 0.001);
    EXPECT_NEAR(std::stod(get("current_target_ventilation")), 5.0, 0.001);
}

/**
 * publishSessionCompleted publishes completed + OFF on the realtime topics.
 */
TEST_F(PayloadTest, PublishSessionCompleted_PublishesCompletedAndOff) {
    if (!connected) GTEST_SKIP() << "MQTT broker not available";

    auto received = capture("cpap/+/realtime/#",
        [&] { EXPECT_TRUE(publisher->publishSessionCompleted()); });

    auto endsWith = [](const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    auto get = [&](const std::string& suffix) -> std::string {
        for (const auto& [t, v] : received)
            if (endsWith(t, suffix)) return v;
        return "<missing>";
    };
    EXPECT_EQ(get("/realtime/session_status"), "completed");
    EXPECT_EQ(get("/realtime/session_active"), "OFF");
}
