#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace hms_cpap {
namespace ml {

enum class TaskType { REGRESSION, CLASSIFICATION };

/**
 * DecisionTree - CART implementation for both regression and classification.
 * Regression: MSE splitting, mean prediction.
 * Classification: Gini impurity splitting, majority vote.
 */
class DecisionTree {
public:
    struct Params {
        int max_depth = 8;
        int min_samples_split = 5;
        int min_samples_leaf = 3;
        int max_features = -1;  // -1 = all, 0 = sqrt(n)
    };

    DecisionTree() = default;
    explicit DecisionTree(Params p) : params_(p) {}

    // Regression
    void fitRegression(const std::vector<std::vector<double>>& X,
                       const std::vector<double>& y,
                       std::mt19937* rng = nullptr);

    double predict(const std::vector<double>& x) const;

    // Classification
    void fitClassification(const std::vector<std::vector<double>>& X,
                           const std::vector<int>& y,
                           int n_classes,
                           const std::vector<double>& class_weights = {},
                           std::mt19937* rng = nullptr);

    int predictClass(const std::vector<double>& x) const;
    std::vector<double> predictProba(const std::vector<double>& x) const;

    // Feature importance (accumulated impurity decrease)
    std::vector<double> featureImportances() const;
    int numFeatures() const { return n_features_; }

    nlohmann::json toJson() const;
    static DecisionTree fromJson(const nlohmann::json& j);

private:
    struct Node {
        int feature = -1;
        double threshold = 0.0;
        double value = 0.0;                // regression prediction
        std::vector<double> class_proba;   // classification probabilities
        int predicted_class = -1;
        double impurity_decrease = 0.0;
        int n_samples = 0;
        std::unique_ptr<Node> left;
        std::unique_ptr<Node> right;

        bool isLeaf() const { return !left && !right; }
    };

    std::unique_ptr<Node> root_;
    Params params_;
    TaskType task_ = TaskType::REGRESSION;
    int n_features_ = 0;
    int n_classes_ = 0;

    // Build tree recursively
    std::unique_ptr<Node> buildRegression(
        const std::vector<std::vector<double>>& X,
        const std::vector<double>& y,
        const std::vector<size_t>& indices,
        int depth, std::mt19937* rng);

    std::unique_ptr<Node> buildClassification(
        const std::vector<std::vector<double>>& X,
        const std::vector<int>& y,
        const std::vector<double>& weights,
        const std::vector<size_t>& indices,
        int depth, std::mt19937* rng);

    // Split finders
    struct Split {
        int feature = -1;
        double threshold = 0.0;
        double gain = 0.0;
        std::vector<size_t> left_idx, right_idx;
    };

    Split bestSplitRegression(
        const std::vector<std::vector<double>>& X,
        const std::vector<double>& y,
        const std::vector<size_t>& indices,
        const std::vector<int>& candidate_features) const;

    Split bestSplitClassification(
        const std::vector<std::vector<double>>& X,
        const std::vector<int>& y,
        const std::vector<double>& weights,
        const std::vector<size_t>& indices,
        const std::vector<int>& candidate_features) const;

    std::vector<int> selectFeatures(int n_features, std::mt19937* rng) const;

    // Impurity measures
    static double mse(const std::vector<double>& y,
                      const std::vector<size_t>& indices);
    static double gini(const std::vector<int>& y,
                       const std::vector<double>& weights,
                       const std::vector<size_t>& indices,
                       int n_classes);

    // Traversal
    const Node* traverse(const Node* node, const std::vector<double>& x) const;

    // Importance accumulation
    void accumulateImportance(const Node* node, std::vector<double>& imp) const;

    // Serialization helpers
    static nlohmann::json nodeToJson(const Node* node);
    static std::unique_ptr<Node> nodeFromJson(const nlohmann::json& j);
};

}  // namespace ml
}  // namespace hms_cpap
