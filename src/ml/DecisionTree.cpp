#include "ml/DecisionTree.h"
#include <algorithm>
#include <cassert>
#include <numeric>

namespace hms_cpap {
namespace ml {

// ---------- Regression ----------

void DecisionTree::fitRegression(const std::vector<std::vector<double>>& X,
                                  const std::vector<double>& y,
                                  std::mt19937* rng) {
    task_ = TaskType::REGRESSION;
    n_features_ = static_cast<int>(X[0].size());
    std::vector<size_t> indices(y.size());
    std::iota(indices.begin(), indices.end(), 0);
    root_ = buildRegression(X, y, indices, 0, rng);
}

double DecisionTree::predict(const std::vector<double>& x) const {
    const Node* leaf = traverse(root_.get(), x);
    return leaf ? leaf->value : 0.0;
}

// ---------- Classification ----------

void DecisionTree::fitClassification(const std::vector<std::vector<double>>& X,
                                      const std::vector<int>& y,
                                      int n_classes,
                                      const std::vector<double>& class_weights,
                                      std::mt19937* rng) {
    task_ = TaskType::CLASSIFICATION;
    n_features_ = static_cast<int>(X[0].size());
    n_classes_ = n_classes;

    // Build sample weights from class weights
    std::vector<double> weights(y.size(), 1.0);
    if (!class_weights.empty()) {
        for (size_t i = 0; i < y.size(); ++i)
            if (y[i] >= 0 && y[i] < static_cast<int>(class_weights.size()))
                weights[i] = class_weights[y[i]];
    }

    std::vector<size_t> indices(y.size());
    std::iota(indices.begin(), indices.end(), 0);
    root_ = buildClassification(X, y, weights, indices, 0, rng);
}

int DecisionTree::predictClass(const std::vector<double>& x) const {
    const Node* leaf = traverse(root_.get(), x);
    return leaf ? leaf->predicted_class : 0;
}

std::vector<double> DecisionTree::predictProba(const std::vector<double>& x) const {
    const Node* leaf = traverse(root_.get(), x);
    if (leaf && !leaf->class_proba.empty()) return leaf->class_proba;
    return std::vector<double>(n_classes_, 1.0 / n_classes_);
}

// ---------- Build Regression Tree ----------

std::unique_ptr<DecisionTree::Node> DecisionTree::buildRegression(
        const std::vector<std::vector<double>>& X,
        const std::vector<double>& y,
        const std::vector<size_t>& indices,
        int depth, std::mt19937* rng) {

    auto node = std::make_unique<Node>();
    node->n_samples = static_cast<int>(indices.size());

    // Leaf: compute mean
    double sum = 0.0;
    for (auto i : indices) sum += y[i];
    node->value = sum / indices.size();

    // Stop conditions
    if (depth >= params_.max_depth ||
        static_cast<int>(indices.size()) < params_.min_samples_split) {
        return node;
    }

    auto candidates = selectFeatures(n_features_, rng);
    Split best = bestSplitRegression(X, y, indices, candidates);

    if (best.feature < 0 ||
        static_cast<int>(best.left_idx.size()) < params_.min_samples_leaf ||
        static_cast<int>(best.right_idx.size()) < params_.min_samples_leaf) {
        return node;
    }

    node->feature = best.feature;
    node->threshold = best.threshold;
    node->impurity_decrease = best.gain * indices.size();
    node->left = buildRegression(X, y, best.left_idx, depth + 1, rng);
    node->right = buildRegression(X, y, best.right_idx, depth + 1, rng);
    return node;
}

// ---------- Build Classification Tree ----------

std::unique_ptr<DecisionTree::Node> DecisionTree::buildClassification(
        const std::vector<std::vector<double>>& X,
        const std::vector<int>& y,
        const std::vector<double>& weights,
        const std::vector<size_t>& indices,
        int depth, std::mt19937* rng) {

    auto node = std::make_unique<Node>();
    node->n_samples = static_cast<int>(indices.size());

    // Leaf: compute class distribution
    std::vector<double> class_counts(n_classes_, 0.0);
    for (auto i : indices)
        if (y[i] >= 0 && y[i] < n_classes_)
            class_counts[y[i]] += weights[i];

    double total = std::accumulate(class_counts.begin(), class_counts.end(), 0.0);
    node->class_proba.resize(n_classes_);
    for (int c = 0; c < n_classes_; ++c)
        node->class_proba[c] = (total > 0) ? class_counts[c] / total : 0.0;

    node->predicted_class = static_cast<int>(
        std::distance(class_counts.begin(),
                      std::max_element(class_counts.begin(), class_counts.end())));

    // Stop conditions
    if (depth >= params_.max_depth ||
        static_cast<int>(indices.size()) < params_.min_samples_split) {
        return node;
    }

    // Check if pure node
    int unique_classes = 0;
    for (auto& c : class_counts) if (c > 0) ++unique_classes;
    if (unique_classes <= 1) return node;

    auto candidates = selectFeatures(n_features_, rng);
    Split best = bestSplitClassification(X, y, weights, indices, candidates);

    if (best.feature < 0 ||
        static_cast<int>(best.left_idx.size()) < params_.min_samples_leaf ||
        static_cast<int>(best.right_idx.size()) < params_.min_samples_leaf) {
        return node;
    }

    node->feature = best.feature;
    node->threshold = best.threshold;
    node->impurity_decrease = best.gain * indices.size();
    node->left = buildClassification(X, y, weights, best.left_idx, depth + 1, rng);
    node->right = buildClassification(X, y, weights, best.right_idx, depth + 1, rng);
    return node;
}

// ---------- Split Finders ----------

DecisionTree::Split DecisionTree::bestSplitRegression(
        const std::vector<std::vector<double>>& X,
        const std::vector<double>& y,
        const std::vector<size_t>& indices,
        const std::vector<int>& candidate_features) const {

    Split best;
    double best_mse = std::numeric_limits<double>::max();
    double parent_mse_val = mse(y, indices);

    for (int feat : candidate_features) {
        // Sort indices by feature value
        auto sorted = indices;
        std::sort(sorted.begin(), sorted.end(),
                  [&](size_t a, size_t b) { return X[a][feat] < X[b][feat]; });

        // Running sums for efficient MSE computation
        double left_sum = 0.0, left_sq = 0.0;
        double total_sum = 0.0, total_sq = 0.0;
        for (auto i : sorted) {
            total_sum += y[i];
            total_sq += y[i] * y[i];
        }

        size_t n = sorted.size();
        for (size_t k = 0; k < n - 1; ++k) {
            left_sum += y[sorted[k]];
            left_sq += y[sorted[k]] * y[sorted[k]];
            size_t left_n = k + 1;
            size_t right_n = n - left_n;

            if (X[sorted[k]][feat] == X[sorted[k + 1]][feat]) continue;
            if (static_cast<int>(left_n) < params_.min_samples_leaf ||
                static_cast<int>(right_n) < params_.min_samples_leaf) continue;

            double left_mean = left_sum / left_n;
            double right_sum = total_sum - left_sum;
            double right_mean = right_sum / right_n;

            double left_mse = (left_sq - left_sum * left_mean);
            double right_sq = total_sq - left_sq;
            double right_mse = (right_sq - right_sum * right_mean);
            double weighted = (left_mse + right_mse) / n;

            if (weighted < best_mse) {
                best_mse = weighted;
                best.feature = feat;
                best.threshold = (X[sorted[k]][feat] + X[sorted[k + 1]][feat]) / 2.0;
                best.gain = parent_mse_val - weighted;
                best.left_idx.assign(sorted.begin(), sorted.begin() + left_n);
                best.right_idx.assign(sorted.begin() + left_n, sorted.end());
            }
        }
    }
    return best;
}

DecisionTree::Split DecisionTree::bestSplitClassification(
        const std::vector<std::vector<double>>& X,
        const std::vector<int>& y,
        const std::vector<double>& weights,
        const std::vector<size_t>& indices,
        const std::vector<int>& candidate_features) const {

    Split best;
    double best_impurity = std::numeric_limits<double>::max();

    for (int feat : candidate_features) {
        auto sorted = indices;
        std::sort(sorted.begin(), sorted.end(),
                  [&](size_t a, size_t b) { return X[a][feat] < X[b][feat]; });

        // Running class counts
        std::vector<double> left_counts(n_classes_, 0.0);
        std::vector<double> total_counts(n_classes_, 0.0);
        for (auto i : sorted) total_counts[y[i]] += weights[i];
        double total_w = std::accumulate(total_counts.begin(), total_counts.end(), 0.0);

        double left_w = 0.0;
        size_t n = sorted.size();

        for (size_t k = 0; k < n - 1; ++k) {
            left_counts[y[sorted[k]]] += weights[sorted[k]];
            left_w += weights[sorted[k]];
            double right_w = total_w - left_w;

            if (X[sorted[k]][feat] == X[sorted[k + 1]][feat]) continue;
            if (left_w < 1e-12 || right_w < 1e-12) continue;

            // Gini for left
            double left_gini = 1.0;
            for (int c = 0; c < n_classes_; ++c) {
                double p = left_counts[c] / left_w;
                left_gini -= p * p;
            }

            // Gini for right
            double right_gini = 1.0;
            for (int c = 0; c < n_classes_; ++c) {
                double p = (total_counts[c] - left_counts[c]) / right_w;
                right_gini -= p * p;
            }

            double weighted = (left_w * left_gini + right_w * right_gini) / total_w;
            if (weighted < best_impurity) {
                best_impurity = weighted;
                best.feature = feat;
                best.threshold = (X[sorted[k]][feat] + X[sorted[k + 1]][feat]) / 2.0;
                best.gain = gini(y, weights, indices, n_classes_) - weighted;
                best.left_idx.assign(sorted.begin(), sorted.begin() + k + 1);
                best.right_idx.assign(sorted.begin() + k + 1, sorted.end());
            }
        }
    }
    return best;
}

// ---------- Helpers ----------

std::vector<int> DecisionTree::selectFeatures(int n_features, std::mt19937* rng) const {
    int max_feat = params_.max_features;
    if (max_feat < 0) max_feat = n_features;
    else if (max_feat == 0) max_feat = std::max(1, static_cast<int>(std::sqrt(n_features)));

    if (max_feat >= n_features || !rng) {
        std::vector<int> all(n_features);
        std::iota(all.begin(), all.end(), 0);
        return all;
    }

    std::vector<int> all(n_features);
    std::iota(all.begin(), all.end(), 0);
    std::shuffle(all.begin(), all.end(), *rng);
    all.resize(max_feat);
    return all;
}

double DecisionTree::mse(const std::vector<double>& y,
                          const std::vector<size_t>& indices) {
    if (indices.empty()) return 0.0;
    double sum = 0.0, sq = 0.0;
    for (auto i : indices) {
        sum += y[i];
        sq += y[i] * y[i];
    }
    double mean = sum / indices.size();
    return (sq / indices.size()) - mean * mean;
}

double DecisionTree::gini(const std::vector<int>& y,
                           const std::vector<double>& weights,
                           const std::vector<size_t>& indices,
                           int n_classes) {
    std::vector<double> counts(n_classes, 0.0);
    double total = 0.0;
    for (auto i : indices) {
        counts[y[i]] += weights[i];
        total += weights[i];
    }
    if (total < 1e-12) return 0.0;
    double g = 1.0;
    for (auto& c : counts) {
        double p = c / total;
        g -= p * p;
    }
    return g;
}

const DecisionTree::Node* DecisionTree::traverse(const Node* node,
                                                   const std::vector<double>& x) const {
    if (!node) return nullptr;
    if (node->isLeaf()) return node;
    if (x[node->feature] <= node->threshold)
        return traverse(node->left.get(), x);
    else
        return traverse(node->right.get(), x);
}

// ---------- Feature Importance ----------

std::vector<double> DecisionTree::featureImportances() const {
    std::vector<double> imp(n_features_, 0.0);
    if (root_) accumulateImportance(root_.get(), imp);
    double total = std::accumulate(imp.begin(), imp.end(), 0.0);
    if (total > 0) for (auto& v : imp) v /= total;
    return imp;
}

void DecisionTree::accumulateImportance(const Node* node,
                                         std::vector<double>& imp) const {
    if (!node || node->isLeaf()) return;
    if (node->feature >= 0 && node->feature < static_cast<int>(imp.size()))
        imp[node->feature] += node->impurity_decrease;
    accumulateImportance(node->left.get(), imp);
    accumulateImportance(node->right.get(), imp);
}

// ---------- Serialization ----------

nlohmann::json DecisionTree::toJson() const {
    nlohmann::json j;
    j["task"] = (task_ == TaskType::REGRESSION) ? "regression" : "classification";
    j["n_features"] = n_features_;
    j["n_classes"] = n_classes_;
    j["params"] = {
        {"max_depth", params_.max_depth},
        {"min_samples_split", params_.min_samples_split},
        {"min_samples_leaf", params_.min_samples_leaf},
        {"max_features", params_.max_features}
    };
    j["tree"] = nodeToJson(root_.get());
    return j;
}

DecisionTree DecisionTree::fromJson(const nlohmann::json& j) {
    DecisionTree dt;
    dt.task_ = (j.at("task").get<std::string>() == "regression")
                   ? TaskType::REGRESSION : TaskType::CLASSIFICATION;
    dt.n_features_ = j.at("n_features").get<int>();
    dt.n_classes_ = j.value("n_classes", 0);
    auto& p = j.at("params");
    dt.params_.max_depth = p.at("max_depth").get<int>();
    dt.params_.min_samples_split = p.at("min_samples_split").get<int>();
    dt.params_.min_samples_leaf = p.at("min_samples_leaf").get<int>();
    dt.params_.max_features = p.value("max_features", -1);
    dt.root_ = nodeFromJson(j.at("tree"));
    return dt;
}

nlohmann::json DecisionTree::nodeToJson(const Node* node) {
    if (!node) return nullptr;
    nlohmann::json j;
    j["f"] = node->feature;
    j["t"] = node->threshold;
    j["v"] = node->value;
    j["n"] = node->n_samples;
    j["pc"] = node->predicted_class;
    if (!node->class_proba.empty()) j["cp"] = node->class_proba;
    if (node->left) j["l"] = nodeToJson(node->left.get());
    if (node->right) j["r"] = nodeToJson(node->right.get());
    return j;
}

std::unique_ptr<DecisionTree::Node> DecisionTree::nodeFromJson(const nlohmann::json& j) {
    if (j.is_null()) return nullptr;
    auto node = std::make_unique<Node>();
    node->feature = j.at("f").get<int>();
    node->threshold = j.at("t").get<double>();
    node->value = j.at("v").get<double>();
    node->n_samples = j.value("n", 0);
    node->predicted_class = j.value("pc", -1);
    if (j.contains("cp")) node->class_proba = j.at("cp").get<std::vector<double>>();
    if (j.contains("l")) node->left = nodeFromJson(j.at("l"));
    if (j.contains("r")) node->right = nodeFromJson(j.at("r"));
    return node;
}

}  // namespace ml
}  // namespace hms_cpap
