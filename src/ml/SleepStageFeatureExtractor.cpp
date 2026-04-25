#include "ml/SleepStageFeatureExtractor.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace hms_cpap {
namespace ml {

namespace {

template <typename T>
double safeMean(const std::vector<T>& v) {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

template <typename T>
double safeStd(const std::vector<T>& v) {
    if (v.size() < 2) return 0.0;
    double m = safeMean(v);
    double sq = 0.0;
    for (auto x : v) {
        double d = static_cast<double>(x) - m;
        sq += d * d;
    }
    return std::sqrt(sq / static_cast<double>(v.size()));
}

double linearSlope(const std::vector<double>& y) {
    int n = static_cast<int>(y.size());
    if (n < 2) return 0.0;
    double x_mean = (n - 1) / 2.0;
    double y_mean = safeMean(y);
    double num = 0.0, den = 0.0;
    for (int i = 0; i < n; ++i) {
        double dx = i - x_mean;
        num += dx * (y[i] - y_mean);
        den += dx * dx;
    }
    return den > 1e-12 ? num / den : 0.0;
}

}  // namespace

SleepStageFeatureExtractor::CardiacFeatures
SleepStageFeatureExtractor::computeCardiac(const EpochSample& epoch) {
    CardiacFeatures f;
    if (!epoch.heart_rate) return f;

    double hr = static_cast<double>(*epoch.heart_rate);
    f.hr_mean = hr;
    f.hr_std = 0.0;
    f.hr_min = hr;
    f.hr_max = hr;

    if (hr > 0) {
        double rr_sec = 60.0 / hr;
        f.hrv_rmssd = 0.0;
        f.hrv_sdnn = 0.0;
        // Single-sample epoch: HRV approximated as zero.
        // True HRV requires beat-to-beat intervals from raw PPG.
        // The rolling features across epochs provide temporal variability instead.
        (void)rr_sec;
    }
    return f;
}

SleepStageFeatureExtractor::SpO2Features
SleepStageFeatureExtractor::computeSpO2(const EpochSample& epoch) {
    SpO2Features f;
    if (!epoch.spo2) return f;

    f.mean = *epoch.spo2;
    f.min = *epoch.spo2;
    f.std = 0.0;
    f.odi_proxy = 0.0;
    f.slope = 0.0;
    return f;
}

SleepStageFeatureExtractor::RespiratoryFeatures
SleepStageFeatureExtractor::computeRespiratory(const EpochSample& epoch) {
    RespiratoryFeatures f;
    f.rr_mean = epoch.respiratory_rate.value_or(0.0);
    f.tv_mean = epoch.tidal_volume.value_or(0.0);
    f.mv_mean = epoch.minute_ventilation.value_or(0.0);
    f.ie_ratio_mean = epoch.ie_ratio.value_or(0.0);
    f.leak_mean = epoch.leak_rate.value_or(0.0);
    f.flow_p95 = epoch.flow_p95.value_or(0.0);
    return f;
}

SleepStageFeatureExtractor::MovementFeatures
SleepStageFeatureExtractor::computeMovement(const EpochSample& epoch) {
    MovementFeatures f;
    if (epoch.motion) {
        f.motion_fraction = (*epoch.motion > 0) ? 1.0 : 0.0;
    }
    f.vibration_count = epoch.vibration.value_or(0);
    return f;
}

double SleepStageFeatureExtractor::rollingMean(
    const std::vector<EpochSample>& epochs,
    int center, int start_offset, int end_offset,
    double (*getter)(const EpochSample&))
{
    int n = static_cast<int>(epochs.size());
    int lo = std::max(0, center + start_offset);
    int hi = std::min(n - 1, center + end_offset);
    if (lo > hi) return 0.0;

    double sum = 0.0;
    int count = 0;
    for (int i = lo; i <= hi; ++i) {
        sum += getter(epochs[i]);
        ++count;
    }
    return count > 0 ? sum / count : 0.0;
}

int SleepStageFeatureExtractor::epochIndexForTime(
    std::chrono::system_clock::time_point t,
    std::chrono::system_clock::time_point session_start)
{
    auto dur = std::chrono::duration_cast<std::chrono::seconds>(t - session_start).count();
    return static_cast<int>(dur / EPOCH_DURATION_SEC);
}

std::vector<double> SleepStageFeatureExtractor::extract(
    const std::vector<EpochSample>& all_epochs,
    int epoch_index,
    FeatureMode mode,
    std::chrono::system_clock::time_point /*session_start*/)
{
    int total = static_cast<int>(all_epochs.size());
    if (epoch_index < 0 || epoch_index >= total) return {};

    const auto& epoch = all_epochs[epoch_index];

    auto cardiac = computeCardiac(epoch);
    auto spo2 = computeSpO2(epoch);
    auto resp = computeRespiratory(epoch);
    auto movement = computeMovement(epoch);
    double time_of_night = total > 1 ? static_cast<double>(epoch_index) / (total - 1) : 0.0;

    std::vector<double> features;
    int expected = (mode == FeatureMode::CAUSAL) ? CAUSAL_FEATURE_COUNT : BIDIR_FEATURE_COUNT;
    features.reserve(expected);

    // Cardiac (6)
    features.push_back(cardiac.hr_mean);
    features.push_back(cardiac.hr_std);
    features.push_back(cardiac.hr_min);
    features.push_back(cardiac.hr_max);
    features.push_back(cardiac.hrv_rmssd);
    features.push_back(cardiac.hrv_sdnn);

    // SpO2 (5)
    features.push_back(spo2.mean);
    features.push_back(spo2.min);
    features.push_back(spo2.std);
    features.push_back(spo2.odi_proxy);
    features.push_back(spo2.slope);

    // Respiratory (9)
    features.push_back(resp.rr_mean);
    features.push_back(resp.rr_std);
    features.push_back(resp.tv_mean);
    features.push_back(resp.tv_std);
    features.push_back(resp.mv_mean);
    features.push_back(resp.mv_std);
    features.push_back(resp.ie_ratio_mean);
    features.push_back(resp.leak_mean);
    features.push_back(resp.flow_p95);

    // Movement (2)
    features.push_back(movement.motion_fraction);
    features.push_back(movement.vibration_count);

    // Context (1)
    features.push_back(time_of_night);

    // Rolling features: hr_mean, spo2_mean, rr_mean, motion_fraction
    auto getHrMean = [](const EpochSample& e) -> double {
        return e.heart_rate ? static_cast<double>(*e.heart_rate) : 0.0;
    };
    auto getSpo2Mean = [](const EpochSample& e) -> double {
        return e.spo2.value_or(0.0);
    };
    auto getRrMean = [](const EpochSample& e) -> double {
        return e.respiratory_rate.value_or(0.0);
    };
    auto getMotion = [](const EpochSample& e) -> double {
        return (e.motion && *e.motion > 0) ? 1.0 : 0.0;
    };

    // Past 5
    features.push_back(rollingMean(all_epochs, epoch_index, -5, -1, getHrMean));
    features.push_back(rollingMean(all_epochs, epoch_index, -5, -1, getSpo2Mean));
    features.push_back(rollingMean(all_epochs, epoch_index, -5, -1, getRrMean));
    features.push_back(rollingMean(all_epochs, epoch_index, -5, -1, getMotion));

    // Past 10
    features.push_back(rollingMean(all_epochs, epoch_index, -10, -1, getHrMean));
    features.push_back(rollingMean(all_epochs, epoch_index, -10, -1, getSpo2Mean));
    features.push_back(rollingMean(all_epochs, epoch_index, -10, -1, getRrMean));
    features.push_back(rollingMean(all_epochs, epoch_index, -10, -1, getMotion));

    // Future 5 (bidirectional only)
    if (mode == FeatureMode::BIDIRECTIONAL) {
        features.push_back(rollingMean(all_epochs, epoch_index, 1, 5, getHrMean));
        features.push_back(rollingMean(all_epochs, epoch_index, 1, 5, getSpo2Mean));
        features.push_back(rollingMean(all_epochs, epoch_index, 1, 5, getRrMean));
        features.push_back(rollingMean(all_epochs, epoch_index, 1, 5, getMotion));
    }

    return features;
}

std::vector<EpochSample> SleepStageFeatureExtractor::aggregateToEpochs(
    const std::vector<CPAPVitals>& vitals,
    const std::vector<BreathingSummary>& breathing,
    const std::vector<OximetrySample>& oximetry,
    std::chrono::system_clock::time_point session_start,
    std::chrono::system_clock::time_point session_end)
{
    auto total_sec = std::chrono::duration_cast<std::chrono::seconds>(
        session_end - session_start).count();
    int n_epochs = static_cast<int>(total_sec / EPOCH_DURATION_SEC);
    if (n_epochs <= 0) return {};

    std::vector<EpochSample> epochs(n_epochs);

    for (int i = 0; i < n_epochs; ++i) {
        epochs[i].timestamp = session_start + std::chrono::seconds(i * EPOCH_DURATION_SEC);
    }

    // Bin SAD vitals (per-second SpO2/HR) into epochs
    struct VitalBin { std::vector<double> spo2; std::vector<int> hr; };
    std::vector<VitalBin> vital_bins(n_epochs);

    for (const auto& v : vitals) {
        int idx = epochIndexForTime(v.timestamp, session_start);
        if (idx < 0 || idx >= n_epochs) continue;
        if (v.spo2) vital_bins[idx].spo2.push_back(*v.spo2);
        if (v.heart_rate) vital_bins[idx].hr.push_back(*v.heart_rate);
    }

    // Bin O2 Ring oximetry (per-second, has motion/vibration)
    struct OxiBin {
        std::vector<double> spo2;
        std::vector<int> hr;
        std::vector<int> motion;
        std::vector<int> vibration;
    };
    std::vector<OxiBin> oxi_bins(n_epochs);

    for (const auto& o : oximetry) {
        int idx = epochIndexForTime(o.timestamp, session_start);
        if (idx < 0 || idx >= n_epochs) continue;
        if (o.valid()) {
            oxi_bins[idx].spo2.push_back(static_cast<double>(o.spo2));
            oxi_bins[idx].hr.push_back(static_cast<int>(o.heart_rate));
        }
        oxi_bins[idx].motion.push_back(static_cast<int>(o.motion));
        oxi_bins[idx].vibration.push_back(static_cast<int>(o.vibration));
    }

    // Merge vitals: prefer O2 Ring when available (higher fidelity than SAD)
    for (int i = 0; i < n_epochs; ++i) {
        auto& ob = oxi_bins[i];
        auto& vb = vital_bins[i];

        if (!ob.spo2.empty()) {
            epochs[i].spo2 = safeMean(ob.spo2);
            epochs[i].heart_rate = static_cast<int>(std::round(safeMean(ob.hr)));
        } else if (!vb.spo2.empty() || !vb.hr.empty()) {
            if (!vb.spo2.empty()) epochs[i].spo2 = safeMean(vb.spo2);
            if (!vb.hr.empty()) epochs[i].heart_rate = static_cast<int>(std::round(safeMean(vb.hr)));
        }

        if (!ob.motion.empty()) {
            int moving = 0;
            for (auto m : ob.motion) if (m > 0) ++moving;
            epochs[i].motion = (moving > 0) ? static_cast<int>(
                std::round(safeMean(ob.motion))) : 0;
        }
        if (!ob.vibration.empty()) {
            int total_vib = 0;
            for (auto v : ob.vibration) total_vib += v;
            epochs[i].vibration = total_vib;
        }
    }

    // Bin breathing summaries into epochs
    struct BreathBin {
        std::vector<double> rr, tv, mv, ie, leak, fp95;
    };
    std::vector<BreathBin> breath_bins(n_epochs);

    for (const auto& b : breathing) {
        int idx = epochIndexForTime(b.timestamp, session_start);
        if (idx < 0 || idx >= n_epochs) continue;
        if (b.respiratory_rate) breath_bins[idx].rr.push_back(*b.respiratory_rate);
        if (b.tidal_volume) breath_bins[idx].tv.push_back(*b.tidal_volume);
        if (b.minute_ventilation) breath_bins[idx].mv.push_back(*b.minute_ventilation);
        if (b.ie_ratio) breath_bins[idx].ie.push_back(*b.ie_ratio);
        if (b.leak_rate) breath_bins[idx].leak.push_back(*b.leak_rate);
        if (b.flow_p95) breath_bins[idx].fp95.push_back(*b.flow_p95);
    }

    for (int i = 0; i < n_epochs; ++i) {
        auto& bb = breath_bins[i];
        if (!bb.rr.empty()) epochs[i].respiratory_rate = safeMean(bb.rr);
        if (!bb.tv.empty()) epochs[i].tidal_volume = safeMean(bb.tv);
        if (!bb.mv.empty()) epochs[i].minute_ventilation = safeMean(bb.mv);
        if (!bb.ie.empty()) epochs[i].ie_ratio = safeMean(bb.ie);
        if (!bb.leak.empty()) epochs[i].leak_rate = safeMean(bb.leak);
        if (!bb.fp95.empty()) epochs[i].flow_p95 = safeMean(bb.fp95);
    }

    return epochs;
}

}  // namespace ml
}  // namespace hms_cpap
