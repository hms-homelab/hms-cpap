#pragma once

#include "ml/RandomForest.h"
#include "ml/StandardScaler.h"
#include "ml/CrossValidator.h"
#include "ml/SleepStageTypes.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace hms_cpap {
namespace ml {

struct TrainingConfig {
    RandomForest::Params rf_params{
        200,    // n_estimators
        14,     // max_depth
        20,     // min_samples_split
        10,     // min_samples_leaf
        0,      // max_features (0 = sqrt(n))
        true,   // class_weight_balanced
        42      // random_seed
    };
    int cv_folds = 5;
    FeatureMode mode = FeatureMode::BIDIRECTIONAL;
};

struct TrainingMetrics {
    double accuracy = 0;
    double macro_f1 = 0;
    double wake_f1 = 0, light_f1 = 0, deep_f1 = 0, rem_f1 = 0;
    double wake_recall = 0, light_recall = 0, deep_recall = 0, rem_recall = 0;
    int n_samples = 0;
    std::vector<std::vector<int>> confusion_matrix;
};

class SleepStageTrainer {
public:
    struct Result {
        bool success = false;
        RandomForest model;
        StandardScaler scaler;
        std::vector<std::string> feature_names;
        TrainingMetrics metrics;
        std::string error;
    };

    static Result train(
        const std::vector<std::vector<double>>& X,
        const std::vector<int>& y,
        const TrainingConfig& config = {});

    static bool loadCSV(const std::string& path,
                        std::vector<std::vector<double>>& X,
                        std::vector<int>& y);

    static bool saveBundle(const std::string& dir,
                           const std::string& name,
                           const Result& result);

    static bool loadBundle(const std::string& dir,
                           const std::string& name,
                           RandomForest& model,
                           StandardScaler& scaler,
                           std::vector<std::string>& features);

private:
    static TrainingMetrics computeMetrics(
        const RandomForest& model,
        const std::vector<std::vector<double>>& X_scaled,
        const std::vector<int>& y);
};

}  // namespace ml
}  // namespace hms_cpap
