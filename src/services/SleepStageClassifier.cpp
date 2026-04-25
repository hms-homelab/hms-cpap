#include "utils/TimeCompat.h"
#include "services/SleepStageClassifier.h"
#include "ml/HmmSmoother.h"
#include "ml/SleepStageFeatureExtractor.h"
#include "parsers/CpapdashBridge.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace hms_cpap {

SleepStageClassifier::SleepStageClassifier(Config config)
    : config_(std::move(config)),
      hmm_(std::make_unique<ml::HmmSmoother>()) {}

SleepStageClassifier::~SleepStageClassifier() = default;

std::string SleepStageClassifier::modelPath(const std::string& name) const {
    return config_.model_dir + "/" + name + ".json";
}

bool SleepStageClassifier::loadOneModel(const std::string& name,
                                         ml::RandomForest& rf,
                                         ml::StandardScaler& scaler,
                                         std::vector<std::string>& features) {
    std::string path = modelPath(name);
    if (!fs::exists(path)) {
        spdlog::warn("SleepStageClassifier: model file not found: {}", path);
        return false;
    }

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        spdlog::error("SleepStageClassifier: cannot open model file: {}", path);
        return false;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(ifs);
        rf = ml::RandomForest::fromJson(j.at("model"));
        scaler = ml::StandardScaler::fromJson(j.at("scaler"));
        features = j.at("features").get<std::vector<std::string>>();
        spdlog::info("SleepStageClassifier: loaded model '{}' ({} features)",
                     name, features.size());
        return true;
    } catch (const std::exception& e) {
        spdlog::error("SleepStageClassifier: failed to parse model '{}': {}",
                      path, e.what());
        return false;
    }
}

bool SleepStageClassifier::loadModels() {
    if (config_.model_dir.empty()) {
        spdlog::warn("SleepStageClassifier: model_dir is empty, cannot load");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    final_loaded_ = loadOneModel(
        config_.model_version + "_final", final_model_, final_scaler_, final_features_);

    live_loaded_ = loadOneModel(
        config_.model_version + "_live", live_model_, live_scaler_, live_features_);

    if (final_loaded_) {
        spdlog::info("SleepStageClassifier: ready (final={}, live={})",
                     final_loaded_, live_loaded_);
    } else {
        spdlog::warn("SleepStageClassifier: final model not loaded");
    }

    return final_loaded_;
}

bool SleepStageClassifier::isReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return final_loaded_;
}

// Convert vector<double> (proba from RF) to array<double,4> for HMM
static std::vector<std::array<double, 4>> toObservations(
        const std::vector<std::vector<double>>& all_proba) {
    std::vector<std::array<double, 4>> obs;
    obs.reserve(all_proba.size());
    for (const auto& p : all_proba) {
        std::array<double, 4> a{};
        for (size_t k = 0; k < std::min(p.size(), size_t(4)); ++k)
            a[k] = p[k];
        obs.push_back(a);
    }
    return obs;
}

std::vector<ml::SleepStageEpoch> SleepStageClassifier::classifyFinal(
        const CPAPSession& session,
        const std::vector<OximetrySample>& oximetry) const {

    std::lock_guard<std::mutex> lock(mutex_);

    if (!final_loaded_) {
        spdlog::warn("SleepStageClassifier::classifyFinal: final model not loaded");
        return {};
    }

    if (!session.session_start) {
        spdlog::warn("SleepStageClassifier::classifyFinal: session has no start time");
        return {};
    }

    auto session_end = session.session_end.value_or(
        *session.session_start + std::chrono::seconds(session.duration_seconds.value_or(0)));

    auto epoch_data = ml::SleepStageFeatureExtractor::aggregateToEpochs(
        session.vitals, session.breathing_summary, oximetry,
        *session.session_start, session_end);

    if (epoch_data.empty()) {
        spdlog::warn("SleepStageClassifier::classifyFinal: no epochs produced");
        return {};
    }

    spdlog::info("SleepStageClassifier: classifyFinal — {} epochs", epoch_data.size());

    std::vector<std::vector<double>> all_proba;
    all_proba.reserve(epoch_data.size());

    for (size_t i = 0; i < epoch_data.size(); ++i) {
        auto features = ml::SleepStageFeatureExtractor::extract(
            epoch_data, static_cast<int>(i),
            ml::FeatureMode::BIDIRECTIONAL, *session.session_start);
        auto scaled = final_scaler_.transformRow(features);
        all_proba.push_back(final_model_.predictProba(scaled));
    }

    auto observations = toObservations(all_proba);
    auto smoothed = hmm_->smooth(observations);

    std::vector<ml::SleepStageEpoch> results;
    results.reserve(epoch_data.size());

    for (size_t i = 0; i < epoch_data.size(); ++i) {
        ml::SleepStageEpoch epoch;
        epoch.epoch_start = epoch_data[i].timestamp;
        epoch.epoch_duration_sec = ml::EPOCH_DURATION_SEC;
        epoch.stage = (i < smoothed.size()) ? smoothed[i]
            : ml::sleepStageFromInt(final_model_.predictClass(
                  final_scaler_.transformRow(
                      ml::SleepStageFeatureExtractor::extract(
                          epoch_data, static_cast<int>(i),
                          ml::FeatureMode::BIDIRECTIONAL, *session.session_start))));

        int stage_int = static_cast<int>(epoch.stage);
        if (stage_int >= 0 && stage_int < static_cast<int>(all_proba[i].size()))
            epoch.confidence = all_proba[i][stage_int];

        epoch.provisional = false;
        epoch.model_version = config_.model_version;
        results.push_back(std::move(epoch));
    }

    auto summary = ml::SleepStageSummary::fromEpochs(results);
    spdlog::info("SleepStageClassifier: final — W={}m L={}m D={}m R={}m eff={:.1f}%",
                 summary.wake_minutes, summary.light_minutes,
                 summary.deep_minutes, summary.rem_minutes,
                 summary.sleep_efficiency_pct);

    return results;
}

