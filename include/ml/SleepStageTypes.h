#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace hms_cpap {
namespace ml {

enum class SleepStage : int {
    WAKE  = 0,
    LIGHT = 1,  // AASM N1+N2, R&K S1+S2
    DEEP  = 2,  // AASM N3, R&K S3+S4
    REM   = 3
};

inline std::string sleepStageToString(SleepStage s) {
    switch (s) {
        case SleepStage::WAKE:  return "Wake";
        case SleepStage::LIGHT: return "Light";
        case SleepStage::DEEP:  return "Deep";
        case SleepStage::REM:   return "REM";
    }
    return "Unknown";
}

inline SleepStage sleepStageFromInt(int v) {
    if (v >= 0 && v <= 3) return static_cast<SleepStage>(v);
    return SleepStage::WAKE;
}

struct SleepStageEpoch {
    std::chrono::system_clock::time_point epoch_start;
    int epoch_duration_sec = 30;
    SleepStage stage = SleepStage::WAKE;
    double confidence = 0.0;
    bool provisional = false;
    std::string model_version;
};

struct SleepStageSummary {
    int wake_minutes = 0;
    int light_minutes = 0;
    int deep_minutes = 0;
    int rem_minutes = 0;
    int total_epochs = 0;
    double sleep_efficiency_pct = 0.0;
    int rem_latency_min = 0;
    int first_deep_min = 0;

    static SleepStageSummary fromEpochs(const std::vector<SleepStageEpoch>& epochs) {
        SleepStageSummary s;
        s.total_epochs = static_cast<int>(epochs.size());
        int first_sleep = -1;
        int first_rem = -1;
        int first_deep = -1;

        for (int i = 0; i < static_cast<int>(epochs.size()); ++i) {
            switch (epochs[i].stage) {
                case SleepStage::WAKE:  ++s.wake_minutes; break;  // half-minute per epoch, but * 0.5 rounds ugly — see below
                case SleepStage::LIGHT: ++s.light_minutes; break;
                case SleepStage::DEEP:  ++s.deep_minutes; break;
                case SleepStage::REM:   ++s.rem_minutes; break;
            }
            if (first_sleep < 0 && epochs[i].stage != SleepStage::WAKE)
                first_sleep = i;
            if (first_rem < 0 && epochs[i].stage == SleepStage::REM)
                first_rem = i;
            if (first_deep < 0 && epochs[i].stage == SleepStage::DEEP)
                first_deep = i;
        }

        // Convert epoch counts to minutes (30s per epoch)
        s.wake_minutes  = s.wake_minutes / 2;
        s.light_minutes = s.light_minutes / 2;
        s.deep_minutes  = s.deep_minutes / 2;
        s.rem_minutes   = s.rem_minutes / 2;

        int total_min = s.wake_minutes + s.light_minutes + s.deep_minutes + s.rem_minutes;
        int sleep_min = s.light_minutes + s.deep_minutes + s.rem_minutes;
        s.sleep_efficiency_pct = total_min > 0 ? 100.0 * sleep_min / total_min : 0.0;

        s.rem_latency_min = (first_sleep >= 0 && first_rem >= 0)
            ? (first_rem - first_sleep) / 2 : 0;
        s.first_deep_min = (first_sleep >= 0 && first_deep >= 0)
            ? (first_deep - first_sleep) / 2 : 0;

        return s;
    }
};

enum class FeatureMode {
    CAUSAL,        // Live: only past + current context
    BIDIRECTIONAL  // Post-session: ±N epoch context
};

// Feature names in canonical order — MUST match Python extractor exactly.
// Grouped: cardiac (6), spo2 (5), respiratory (9), movement (2), context (1+rolling)
inline std::vector<std::string> sleepStageFeatureNames(FeatureMode mode) {
    std::vector<std::string> names = {
        // Cardiac (O2 Ring pulse) — 6
        "hr_mean", "hr_std", "hr_min", "hr_max",
        "hrv_rmssd", "hrv_sdnn",
        // SpO2 — 5
        "spo2_mean", "spo2_min", "spo2_std", "spo2_odi_proxy", "spo2_slope",
        // Respiratory (CPAP) — 9
        "rr_mean", "rr_std", "tv_mean", "tv_std",
        "mv_mean", "mv_std", "ie_ratio_mean",
        "leak_mean", "flow_p95",
        // Movement (O2 Ring) — 2
        "motion_fraction", "vibration_count",
        // Context — 1
        "time_of_night"
    };

    auto addRolling = [&](const std::string& prefix, int window) {
        for (const auto& base : {"hr_mean", "spo2_mean", "rr_mean", "motion_fraction"}) {
            names.push_back(prefix + std::to_string(window) + "_" + base);
        }
    };

    // Past rolling context (both modes)
    addRolling("past_", 5);
    addRolling("past_", 10);

    // Future rolling context (bidirectional only)
    if (mode == FeatureMode::BIDIRECTIONAL) {
        addRolling("future_", 5);
    }

    return names;
}

constexpr int EPOCH_DURATION_SEC = 30;
constexpr int CAUSAL_FEATURE_COUNT = 31;       // 23 base + 8 rolling past
constexpr int BIDIR_FEATURE_COUNT = 35;        // 23 base + 8 rolling past + 4 rolling future

}  // namespace ml
}  // namespace hms_cpap
