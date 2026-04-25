#include "services/LiveSleepStageRunner.h"
#include "services/SleepStageClassifier.h"
#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"
#include "mqtt_client.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace hms_cpap {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LiveSleepStageRunner::LiveSleepStageRunner(
        std::shared_ptr<SleepStageClassifier> classifier,
        std::shared_ptr<IDatabase> db,
        std::shared_ptr<hms::MqttClient> mqtt,
        const std::string& device_id)
    : classifier_(std::move(classifier)),
      db_(std::move(db)),
      mqtt_(std::move(mqtt)),
      device_id_(device_id) {}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

void LiveSleepStageRunner::reset() {
    last_epoch_processed_ = 0;
    accumulated_epochs_.clear();
    spdlog::debug("LiveSleepStageRunner: reset for new session");
}

// ---------------------------------------------------------------------------
// Live burst processing
// ---------------------------------------------------------------------------

void LiveSleepStageRunner::onBurstComplete(
        const std::vector<CPAPVitals>& vitals,
        const std::vector<BreathingSummary>& breathing,
        const std::vector<OximetrySample>& oximetry,
        std::chrono::system_clock::time_point session_start,
        int session_id) {

    if (!classifier_ || !classifier_->isReady()) return;

    auto new_epochs = classifier_->classifyLive(
        vitals, breathing, oximetry, session_start, last_epoch_processed_);

    if (new_epochs.empty()) return;

    spdlog::debug("LiveSleepStageRunner: burst produced {} new epochs (session {})",
                  new_epochs.size(), session_id);

    // Persist provisional epochs to DB
    persistEpochs(session_id, new_epochs, /*provisional=*/true);

    // Accumulate for elapsed rollup
    accumulated_epochs_.insert(accumulated_epochs_.end(),
                                new_epochs.begin(), new_epochs.end());

    // Advance epoch cursor
    last_epoch_processed_ += static_cast<int>(new_epochs.size());

    // Publish current stage (latest epoch)
    publishCurrentStage(new_epochs.back());

    // Publish elapsed rollup (all accumulated epochs)
    publishElapsedRollup(accumulated_epochs_);
}

// ---------------------------------------------------------------------------
// Session complete — final inference
// ---------------------------------------------------------------------------

void LiveSleepStageRunner::onSessionComplete(
        const CPAPSession& session,
        const std::vector<OximetrySample>& oximetry,
        int session_id) {

    if (!classifier_ || !classifier_->isReady()) return;

    spdlog::info("LiveSleepStageRunner: running final classification for session {}",
                 session_id);

    auto final_epochs = classifier_->classifyFinal(session, oximetry);

    if (final_epochs.empty()) {
        spdlog::warn("LiveSleepStageRunner: final classification produced no epochs");
        return;
    }

    // Persist final (non-provisional) epochs, overwriting any provisional ones
    persistEpochs(session_id, final_epochs, /*provisional=*/false);

    // Compute and publish summary
    auto summary = ml::SleepStageSummary::fromEpochs(final_epochs);
    publishFinalSummary(summary);

    spdlog::info("LiveSleepStageRunner: final — {} epochs, eff={:.1f}%, "
                 "W={}m L={}m D={}m R={}m",
                 final_epochs.size(), summary.sleep_efficiency_pct,
                 summary.wake_minutes, summary.light_minutes,
                 summary.deep_minutes, summary.rem_minutes);
}

// ---------------------------------------------------------------------------
// DB persistence
// ---------------------------------------------------------------------------

