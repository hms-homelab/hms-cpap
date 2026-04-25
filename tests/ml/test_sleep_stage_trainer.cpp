#include <gtest/gtest.h>
#include "ml/SleepStageTrainer.h"
#include "ml/SleepStageTypes.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace hms_cpap::ml;

class SleepStageTrainerTest : public ::testing::Test {
protected:
    std::filesystem::path temp_dir_;

    void SetUp() override {
        temp_dir_ = std::filesystem::temp_directory_path() / "hms_cpap_trainer_test";
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        if (std::filesystem::exists(temp_dir_)) {
            std::filesystem::remove_all(temp_dir_);
        }
    }

    static void generateSeparableData(
            std::vector<std::vector<double>>& X,
            std::vector<int>& y,
            int samples_per_class = 40,
            int n_features = CAUSAL_FEATURE_COUNT,
            int seed = 42) {
        std::mt19937 rng(seed);
        std::normal_distribution<double> noise(0.0, 0.3);

        double centers[4][2] = {
            {-3.0, -3.0}, { 3.0, -3.0},
            {-3.0,  3.0}, { 3.0,  3.0}
        };

        X.clear();
        y.clear();

        for (int cls = 0; cls < 4; ++cls) {
            for (int i = 0; i < samples_per_class; ++i) {
                std::vector<double> row(n_features, 0.0);
                row[0] = centers[cls][0] + noise(rng);
                row[1] = centers[cls][1] + noise(rng);
                for (int f = 2; f < n_features; ++f)
                    row[f] = noise(rng) * 0.1;
                X.push_back(std::move(row));
                y.push_back(cls);
            }
        }
    }

    void writeCSV(const std::filesystem::path& path,
                  const std::vector<std::vector<double>>& X,
                  const std::vector<int>& y,
                  int n_features) {
        std::ofstream ofs(path);
        for (int i = 0; i < n_features; ++i) {
            if (i > 0) ofs << ",";
            ofs << "f" << i;
        }
        ofs << ",label\n";

        for (size_t i = 0; i < X.size(); ++i) {
            for (int j = 0; j < n_features; ++j) {
                if (j > 0) ofs << ",";
                ofs << X[i][j];
            }
            ofs << "," << y[i] << "\n";
        }
    }
};

TEST_F(SleepStageTrainerTest, LoadCSVValid) {
    std::vector<std::vector<double>> X_orig;
    std::vector<int> y_orig;
    generateSeparableData(X_orig, y_orig, 10, 5);

    auto csv_path = temp_dir_ / "test_features.csv";
    writeCSV(csv_path, X_orig, y_orig, 5);

    std::vector<std::vector<double>> X_loaded;
    std::vector<int> y_loaded;
    bool ok = SleepStageTrainer::loadCSV(csv_path.string(), X_loaded, y_loaded);

    ASSERT_TRUE(ok);
    EXPECT_EQ(X_loaded.size(), X_orig.size());
    EXPECT_EQ(y_loaded.size(), y_orig.size());
    ASSERT_FALSE(X_loaded.empty());
    EXPECT_EQ(X_loaded[0].size(), 5u);

    for (size_t i = 0; i < y_orig.size(); ++i) {
        EXPECT_EQ(y_loaded[i], y_orig[i]);
    }
}

TEST_F(SleepStageTrainerTest, LoadCSVMissingFile) {
    std::vector<std::vector<double>> X;
    std::vector<int> y;
    bool ok = SleepStageTrainer::loadCSV("/nonexistent/path.csv", X, y);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(X.empty());
    EXPECT_TRUE(y.empty());
}

