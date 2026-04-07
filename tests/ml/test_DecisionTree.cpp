#include <gtest/gtest.h>
#include "ml/DecisionTree.h"
#include <cmath>
#include <numeric>
#include <random>

using namespace hms_cpap::ml;

TEST(DecisionTreeTest, RegressionSimple) {
    // y = x (single feature, linear)
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    for (int i = 0; i < 20; ++i) {
        double val = static_cast<double>(i);
        X.push_back({val});
        y.push_back(val);
    }

    DecisionTree::Params p;
    p.max_depth = 10;
    p.min_samples_leaf = 1;
    p.min_samples_split = 2;

    DecisionTree tree(p);
    tree.fitRegression(X, y);

    // Tree should approximate the identity function on training data
    for (int i = 0; i < 20; ++i) {
        double pred = tree.predict({static_cast<double>(i)});
        EXPECT_NEAR(pred, static_cast<double>(i), 2.0)
            << "Prediction at i=" << i << " was " << pred;
    }
}

TEST(DecisionTreeTest, ClassificationSimple) {
    // Two linearly separable classes: class 0 if x < 5, class 1 if x >= 5
    std::vector<std::vector<double>> X;
    std::vector<int> y;
    for (int i = 0; i < 10; ++i) {
        X.push_back({static_cast<double>(i)});
        y.push_back(i < 5 ? 0 : 1);
    }

    DecisionTree::Params p;
    p.max_depth = 5;
    p.min_samples_leaf = 1;
    p.min_samples_split = 2;

    DecisionTree tree(p);
    tree.fitClassification(X, y, 2);

    // Should classify training data correctly
    for (int i = 0; i < 10; ++i) {
        int pred = tree.predictClass({static_cast<double>(i)});
        int expected = i < 5 ? 0 : 1;
        EXPECT_EQ(pred, expected) << "Misclassified at i=" << i;
    }
}

TEST(DecisionTreeTest, MaxDepthLimit) {
    // With max_depth=1, tree can have at most 1 split (2 leaves)
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    for (int i = 0; i < 50; ++i) {
        X.push_back({static_cast<double>(i)});
        y.push_back(static_cast<double>(i));
    }

    DecisionTree::Params p;
    p.max_depth = 1;
    p.min_samples_leaf = 1;
    p.min_samples_split = 2;

    DecisionTree tree(p);
    tree.fitRegression(X, y);

    // With depth 1, the tree can produce at most 2 distinct predictions
    std::set<double> predictions;
    for (int i = 0; i < 50; ++i) {
        predictions.insert(tree.predict({static_cast<double>(i)}));
    }
    EXPECT_LE(predictions.size(), 2u);
}

TEST(DecisionTreeTest, MinSamplesLeaf) {
    // min_samples_leaf = 50 on 20 samples => no split possible, single node
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    for (int i = 0; i < 20; ++i) {
        X.push_back({static_cast<double>(i)});
        y.push_back(static_cast<double>(i));
    }

    DecisionTree::Params p;
    p.max_depth = 10;
    p.min_samples_leaf = 50;  // larger than dataset
    p.min_samples_split = 2;

    DecisionTree tree(p);
    tree.fitRegression(X, y);

    // All predictions should be the same (mean of all y)
    double mean_y = 9.5;  // mean of 0..19
    for (int i = 0; i < 20; ++i) {
        EXPECT_NEAR(tree.predict({static_cast<double>(i)}), mean_y, 1e-10);
    }
}

TEST(DecisionTreeTest, FeatureImportance) {
    // 2 features: y depends only on feature 0
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> noise(-0.01, 0.01);

    for (int i = 0; i < 30; ++i) {
        double x0 = static_cast<double>(i);
        double x1 = noise(rng);  // irrelevant feature
        X.push_back({x0, x1});
        y.push_back(x0);
    }

    DecisionTree::Params p;
    p.max_depth = 6;
    p.min_samples_leaf = 1;
    p.min_samples_split = 2;

    DecisionTree tree(p);
    tree.fitRegression(X, y);

    auto imp = tree.featureImportances();
    ASSERT_EQ(imp.size(), 2u);

    // Feature 0 should have the dominant importance
    EXPECT_GT(imp[0], 0.5);
}

TEST(DecisionTreeTest, JsonRoundTrip) {
    // Build a regression tree
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    for (int i = 0; i < 30; ++i) {
        double val = static_cast<double>(i);
        X.push_back({val, val * 2});
        y.push_back(val + 1);
    }

    DecisionTree::Params p;
    p.max_depth = 4;
    p.min_samples_leaf = 2;
    p.min_samples_split = 4;

    DecisionTree tree(p);
    tree.fitRegression(X, y);

    // Serialize
    auto json = tree.toJson();

    // Deserialize
    auto restored = DecisionTree::fromJson(json);

    // Predictions should match exactly
    for (int i = 0; i < 30; ++i) {
        double orig = tree.predict({static_cast<double>(i), static_cast<double>(i * 2)});
        double rest = restored.predict({static_cast<double>(i), static_cast<double>(i * 2)});
        EXPECT_DOUBLE_EQ(orig, rest) << "Mismatch at i=" << i;
    }
}

TEST(DecisionTreeTest, ClassificationJsonRoundTrip) {
    std::vector<std::vector<double>> X;
    std::vector<int> y;
    for (int i = 0; i < 20; ++i) {
        X.push_back({static_cast<double>(i)});
        y.push_back(i < 10 ? 0 : 1);
    }

    DecisionTree::Params p;
    p.max_depth = 4;
    p.min_samples_leaf = 1;

    DecisionTree tree(p);
    tree.fitClassification(X, y, 2);

    auto json = tree.toJson();
    auto restored = DecisionTree::fromJson(json);

    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(tree.predictClass({static_cast<double>(i)}),
                  restored.predictClass({static_cast<double>(i)}));
    }
}

TEST(DecisionTreeTest, PredictProba) {
    std::vector<std::vector<double>> X;
    std::vector<int> y;
    for (int i = 0; i < 20; ++i) {
        X.push_back({static_cast<double>(i)});
        y.push_back(i < 10 ? 0 : 1);
    }

    DecisionTree::Params p;
    p.max_depth = 4;
    p.min_samples_leaf = 1;

    DecisionTree tree(p);
    tree.fitClassification(X, y, 2);

    auto proba = tree.predictProba({0.0});
    ASSERT_EQ(proba.size(), 2u);

    double sum = proba[0] + proba[1];
    EXPECT_NEAR(sum, 1.0, 1e-10);

    // Point clearly in class 0 territory
    EXPECT_GT(proba[0], 0.5);
}
