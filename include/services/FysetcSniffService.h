#pragma once

#include "mqtt_client.h"
#include "database/DatabaseService.h"
#include <memory>
#include <string>
#include <mutex>
#include <atomic>
#include <vector>
#include <chrono>

namespace hms_cpap {

/**
 * FysetcSniffService - Receives raw PCNT pulse count data from FYSETC in sniff mode.
 *
 * Subscribes to cpap/fysetc/{device_id}/sniff and logs every batch to PostgreSQL
 * for offline bus activity pattern analysis (rest vs therapy).
 *
 * Lightweight: no file handling, no EDF parsing, no session logic.
 * Started when CPAP_SOURCE=fysetc_sniff.
 */
class FysetcSniffService {
public:
    FysetcSniffService(
        std::shared_ptr<hms::MqttClient> mqtt,
        std::shared_ptr<DatabaseService> db);

    ~FysetcSniffService();

    FysetcSniffService(const FysetcSniffService&) = delete;
    FysetcSniffService& operator=(const FysetcSniffService&) = delete;

    bool start();
    void stop();

    uint64_t rows_inserted() const { return rows_inserted_.load(); }
    uint64_t rows_failed() const { return rows_failed_.load(); }

private:
    void onSniffData(const std::string& topic, const std::string& payload);
    bool ensureTable();

    std::string device_id_;
    std::string topic_prefix_;

    std::shared_ptr<hms::MqttClient> mqtt_;
    std::shared_ptr<DatabaseService> db_;

    std::atomic<uint64_t> rows_inserted_{0};
    std::atomic<uint64_t> rows_failed_{0};
    int last_seq_ = -1;
};

} // namespace hms_cpap
