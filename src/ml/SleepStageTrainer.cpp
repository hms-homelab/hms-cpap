#include "ml/SleepStageTrainer.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <algorithm>
#include <numeric>

namespace hms_cpap {
namespace ml {

namespace {

constexpr int N_CLASSES = 4;

double f1Score(int tp, int fp, int fn) {
    double prec = (tp + fp > 0) ? static_cast<double>(tp) / (tp + fp) : 0.0;
    double rec = (tp + fn > 0) ? static_cast<double>(tp) / (tp + fn) : 0.0;
    return (prec + rec > 0) ? 2.0 * prec * rec / (prec + rec) : 0.0;
}

}  // namespace

TrainingMetrics SleepStageTrainer::computeMetrics(
    const RandomForest& model,
    const std::vector<std::vector<double>>& X_scaled,
    const std::vector<int>& y)
{
    TrainingMetrics m;
    m.n_samples = static_cast<int>(y.size());
    m.confusion_matrix.assign(N_CLASSES, std::vector<int>(N_CLASSES, 0));

    int correct = 0;
    std::vector<int> tp(N_CLASSES, 0), fp(N_CLASSES, 0), fn(N_CLASSES, 0);

    for (size_t i = 0; i < y.size(); ++i) {
        int pred = model.predictClass(X_scaled[i]);
        int actual = y[i];
        m.confusion_matrix[actual][pred]++;
        if (pred == actual) {
            ++correct;
            ++tp[pred];
        } else {
            ++fp[pred];
            ++fn[actual];
        }
    }

    m.accuracy = m.n_samples > 0 ? static_cast<double>(correct) / m.n_samples : 0.0;

    auto perClassF1 = [&](int c) { return f1Score(tp[c], fp[c], fn[c]); };
    auto perClassRecall = [&](int c) -> double {
        return (tp[c] + fn[c] > 0) ? static_cast<double>(tp[c]) / (tp[c] + fn[c]) : 0.0;
    };

    m.wake_f1 = perClassF1(0);
    m.light_f1 = perClassF1(1);
    m.deep_f1 = perClassF1(2);
    m.rem_f1 = perClassF1(3);

    m.wake_recall = perClassRecall(0);
    m.light_recall = perClassRecall(1);
    m.deep_recall = perClassRecall(2);
    m.rem_recall = perClassRecall(3);

    m.macro_f1 = (m.wake_f1 + m.light_f1 + m.deep_f1 + m.rem_f1) / N_CLASSES;

    return m;
}

SleepStageTrainer::Result SleepStageTrainer::train(
    const std::vector<std::vector<double>>& X,
    const std::vector<int>& y,
    const TrainingConfig& config)
{
    Result result;

    if (X.empty() || X.size() != y.size()) {
        result.error = "Empty or mismatched X/y";
        return result;
    }

    for (int label : y) {
        if (label < 0 || label >= N_CLASSES) {
            result.error = "Label out of range [0,3]: " + std::to_string(label);
            return result;
        }
    }

    spdlog::info("SleepStageTrainer: {} samples, {} features, mode={}",
                 X.size(), X[0].size(),
                 config.mode == FeatureMode::CAUSAL ? "causal" : "bidirectional");

    result.feature_names = sleepStageFeatureNames(config.mode);
    result.scaler.fit(X);
    auto X_scaled = result.scaler.transform(X);

    result.model = RandomForest(config.rf_params);
    result.model.fitClassification(X_scaled, y, N_CLASSES);

    result.metrics = computeMetrics(result.model, X_scaled, y);

    spdlog::info("SleepStageTrainer: train accuracy={:.3f}, macro_f1={:.3f}",
                 result.metrics.accuracy, result.metrics.macro_f1);

    // Cross-validation for generalization estimate
    auto cv = CrossValidator::cvClassification(
        X, y, N_CLASSES, config.rf_params, config.cv_folds);

    spdlog::info("SleepStageTrainer: CV accuracy={:.3f}+/-{:.3f}, f1={:.3f}+/-{:.3f}",
                 cv.accuracy_mean, cv.accuracy_std, cv.f1_mean, cv.f1_std);

    result.success = true;
    return result;
}

bool SleepStageTrainer::loadCSV(const std::string& path,
                                 std::vector<std::vector<double>>& X,
                                 std::vector<int>& y)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        spdlog::error("SleepStageTrainer: cannot open {}", path);
        return false;
    }

