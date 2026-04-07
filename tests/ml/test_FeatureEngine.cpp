#include <gtest/gtest.h>
#include "ml/FeatureEngine.h"
#include <cmath>

using namespace hms_cpap::ml;

TEST(FeatureEngineTest, RollingMean) {
    std::vector<double> vals = {1, 2, 3, 4, 5};
    auto result = FeatureEngine::rollingMean(vals, 3);

    ASSERT_EQ(result.size(), 5u);

    // i=0: count=1 (<3) => 0
    EXPECT_DOUBLE_EQ(result[0], 0.0);
    // i=1: count=2 (<3) => 0
    EXPECT_DOUBLE_EQ(result[1], 0.0);
    // i=2: window [1,2,3], count=3, mean = 2.0
    EXPECT_NEAR(result[2], 2.0, 1e-10);
    // i=3: window [2,3,4], count=3, mean = 3.0
    EXPECT_NEAR(result[3], 3.0, 1e-10);
    // i=4: window [3,4,5], count=3, mean = 4.0
    EXPECT_NEAR(result[4], 4.0, 1e-10);
}

TEST(FeatureEngineTest, RollingStd) {
    std::vector<double> vals = {1, 1, 1, 1, 1};
    auto result = FeatureEngine::rollingStd(vals, 3);

    ASSERT_EQ(result.size(), 5u);

    // i=0,1: count < 3 => 0
    EXPECT_DOUBLE_EQ(result[0], 0.0);
    EXPECT_DOUBLE_EQ(result[1], 0.0);

    // Constant values => std = 0
    EXPECT_DOUBLE_EQ(result[2], 0.0);
    EXPECT_DOUBLE_EQ(result[3], 0.0);
    EXPECT_DOUBLE_EQ(result[4], 0.0);

    // Now test with varying values
    std::vector<double> vals2 = {10, 20, 30, 40, 50};
    auto result2 = FeatureEngine::rollingStd(vals2, 3);

    // i=2: window [10,20,30], mean=20, var = ((100+0+100)/3) = 66.67, std ~= 8.165
    double expected = std::sqrt((100.0 + 0.0 + 100.0) / 3.0);
    EXPECT_NEAR(result2[2], expected, 1e-6);

    // i=3: window [20,30,40], mean=30, same std
    EXPECT_NEAR(result2[3], expected, 1e-6);
}

TEST(FeatureEngineTest, RollingMax) {
    std::vector<double> vals = {5, 3, 8, 1, 9};
    auto result = FeatureEngine::rollingMax(vals, 3);

    ASSERT_EQ(result.size(), 5u);

    // i=0,1: count < 3 => 0
    EXPECT_DOUBLE_EQ(result[0], 0.0);
    EXPECT_DOUBLE_EQ(result[1], 0.0);

    // i=2: window [5,3,8] => max=8
    EXPECT_DOUBLE_EQ(result[2], 8.0);
    // i=3: window [3,8,1] => max=8
    EXPECT_DOUBLE_EQ(result[3], 8.0);
    // i=4: window [8,1,9] => max=9
    EXPECT_DOUBLE_EQ(result[4], 9.0);
}

TEST(FeatureEngineTest, TrendSlopeConstant) {
    // Constant values => slope ~0
    std::vector<double> vals = {5, 5, 5, 5, 5};
    auto result = FeatureEngine::trendSlope(vals, 3);

    ASSERT_EQ(result.size(), 5u);
    // i=0,1: i < window-1 => 0
    EXPECT_DOUBLE_EQ(result[0], 0.0);
    EXPECT_DOUBLE_EQ(result[1], 0.0);

    // Constant => slope = 0
    EXPECT_NEAR(result[2], 0.0, 1e-10);
    EXPECT_NEAR(result[3], 0.0, 1e-10);
    EXPECT_NEAR(result[4], 0.0, 1e-10);
}

TEST(FeatureEngineTest, TrendSlopeIncreasing) {
    // Linearly increasing: y = x
    std::vector<double> vals = {0, 1, 2, 3, 4};
    auto result = FeatureEngine::trendSlope(vals, 3);

    // i=2: window [0,1,2], x=[0,1,2], y=[0,1,2] => slope = 1.0
    EXPECT_NEAR(result[2], 1.0, 1e-10);
    EXPECT_NEAR(result[3], 1.0, 1e-10);
    EXPECT_NEAR(result[4], 1.0, 1e-10);
}

TEST(FeatureEngineTest, ZScore) {
    std::vector<double> vals = {10, 20, 30};
    auto result = FeatureEngine::zScore(vals);

    ASSERT_EQ(result.size(), 3u);

    // mean = 20, std = sqrt(((100+0+100)/3)) = sqrt(200/3)
    double mean = 20.0;
    double std_dev = std::sqrt(200.0 / 3.0);

    EXPECT_NEAR(result[0], (10 - mean) / std_dev, 1e-10);
    EXPECT_NEAR(result[1], (20 - mean) / std_dev, 1e-10);
    EXPECT_NEAR(result[2], (30 - mean) / std_dev, 1e-10);

    // Middle value z-score should be 0
    EXPECT_NEAR(result[1], 0.0, 1e-10);
}

TEST(FeatureEngineTest, ZScoreEmpty) {
    std::vector<double> vals;
    auto result = FeatureEngine::zScore(vals);
    EXPECT_TRUE(result.empty());
}

