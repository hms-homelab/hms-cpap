#include "ml/HmmSmoother.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace hms_cpap {
namespace ml {

HmmSmoother::TransMatrix HmmSmoother::defaultTransitions() {
    // Empirical transition probabilities derived from SHHS polysomnography data.
    // Rows = from-state, cols = to-state.  Order: Wake, Light, Deep, REM.
    return {{
        {{ 0.85, 0.13, 0.01, 0.01 }},  // Wake  ->
        {{ 0.08, 0.80, 0.08, 0.04 }},  // Light ->
        {{ 0.01, 0.13, 0.85, 0.01 }},  // Deep  ->
        {{ 0.07, 0.10, 0.01, 0.82 }}   // REM   ->
    }};
}

std::array<double, HmmSmoother::N_STATES> HmmSmoother::defaultPrior() {
    return {{ 0.60, 0.30, 0.05, 0.05 }};
}

HmmSmoother::HmmSmoother(TransMatrix trans, std::array<double, N_STATES> prior)
    : trans_(trans), prior_(prior) {}

std::vector<SleepStage> HmmSmoother::smooth(
    const std::vector<std::array<double, N_STATES>>& observations) const
{
    int T = static_cast<int>(observations.size());
    if (T == 0) return {};

    constexpr double NEG_INF = -std::numeric_limits<double>::infinity();

    // Viterbi in log-space
    // delta[t][s] = log probability of best path ending in state s at time t
    std::vector<std::array<double, N_STATES>> delta(T);
    std::vector<std::array<int, N_STATES>> psi(T);

    // Log transition matrix
    TransMatrix log_trans;
    for (int i = 0; i < N_STATES; ++i)
        for (int j = 0; j < N_STATES; ++j)
            log_trans[i][j] = (trans_[i][j] > 0) ? std::log(trans_[i][j]) : NEG_INF;

    // Initialization
    for (int s = 0; s < N_STATES; ++s) {
        double log_prior = (prior_[s] > 0) ? std::log(prior_[s]) : NEG_INF;
        double log_obs = (observations[0][s] > 0) ? std::log(observations[0][s]) : NEG_INF;
        delta[0][s] = log_prior + log_obs;
        psi[0][s] = 0;
    }

    // Recursion
    for (int t = 1; t < T; ++t) {
        for (int j = 0; j < N_STATES; ++j) {
            double best_val = NEG_INF;
            int best_src = 0;
            for (int i = 0; i < N_STATES; ++i) {
                double val = delta[t - 1][i] + log_trans[i][j];
                if (val > best_val) {
                    best_val = val;
                    best_src = i;
                }
            }
            double log_obs = (observations[t][j] > 0) ? std::log(observations[t][j]) : NEG_INF;
            delta[t][j] = best_val + log_obs;
            psi[t][j] = best_src;
        }
    }

    // Backtrack
    std::vector<SleepStage> path(T);
    int best_last = 0;
    double best_val = delta[T - 1][0];
    for (int s = 1; s < N_STATES; ++s) {
        if (delta[T - 1][s] > best_val) {
            best_val = delta[T - 1][s];
            best_last = s;
        }
    }

    path[T - 1] = sleepStageFromInt(best_last);
    for (int t = T - 2; t >= 0; --t) {
        best_last = psi[t + 1][best_last];
        path[t] = sleepStageFromInt(best_last);
    }

    return path;
}

SleepStage HmmSmoother::smoothIncremental(
    const std::vector<std::array<double, N_STATES>>& recent_observations,
    int window) const
{
    int n = static_cast<int>(recent_observations.size());
    if (n == 0) return SleepStage::WAKE;

    int start = std::max(0, n - window);
    std::vector<std::array<double, N_STATES>> window_obs(
        recent_observations.begin() + start, recent_observations.end());

    auto path = smooth(window_obs);
    return path.back();
}

nlohmann::json HmmSmoother::toJson() const {
    nlohmann::json j;
    j["transitions"] = nlohmann::json::array();
    for (int i = 0; i < N_STATES; ++i) {
        j["transitions"].push_back({trans_[i][0], trans_[i][1], trans_[i][2], trans_[i][3]});
    }
    j["prior"] = {prior_[0], prior_[1], prior_[2], prior_[3]};
    return j;
}

HmmSmoother HmmSmoother::fromJson(const nlohmann::json& j) {
    TransMatrix trans;
    for (int i = 0; i < N_STATES; ++i)
        for (int k = 0; k < N_STATES; ++k)
            trans[i][k] = j["transitions"][i][k].get<double>();

    std::array<double, N_STATES> prior;
    for (int i = 0; i < N_STATES; ++i)
        prior[i] = j["prior"][i].get<double>();

    return HmmSmoother(trans, prior);
}

}  // namespace ml
}  // namespace hms_cpap
