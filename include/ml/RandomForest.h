#pragma once

#include "ml/DecisionTree.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace hms_cpap {
namespace ml {

/**
 * RandomForest - Ensemble of DecisionTrees with bootstrap sampling.
 * Supports both regression (averaging) and classification (majority vote).
 */
class RandomForest {
public:
    struct Params {
        int n_estimators = 200;
        int max_depth = 8;
        int min_samples_split = 5;
        int min_samples_leaf = 3;
        int max_features = 0;     // 0 = sqrt(n) for clf, n/3 for reg
        bool class_weight_balanced = false;
        int random_seed = 42;
    };

    RandomForest() = default;
    explicit RandomForest(Params p) : params_(p) {}

    // -- Regression --
    void fitRegression(const std::vector<std::vector<double>>& X,
                       const std::vector<double>& y);

    double predict(const std::vector<double>& x) const;

    // -- Classification --
    void fitClassification(const std::vector<std::vector<double>>& X,
                           const std::vector<int>& y,
                           int n_classes);

    int predictClass(const std::vector<double>& x) const;
    std::vector<double> predictProba(const std::vector<double>& x) const;

    // -- Shared --
    std::vector<double> featureImportances() const;
    TaskType taskType() const { return task_; }
    int numClasses() const { return n_classes_; }
    int numFeatures() const { return n_features_; }
    bool isTrained() const { return !trees_.empty(); }

    // -- Serialization --
    nlohmann::json toJson() const;
    static RandomForest fromJson(const nlohmann::json& j);

private:
    Params params_;
    TaskType task_ = TaskType::REGRESSION;
    int n_classes_ = 0;
    int n_features_ = 0;
    std::vector<DecisionTree> trees_;

    // Bootstrap sampling
    std::vector<size_t> bootstrapSample(size_t n, std::mt19937& rng) const;

    // Balanced class weights
    std::vector<double> computeBalancedWeights(const std::vector<int>& y,
                                                int n_classes) const;
};

}  // namespace ml
}  // namespace hms_cpap
