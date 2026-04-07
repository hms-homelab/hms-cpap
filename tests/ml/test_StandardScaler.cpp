#include <gtest/gtest.h>
#include "ml/StandardScaler.h"
#include <cmath>

using namespace hms_cpap::ml;

TEST(StandardScalerTest, FitTransform) {
    StandardScaler scaler;
    std::vector<std::vector<double>> X = {{1, 2}, {3, 4}, {5, 6}};
    scaler.fit(X);

    EXPECT_TRUE(scaler.isFitted());

    // mean should be [3, 4]
    EXPECT_DOUBLE_EQ(scaler.mean()[0], 3.0);
    EXPECT_DOUBLE_EQ(scaler.mean()[1], 4.0);

    // population std: sqrt(((1-3)^2 + (3-3)^2 + (5-3)^2) / 3) = sqrt(8/3)
    double expected_std = std::sqrt(8.0 / 3.0);
    EXPECT_NEAR(scaler.stddev()[0], expected_std, 1e-10);
    EXPECT_NEAR(scaler.stddev()[1], expected_std, 1e-10);

    auto transformed = scaler.transform(X);
    ASSERT_EQ(transformed.size(), 3u);

    // Row [3, 4] (the mean) should transform to [0, 0]
    EXPECT_NEAR(transformed[1][0], 0.0, 1e-10);
    EXPECT_NEAR(transformed[1][1], 0.0, 1e-10);

    // Row [1, 2]: (1-3)/std, (2-4)/std  => -2/std
    EXPECT_NEAR(transformed[0][0], -2.0 / expected_std, 1e-10);
    EXPECT_NEAR(transformed[0][1], -2.0 / expected_std, 1e-10);

    // Row [5, 6]: (5-3)/std, (6-4)/std  => +2/std
    EXPECT_NEAR(transformed[2][0], 2.0 / expected_std, 1e-10);
    EXPECT_NEAR(transformed[2][1], 2.0 / expected_std, 1e-10);
}

TEST(StandardScalerTest, TransformRow) {
    StandardScaler scaler;
    std::vector<std::vector<double>> X = {{1, 2}, {3, 4}, {5, 6}};
    scaler.fit(X);

    auto row = scaler.transformRow({3, 4});
    ASSERT_EQ(row.size(), 2u);
    EXPECT_NEAR(row[0], 0.0, 1e-10);
    EXPECT_NEAR(row[1], 0.0, 1e-10);

    // Verify transformRow matches transform for a single row
    auto full = scaler.transform({{7, 8}});
    auto single = scaler.transformRow({7, 8});
    EXPECT_NEAR(full[0][0], single[0], 1e-10);
    EXPECT_NEAR(full[0][1], single[1], 1e-10);
}

TEST(StandardScalerTest, EmptyInput) {
    StandardScaler scaler;
    std::vector<std::vector<double>> empty;
    scaler.fit(empty);

    // fit on empty should not crash, scaler should remain unfitted
    EXPECT_FALSE(scaler.isFitted());
}

TEST(StandardScalerTest, NotFitted) {
    StandardScaler scaler;
    EXPECT_FALSE(scaler.isFitted());

    std::vector<std::vector<double>> X = {{1, 2}};
    EXPECT_THROW(scaler.transform(X), std::runtime_error);
    EXPECT_THROW(scaler.transformRow({1, 2}), std::runtime_error);
}

TEST(StandardScalerTest, JsonRoundTrip) {
    StandardScaler scaler;
    std::vector<std::vector<double>> X = {{1, 10}, {2, 20}, {3, 30}, {4, 40}};
    scaler.fit(X);

    auto json = scaler.toJson();
    auto restored = StandardScaler::fromJson(json);

    EXPECT_TRUE(restored.isFitted());
    ASSERT_EQ(restored.mean().size(), scaler.mean().size());
    ASSERT_EQ(restored.stddev().size(), scaler.stddev().size());

    for (size_t i = 0; i < scaler.mean().size(); ++i) {
        EXPECT_DOUBLE_EQ(restored.mean()[i], scaler.mean()[i]);
        EXPECT_DOUBLE_EQ(restored.stddev()[i], scaler.stddev()[i]);
    }

    // Verify transform produces same results
    std::vector<double> test_row = {2.5, 25.0};
    auto orig = scaler.transformRow(test_row);
    auto rest = restored.transformRow(test_row);
    EXPECT_NEAR(orig[0], rest[0], 1e-10);
    EXPECT_NEAR(orig[1], rest[1], 1e-10);
}

TEST(StandardScalerTest, FitTransformIdentical) {
    std::vector<std::vector<double>> X = {{1, 2}, {3, 4}, {5, 6}, {7, 8}};

    // Method 1: fit then transform
    StandardScaler s1;
    s1.fit(X);
    auto result1 = s1.transform(X);

    // Method 2: fitTransform
    StandardScaler s2;
    auto result2 = s2.fitTransform(X);

    ASSERT_EQ(result1.size(), result2.size());
    for (size_t i = 0; i < result1.size(); ++i) {
        ASSERT_EQ(result1[i].size(), result2[i].size());
        for (size_t j = 0; j < result1[i].size(); ++j) {
            EXPECT_DOUBLE_EQ(result1[i][j], result2[i][j]);
        }
    }
}

TEST(StandardScalerTest, ConstantFeatureHandled) {
    // All values the same in one feature => std would be 0, should use 1.0 fallback
    StandardScaler scaler;
    std::vector<std::vector<double>> X = {{5, 1}, {5, 2}, {5, 3}};
    scaler.fit(X);

    // First feature is constant => std should be clamped to 1.0
    EXPECT_DOUBLE_EQ(scaler.stddev()[0], 1.0);

    // Transform should not divide by zero
    auto t = scaler.transformRow({5, 2});
    EXPECT_NEAR(t[0], 0.0, 1e-10);  // (5 - 5) / 1.0 = 0
}
