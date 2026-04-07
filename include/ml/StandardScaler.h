#pragma once

#include <nlohmann/json.hpp>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace hms_cpap {
namespace ml {

/**
 * StandardScaler - Per-feature mean/std normalization.
 * Mirrors sklearn.preprocessing.StandardScaler.
 */
class StandardScaler {
public:
    void fit(const std::vector<std::vector<double>>& X) {
        if (X.empty()) return;
        size_t n_features = X[0].size();
        size_t n_samples = X.size();

        mean_.assign(n_features, 0.0);
        std_.assign(n_features, 0.0);
        fitted_ = true;

        for (const auto& row : X)
            for (size_t j = 0; j < n_features; ++j)
                mean_[j] += row[j];

        for (auto& m : mean_) m /= static_cast<double>(n_samples);

        for (const auto& row : X)
            for (size_t j = 0; j < n_features; ++j) {
                double d = row[j] - mean_[j];
                std_[j] += d * d;
            }

        for (auto& s : std_) {
            s = std::sqrt(s / static_cast<double>(n_samples));
            if (s < 1e-12) s = 1.0;  // avoid division by zero
        }
    }

    std::vector<std::vector<double>> transform(
            const std::vector<std::vector<double>>& X) const {
        if (!fitted_) throw std::runtime_error("StandardScaler not fitted");
        std::vector<std::vector<double>> result;
        result.reserve(X.size());
        for (const auto& row : X) {
            std::vector<double> scaled(row.size());
            for (size_t j = 0; j < row.size(); ++j)
                scaled[j] = (row[j] - mean_[j]) / std_[j];
            result.push_back(std::move(scaled));
        }
        return result;
    }

    std::vector<double> transformRow(const std::vector<double>& row) const {
        if (!fitted_) throw std::runtime_error("StandardScaler not fitted");
        std::vector<double> scaled(row.size());
        for (size_t j = 0; j < row.size(); ++j)
            scaled[j] = (row[j] - mean_[j]) / std_[j];
        return scaled;
    }

    std::vector<std::vector<double>> fitTransform(
            const std::vector<std::vector<double>>& X) {
        fit(X);
        return transform(X);
    }

    bool isFitted() const { return fitted_; }
    const std::vector<double>& mean() const { return mean_; }
    const std::vector<double>& stddev() const { return std_; }

    nlohmann::json toJson() const {
        return {{"mean", mean_}, {"std", std_}, {"fitted", fitted_}};
    }

    static StandardScaler fromJson(const nlohmann::json& j) {
        StandardScaler s;
        s.mean_ = j.at("mean").get<std::vector<double>>();
        s.std_ = j.at("std").get<std::vector<double>>();
        s.fitted_ = j.value("fitted", true);
        return s;
    }

private:
    std::vector<double> mean_;
    std::vector<double> std_;
    bool fitted_ = false;
};

}  // namespace ml
}  // namespace hms_cpap
