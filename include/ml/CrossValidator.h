#pragma once

#include "ml/RandomForest.h"
#include "ml/StandardScaler.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <vector>

namespace hms_cpap {
namespace ml {

/**
 * CrossValidator - K-fold cross-validation for RandomForest.
 */
class CrossValidator {
public:
    struct RegressionResult {
        double r2_mean = 0, r2_std = 0;
        double mae_mean = 0, mae_std = 0;
    };

    struct ClassificationResult {
        double accuracy_mean = 0, accuracy_std = 0;
        double f1_mean = 0, f1_std = 0;
    };

    static RegressionResult cvRegression(
            const std::vector<std::vector<double>>& X,
            const std::vector<double>& y,
            const RandomForest::Params& params,
            int k = 5, int seed = 42) {

        auto folds = kfoldSplit(X.size(), k, seed);
        std::vector<double> r2s, maes;

        for (int fold = 0; fold < k; ++fold) {
            std::vector<std::vector<double>> X_train, X_test;
            std::vector<double> y_train, y_test;

            for (int f = 0; f < k; ++f) {
                for (auto idx : folds[f]) {
                    if (f == fold) {
                        X_test.push_back(X[idx]);
                        y_test.push_back(y[idx]);
                    } else {
                        X_train.push_back(X[idx]);
                        y_train.push_back(y[idx]);
                    }
                }
            }

            StandardScaler scaler;
            auto X_train_s = scaler.fitTransform(X_train);
            auto X_test_s = scaler.transform(X_test);

            RandomForest rf(params);
            rf.fitRegression(X_train_s, y_train);

            // Score
            double ss_res = 0, ss_tot = 0, mae_sum = 0;
            double y_mean = std::accumulate(y_test.begin(), y_test.end(), 0.0) / y_test.size();
            for (size_t i = 0; i < y_test.size(); ++i) {
                double pred = rf.predict(X_test_s[i]);
                ss_res += (y_test[i] - pred) * (y_test[i] - pred);
                ss_tot += (y_test[i] - y_mean) * (y_test[i] - y_mean);
                mae_sum += std::abs(y_test[i] - pred);
            }
            r2s.push_back(ss_tot > 0 ? 1.0 - ss_res / ss_tot : 0.0);
            maes.push_back(mae_sum / y_test.size());
        }

        RegressionResult res;
        res.r2_mean = mean(r2s);
        res.r2_std = stddev(r2s);
        res.mae_mean = mean(maes);
        res.mae_std = stddev(maes);
        return res;
    }

    static ClassificationResult cvClassification(
            const std::vector<std::vector<double>>& X,
            const std::vector<int>& y,
            int n_classes,
            const RandomForest::Params& params,
            int k = 5, int seed = 42) {

        auto folds = kfoldSplit(X.size(), k, seed);
        std::vector<double> accs, f1s;

        for (int fold = 0; fold < k; ++fold) {
            std::vector<std::vector<double>> X_train, X_test;
            std::vector<int> y_train, y_test;

            for (int f = 0; f < k; ++f) {
                for (auto idx : folds[f]) {
                    if (f == fold) {
                        X_test.push_back(X[idx]);
                        y_test.push_back(y[idx]);
                    } else {
                        X_train.push_back(X[idx]);
                        y_train.push_back(y[idx]);
                    }
                }
            }

            StandardScaler scaler;
            auto X_train_s = scaler.fitTransform(X_train);
            auto X_test_s = scaler.transform(X_test);

            RandomForest rf(params);
            rf.fitClassification(X_train_s, y_train, n_classes);

            // Score
            int correct = 0;
            std::vector<int> tp(n_classes, 0), fp(n_classes, 0), fn(n_classes, 0);
            for (size_t i = 0; i < y_test.size(); ++i) {
                int pred = rf.predictClass(X_test_s[i]);
                if (pred == y_test[i]) {
                    ++correct;
                    ++tp[pred];
                } else {
                    ++fp[pred];
                    ++fn[y_test[i]];
                }
            }
            accs.push_back(static_cast<double>(correct) / y_test.size());

            // Weighted F1
            double f1_weighted = 0.0;
            int total = static_cast<int>(y_test.size());
            for (int c = 0; c < n_classes; ++c) {
                double prec = (tp[c] + fp[c] > 0) ? static_cast<double>(tp[c]) / (tp[c] + fp[c]) : 0;
                double rec = (tp[c] + fn[c] > 0) ? static_cast<double>(tp[c]) / (tp[c] + fn[c]) : 0;
                double f1 = (prec + rec > 0) ? 2.0 * prec * rec / (prec + rec) : 0;
                int support = tp[c] + fn[c];
                f1_weighted += f1 * support;
            }
            f1s.push_back(total > 0 ? f1_weighted / total : 0.0);
        }

        ClassificationResult res;
        res.accuracy_mean = mean(accs);
        res.accuracy_std = stddev(accs);
        res.f1_mean = mean(f1s);
        res.f1_std = stddev(f1s);
        return res;
    }

private:
    static std::vector<std::vector<size_t>> kfoldSplit(size_t n, int k, int seed) {
        std::vector<size_t> indices(n);
        std::iota(indices.begin(), indices.end(), 0);
        std::mt19937 rng(seed);
        std::shuffle(indices.begin(), indices.end(), rng);

        std::vector<std::vector<size_t>> folds(k);
        for (size_t i = 0; i < n; ++i)
            folds[i % k].push_back(indices[i]);
        return folds;
    }

    static double mean(const std::vector<double>& v) {
        return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    }

    static double stddev(const std::vector<double>& v) {
        double m = mean(v);
        double sq = 0;
        for (auto x : v) sq += (x - m) * (x - m);
        return std::sqrt(sq / v.size());
    }
};

}  // namespace ml
}  // namespace hms_cpap
