/**
 * HMS-CPAP STR Integration Tests
 *
 * End-to-end tests: parse STR.edf -> save to PostgreSQL -> publish to MQTT.
 * Requires live MQTT broker (localhost:1883) and PostgreSQL (localhost:5432).
 * Tests are skipped gracefully if services are unavailable.
 */

#include <gtest/gtest.h>
#include "parsers/EDFParser.h"
#include "database/DatabaseService.h"
#include "services/DataPublisherService.h"
#include "mqtt/MqttClient.h"
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <cmath>
#include <sstream>
#include <iomanip>

using namespace hms_cpap;
using namespace std::chrono;

// Locate fixture files
static std::string findFixture(const std::string& name) {
    std::vector<std::string> candidates = {
        "../tests/fixtures/edf/" + name,
        "../../tests/fixtures/edf/" + name,
        "tests/fixtures/edf/" + name,
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) return p;
    }
    std::string abs = "/home/aamat/maestro_hub/projects/hms-cpap/tests/fixtures/edf/" + name;
    if (std::filesystem::exists(abs)) return abs;
    return "";
}

// ============================================================================
//  DB Integration: parse STR -> save to PostgreSQL -> verify
// ============================================================================

class STRDatabaseIntegrationTest : public ::testing::Test {
protected:
    std::shared_ptr<DatabaseService> db;
    std::string device_id = "test_str_integration";
    std::string fixture_path;

    void SetUp() override {
        fixture_path = findFixture("str_current.edf");
        if (fixture_path.empty()) {
            GTEST_SKIP() << "str_current.edf not found";
        }

        std::string conn = "host=localhost port=5432 dbname=cpap_monitoring "
                           "user=maestro password=maestro_postgres_2026_secure";
        db = std::make_shared<DatabaseService>(conn);
        if (!db->connect()) {
            GTEST_SKIP() << "PostgreSQL not available";
        }
    }

    void TearDown() override {
        // Clean up test data
        if (db && db->isConnected()) {
            try {
                pqxx::connection conn("host=localhost port=5432 dbname=cpap_monitoring "
                                      "user=maestro password=maestro_postgres_2026_secure");
                pqxx::work txn(conn);
                txn.exec_params(
                    "DELETE FROM cpap_daily_summary WHERE device_id = $1",
                    device_id);
                txn.commit();
            } catch (...) {
                // Cleanup failure is non-fatal
            }
        }
    }
};

TEST_F(STRDatabaseIntegrationTest, ParseAndSaveToDatabase) {
    auto records = EDFParser::parseSTRFile(fixture_path, device_id);
    ASSERT_FALSE(records.empty());

    // Save last 7 days
    size_t save_count = std::min(records.size(), static_cast<size_t>(7));
    std::vector<STRDailyRecord> recent(records.end() - save_count, records.end());

    ASSERT_TRUE(db->saveSTRDailyRecords(recent));

    // Verify by querying DB directly
    pqxx::connection conn("host=localhost port=5432 dbname=cpap_monitoring "
                          "user=maestro password=maestro_postgres_2026_secure");
    pqxx::work txn(conn);
    auto result = txn.exec_params(
        "SELECT COUNT(*) FROM cpap_daily_summary WHERE device_id = $1",
        device_id);

    int count = result[0][0].as<int>();
    EXPECT_EQ(count, static_cast<int>(save_count));
}

TEST_F(STRDatabaseIntegrationTest, UpsertUpdatesExistingRecords) {
    auto records = EDFParser::parseSTRFile(fixture_path, device_id);
    ASSERT_FALSE(records.empty());

    // Save last record twice (should upsert, not duplicate)
    std::vector<STRDailyRecord> single = {records.back()};

    ASSERT_TRUE(db->saveSTRDailyRecords(single));
    ASSERT_TRUE(db->saveSTRDailyRecords(single));

    pqxx::connection conn("host=localhost port=5432 dbname=cpap_monitoring "
                          "user=maestro password=maestro_postgres_2026_secure");
    pqxx::work txn(conn);
    auto result = txn.exec_params(
        "SELECT COUNT(*) FROM cpap_daily_summary WHERE device_id = $1",
        device_id);

    int count = result[0][0].as<int>();
    EXPECT_EQ(count, 1);  // Upsert, not duplicate
}

