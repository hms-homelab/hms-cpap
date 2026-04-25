#pragma once

#include "ml/SleepStageTypes.h"
#include "parsers/CpapdashBridge.h"
#include <chrono>
#include <optional>
#include <vector>

namespace hms_cpap {
namespace ml {

struct EpochSample {
    std::chrono::system_clock::time_point timestamp;
    std::optional<double> spo2;
    std::optional<int> heart_rate;
    std::optional<double> respiratory_rate;
    std::optional<double> tidal_volume;
    std::optional<double> minute_ventilation;
    std::optional<double> ie_ratio;
    std::optional<double> leak_rate;
    std::optional<double> flow_p95;
    std::optional<int> motion;
    std::optional<int> vibration;
};

class SleepStageFeatureExtractor {
public:
    static std::vector<double> extract(
        const std::vector<EpochSample>& all_epochs,
        int epoch_index,
        FeatureMode mode,
        std::chrono::system_clock::time_point session_start);

    static std::vector<EpochSample> aggregateToEpochs(
        const std::vector<hms_cpap::CPAPVitals>& vitals,
        const std::vector<hms_cpap::BreathingSummary>& breathing,
        const std::vector<hms_cpap::OximetrySample>& oximetry,
        std::chrono::system_clock::time_point session_start,
        std::chrono::system_clock::time_point session_end);

private:
    struct CardiacFeatures {
        double hr_mean = 0, hr_std = 0, hr_min = 0, hr_max = 0;
        double hrv_rmssd = 0, hrv_sdnn = 0;
    };

    struct SpO2Features {
        double mean = 0, min = 0, std = 0;
        double odi_proxy = 0, slope = 0;
    };

    struct RespiratoryFeatures {
        double rr_mean = 0, rr_std = 0;
        double tv_mean = 0, tv_std = 0;
        double mv_mean = 0, mv_std = 0;
        double ie_ratio_mean = 0;
        double leak_mean = 0;
        double flow_p95 = 0;
    };

    struct MovementFeatures {
        double motion_fraction = 0;
        double vibration_count = 0;
    };

    static CardiacFeatures computeCardiac(const EpochSample& epoch);
    static SpO2Features computeSpO2(const EpochSample& epoch);
    static RespiratoryFeatures computeRespiratory(const EpochSample& epoch);
    static MovementFeatures computeMovement(const EpochSample& epoch);

    static double rollingMean(
        const std::vector<EpochSample>& epochs,
        int center, int start_offset, int end_offset,
        double (*getter)(const EpochSample&));

    static int epochIndexForTime(
        std::chrono::system_clock::time_point t,
        std::chrono::system_clock::time_point session_start);
};

}  // namespace ml
}  // namespace hms_cpap
