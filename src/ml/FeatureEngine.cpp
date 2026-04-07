#include "ml/FeatureEngine.h"
#include <algorithm>
#include <cmath>

namespace hms_cpap {
namespace ml {

std::vector<double> FeatureEngine::rollingMean(const std::vector<double>& vals, int window) {
    std::vector<double> result(vals.size(), 0.0);
    for (size_t i = 0; i < vals.size(); ++i) {
        int start = std::max(0, static_cast<int>(i) - window + 1);
        int count = static_cast<int>(i) - start + 1;
        if (count < 3) { result[i] = 0.0; continue; }
        double sum = 0.0;
        for (int j = start; j <= static_cast<int>(i); ++j) sum += vals[j];
        result[i] = sum / count;
    }
    return result;
}

std::vector<double> FeatureEngine::rollingStd(const std::vector<double>& vals, int window) {
    std::vector<double> result(vals.size(), 0.0);
    for (size_t i = 0; i < vals.size(); ++i) {
        int start = std::max(0, static_cast<int>(i) - window + 1);
        int count = static_cast<int>(i) - start + 1;
        if (count < 3) { result[i] = 0.0; continue; }
        double sum = 0.0, sq = 0.0;
        for (int j = start; j <= static_cast<int>(i); ++j) {
            sum += vals[j];
            sq += vals[j] * vals[j];
        }
        double mean = sum / count;
        double var = (sq / count) - mean * mean;
        result[i] = (var > 0) ? std::sqrt(var) : 0.0;
    }
    return result;
}

std::vector<double> FeatureEngine::rollingMax(const std::vector<double>& vals, int window) {
    std::vector<double> result(vals.size(), 0.0);
    for (size_t i = 0; i < vals.size(); ++i) {
        int start = std::max(0, static_cast<int>(i) - window + 1);
        int count = static_cast<int>(i) - start + 1;
        if (count < 3) { result[i] = 0.0; continue; }
        double mx = vals[start];
        for (int j = start + 1; j <= static_cast<int>(i); ++j)
            mx = std::max(mx, vals[j]);
        result[i] = mx;
    }
    return result;
}

std::vector<double> FeatureEngine::trendSlope(const std::vector<double>& vals, int window) {
    std::vector<double> result(vals.size(), 0.0);
    for (size_t i = 0; i < vals.size(); ++i) {
        if (static_cast<int>(i) < window - 1) continue;
        int start = static_cast<int>(i) - window + 1;
        // Linear regression: y = a*x + b, return a
        int n = window;
        double sx = 0, sy = 0, sxy = 0, sxx = 0;
        for (int j = 0; j < n; ++j) {
            double x = j;
            double y = vals[start + j];
            sx += x;
            sy += y;
            sxy += x * y;
            sxx += x * x;
        }
        double denom = n * sxx - sx * sx;
        if (std::abs(denom) > 1e-12)
            result[i] = (n * sxy - sx * sy) / denom;
    }
    return result;
}

std::vector<double> FeatureEngine::zScore(const std::vector<double>& vals) {
    std::vector<double> result(vals.size(), 0.0);
    if (vals.empty()) return result;

    double sum = 0.0;
    for (auto v : vals) sum += v;
    double mean = sum / vals.size();

    double sq_sum = 0.0;
    for (auto v : vals) sq_sum += (v - mean) * (v - mean);
    double std_dev = std::sqrt(sq_sum / vals.size());

    if (std_dev < 1e-12) return result;

    for (size_t i = 0; i < vals.size(); ++i)
        result[i] = (vals[i] - mean) / std_dev;

    return result;
}

FeatureEngine::Result FeatureEngine::build(const std::vector<DailyRecord>& records) {
    Result result;
    size_t n = records.size();
    if (n < 8) return result;  // need at least 7 days + 1 target

    // Extract raw columns
    std::vector<double> ahi(n), leak_95(n), mask_press_95(n), duration(n);
    std::vector<double> oai(n), cai(n), hi(n);
    std::vector<double> leak_50(n), leak_max(n);
    std::vector<double> mask_press_50(n), mask_press_max(n);
    std::vector<double> resp_rate_50(n), tid_vol_50(n), min_vent_50(n);
    std::vector<int> day_of_week(n);

    for (size_t i = 0; i < n; ++i) {
        ahi[i] = records[i].ahi;
        leak_95[i] = records[i].leak_95;
        mask_press_95[i] = records[i].mask_press_95;
        duration[i] = records[i].duration_minutes;
        oai[i] = records[i].oai;
        cai[i] = records[i].cai;
        hi[i] = records[i].hi;
        leak_50[i] = records[i].leak_50;
        leak_max[i] = records[i].leak_max;
        mask_press_50[i] = records[i].mask_press_50;
        mask_press_max[i] = records[i].mask_press_max;
        resp_rate_50[i] = records[i].resp_rate_50;
        tid_vol_50[i] = records[i].tid_vol_50;
        min_vent_50[i] = records[i].min_vent_50;
        day_of_week[i] = records[i].day_of_week;
    }

    // Compute rolling features
    auto ahi_mean_7 = rollingMean(ahi, 7);
    auto ahi_mean_14 = rollingMean(ahi, 14);
    auto ahi_std_7 = rollingStd(ahi, 7);
    auto ahi_std_14 = rollingStd(ahi, 14);
    auto ahi_max_7 = rollingMax(ahi, 7);
    auto ahi_trend_7 = trendSlope(ahi, 7);
    auto ahi_z = zScore(ahi);

    auto leak_mean_7 = rollingMean(leak_95, 7);
    auto leak_mean_14 = rollingMean(leak_95, 14);
    auto leak_std_7 = rollingStd(leak_95, 7);
    auto leak_std_14 = rollingStd(leak_95, 14);
    auto leak_max_7 = rollingMax(leak_95, 7);
    auto leak_trend_7 = trendSlope(leak_95, 7);
    auto leak_z = zScore(leak_95);

    auto press_mean_7 = rollingMean(mask_press_95, 7);
    auto press_mean_14 = rollingMean(mask_press_95, 14);
    auto press_std_7 = rollingStd(mask_press_95, 7);
    auto press_max_7 = rollingMax(mask_press_95, 7);
    auto press_z = zScore(mask_press_95);

    auto dur_mean_7 = rollingMean(duration, 7);
    auto dur_mean_14 = rollingMean(duration, 14);
    auto dur_std_7 = rollingStd(duration, 7);
    auto dur_max_7 = rollingMax(duration, 7);

    auto oai_mean_7 = rollingMean(oai, 7);
    auto cai_mean_7 = rollingMean(cai, 7);
    auto hi_mean_7 = rollingMean(hi, 7);

    // Days gap
    std::vector<double> days_gap(n, 1.0);
    // Would need date parsing for real gaps; use 1.0 default

    // Build feature names
    result.feature_names = {
        "ahi", "ahi_mean_7d", "ahi_mean_14d", "ahi_std_7d", "ahi_std_14d",
        "ahi_max_7d", "ahi_trend_7d", "ahi_zscore",
        "leak_95", "leak_95_mean_7d", "leak_95_mean_14d", "leak_95_std_7d",
        "leak_95_std_14d", "leak_95_max_7d", "leak_95_trend_7d", "leak_95_zscore",
        "mask_press_95", "mask_press_95_mean_7d", "mask_press_95_mean_14d",
        "mask_press_95_std_7d", "mask_press_95_max_7d", "mask_press_95_zscore",
        "duration_minutes", "duration_minutes_mean_7d", "duration_minutes_mean_14d",
        "duration_minutes_std_7d", "duration_minutes_max_7d",
        "leak_50", "leak_max", "mask_press_50", "mask_press_max",
        "resp_rate_50", "tid_vol_50", "min_vent_50",
        "oai", "cai", "hi",
        "oai_mean_7d", "cai_mean_7d", "hi_mean_7d",
        "day_of_week", "is_weekend", "days_gap"
    };

    // Anomaly labels
    result.anomaly_labels = {"NORMAL", "AHI_ANOMALY", "LEAK_ANOMALY", "PRESSURE_ANOMALY"};

    // Build rows (skip last row -- no next-day target)
    size_t n_samples = n - 1;
    result.X.resize(n_samples);
    result.target_ahi.resize(n_samples);
    result.target_hours.resize(n_samples);
    result.target_high_leak.resize(n_samples);
    result.target_anomaly.resize(n_samples);

    for (size_t i = 0; i < n_samples; ++i) {
        int is_weekend = (day_of_week[i] == 5 || day_of_week[i] == 6) ? 1 : 0;

        result.X[i] = {
            ahi[i], ahi_mean_7[i], ahi_mean_14[i], ahi_std_7[i], ahi_std_14[i],
            ahi_max_7[i], ahi_trend_7[i], ahi_z[i],
            leak_95[i], leak_mean_7[i], leak_mean_14[i], leak_std_7[i],
            leak_std_14[i], leak_max_7[i], leak_trend_7[i], leak_z[i],
            mask_press_95[i], press_mean_7[i], press_mean_14[i],
            press_std_7[i], press_max_7[i], press_z[i],
            duration[i], dur_mean_7[i], dur_mean_14[i],
            dur_std_7[i], dur_max_7[i],
            leak_50[i], leak_max[i], mask_press_50[i], mask_press_max[i],
            resp_rate_50[i], tid_vol_50[i], min_vent_50[i],
            oai[i], cai[i], hi[i],
            oai_mean_7[i], cai_mean_7[i], hi_mean_7[i],
            static_cast<double>(day_of_week[i]), static_cast<double>(is_weekend),
            days_gap[i]
        };

        // Targets: next day values
        result.target_ahi[i] = ahi[i + 1];
        result.target_hours[i] = duration[i + 1] / 60.0;
        result.target_high_leak[i] = (leak_95[i + 1] > HIGH_LEAK_THRESHOLD) ? 1 : 0;

        // Anomaly: rule-based labeling from z-scores
        int anomaly = 0;  // NORMAL
        if (ahi_z[i] > ANOMALY_SIGMA) anomaly = 1;       // AHI_ANOMALY
        if (leak_z[i] > ANOMALY_SIGMA) anomaly = 2;      // LEAK_ANOMALY
        if (press_z[i] > ANOMALY_SIGMA) anomaly = 3;     // PRESSURE_ANOMALY
        result.target_anomaly[i] = anomaly;
    }

    return result;
}

}  // namespace ml
}  // namespace hms_cpap