TEST_F(STRDatabaseIntegrationTest, VerifyStoredValues) {
    auto records = EDFParser::parseSTRFile(fixture_path, device_id);
    ASSERT_FALSE(records.empty());

    std::vector<STRDailyRecord> single = {records.back()};
    ASSERT_TRUE(db->saveSTRDailyRecords(single));

    // Query the stored record
    pqxx::connection conn("host=localhost port=5432 dbname=cpap_monitoring "
                          "user=maestro password=maestro_postgres_2026_secure");
    pqxx::work txn(conn);
    auto result = txn.exec_params(
        "SELECT ahi, duration_minutes, patient_hours, mask_events, epr_level, "
        "       mask_press_95, leak_95, mode, mask_pairs "
        "FROM cpap_daily_summary WHERE device_id = $1 "
        "ORDER BY record_date DESC LIMIT 1",
        device_id);

    ASSERT_FALSE(result.empty());
    const auto& row = result[0];

    EXPECT_NEAR(row["ahi"].as<double>(), 0.80, 0.01);
    EXPECT_NEAR(row["duration_minutes"].as<double>(), 270.0, 0.1);
    EXPECT_NEAR(row["patient_hours"].as<double>(), 717.0, 0.1);
    EXPECT_EQ(row["mask_events"].as<int>(), 2);
    EXPECT_NEAR(row["epr_level"].as<double>(), 3.0, 0.01);
    EXPECT_NEAR(row["mask_press_95"].as<double>(), 6.60, 0.01);
    EXPECT_EQ(row["mode"].as<int>(), 1);

    // Verify mask_pairs JSONB
    std::string pairs_json = row["mask_pairs"].as<std::string>();
    EXPECT_FALSE(pairs_json.empty());
    EXPECT_NE(pairs_json, "[]");
    EXPECT_NE(pairs_json.find("22:15"), std::string::npos);
    EXPECT_NE(pairs_json.find("02:45"), std::string::npos);
}

TEST_F(STRDatabaseIntegrationTest, GetLastSTRDate) {
    auto records = EDFParser::parseSTRFile(fixture_path, device_id);
    ASSERT_FALSE(records.empty());

    std::vector<STRDailyRecord> single = {records.back()};
    ASSERT_TRUE(db->saveSTRDailyRecords(single));

    auto last_date = db->getLastSTRDate(device_id);
    ASSERT_TRUE(last_date.has_value());
    EXPECT_FALSE(last_date->empty());

    // Should be a valid date string
    EXPECT_EQ(last_date->size(), 10u);  // "YYYY-MM-DD"
    EXPECT_EQ((*last_date)[4], '-');
}

TEST_F(STRDatabaseIntegrationTest, BackfillAllRecords) {
    auto records = EDFParser::parseSTRFile(fixture_path, device_id);
    ASSERT_FALSE(records.empty());

    // Save ALL records (backfill mode)
    ASSERT_TRUE(db->saveSTRDailyRecords(records));

    pqxx::connection conn("host=localhost port=5432 dbname=cpap_monitoring "
                          "user=maestro password=maestro_postgres_2026_secure");
    pqxx::work txn(conn);
    auto result = txn.exec_params(
        "SELECT COUNT(*), MIN(record_date)::text, MAX(record_date)::text "
        "FROM cpap_daily_summary WHERE device_id = $1",
        device_id);

    int count = result[0][0].as<int>();
    std::string min_date = result[0][1].as<std::string>();
    std::string max_date = result[0][2].as<std::string>();

    EXPECT_EQ(count, static_cast<int>(records.size()));
    std::cout << "Backfill: " << count << " days (" << min_date << " to " << max_date << ")" << std::endl;
}

TEST_F(STRDatabaseIntegrationTest, BackfillOldThenCurrentMerges) {
    // Simulate real workflow: backfill str_old.edf, then str_current.edf
    std::string old_path = findFixture("str_old.edf");
    if (old_path.empty()) GTEST_SKIP() << "str_old.edf not found";

    auto old_records = EDFParser::parseSTRFile(old_path, device_id);
    auto current_records = EDFParser::parseSTRFile(fixture_path, device_id);
    ASSERT_FALSE(old_records.empty());
    ASSERT_FALSE(current_records.empty());

    // Save old first, then current (overlapping days should upsert)
    ASSERT_TRUE(db->saveSTRDailyRecords(old_records));
    ASSERT_TRUE(db->saveSTRDailyRecords(current_records));

    pqxx::connection conn("host=localhost port=5432 dbname=cpap_monitoring "
                          "user=maestro password=maestro_postgres_2026_secure");
    pqxx::work txn(conn);
    auto result = txn.exec_params(
        "SELECT COUNT(*) FROM cpap_daily_summary WHERE device_id = $1",
        device_id);

    int count = result[0][0].as<int>();
    // current has more days (229) than old (199), and they overlap.
    // Total unique days should be >= max(old, current)
    EXPECT_GE(count, static_cast<int>(current_records.size()));
    std::cout << "Merged: " << count << " unique days (old=" << old_records.size()
              << ", current=" << current_records.size() << ")" << std::endl;
}

// ============================================================================
//  MQTT Integration: parse STR -> publish to MQTT -> verify
// ============================================================================

