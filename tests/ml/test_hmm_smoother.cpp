#include <gtest/gtest.h>
#include "ml/HmmSmoother.h"
#include "ml/SleepStageTypes.h"

#include <array>
#include <cmath>
#include <numeric>
#include <vector>

using namespace hms_cpap::ml;

static std::array<double, 4> strongObs(int cls) {
    std::array<double, 4> obs;
    obs.fill(0.1 / 3.0);
    obs[cls] = 0.9;
    return obs;
}

static std::array<double, 4> uniformObs() {
    return {0.25, 0.25, 0.25, 0.25};
}

TEST(HmmSmootherTest, DefaultTransitionsSumToOne) {
    auto trans = HmmSmoother::defaultTransitions();

    for (int i = 0; i < HmmSmoother::N_STATES; ++i) {
        double sum = 0.0;
        for (int j = 0; j < HmmSmoother::N_STATES; ++j) {
            EXPECT_GE(trans[i][j], 0.0);
            sum += trans[i][j];
        }
        EXPECT_NEAR(sum, 1.0, 1e-6)
            << "Row " << i << " of transition matrix does not sum to 1.0";
    }
}

TEST(HmmSmootherTest, DefaultPriorSumsToOne) {
    auto prior = HmmSmoother::defaultPrior();

    double sum = 0.0;
    for (int i = 0; i < HmmSmoother::N_STATES; ++i) {
        EXPECT_GE(prior[i], 0.0);
        sum += prior[i];
    }
    EXPECT_NEAR(sum, 1.0, 1e-6);
}

TEST(HmmSmootherTest, SmoothUniformObservationsReturnsMostLikelyState) {
    HmmSmoother smoother;

    std::vector<std::array<double, 4>> observations;
    for (int i = 0; i < 10; ++i) {
        observations.push_back(strongObs(1));
    }

    auto smoothed = smoother.smooth(observations);
    ASSERT_EQ(smoothed.size(), 10u);

    for (size_t i = 0; i < smoothed.size(); ++i) {
        EXPECT_EQ(smoothed[i], SleepStage::LIGHT)
            << "Epoch " << i << " should be LIGHT with strong observations";
    }
}

TEST(HmmSmootherTest, SmoothClearObservationsReturnsCorrectStates) {
    HmmSmoother smoother;

    std::vector<std::array<double, 4>> observations;
    for (int i = 0; i < 5; ++i) observations.push_back(strongObs(0));
    for (int i = 0; i < 5; ++i) observations.push_back(strongObs(2));
    for (int i = 0; i < 5; ++i) observations.push_back(strongObs(3));

    auto smoothed = smoother.smooth(observations);
    ASSERT_EQ(smoothed.size(), 15u);

    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(smoothed[i], SleepStage::WAKE);
    for (int i = 5; i < 10; ++i)
        EXPECT_EQ(smoothed[i], SleepStage::DEEP);
    for (int i = 10; i < 15; ++i)
        EXPECT_EQ(smoothed[i], SleepStage::REM);
}

TEST(HmmSmootherTest, SmoothIncrementalReturnsValidStage) {
    HmmSmoother smoother;

    std::vector<std::array<double, 4>> recent;
    for (int i = 0; i < 5; ++i) recent.push_back(strongObs(1));

    SleepStage result = smoother.smoothIncremental(recent, 10);
    int stage = static_cast<int>(result);
    EXPECT_GE(stage, 0);
    EXPECT_LE(stage, 3);
}

TEST(HmmSmootherTest, SmoothIncrementalDefaultWindow) {
    HmmSmoother smoother;

    std::vector<std::array<double, 4>> recent;
    for (int i = 0; i < 3; ++i) recent.push_back(strongObs(2));

    SleepStage result = smoother.smoothIncremental(recent);
    EXPECT_EQ(result, SleepStage::DEEP);
}

TEST(HmmSmootherTest, EmptyObservationsReturnsEmpty) {
    HmmSmoother smoother;
    std::vector<std::array<double, 4>> empty;

    auto result = smoother.smooth(empty);
    EXPECT_TRUE(result.empty());
}

TEST(HmmSmootherTest, SingleObservationReturnsSingleResult) {
    HmmSmoother smoother;

    std::vector<std::array<double, 4>> obs = {strongObs(3)};
    auto result = smoother.smooth(obs);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], SleepStage::REM);
}

TEST(HmmSmootherTest, JsonRoundTrip) {
    HmmSmoother original;

    auto json = original.toJson();
    auto restored = HmmSmoother::fromJson(json);

    std::vector<std::array<double, 4>> observations;
    for (int i = 0; i < 5; ++i) observations.push_back(strongObs(0));
    for (int i = 0; i < 5; ++i) observations.push_back(strongObs(1));
    for (int i = 0; i < 5; ++i) observations.push_back(strongObs(2));

    auto result_orig = original.smooth(observations);
    auto result_restored = restored.smooth(observations);

    ASSERT_EQ(result_orig.size(), result_restored.size());
    for (size_t i = 0; i < result_orig.size(); ++i) {
        EXPECT_EQ(result_orig[i], result_restored[i])
            << "Mismatch at epoch " << i << " after JSON round-trip";
    }
}

TEST(HmmSmootherTest, CustomTransitionsViaConstructor) {
    HmmSmoother::TransMatrix trans;
    trans[0] = {0.7, 0.2, 0.05, 0.05};
    trans[1] = {0.1, 0.6, 0.2, 0.1};
    trans[2] = {0.1, 0.2, 0.6, 0.1};
    trans[3] = {0.1, 0.1, 0.1, 0.7};
    std::array<double, 4> prior = {0.4, 0.3, 0.1, 0.2};

    HmmSmoother smoother(trans, prior);

    auto json = smoother.toJson();
    auto restored = HmmSmoother::fromJson(json);

    std::vector<std::array<double, 4>> observations;
    for (int i = 0; i < 10; ++i) observations.push_back(strongObs(0));

    auto result_orig = smoother.smooth(observations);
    auto result_restored = restored.smooth(observations);

    ASSERT_EQ(result_orig.size(), result_restored.size());
    for (size_t i = 0; i < result_orig.size(); ++i) {
        EXPECT_EQ(result_orig[i], result_restored[i]);
    }
}
