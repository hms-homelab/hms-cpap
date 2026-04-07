#include "ml/RandomForest.h"
#include <algorithm>
#include <numeric>

namespace hms_cpap {
namespace ml {

// ---------- Regression ----------

void RandomForest::fitRegression(const std::vector<std::vector<double>>& X,
                                  const std::vector<double>& y) {
    task_ = TaskType::REGRESSION;
    n_features_ = static_cast<int>(X[0].size());
    trees_.clear();
    trees_.reserve(params_.n_estimators);

    std::mt19937 rng(params_.random_seed);

    int max_feat = params_.max_features;
    if (max_feat <= 0) max_feat = std::max(1, n_features_ / 3);

    for (int i = 0; i < params_.n_estimators; ++i) {
        auto sample_idx = bootstrapSample(X.size(), rng);

        DecisionTree::Params tp;
        tp.max_depth = params_.max_depth;
        tp.min_samples_split = params_.min_samples_split;
        tp.min_samples_leaf = params_.min_samples_leaf;
        tp.max_features = max_feat;

        DecisionTree tree(tp);

        // Build bootstrap dataset
        std::vector<std::vector<double>> X_boot;
        std::vector<double> y_boot;
        X_boot.reserve(sample_idx.size());
        y_boot.reserve(sample_idx.size());
        for (auto idx : sample_idx) {
            X_boot.push_back(X[idx]);
            y_boot.push_back(y[idx]);
        }

        std::mt19937 tree_rng(rng());
        tree.fitRegression(X_boot, y_boot, &tree_rng);
        trees_.push_back(std::move(tree));
    }
}

double RandomForest::predict(const std::vector<double>& x) const {
    if (trees_.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& tree : trees_)
        sum += tree.predict(x);
    return sum / trees_.size();
}

// ---------- Classification ----------

void RandomForest::fitClassification(const std::vector<std::vector<double>>& X,
                                      const std::vector<int>& y,
                                      int n_classes) {
    task_ = TaskType::CLASSIFICATION;
    n_classes_ = n_classes;
    n_features_ = static_cast<int>(X[0].size());
    trees_.clear();
    trees_.reserve(params_.n_estimators);

    std::mt19937 rng(params_.random_seed);

    int max_feat = params_.max_features;
    if (max_feat <= 0) max_feat = std::max(1, static_cast<int>(std::sqrt(n_features_)));

    std::vector<double> class_weights;
    if (params_.class_weight_balanced)
        class_weights = computeBalancedWeights(y, n_classes);

    for (int i = 0; i < params_.n_estimators; ++i) {
        auto sample_idx = bootstrapSample(X.size(), rng);

        DecisionTree::Params tp;
        tp.max_depth = params_.max_depth;
        tp.min_samples_split = params_.min_samples_split;
        tp.min_samples_leaf = params_.min_samples_leaf;
        tp.max_features = max_feat;

        DecisionTree tree(tp);

        std::vector<std::vector<double>> X_boot;
        std::vector<int> y_boot;
        X_boot.reserve(sample_idx.size());
        y_boot.reserve(sample_idx.size());
        for (auto idx : sample_idx) {
            X_boot.push_back(X[idx]);
            y_boot.push_back(y[idx]);
        }

        std::mt19937 tree_rng(rng());
        tree.fitClassification(X_boot, y_boot, n_classes, class_weights, &tree_rng);
        trees_.push_back(std::move(tree));
    }
}

int RandomForest::predictClass(const std::vector<double>& x) const {
    auto proba = predictProba(x);
    return static_cast<int>(
        std::distance(proba.begin(), std::max_element(proba.begin(), proba.end())));
}

std::vector<double> RandomForest::predictProba(const std::vector<double>& x) const {
    if (trees_.empty()) return std::vector<double>(n_classes_, 1.0 / n_classes_);

    std::vector<double> avg(n_classes_, 0.0);
    for (const auto& tree : trees_) {
        auto p = tree.predictProba(x);
        for (int c = 0; c < n_classes_; ++c)
            avg[c] += p[c];
    }
    for (auto& v : avg) v /= trees_.size();
    return avg;
}

// ---------- Feature Importance ----------

std::vector<double> RandomForest::featureImportances() const {
    if (trees_.empty()) return {};
    std::vector<double> imp(n_features_, 0.0);
    for (const auto& tree : trees_) {
        auto ti = tree.featureImportances();
        for (int j = 0; j < n_features_; ++j)
            imp[j] += ti[j];
    }
    double total = std::accumulate(imp.begin(), imp.end(), 0.0);
    if (total > 0) for (auto& v : imp) v /= total;
    return imp;
}

// ---------- Helpers ----------

std::vector<size_t> RandomForest::bootstrapSample(size_t n, std::mt19937& rng) const {
    std::uniform_int_distribution<size_t> dist(0, n - 1);
    std::vector<size_t> indices(n);
    for (size_t i = 0; i < n; ++i)
        indices[i] = dist(rng);
    return indices;
}

std::vector<double> RandomForest::computeBalancedWeights(
        const std::vector<int>& y, int n_classes) const {
    std::vector<int> counts(n_classes, 0);
    for (auto c : y) if (c >= 0 && c < n_classes) ++counts[c];

    double n = static_cast<double>(y.size());
    std::vector<double> weights(n_classes, 1.0);
    for (int c = 0; c < n_classes; ++c) {
        if (counts[c] > 0)
            weights[c] = n / (n_classes * counts[c]);
    }
    return weights;
}

// ---------- Serialization ----------

nlohmann::json RandomForest::toJson() const {
    nlohmann::json j;
    j["task"] = (task_ == TaskType::REGRESSION) ? "regression" : "classification";
    j["n_classes"] = n_classes_;
    j["n_features"] = n_features_;
    j["params"] = {
        {"n_estimators", params_.n_estimators},
        {"max_depth", params_.max_depth},
        {"min_samples_split", params_.min_samples_split},
        {"min_samples_leaf", params_.min_samples_leaf},
        {"max_features", params_.max_features},
        {"class_weight_balanced", params_.class_weight_balanced},
        {"random_seed", params_.random_seed}
    };
    nlohmann::json trees_json = nlohmann::json::array();
    for (const auto& tree : trees_)
        trees_json.push_back(tree.toJson());
    j["trees"] = std::move(trees_json);
    return j;
}

RandomForest RandomForest::fromJson(const nlohmann::json& j) {
    RandomForest rf;
    rf.task_ = (j.at("task").get<std::string>() == "regression")
                   ? TaskType::REGRESSION : TaskType::CLASSIFICATION;
    rf.n_classes_ = j.value("n_classes", 0);
    rf.n_features_ = j.at("n_features").get<int>();

    auto& p = j.at("params");
    rf.params_.n_estimators = p.at("n_estimators").get<int>();
    rf.params_.max_depth = p.at("max_depth").get<int>();
    rf.params_.min_samples_split = p.at("min_samples_split").get<int>();
    rf.params_.min_samples_leaf = p.at("min_samples_leaf").get<int>();
    rf.params_.max_features = p.value("max_features", 0);
    rf.params_.class_weight_balanced = p.value("class_weight_balanced", false);
    rf.params_.random_seed = p.value("random_seed", 42);

    for (const auto& tj : j.at("trees"))
        rf.trees_.push_back(DecisionTree::fromJson(tj));

    return rf;
}

}  // namespace ml
}  // namespace hms_cpap