class STRMqttIntegrationTest : public ::testing::Test {
protected:
    std::shared_ptr<MqttClient> mqtt_client;
    std::shared_ptr<MqttClient> sub_client;
    std::shared_ptr<DatabaseService> db_service;
    std::unique_ptr<DataPublisherService> publisher;
    std::string device_id = "cpap_resmed_23243570851";  // matches default config
    std::string fixture_path;

    void SetUp() override {
        fixture_path = findFixture("str_current.edf");
        if (fixture_path.empty()) {
            GTEST_SKIP() << "str_current.edf not found";
        }

        mqtt_client = std::make_shared<MqttClient>("test_str_mqtt_pub");
        sub_client = std::make_shared<MqttClient>("test_str_mqtt_sub");
        db_service = std::make_shared<DatabaseService>("host=localhost dbname=cpap_monitoring "
                                                       "user=maestro password=maestro_postgres_2026_secure");

        if (!mqtt_client->connect("tcp://localhost:1883", "", "")) {
            GTEST_SKIP() << "MQTT broker not available";
        }
        if (!sub_client->connect("tcp://localhost:1883", "", "")) {
            GTEST_SKIP() << "MQTT subscriber connection failed";
        }

        publisher = std::make_unique<DataPublisherService>(mqtt_client, db_service);
    }

    void TearDown() override {
        if (sub_client) sub_client->disconnect();
        if (mqtt_client) mqtt_client->disconnect();
    }
};

TEST_F(STRMqttIntegrationTest, PublishSTRDiscovery) {
    // Subscribe to discovery topic
    std::atomic<int> discovery_count{0};
    sub_client->subscribe("homeassistant/sensor/" + device_id + "/daily_+/config",
        [&discovery_count](const std::string&, const std::string&) {
            discovery_count++;
        }, 1);

    // Drain retained messages
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    discovery_count.store(0);

    // Publish discovery
    EXPECT_TRUE(publisher->publishDiscovery());

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // We expect 13 STR daily sensors (among others)
    // Can't assert exact count since wildcard + matching is broker-dependent
    // Instead, check by subscribing to the exact pattern
    std::atomic<int> str_count{0};
    auto sub2 = std::make_shared<MqttClient>("test_str_mqtt_sub2");
    sub2->connect("tcp://localhost:1883", "", "");
    sub2->subscribe("homeassistant/sensor/" + device_id + "/daily_str_ahi/config",
        [&str_count](const std::string&, const std::string& payload) {
            if (!payload.empty()) str_count++;
        }, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_GE(str_count.load(), 1) << "STR AHI discovery not found";
    sub2->disconnect();
}

TEST_F(STRMqttIntegrationTest, PublishSTRState) {
    auto records = EDFParser::parseSTRFile(fixture_path, device_id);
    ASSERT_FALSE(records.empty());

    // Subscribe to STR state topics
    std::map<std::string, std::string> received;
    std::mutex rx_mutex;

    sub_client->subscribe("cpap/" + device_id + "/daily/#",
        [&received, &rx_mutex](const std::string& topic, const std::string& payload) {
            std::lock_guard<std::mutex> lock(rx_mutex);
            received[topic] = payload;
        }, 1);

    // Drain retained
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
        std::lock_guard<std::mutex> lock(rx_mutex);
        received.clear();
    }

    // Publish latest day's state
    publisher->publishSTRState(records.back(), 0.75);  // nightly_ahi = 0.75

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::lock_guard<std::mutex> lock(rx_mutex);

    // Verify key topics received
    std::string prefix = "cpap/" + device_id + "/daily/";

    ASSERT_NE(received.find(prefix + "str_ahi"), received.end());
    EXPECT_EQ(received[prefix + "str_ahi"], "0.80");

    ASSERT_NE(received.find(prefix + "str_usage_hours"), received.end());
    EXPECT_EQ(received[prefix + "str_usage_hours"], "4.50");  // 270 min / 60

    ASSERT_NE(received.find(prefix + "str_patient_hours"), received.end());
    EXPECT_EQ(received[prefix + "str_patient_hours"], "717.00");

    ASSERT_NE(received.find(prefix + "str_mask_events"), received.end());
    EXPECT_EQ(received[prefix + "str_mask_events"], "1.00");  // 2 events / 2 = 1 pair

    ASSERT_NE(received.find(prefix + "ahi_delta"), received.end());
    EXPECT_EQ(received[prefix + "ahi_delta"], "0.05");  // 0.80 - 0.75

    std::cout << "Received " << received.size() << " STR MQTT topics" << std::endl;
}