TEST_F(SleepStageTrainerTest, TrainSyntheticData) {
    std::vector<std::vector<double>> X;
    std::vector<int> y;
    generateSeparableData(X, y, 40, CAUSAL_FEATURE_COUNT, 42);

    TrainingConfig config;
    config.rf_params.n_estimators = 30;
    config.rf_params.max_depth = 8;
    config.rf_params.random_seed = 42;
    config.cv_folds = 3;
    config.mode = FeatureMode::CAUSAL;

    auto result = SleepStageTrainer::train(X, y, config);

    ASSERT_TRUE(result.success) << "Training failed: " << result.error;
    EXPECT_TRUE(result.model.isTrained());
    EXPECT_TRUE(result.scaler.isFitted());

    EXPECT_GT(result.metrics.accuracy, 0.50);
    EXPECT_GT(result.metrics.macro_f1, 0.40);
    EXPECT_EQ(result.metrics.n_samples, static_cast<int>(X.size()));

    EXPECT_GT(result.metrics.wake_f1, 0.0);
    EXPECT_GT(result.metrics.light_f1, 0.0);
    EXPECT_GT(result.metrics.deep_f1, 0.0);
    EXPECT_GT(result.metrics.rem_f1, 0.0);

    ASSERT_EQ(result.metrics.confusion_matrix.size(), 4u);
    for (const auto& row : result.metrics.confusion_matrix) {
        EXPECT_EQ(row.size(), 4u);
    }
}

TEST_F(SleepStageTrainerTest, SaveLoadBundleRoundTrip) {
    std::vector<std::vector<double>> X;
    std::vector<int> y;
    generateSeparableData(X, y, 30, CAUSAL_FEATURE_COUNT, 42);

    TrainingConfig config;
    config.rf_params.n_estimators = 20;
    config.rf_params.max_depth = 6;
    config.rf_params.random_seed = 42;
    config.cv_folds = 3;

    auto result = SleepStageTrainer::train(X, y, config);
    ASSERT_TRUE(result.success);

    auto model_dir = temp_dir_ / "models";
    std::filesystem::create_directories(model_dir);
    bool saved = SleepStageTrainer::saveBundle(model_dir.string(), "test_model", result);
    ASSERT_TRUE(saved);

    RandomForest loaded_model;
    StandardScaler loaded_scaler;
    std::vector<std::string> loaded_features;
    bool loaded = SleepStageTrainer::loadBundle(
        model_dir.string(), "test_model", loaded_model, loaded_scaler, loaded_features);
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded_model.isTrained());
    EXPECT_TRUE(loaded_scaler.isFitted());

    auto X_scaled_orig = result.scaler.transform({X[0]});
    auto X_scaled_loaded = loaded_scaler.transform({X[0]});

    int pred_orig = result.model.predictClass(X_scaled_orig[0]);
    int pred_loaded = loaded_model.predictClass(X_scaled_loaded[0]);
    EXPECT_EQ(pred_orig, pred_loaded);
}

TEST_F(SleepStageTrainerTest, ImbalancedDatasetStillTrains) {
    std::vector<std::vector<double>> X;
    std::vector<int> y;
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0.0, 0.5);
    int n_features = CAUSAL_FEATURE_COUNT;

    auto addSamples = [&](int cls, int count, double cx, double cy) {
        for (int i = 0; i < count; ++i) {
            std::vector<double> row(n_features, 0.0);
            row[0] = cx + noise(rng);
            row[1] = cy + noise(rng);
            X.push_back(std::move(row));
            y.push_back(cls);
        }
    };

    addSamples(0, 160, -3.0, -3.0);
    addSamples(1,  20,  3.0, -3.0);
    addSamples(2,  10, -3.0,  3.0);
    addSamples(3,  10,  3.0,  3.0);

    TrainingConfig config;
    config.rf_params.n_estimators = 30;
    config.rf_params.max_depth = 8;
    config.rf_params.class_weight_balanced = true;
    config.rf_params.random_seed = 42;
    config.cv_folds = 3;

    auto result = SleepStageTrainer::train(X, y, config);

    ASSERT_TRUE(result.success) << "Training failed: " << result.error;
    EXPECT_TRUE(result.model.isTrained());
    EXPECT_GT(result.metrics.wake_recall, 0.5);
    EXPECT_GT(result.metrics.accuracy, 0.5);
}

TEST_F(SleepStageTrainerTest, FeatureNamesPopulated) {
    std::vector<std::vector<double>> X;
    std::vector<int> y;
    generateSeparableData(X, y, 20, CAUSAL_FEATURE_COUNT, 42);

    TrainingConfig config;
    config.rf_params.n_estimators = 10;
    config.rf_params.max_depth = 4;
    config.rf_params.random_seed = 42;
    config.cv_folds = 3;
    config.mode = FeatureMode::CAUSAL;

    auto result = SleepStageTrainer::train(X, y, config);

    ASSERT_TRUE(result.success);
    EXPECT_FALSE(result.feature_names.empty());
}
