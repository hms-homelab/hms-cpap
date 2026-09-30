#pragma once

#include "ml/SleepStageTypes.h"
#include "parsers/CpapdashBridge.h"
#include <json/json.h>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
namespace hms { class MqttClient; }

namespace hms_cpap {

class SleepStageClassifier;
class IDatabase;

/**
 * LiveSleepStageRunner - Stateful service that drives live and final sleep
 * stage inference, persists results, and publishes MQTT updates.
 *
 * Called from the main burst-collection loop:
 *   - onBurstComplete() on each burst (~65s) during a session
 *   - finishIfNightOver() on each burst after that; it runs the final pass
 *     (onSessionComplete) once the night is over by the settle rule
 *   - reset() when session is torn down
 *
 * The night is over when nothing in its folder has grown for an hour
 * (SDD-046, nightIsOver in utils/NightQuiet.h). An EVE file arriving is NOT an
 * end (SDD-047): the machine creates the EVE/CSL pair at the first mask-on
 * after the card is opened, then appends to it at every later mask-on, so on a
 * live night the EVE appears near the start, and a mask back on within the
 * hour is still the same night.
 */
class LiveSleepStageRunner {
public:
    LiveSleepStageRunner(std::shared_ptr<SleepStageClassifier> classifier,
                         std::shared_ptr<IDatabase> db,
                         std::shared_ptr<hms::MqttClient> mqtt,
                         const std::string& device_id);

    /**
     * Called on each burst when a session is in progress.
     * Runs causal (live) inference on new epochs since last call.
     */
    void onBurstComplete(
        const std::vector<CPAPVitals>& vitals,
        const std::vector<BreathingSummary>& breathing,
        const std::vector<OximetrySample>& oximetry,
        std::chrono::system_clock::time_point session_start,
        int session_id);

    /**
     * Called when session completes — re-infers all epochs with final
     * bidirectional model, overwrites provisional results.
     */
    void onSessionComplete(
        const CPAPSession& session,
        const std::vector<OximetrySample>& oximetry,
        int session_id);

    /**
     * Run the final pass (onSessionComplete) only when the night in
     * [date_folder] is over by the settle rule at [now] (nightIsOver). Which
     * files the folder holds does not enter into it. Returns whether the night
     * was over; false as well without a database to ask.
     */
    bool finishIfNightOver(
        const CPAPSession& session,
        const std::vector<OximetrySample>& oximetry,
        int session_id,
        const std::string& date_folder,
        std::chrono::system_clock::time_point now);

    /**
     * Reset state for a new session.
     */
    void reset();

private:
    std::shared_ptr<SleepStageClassifier> classifier_;
    std::shared_ptr<IDatabase> db_;
    std::shared_ptr<hms::MqttClient> mqtt_;
    std::string device_id_;
    int last_epoch_processed_ = 0;

    // All live epochs accumulated during session (for elapsed rollup)
    std::vector<ml::SleepStageEpoch> accumulated_epochs_;

    // DB persistence
    void persistEpochs(int session_id,
                       const std::vector<ml::SleepStageEpoch>& epochs,
                       bool provisional);

    // MQTT publishing
    void publishCurrentStage(const ml::SleepStageEpoch& epoch);
    void publishElapsedRollup(const std::vector<ml::SleepStageEpoch>& epochs);
    void publishFinalSummary(const ml::SleepStageSummary& summary);

    // Helpers
    static std::string formatTimestamp(std::chrono::system_clock::time_point tp);
};

}  // namespace hms_cpap