    X.clear();
    y.clear();

    std::string line;
    bool header = true;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        if (header) { header = false; continue; }

        std::stringstream ss(line);
        std::string cell;
        std::vector<double> row;

        while (std::getline(ss, cell, ',')) {
            try {
                row.push_back(std::stod(cell));
            } catch (...) {
                row.push_back(0.0);
            }
        }

        if (row.size() < 2) continue;

        int label = static_cast<int>(row.back());
        row.pop_back();
        X.push_back(std::move(row));
        y.push_back(label);
    }

    spdlog::info("SleepStageTrainer: loaded {} samples from {}", X.size(), path);
    return !X.empty();
}

bool SleepStageTrainer::saveBundle(const std::string& dir,
                                    const std::string& name,
                                    const Result& result)
{
    if (!result.success) return false;

    auto writefile = [](const std::string& path, const nlohmann::json& j) -> bool {
        std::ofstream f(path);
        if (!f.is_open()) return false;
        f << j.dump(2);
        return f.good();
    };

    std::string prefix = dir + "/" + name;

    if (!writefile(prefix + ".json", result.model.toJson())) {
        spdlog::error("SleepStageTrainer: failed to write model to {}.json", prefix);
        return false;
    }

    if (!writefile(prefix + "_scaler.json", result.scaler.toJson())) {
        spdlog::error("SleepStageTrainer: failed to write scaler to {}_scaler.json", prefix);
        return false;
    }

    nlohmann::json features_json;
    features_json["feature_names"] = result.feature_names;
    features_json["n_features"] = result.feature_names.size();

    nlohmann::json metrics_json;
    metrics_json["accuracy"] = result.metrics.accuracy;
    metrics_json["macro_f1"] = result.metrics.macro_f1;
    metrics_json["wake_f1"] = result.metrics.wake_f1;
    metrics_json["light_f1"] = result.metrics.light_f1;
    metrics_json["deep_f1"] = result.metrics.deep_f1;
    metrics_json["rem_f1"] = result.metrics.rem_f1;
    metrics_json["wake_recall"] = result.metrics.wake_recall;
    metrics_json["light_recall"] = result.metrics.light_recall;
    metrics_json["deep_recall"] = result.metrics.deep_recall;
    metrics_json["rem_recall"] = result.metrics.rem_recall;
    metrics_json["n_samples"] = result.metrics.n_samples;
    metrics_json["confusion_matrix"] = result.metrics.confusion_matrix;
    features_json["metrics"] = metrics_json;

    if (!writefile(prefix + "_features.json", features_json)) {
        spdlog::error("SleepStageTrainer: failed to write features to {}_features.json", prefix);
        return false;
    }

    spdlog::info("SleepStageTrainer: saved bundle to {}", prefix);
    return true;
}

bool SleepStageTrainer::loadBundle(const std::string& dir,
                                    const std::string& name,
                                    RandomForest& model,
                                    StandardScaler& scaler,
                                    std::vector<std::string>& features)
{
    auto readfile = [](const std::string& path) -> std::optional<nlohmann::json> {
        std::ifstream f(path);
        if (!f.is_open()) return std::nullopt;
        try {
            return nlohmann::json::parse(f);
        } catch (...) {
            return std::nullopt;
        }
    };

    std::string prefix = dir + "/" + name;

    auto model_json = readfile(prefix + ".json");
    if (!model_json) {
        spdlog::error("SleepStageTrainer: cannot load model from {}.json", prefix);
        return false;
    }
    model = RandomForest::fromJson(*model_json);

    auto scaler_json = readfile(prefix + "_scaler.json");
    if (!scaler_json) {
        spdlog::error("SleepStageTrainer: cannot load scaler from {}_scaler.json", prefix);
        return false;
    }
    scaler = StandardScaler::fromJson(*scaler_json);

    auto features_json = readfile(prefix + "_features.json");
    if (!features_json) {
        spdlog::error("SleepStageTrainer: cannot load features from {}_features.json", prefix);
        return false;
    }
    features = (*features_json)["feature_names"].get<std::vector<std::string>>();

    spdlog::info("SleepStageTrainer: loaded bundle from {} ({} features)", prefix, features.size());
    return true;
}

}  // namespace ml
}  // namespace hms_cpap