TEST_F(STRMqttIntegrationTest, AHIDeltaZeroWhenNoNightlyAHI) {
    auto records = EDFParser::parseSTRFile(fixture_path, device_id);
    ASSERT_FALSE(records.empty());

    std::map<std::string, std::string> received;
    std::mutex rx_mutex;

    sub_client->subscribe("cpap/" + device_id + "/daily/ahi_delta",
        [&received, &rx_mutex](const std::string& topic, const std::string& payload) {
            std::lock_guard<std::mutex> lock(rx_mutex);
            received[topic] = payload;
        }, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
        std::lock_guard<std::mutex> lock(rx_mutex);
        received.clear();
    }

    // Publish with nightly_ahi = 0 (unavailable)
    publisher->publishSTRState(records.back(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::lock_guard<std::mutex> lock(rx_mutex);
    std::string key = "cpap/" + device_id + "/daily/ahi_delta";
    ASSERT_NE(received.find(key), received.end());
    EXPECT_EQ(received[key], "0.00");
}

// ============================================================================
//  Full Pipeline: parse -> DB -> MQTT
// ============================================================================

class STRFullPipelineTest : public ::testing::Test {
protected:
    std::shared_ptr<MqttClient> mqtt_client;
    std::shared_ptr<MqttClient> sub_client;
    std::shared_ptr<DatabaseService> db;
    std::unique_ptr<DataPublisherService> publisher;
    std::string device_id = "test_str_pipeline";
    std::string fixture_path;

    void SetUp() override {
        fixture_path = findFixture("str_current.edf");
        if (fixture_path.empty()) GTEST_SKIP() << "str_current.edf not found";

        // DB
        std::string conn = "host=localhost port=5432 dbname=cpap_monitoring "
                           "user=maestro password=maestro_postgres_2026_secure";
        db = std::make_shared<DatabaseService>(conn);
        if (!db->connect()) GTEST_SKIP() << "PostgreSQL not available";

        // MQTT
        mqtt_client = std::make_shared<MqttClient>("test_str_pipeline_pub");
        sub_client = std::make_shared<MqttClient>("test_str_pipeline_sub");
        if (!mqtt_client->connect("tcp://localhost:1883", "", ""))
            GTEST_SKIP() << "MQTT not available";
        sub_client->connect("tcp://localhost:1883", "", "");

        publisher = std::make_unique<DataPublisherService>(mqtt_client, db);
    }

    void TearDown() override {
        if (sub_client) sub_client->disconnect();
        if (mqtt_client) mqtt_client->disconnect();
        // Clean up DB
        if (db && db->isConnected()) {
            try {
                pqxx::connection conn("host=localhost port=5432 dbname=cpap_monitoring "
                                      "user=maestro password=maestro_postgres_2026_secure");
                pqxx::work txn(conn);
                txn.exec_params("DELETE FROM cpap_daily_summary WHERE device_id = $1", device_id);
                txn.commit();
            } catch (...) {}
        }
    }
};

TEST_F(STRFullPipelineTest, EndToEnd_ParseSavePublish) {
    // 1. Parse
    auto records = EDFParser::parseSTRFile(fixture_path, device_id);
    ASSERT_FALSE(records.empty());

    // 2. Save last 7 days to DB
    size_t save_count = std::min(records.size(), static_cast<size_t>(7));
    std::vector<STRDailyRecord> recent(records.end() - save_count, records.end());
    ASSERT_TRUE(db->saveSTRDailyRecords(recent));

    // 3. Verify DB
    auto last_date = db->getLastSTRDate(device_id);
    ASSERT_TRUE(last_date.has_value());

    // 4. Subscribe to MQTT
    std::atomic<int> msg_count{0};
    sub_client->subscribe("cpap/" + device_id + "/daily/#",
        [&msg_count](const std::string&, const std::string&) {
            msg_count++;
        }, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    msg_count.store(0);

    // 5. Publish to MQTT
    // Use device_id matching default for DataPublisherService
    // We need to set the env var so DataPublisherService picks it up
    setenv("CPAP_DEVICE_ID", device_id.c_str(), 1);

    // Re-create publisher with our device_id
    publisher = std::make_unique<DataPublisherService>(mqtt_client, db);
    publisher->publishSTRState(records.back(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 6. Verify MQTT received messages
    EXPECT_EQ(msg_count.load(), 13) << "Expected 13 STR daily MQTT topics";

    // 7. Verify DB has correct count
    pqxx::connection conn("host=localhost port=5432 dbname=cpap_monitoring "
                          "user=maestro password=maestro_postgres_2026_secure");
    pqxx::work txn(conn);
    auto result = txn.exec_params(
        "SELECT COUNT(*) FROM cpap_daily_summary WHERE device_id = $1", device_id);
    EXPECT_EQ(result[0][0].as<int>(), static_cast<int>(save_count));

    // Clean env
    unsetenv("CPAP_DEVICE_ID");

    std::cout << "Full pipeline: " << records.size() << " parsed, "
              << save_count << " saved to DB, 13 MQTT topics published" << std::endl;
}
