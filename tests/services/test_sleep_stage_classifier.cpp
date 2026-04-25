#include <gtest/gtest.h>
#include "services/SleepStageClassifier.h"
#include "ml/SleepStageTypes.h"
#include "ml/SleepStageTrainer.h"
#include "ml/RandomForest.h"
#include "ml/StandardScaler.h"
#include "parsers/CpapdashBridge.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <json/json.h>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <vector>

using namespace hms_cpap;
using namespace hms_cpap::ml;
using Clock = std::chrono::system_clock;

class SleepStageClassifierTest : public ::testing::Test {
protected:
    std::filesystem::path temp_dir_;
    SleepStageClassifier::Config config_;

    void SetUp() override {
        temp_dir_ = std::filesystem::temp_directory_path() / "hms_cpap_classifier_test";
        std::filesystem::create_directories(temp_dir_);

        config_.enabled = true;
        config_.live_inference = true;
        config_.model_dir = temp_dir_.string();
        config_.model_version = "test-v1";
    }

    void TearDown() override {
        if (std::filesystem::exists(temp_dir_))
            std::filesystem::remove_all(temp_dir_);
    }

    bool writeClassifierBundle(const std::string& path,
                               const ml::RandomForest& model,
                               const ml::StandardScaler& scaler,
                               const std::vector<std::string>& features) {
        nlohmann::json j;
        j["model"] = model.toJson();
        j["scaler"] = scaler.toJson();
        j["features"] = features;
        std::ofstream ofs(path);
        if (!ofs.is_open()) return false;
        ofs << j.dump(2);
        return ofs.good();
    }

    bool createTestModels() {
        std::mt19937 rng(42);
        std::normal_distribution<double> noise(0.0, 0.3);

        double centers[4][2] = {
            {-3.0, -3.0}, { 3.0, -3.0},
            {-3.0,  3.0}, { 3.0,  3.0}
        };

        auto generateData = [&](int n_features) {
            std::vector<std::vector<double>> X;
            std::vector<int> y;
            for (int cls = 0; cls < 4; ++cls) {
                for (int i = 0; i < 30; ++i) {
                    std::vector<double> row(n_features, 0.0);
                    row[0] = centers[cls][0] + noise(rng);
                    row[1] = centers[cls][1] + noise(rng);
                    X.push_back(std::move(row));
                    y.push_back(cls);
                }
            }
            return std::make_pair(X, y);
        };

        TrainingConfig tc;
        tc.rf_params.n_estimators = 20;
        tc.rf_params.max_depth = 6;
        tc.rf_params.random_seed = 42;
        tc.cv_folds = 3;

        auto [X_bidir, y_bidir] = generateData(BIDIR_FEATURE_COUNT);
        tc.mode = FeatureMode::BIDIRECTIONAL;
        auto bidir_result = SleepStageTrainer::train(X_bidir, y_bidir, tc);
        if (!bidir_result.success) return false;

        std::string final_path = temp_dir_.string() + "/" + config_.model_version + "_final.json";
        if (!writeClassifierBundle(final_path, bidir_result.model, bidir_result.scaler,
                                   bidir_result.feature_names))
            return false;

        rng.seed(42);
        auto [X_causal, y_causal] = generateData(CAUSAL_FEATURE_COUNT);
        tc.mode = FeatureMode::CAUSAL;
        auto causal_result = SleepStageTrainer::train(X_causal, y_causal, tc);
        if (!causal_result.success) return false;

        std::string live_path = temp_dir_.string() + "/" + config_.model_version + "_live.json";
        if (!writeClassifierBundle(live_path, causal_result.model, causal_result.scaler,
                                   causal_result.feature_names))
            return false;

        return true;
    }

    CPAPSession makeSyntheticSession(int duration_sec) {
        CPAPSession session;
        auto start = Clock::now() - std::chrono::seconds(duration_sec);
        auto end = Clock::now();
        session.session_start = start;
        session.session_end = end;
        session.duration_seconds = duration_sec;

        for (int i = 0; i < duration_sec; ++i) {
            auto ts = start + std::chrono::seconds(i);

            CPAPVitals v(ts);
            v.spo2 = 96.0;
            v.heart_rate = 65;
            session.vitals.push_back(v);

            BreathingSummary b(ts);
            b.respiratory_rate = 14.0;
            b.tidal_volume = 0.4;
            b.minute_ventilation = 6.0;
            b.ie_ratio = 0.4;
            b.leak_rate = 2.0;
            b.flow_p95 = 30.0;
            session.breathing_summary.push_back(b);
        }

        return session;
    }

    std::vector<OximetrySample> makeSyntheticOximetry(int duration_sec, Clock::time_point start) {
        std::vector<OximetrySample> samples;
        for (int i = 0; i < duration_sec; ++i) {
            OximetrySample o{};
            o.timestamp = start + std::chrono::seconds(i);
            o.spo2 = 96;
            o.heart_rate = 65;
            o.invalid_flag = 0;
            o.motion = 0;
            o.vibration = 0;
            samples.push_back(o);
        }
        return samples;
    }
};