std::vector<ml::SleepStageEpoch> SleepStageClassifier::classifyLive(
        const std::vector<CPAPVitals>& vitals,
        const std::vector<BreathingSummary>& breathing,
        const std::vector<OximetrySample>& oximetry,
        std::chrono::system_clock::time_point session_start,
        int start_epoch) const {

    std::lock_guard<std::mutex> lock(mutex_);

    if (!live_loaded_) {
        spdlog::debug("SleepStageClassifier::classifyLive: live model not loaded");
        return {};
    }

    auto now = std::chrono::system_clock::now();
    auto epoch_data = ml::SleepStageFeatureExtractor::aggregateToEpochs(
        vitals, breathing, oximetry, session_start, now);

    if (epoch_data.empty() || start_epoch >= static_cast<int>(epoch_data.size()))
        return {};

    std::vector<std::vector<double>> new_proba;
    for (int i = start_epoch; i < static_cast<int>(epoch_data.size()); ++i) {
        auto features = ml::SleepStageFeatureExtractor::extract(
            epoch_data, i, ml::FeatureMode::CAUSAL, session_start);
        auto scaled = live_scaler_.transformRow(features);
        new_proba.push_back(live_model_.predictProba(scaled));
    }

    auto observations = toObservations(new_proba);
    auto smoothed = hmm_->smooth(observations);

    std::vector<ml::SleepStageEpoch> results;
    for (size_t j = 0; j < new_proba.size(); ++j) {
        int epoch_idx = start_epoch + static_cast<int>(j);
        ml::SleepStageEpoch epoch;
        epoch.epoch_start = epoch_data[epoch_idx].timestamp;
        epoch.epoch_duration_sec = ml::EPOCH_DURATION_SEC;
        epoch.stage = (j < smoothed.size()) ? smoothed[j]
            : ml::sleepStageFromInt(live_model_.predictClass(
                  live_scaler_.transformRow(
                      ml::SleepStageFeatureExtractor::extract(
                          epoch_data, epoch_idx, ml::FeatureMode::CAUSAL, session_start))));

        int stage_int = static_cast<int>(epoch.stage);
        if (stage_int >= 0 && stage_int < static_cast<int>(new_proba[j].size()))
            epoch.confidence = new_proba[j][stage_int];

        epoch.provisional = true;
        epoch.model_version = config_.model_version;
        results.push_back(std::move(epoch));
    }

    if (!results.empty()) {
        spdlog::debug("SleepStageClassifier: live — {} new epochs (start={})",
                      results.size(), start_epoch);
    }

    return results;
}

Json::Value SleepStageClassifier::getStatus() const {
    Json::Value status;
    if (!config_.enabled) {
        status["status"] = "not_configured";
        return status;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    status["enabled"] = config_.enabled;
    status["live_inference"] = config_.live_inference;
    status["model_version"] = config_.model_version;
    status["model_dir"] = config_.model_dir;
    status["final_model_loaded"] = final_loaded_;
    status["live_model_loaded"] = live_loaded_;
    status["status"] = final_loaded_ ? "ready" : "no_models";

    if (final_loaded_)
        status["final_features"] = static_cast<int>(final_features_.size());
    if (live_loaded_)
        status["live_features"] = static_cast<int>(live_features_.size());

    return status;
}

std::string SleepStageClassifier::currentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

}  // namespace hms_cpap
