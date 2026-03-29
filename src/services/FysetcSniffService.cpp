#include "services/FysetcSniffService.h"
#include "utils/ConfigManager.h"

#include <json/json.h>
#include <iostream>
#include <sstream>

namespace hms_cpap {

FysetcSniffService::FysetcSniffService(
    std::shared_ptr<hms::MqttClient> mqtt,
    std::shared_ptr<DatabaseService> db)
    : mqtt_(std::move(mqtt)),
      db_(std::move(db)) {

    device_id_ = ConfigManager::get("CPAP_DEVICE_ID", "cpap_resmed_23243570851");
    topic_prefix_ = "cpap/fysetc/" + device_id_;
}

FysetcSniffService::~FysetcSniffService() {
    stop();
}

bool FysetcSniffService::start() {
    std::cout << "FysetcSniffService: Starting (passive bus pattern capture)" << std::endl;
    std::cout << "  Device: " << device_id_ << std::endl;
    std::cout << "  Topic: " << topic_prefix_ << "/sniff" << std::endl;

    if (!ensureTable()) {
        std::cerr << "FysetcSniffService: Failed to create sniff table" << std::endl;
        return false;
    }

    std::vector<std::string> topics = {
        topic_prefix_ + "/sniff"
    };

    mqtt_->subscribe(topics, [this](const std::string& topic, const std::string& payload) {
        onSniffData(topic, payload);
    });

    std::cout << "FysetcSniffService: Subscribed, waiting for sniff data..." << std::endl;
    return true;
}

void FysetcSniffService::stop() {
    std::cout << "FysetcSniffService: Stopped (inserted=" << rows_inserted_.load()
              << " failed=" << rows_failed_.load() << ")" << std::endl;
}

bool FysetcSniffService::ensureTable() {
    const std::string sql = R"(
        CREATE TABLE IF NOT EXISTS cpap_sniff_data (
            id SERIAL PRIMARY KEY,
            device_id VARCHAR(64) NOT NULL,
            timestamp TIMESTAMPTZ NOT NULL,
            uptime_sec INTEGER NOT NULL,
            seq INTEGER NOT NULL,
            pulse_counts SMALLINT[] NOT NULL,
            interval_ms SMALLINT NOT NULL DEFAULT 100,
            therapy_detected BOOLEAN NOT NULL DEFAULT FALSE,
            idle_ms INTEGER NOT NULL DEFAULT 0,
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );
        CREATE INDEX IF NOT EXISTS idx_sniff_device_time
            ON cpap_sniff_data (device_id, timestamp);
        CREATE INDEX IF NOT EXISTS idx_sniff_therapy
            ON cpap_sniff_data (device_id, therapy_detected, timestamp);
    )";

    return db_->executeRaw(sql);
}

void FysetcSniffService::onSniffData(const std::string& /*topic*/,
                                      const std::string& payload) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream ss(payload);
    std::string errors;
    if (!Json::parseFromStream(builder, ss, &root, &errors)) {
        std::cerr << "FysetcSniff: Failed to parse: " << errors << std::endl;
        rows_failed_++;
        return;
    }

    int64_t ts = root.get("ts", 0).asInt64();
    int uptime = root.get("up", 0).asInt();
    int seq = root.get("seq", 0).asInt();
    int interval_ms = root.get("ms", 100).asInt();
    bool therapy = root.get("therapy", false).asBool();
    int idle_ms = root.get("idle_ms", 0).asInt();

    const Json::Value& samples = root["samples"];
    if (!samples.isArray() || samples.empty()) {
        std::cerr << "FysetcSniff: Missing samples array" << std::endl;
        rows_failed_++;
        return;
    }

    // Detect sequence gaps
    if (last_seq_ >= 0 && seq != last_seq_ + 1) {
        int gap = seq - last_seq_ - 1;
        if (gap > 0) {
            std::cout << "FysetcSniff: Sequence gap detected (missed " << gap << " batches)" << std::endl;
        }
    }
    last_seq_ = seq;

    // Build PostgreSQL array literal: '{12,0,0,45,23,0,0,0,0,15}'
    std::ostringstream arr;
    arr << "{";
    for (Json::ArrayIndex i = 0; i < samples.size(); i++) {
        if (i > 0) arr << ",";
        arr << samples[i].asInt();
    }
    arr << "}";

    // Convert epoch to timestamp string
    std::ostringstream ts_str;
    ts_str << "to_timestamp(" << ts << ")";

    // INSERT
    std::ostringstream sql;
    sql << "INSERT INTO cpap_sniff_data "
        << "(device_id, timestamp, uptime_sec, seq, pulse_counts, interval_ms, therapy_detected, idle_ms) "
        << "VALUES ("
        << "'" << device_id_ << "', "
        << ts_str.str() << ", "
        << uptime << ", "
        << seq << ", "
        << "'" << arr.str() << "', "
        << interval_ms << ", "
        << (therapy ? "TRUE" : "FALSE") << ", "
        << idle_ms
        << ")";

    if (db_->executeRaw(sql.str())) {
        rows_inserted_++;
    } else {
        rows_failed_++;
        // Only log every 100th failure to avoid spam
        if (rows_failed_.load() % 100 == 1) {
            std::cerr << "FysetcSniff: INSERT failed (total failures: "
                      << rows_failed_.load() << ")" << std::endl;
        }
    }
}

} // namespace hms_cpap