TEST(FeatureEngineTest, ZScoreConstant) {
    // All same values => std = 0 => should return all zeros
    std::vector<double> vals = {5, 5, 5, 5};
    auto result = FeatureEngine::zScore(vals);
    for (auto v : result) {
        EXPECT_DOUBLE_EQ(v, 0.0);
    }
}

static DailyRecord makeRecord(int day, double ahi, double leak, double duration,
                                int dow = 0) {
    DailyRecord r;
    r.record_date = "2026-03-" + std::to_string(day);
    r.ahi = ahi;
    r.hi = ahi * 0.5;
    r.oai = ahi * 0.3;
    r.cai = ahi * 0.2;
    r.duration_minutes = duration;
    r.mask_press_50 = 8.0;
    r.mask_press_95 = 10.0;
    r.mask_press_max = 12.0;
    r.leak_50 = leak * 0.6;
    r.leak_95 = leak;
    r.leak_max = leak * 1.5;
    r.resp_rate_50 = 14.0;
    r.tid_vol_50 = 500.0;
    r.min_vent_50 = 7.0;
    r.day_of_week = dow;
    return r;
}

TEST(FeatureEngineTest, BuildBasic) {
    // 30 records with varying AHI and leak
    std::vector<DailyRecord> records;
    for (int i = 1; i <= 30; ++i) {
        double ahi = 2.0 + (i % 5) * 0.5;
        double leak = 10.0 + (i % 3) * 3.0;
        double dur = 400.0 + (i % 7) * 10.0;
        records.push_back(makeRecord(i, ahi, leak, dur, i % 7));
    }

    auto result = FeatureEngine::build(records);

    // Should have 29 rows (n - 1)
    EXPECT_EQ(result.X.size(), 29u);

    // All rows should have same number of features
    size_t n_features = result.feature_names.size();
    EXPECT_GT(n_features, 0u);

    for (size_t i = 0; i < result.X.size(); ++i) {
        EXPECT_EQ(result.X[i].size(), n_features)
            << "Row " << i << " has wrong number of features";
    }

    // Targets should have 29 entries
    EXPECT_EQ(result.target_ahi.size(), 29u);
    EXPECT_EQ(result.target_hours.size(), 29u);
    EXPECT_EQ(result.target_high_leak.size(), 29u);
    EXPECT_EQ(result.target_anomaly.size(), 29u);

    // Anomaly labels should be present
    EXPECT_EQ(result.anomaly_labels.size(), 4u);
}

TEST(FeatureEngineTest, BuildMinimum) {
    // Less than 8 records should return empty
    std::vector<DailyRecord> records;
    for (int i = 1; i <= 7; ++i) {
        records.push_back(makeRecord(i, 2.0, 10.0, 400.0));
    }

    auto result = FeatureEngine::build(records);

    EXPECT_TRUE(result.X.empty());
    EXPECT_TRUE(result.target_ahi.empty());
    EXPECT_TRUE(result.feature_names.empty());
}

TEST(FeatureEngineTest, BuildExactlyEight) {
    // Exactly 8 records should work (>= 8 check)
    std::vector<DailyRecord> records;
    for (int i = 1; i <= 8; ++i) {
        records.push_back(makeRecord(i, 2.0 + i * 0.1, 10.0 + i, 400.0, i % 7));
    }

    auto result = FeatureEngine::build(records);

    EXPECT_EQ(result.X.size(), 7u);  // 8 - 1 = 7
    EXPECT_FALSE(result.feature_names.empty());
}

TEST(FeatureEngineTest, TargetsCorrect) {
    // Verify target_ahi[i] == records[i+1].ahi
    std::vector<DailyRecord> records;
    for (int i = 1; i <= 15; ++i) {
        double ahi = static_cast<double>(i);
        records.push_back(makeRecord(i, ahi, 10.0, 420.0));
    }

    auto result = FeatureEngine::build(records);

    for (size_t i = 0; i < result.target_ahi.size(); ++i) {
        EXPECT_DOUBLE_EQ(result.target_ahi[i], records[i + 1].ahi)
            << "target_ahi mismatch at i=" << i;
    }

    // target_hours should be next-day duration / 60
    for (size_t i = 0; i < result.target_hours.size(); ++i) {
        EXPECT_DOUBLE_EQ(result.target_hours[i], records[i + 1].duration_minutes / 60.0)
            << "target_hours mismatch at i=" << i;
    }
}

TEST(FeatureEngineTest, HighLeakTarget) {
    // Verify high_leak threshold labeling
    std::vector<DailyRecord> records;
    for (int i = 1; i <= 10; ++i) {
        // Alternate leak above and below threshold (24.0)
        double leak = (i % 2 == 0) ? 30.0 : 10.0;
        records.push_back(makeRecord(i, 2.0, leak, 400.0));
    }

    auto result = FeatureEngine::build(records);

    for (size_t i = 0; i < result.target_high_leak.size(); ++i) {
        int expected = (records[i + 1].leak_95 > FeatureEngine::HIGH_LEAK_THRESHOLD) ? 1 : 0;
        EXPECT_EQ(result.target_high_leak[i], expected)
            << "high_leak target mismatch at i=" << i;
    }
}