void LiveSleepStageRunner::persistEpochs(
        int session_id,
        const std::vector<ml::SleepStageEpoch>& epochs,
        bool provisional) {

    if (!db_ || !db_->isConnected() || epochs.empty()) return;

    // Build batch SQL — use INSERT ... ON CONFLICT for PostgreSQL upsert
    // Falls back to INSERT OR REPLACE for SQLite
    bool is_pg = (db_->dbType() == DbType::POSTGRESQL);

    for (const auto& epoch : epochs) {
        std::string ts = formatTimestamp(epoch.epoch_start);

        std::string sql;
        if (is_pg) {
            sql = "INSERT INTO cpap_sleep_stages "
                  "(session_id, epoch_start_ts, epoch_duration_sec, "
                  "stage, confidence, provisional, model_version) "
                  "VALUES (" +
                  std::to_string(session_id) + ", " +
                  "'" + ts + "', " +
                  std::to_string(epoch.epoch_duration_sec) + ", " +
                  std::to_string(static_cast<int>(epoch.stage)) + ", " +
                  std::to_string(epoch.confidence) + ", " +
                  (provisional ? "true" : "false") + ", " +
                  "'" + epoch.model_version + "') "
                  "ON CONFLICT (session_id, epoch_start_ts) DO UPDATE SET "
                  "stage = EXCLUDED.stage, "
                  "confidence = EXCLUDED.confidence, "
                  "provisional = EXCLUDED.provisional, "
                  "model_version = EXCLUDED.model_version";
        } else {
            sql = "INSERT OR REPLACE INTO cpap_sleep_stages "
                  "(session_id, epoch_start_ts, epoch_duration_sec, "
                  "stage, confidence, provisional, model_version) "
                  "VALUES (" +
                  std::to_string(session_id) + ", " +
                  "'" + ts + "', " +
                  std::to_string(epoch.epoch_duration_sec) + ", " +
                  std::to_string(static_cast<int>(epoch.stage)) + ", " +
                  std::to_string(epoch.confidence) + ", " +
                  (provisional ? "1" : "0") + ", " +
                  "'" + epoch.model_version + "')";
        }

        try {
            db_->executeQuery(sql);
        } catch (const std::exception& e) {
            spdlog::error("LiveSleepStageRunner: failed to persist epoch: {}", e.what());
        }
    }

    spdlog::debug("LiveSleepStageRunner: persisted {} {} epochs for session {}",
                  epochs.size(), provisional ? "provisional" : "final", session_id);
}

// ---------------------------------------------------------------------------
// MQTT publishing
// ---------------------------------------------------------------------------

void LiveSleepStageRunner::publishCurrentStage(const ml::SleepStageEpoch& epoch) {
    if (!mqtt_ || !mqtt_->isConnected()) return;

    std::string topic = "homeassistant/sensor/" + device_id_ +
                        "/sleep_stage_current/state";
    std::string payload = ml::sleepStageToString(epoch.stage);

    mqtt_->publish(topic, payload, 1, true);
}

void LiveSleepStageRunner::publishElapsedRollup(
        const std::vector<ml::SleepStageEpoch>& epochs) {
    if (!mqtt_ || !mqtt_->isConnected()) return;

    auto summary = ml::SleepStageSummary::fromEpochs(epochs);

    nlohmann::json j;
    j["wake_minutes"] = summary.wake_minutes;
    j["light_minutes"] = summary.light_minutes;
    j["deep_minutes"] = summary.deep_minutes;
    j["rem_minutes"] = summary.rem_minutes;
    j["total_epochs"] = summary.total_epochs;
    j["sleep_efficiency_pct"] = std::round(summary.sleep_efficiency_pct * 10.0) / 10.0;
    j["timestamp"] = formatTimestamp(std::chrono::system_clock::now());

    std::string topic = "homeassistant/sensor/" + device_id_ +
                        "/sleep_stage_elapsed/state";
    mqtt_->publish(topic, j.dump(), 1, true);
}

void LiveSleepStageRunner::publishFinalSummary(const ml::SleepStageSummary& summary) {
    if (!mqtt_ || !mqtt_->isConnected()) return;

    nlohmann::json j;
    j["wake_minutes"] = summary.wake_minutes;
    j["light_minutes"] = summary.light_minutes;
    j["deep_minutes"] = summary.deep_minutes;
    j["rem_minutes"] = summary.rem_minutes;
    j["total_epochs"] = summary.total_epochs;
    j["sleep_efficiency_pct"] = std::round(summary.sleep_efficiency_pct * 10.0) / 10.0;
    j["rem_latency_min"] = summary.rem_latency_min;
    j["first_deep_min"] = summary.first_deep_min;
    j["timestamp"] = formatTimestamp(std::chrono::system_clock::now());

    std::string topic = "homeassistant/sensor/" + device_id_ +
                        "/sleep_stage_summary/state";
    mqtt_->publish(topic, j.dump(), 1, true);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string LiveSleepStageRunner::formatTimestamp(
        std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

}  // namespace hms_cpap
