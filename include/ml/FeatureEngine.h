#pragma once

#include <cmath>
#include <numeric>
#include <string>
#include <vector>

namespace hms_cpap {
namespace ml {

/**
 * DailyRecord - One therapy day from cpap_daily_summary.
 */
struct DailyRecord {
    std::string record_date;
    double duration_minutes = 0;
    double ahi = 0;
    double hi = 0;
    double oai = 0;
    double cai = 0;
    double mask_press_50 = 0;
    double mask_press_95 = 0;
    double mask_press_max = 0;
    double leak_50 = 0;
    double leak_95 = 0;
    double leak_max = 0;
    double resp_rate_50 = 0;
    double tid_vol_50 = 0;
    double min_vent_50 = 0;
    int day_of_week = 0;  // 0=Mon ... 6=Sun
};

/**
 * FeatureEngine - Mirrors the Python cpap_ml_analyzer feature_engineering().
 * Produces ~55 features per sample from a sorted vector of DailyRecords.
 */
class FeatureEngine {
public:
    struct Result {
        std::vector<std::vector<double>> X;
        std::vector<std::string> feature_names;
        // Targets for each model (aligned with X rows)
        std::vector<double> target_ahi;          // next-day AHI
        std::vector<double> target_hours;        // next-day hours
        std::vector<int> target_high_leak;       // next-day high leak (0/1)
        std::vector<int> target_anomaly;         // anomaly class (0-3)
        // Anomaly class labels
        std::vector<std::string> anomaly_labels; // {"NORMAL","AHI_ANOMALY","LEAK_ANOMALY","PRESSURE_ANOMALY"}
    };

    static constexpr double HIGH_LEAK_THRESHOLD = 24.0;
    static constexpr double ANOMALY_SIGMA = 2.0;

    /**
     * Build feature matrix from sorted daily records.
     * Records should be sorted by date ascending.
     * Returns features aligned to records[0..n-2] (last record has no next-day target).
     */
    static Result build(const std::vector<DailyRecord>& records);

    // Individual feature helpers (public for testing)
    static std::vector<double> rollingMean(const std::vector<double>& vals, int window);
    static std::vector<double> rollingStd(const std::vector<double>& vals, int window);
    static std::vector<double> rollingMax(const std::vector<double>& vals, int window);
    static std::vector<double> trendSlope(const std::vector<double>& vals, int window);
    static std::vector<double> zScore(const std::vector<double>& vals);
};

}  // namespace ml
}  // namespace hms_cpap
