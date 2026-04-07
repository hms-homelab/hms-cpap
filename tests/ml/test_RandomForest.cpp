#include <gtest/gtest.h>
#include "ml/RandomForest.h"
#include <cmath>
#include <numeric>
#include <random>

using namespace hms_cpap::ml;

TEST(RandomForestTest, RegressionBasic) {
    // y = 2*x1 + x2 + noise
    std::mt19937 rng(123);
    std::normal_distribution<double> noise(0.0, 0.5);

    std::vector<std::vector<double>> X;
    std::vector<double> y;
    for (int i = 0; i < 50; ++i) {
        double x1 = static_cast<double>(i) / 10.0;
        double x2 = static_cast<double>(i % 5);
        X.push_back({x1, x2});
        y.push_back(2.0 * x1 + x2 + noise(rng));
    }

    RandomForest::Params p;
    p.n_estimators = 30;
    p.max_depth = 6;
    p.min_samples_leaf = 2;
    p.random_seed = 42;

    RandomForest rf(p);
    rf.fitRegression(X, y);

    EXPECT_TRUE(rf.isTrained());
    EXPECT_EQ(rf.numFeatures(), 2);

    // Compute R2 on training data
    double y_mean = std::accumulate(y.begin(), y.end(), 0.0) / y.size();
    double ss_res = 0.0, ss_tot = 0.0;
    for (size_t i = 0; i < X.size(); ++i) {
        double pred = rf.predict(X[i]);
        ss_res += (y[i] - pred) * (y[i] - pred);
        ss_tot += (y[i] - y_mean) * (y[i] - y_mean);
    }
    double r2 = 1.0 - ss_res / ss_tot;
    EXPECT_GT(r2, 0.5);
}

TEST(RandomForestTest, ClassificationBasic) {
    // 2 classes linearly separable: class 0 if x1 < 5, class 1 otherwise
    std::vector<std::vector<double>> X;
    std::vector<int> y;
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0.0, 0.3);

    for (int i = 0; i < 40; ++i) {
        double x1 = static_cast<double>(i) / 4.0 + noise(rng);
        double x2 = noise(rng);  // irrelevant
        X.push_back({x1, x2});
        y.push_back(i < 20 ? 0 : 1);
    }

    RandomForest::Params p;
    p.n_estimators = 30;
    p.max_depth = 5;
    p.random_seed = 42;

    RandomForest rf(p);
    rf.fitClassification(X, y, 2);

    EXPECT_TRUE(rf.isTrained());
    EXPECT_EQ(rf.numClasses(), 2);

    // Check accuracy on training data
    int correct = 0;
    for (size_t i = 0; i < X.size(); ++i) {
        if (rf.predictClass(X[i]) == y[i]) ++correct;
    }
    double accuracy = static_cast<double>(correct) / X.size();
    EXPECT_GT(accuracy, 0.80);
}

TEST(RandomForestTest, BalancedWeights) {
    // Imbalanced data: 90% class 0, 10% class 1
    std::vector<std::vector<double>> X;
    std::vector<int> y;
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0.0, 0.1);

    // Class 0: x1 in [0, 5)
    for (int i = 0; i < 45; ++i) {
        X.push_back({static_cast<double>(i % 5) + noise(rng), noise(rng)});
        y.push_back(0);
    }
    // Class 1: x1 in [7, 12)
    for (int i = 0; i < 5; ++i) {
        X.push_back({7.0 + static_cast<double>(i) + noise(rng), noise(rng)});
        y.push_back(1);
    }

    RandomForest::Params p;
    p.n_estimators = 50;
    p.max_depth = 5;
    p.class_weight_balanced = true;
    p.random_seed = 42;

    RandomForest rf(p);
    rf.fitClassification(X, y, 2);

    // Minority class (1) should have at least some correct predictions
    int minority_correct = 0;
    int minority_total = 0;
    for (size_t i = 0; i < X.size(); ++i) {
        if (y[i] == 1) {
            ++minority_total;
            if (rf.predictClass(X[i]) == 1) ++minority_correct;
        }
    }
    EXPECT_GT(minority_correct, 0)
        << "Balanced weights should help detect minority class";
}