TEST_F(SleepStageClassifierTest, IsReadyFalseBeforeLoad) {
    SleepStageClassifier classifier(config_);
    EXPECT_FALSE(classifier.isReady());
}

TEST_F(SleepStageClassifierTest, LoadModelsFailsWhenDirMissing) {
    config_.model_dir = "/nonexistent/dir/that/does/not/exist";
    SleepStageClassifier classifier(config_);
    EXPECT_FALSE(classifier.loadModels());
    EXPECT_FALSE(classifier.isReady());
}

TEST_F(SleepStageClassifierTest, LoadModelsFailsWhenNoFiles) {
    SleepStageClassifier classifier(config_);
    EXPECT_FALSE(classifier.loadModels());
    EXPECT_FALSE(classifier.isReady());
}

TEST_F(SleepStageClassifierTest, GetStatusNotConfigured) {
    config_.enabled = false;
    SleepStageClassifier classifier(config_);

    auto status = classifier.getStatus();

    EXPECT_TRUE(status.isMember("status"));
    EXPECT_EQ(status["status"].asString(), "not_configured");
}

TEST_F(SleepStageClassifierTest, GetStatusNoModels) {
    SleepStageClassifier classifier(config_);

    auto status = classifier.getStatus();

    EXPECT_TRUE(status["enabled"].asBool());
    EXPECT_FALSE(status["ready"].asBool());
    EXPECT_TRUE(status.isMember("model_dir"));
    EXPECT_EQ(status["model_dir"].asString(), config_.model_dir);
}

TEST_F(SleepStageClassifierTest, LoadModelsSuccess) {
    ASSERT_TRUE(createTestModels());

    SleepStageClassifier classifier(config_);
    bool loaded = classifier.loadModels();

    EXPECT_TRUE(loaded);
    EXPECT_TRUE(classifier.isReady());
}

TEST_F(SleepStageClassifierTest, GetStatusAfterLoad) {
    ASSERT_TRUE(createTestModels());

    SleepStageClassifier classifier(config_);
    ASSERT_TRUE(classifier.loadModels());

    auto status = classifier.getStatus();
    EXPECT_EQ(status["status"].asString(), "ready");
    EXPECT_TRUE(status["enabled"].asBool());
    EXPECT_TRUE(status["final_model_loaded"].asBool());
    EXPECT_TRUE(status["live_model_loaded"].asBool());
}

TEST_F(SleepStageClassifierTest, ClassifyFinalReturnsEpochs) {
    ASSERT_TRUE(createTestModels());

    SleepStageClassifier classifier(config_);
    ASSERT_TRUE(classifier.loadModels());

    int duration = 300;
    auto session = makeSyntheticSession(duration);
    auto start = *session.session_start;
    auto oximetry = makeSyntheticOximetry(duration, start);

    auto epochs = classifier.classifyFinal(session, oximetry);

    int expected = duration / EPOCH_DURATION_SEC;
    EXPECT_GE(static_cast<int>(epochs.size()), 1);
    EXPECT_LE(static_cast<int>(epochs.size()), expected + 1);

    for (const auto& epoch : epochs) {
        int stage = static_cast<int>(epoch.stage);
        EXPECT_GE(stage, 0);
        EXPECT_LE(stage, 3);
        EXPECT_GE(epoch.confidence, 0.0);
        EXPECT_LE(epoch.confidence, 1.0);
    }
}

TEST_F(SleepStageClassifierTest, ClassifyLiveReturnsEpochs) {
    ASSERT_TRUE(createTestModels());

    SleepStageClassifier classifier(config_);
    ASSERT_TRUE(classifier.loadModels());

    int duration = 300;
    auto start = Clock::now() - std::chrono::seconds(duration);

    std::vector<CPAPVitals> vitals;
    std::vector<BreathingSummary> breathing;
    for (int i = 0; i < duration; ++i) {
        auto ts = start + std::chrono::seconds(i);
        CPAPVitals v(ts);
        v.spo2 = 96.0;
        v.heart_rate = 65;
        vitals.push_back(v);

        BreathingSummary b(ts);
        b.respiratory_rate = 14.0;
        b.tidal_volume = 0.4;
        b.minute_ventilation = 6.0;
        b.ie_ratio = 0.4;
        b.leak_rate = 2.0;
        b.flow_p95 = 30.0;
        breathing.push_back(b);
    }

    auto oximetry = makeSyntheticOximetry(duration, start);

    auto epochs = classifier.classifyLive(vitals, breathing, oximetry, start, 0);

    EXPECT_GE(static_cast<int>(epochs.size()), 1);

    for (const auto& epoch : epochs) {
        int stage = static_cast<int>(epoch.stage);
        EXPECT_GE(stage, 0);
        EXPECT_LE(stage, 3);
    }
}

TEST_F(SleepStageClassifierTest, DisabledConfig) {
    config_.enabled = false;
    SleepStageClassifier classifier(config_);

    EXPECT_FALSE(classifier.isReady());

    auto status = classifier.getStatus();
    EXPECT_FALSE(status["enabled"].asBool());
}
