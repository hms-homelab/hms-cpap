#include <gtest/gtest.h>
#include "ml/SleepStageFeatureExtractor.h"
#include "ml/SleepStageTypes.h"

#include <chrono>
#include <cmath>
#include <vector>

using namespace hms_cpap::ml;
using namespace hms_cpap;
using Clock = std::chrono::system_clock;

static EpochSample makeEpoch(Clock::time_point ts, int hr, double spo2,
                             double rr, double tv, double mv, double ie,
                             double leak, double flow, int motion, int vib) {
    EpochSample e;
    e.timestamp = ts;
    e.heart_rate = hr;
    e.spo2 = spo2;
    e.respiratory_rate = rr;
    e.tidal_volume = tv;
    e.minute_ventilation = mv;
    e.ie_ratio = ie;
    e.leak_rate = leak;
    e.flow_p95 = flow;
    e.motion = motion;
    e.vibration = vib;
    return e;
}

static std::vector<EpochSample> makeSyntheticEpochs(int n, Clock::time_point start) {
    std::vector<EpochSample> epochs;
    epochs.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto ts = start + std::chrono::seconds(i * EPOCH_DURATION_SEC);
        double t = static_cast<double>(i) / n;
        int hr = static_cast<int>(60.0 + 10.0 * std::sin(2 * M_PI * t));
        double spo2 = 96.0 + 2.0 * std::cos(2 * M_PI * t);
        double rr = 14.0 + 2.0 * std::sin(2 * M_PI * t);
        int motion = (i % 10 == 0) ? 1 : 0;
        epochs.push_back(makeEpoch(ts, hr, spo2, rr, 0.4, 6.0, 0.4, 2.0, 30.0, motion, 0));
    }
    return epochs;
}

TEST(SleepStageFeatureExtractorTest, CausalFeatureCount) {
    auto start = Clock::now();
    auto epochs = makeSyntheticEpochs(20, start);

    for (int i = 0; i < static_cast<int>(epochs.size()); ++i) {
        auto features = SleepStageFeatureExtractor::extract(epochs, i, FeatureMode::CAUSAL, start);
        EXPECT_EQ(static_cast<int>(features.size()), CAUSAL_FEATURE_COUNT)
            << "Causal mode should produce " << CAUSAL_FEATURE_COUNT << " features at epoch " << i;
    }
}

TEST(SleepStageFeatureExtractorTest, BidirFeatureCount) {
    auto start = Clock::now();
    auto epochs = makeSyntheticEpochs(20, start);

    for (int i = 0; i < static_cast<int>(epochs.size()); ++i) {
        auto features = SleepStageFeatureExtractor::extract(epochs, i, FeatureMode::BIDIRECTIONAL, start);
        EXPECT_EQ(static_cast<int>(features.size()), BIDIR_FEATURE_COUNT)
            << "Bidirectional mode should produce " << BIDIR_FEATURE_COUNT << " features at epoch " << i;
    }
}

TEST(SleepStageFeatureExtractorTest, EmptyEpochsReturnsEmpty) {
    auto start = Clock::now();
    std::vector<EpochSample> empty;

    auto features = SleepStageFeatureExtractor::extract(empty, 0, FeatureMode::CAUSAL, start);
    EXPECT_TRUE(features.empty());
}

TEST(SleepStageFeatureExtractorTest, EmptyEpochsBidirReturnsEmpty) {
    auto start = Clock::now();
    std::vector<EpochSample> empty;

    auto features = SleepStageFeatureExtractor::extract(empty, 0, FeatureMode::BIDIRECTIONAL, start);
    EXPECT_TRUE(features.empty());
}

TEST(SleepStageFeatureExtractorTest, AggregateToEpochsCount) {
    auto start = Clock::now();
    auto end = start + std::chrono::seconds(600);

    std::vector<CPAPVitals> vitals;
    std::vector<BreathingSummary> breathing;
    std::vector<OximetrySample> oximetry;

    for (int i = 0; i < 600; ++i) {
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

        OximetrySample o{};
        o.timestamp = ts;
        o.spo2 = 96;
        o.heart_rate = 65;
        o.invalid_flag = 0;
        o.motion = 0;
        o.vibration = 0;
        oximetry.push_back(o);
    }

    int expected_epochs = 600 / EPOCH_DURATION_SEC;
    auto epochs = SleepStageFeatureExtractor::aggregateToEpochs(vitals, breathing, oximetry, start, end);

    EXPECT_EQ(static_cast<int>(epochs.size()), expected_epochs);
}

TEST(SleepStageFeatureExtractorTest, TimeOfNightInRange) {
    auto start = Clock::now();
    auto epochs = makeSyntheticEpochs(100, start);

    auto names = sleepStageFeatureNames(FeatureMode::CAUSAL);
    int ton_idx = -1;
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == "time_of_night") {
            ton_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(ton_idx, 0) << "time_of_night not found in feature names";

    for (int i = 0; i < static_cast<int>(epochs.size()); ++i) {
        auto features = SleepStageFeatureExtractor::extract(epochs, i, FeatureMode::CAUSAL, start);
        EXPECT_GE(features[ton_idx], 0.0)
            << "time_of_night at epoch " << i << " below 0";
        EXPECT_LE(features[ton_idx], 1.0)
            << "time_of_night at epoch " << i << " above 1";
    }
}

TEST(SleepStageFeatureExtractorTest, SingleEpochCausalRollingFeaturesAreZero) {
    auto start = Clock::now();
    auto ts = start + std::chrono::seconds(0);
    std::vector<EpochSample> epochs = {makeEpoch(ts, 65, 97.0, 15.0, 0.4, 6.0, 0.4, 2.0, 30.0, 0, 0)};

    auto features = SleepStageFeatureExtractor::extract(epochs, 0, FeatureMode::CAUSAL, start);
    ASSERT_EQ(static_cast<int>(features.size()), CAUSAL_FEATURE_COUNT);

    auto names = sleepStageFeatureNames(FeatureMode::CAUSAL);
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i].find("past_") == 0) {
            EXPECT_DOUBLE_EQ(features[i], 0.0)
                << "Rolling feature " << names[i] << " should be 0 for single epoch";
        }
    }
}

TEST(SleepStageFeatureExtractorTest, BiDirHasMoreFeaturesThanCausal) {
    auto start = Clock::now();
    auto epochs = makeSyntheticEpochs(20, start);

    auto causal = SleepStageFeatureExtractor::extract(epochs, 10, FeatureMode::CAUSAL, start);
    auto bidir = SleepStageFeatureExtractor::extract(epochs, 10, FeatureMode::BIDIRECTIONAL, start);

    EXPECT_EQ(bidir.size() - causal.size(), 4u);
}
