#pragma once

#include "ml/RandomForest.h"
#include "ml/StandardScaler.h"
#include "ml/SleepStageTypes.h"
#include "parsers/CpapdashBridge.h"

#include <json/json.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Forward declarations — full headers only in .cpp
namespace hms_cpap { namespace ml {
class HmmSmoother;
class SleepStageFeatureExtractor;
}}

namespace hms_cpap {

/**
 * SleepStageClassifier - Owns loaded sleep-stage models and provides inference.
 *
 * Two modes:
 *   - classifyFinal(): post-session batch with bidirectional model (full context)
 *   - classifyLive():  live inference with causal model (past-only context + HMM)
 *
 * Thread-safe: all public methods lock internal mutex.
 * Does NOT access DB or MQTT — returns results to caller.
 */
class SleepStageClassifier {
public:
    struct Config {
        bool enabled = false;
        bool live_inference = true;
        std::string model_dir;
        std::string model_version = "shhs-rf-v1";
    };

    explicit SleepStageClassifier(Config config);
    ~SleepStageClassifier();

    /// Load models from disk. Returns true if at least the final model loaded.
    bool loadModels();

    /// True if at least the final model is loaded and ready for inference.
    bool isReady() const;

    /// Post-session: classify all epochs with bidirectional model + HMM smoothing.
    std::vector<ml::SleepStageEpoch> classifyFinal(
        const CPAPSession& session,
        const std::vector<OximetrySample>& oximetry) const;

    /// Live: classify recent epochs with causal model + incremental HMM smoothing.
    std::vector<ml::SleepStageEpoch> classifyLive(
        const std::vector<CPAPVitals>& vitals,
        const std::vector<BreathingSummary>& breathing,
        const std::vector<OximetrySample>& oximetry,
        std::chrono::system_clock::time_point session_start,
        int start_epoch = 0) const;

    /// JSON status for /api endpoints.
    Json::Value getStatus() const;

private:
    Config config_;

    // Final (bidirectional) model
    ml::RandomForest final_model_;
    ml::StandardScaler final_scaler_;
    std::vector<std::string> final_features_;
    bool final_loaded_ = false;

    // Live (causal) model
    ml::RandomForest live_model_;
    ml::StandardScaler live_scaler_;
    std::vector<std::string> live_features_;
    bool live_loaded_ = false;

    // HMM smoother (heap-allocated to allow forward decl)
    std::unique_ptr<ml::HmmSmoother> hmm_;

    mutable std::mutex mutex_;

    // Helpers
    std::string modelPath(const std::string& name) const;
    bool loadOneModel(const std::string& name,
                      ml::RandomForest& rf,
                      ml::StandardScaler& scaler,
                      std::vector<std::string>& features);

    static std::string currentTimestamp();
};

}  // namespace hms_cpap