TEST(RandomForestTest, FeatureImportance) {
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    std::mt19937 rng(42);

    for (int i = 0; i < 50; ++i) {
        double x1 = static_cast<double>(i);
        double x2 = static_cast<double>(rng() % 100) * 0.001;  // noise
        X.push_back({x1, x2});
        y.push_back(x1 * 2.0);
    }

    RandomForest::Params p;
    p.n_estimators = 20;
    p.max_depth = 5;
    p.random_seed = 42;

    RandomForest rf(p);
    rf.fitRegression(X, y);

    auto imp = rf.featureImportances();
    ASSERT_EQ(imp.size(), 2u);

    double total = imp[0] + imp[1];
    EXPECT_NEAR(total, 1.0, 0.05);
}

TEST(RandomForestTest, PredictProba) {
    std::vector<std::vector<double>> X;
    std::vector<int> y;
    for (int i = 0; i < 20; ++i) {
        X.push_back({static_cast<double>(i)});
        y.push_back(i < 10 ? 0 : 1);
    }

    RandomForest::Params p;
    p.n_estimators = 20;
    p.max_depth = 4;
    p.random_seed = 42;

    RandomForest rf(p);
    rf.fitClassification(X, y, 2);

    auto proba = rf.predictProba({0.0});
    ASSERT_EQ(proba.size(), 2u);

    double sum = proba[0] + proba[1];
    EXPECT_NEAR(sum, 1.0, 1e-10);

    // All probabilities should be non-negative
    EXPECT_GE(proba[0], 0.0);
    EXPECT_GE(proba[1], 0.0);
}

TEST(RandomForestTest, JsonRoundTrip) {
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    for (int i = 0; i < 30; ++i) {
        X.push_back({static_cast<double>(i), static_cast<double>(i * 2)});
        y.push_back(static_cast<double>(i) + 1.0);
    }

    RandomForest::Params p;
    p.n_estimators = 10;
    p.max_depth = 4;
    p.random_seed = 42;

    RandomForest rf(p);
    rf.fitRegression(X, y);

    auto json = rf.toJson();
    auto restored = RandomForest::fromJson(json);

    EXPECT_TRUE(restored.isTrained());
    EXPECT_EQ(restored.numFeatures(), rf.numFeatures());

    // Predictions should match exactly
    for (int i = 0; i < 30; ++i) {
        std::vector<double> row = {static_cast<double>(i), static_cast<double>(i * 2)};
        double orig = rf.predict(row);
        double rest = restored.predict(row);
        EXPECT_DOUBLE_EQ(orig, rest) << "Mismatch at i=" << i;
    }
}

TEST(RandomForestTest, NotTrained) {
    RandomForest rf;
    EXPECT_FALSE(rf.isTrained());

    // predict on untrained should return 0
    double pred = rf.predict({1.0, 2.0});
    EXPECT_DOUBLE_EQ(pred, 0.0);
}

TEST(RandomForestTest, ClassificationJsonRoundTrip) {
    std::vector<std::vector<double>> X;
    std::vector<int> y;
    for (int i = 0; i < 30; ++i) {
        X.push_back({static_cast<double>(i), static_cast<double>(i % 3)});
        y.push_back(i < 15 ? 0 : 1);
    }

    RandomForest::Params p;
    p.n_estimators = 10;
    p.max_depth = 4;
    p.random_seed = 42;

    RandomForest rf(p);
    rf.fitClassification(X, y, 2);

    auto json = rf.toJson();
    auto restored = RandomForest::fromJson(json);

    EXPECT_EQ(restored.numClasses(), 2);

    for (int i = 0; i < 30; ++i) {
        std::vector<double> row = {static_cast<double>(i), static_cast<double>(i % 3)};
        EXPECT_EQ(rf.predictClass(row), restored.predictClass(row));
    }
}
