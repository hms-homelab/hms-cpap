#pragma once

#include "ml/SleepStageTypes.h"
#include <nlohmann/json.hpp>
#include <array>
#include <vector>

namespace hms_cpap {
namespace ml {

class HmmSmoother {
public:
    static constexpr int N_STATES = 4;
    using TransMatrix = std::array<std::array<double, N_STATES>, N_STATES>;

    static TransMatrix defaultTransitions();
    static std::array<double, N_STATES> defaultPrior();

    explicit HmmSmoother(TransMatrix trans = defaultTransitions(),
                         std::array<double, N_STATES> prior = defaultPrior());

    std::vector<SleepStage> smooth(
        const std::vector<std::array<double, N_STATES>>& observations) const;

    SleepStage smoothIncremental(
        const std::vector<std::array<double, N_STATES>>& recent_observations,
        int window = 10) const;

    nlohmann::json toJson() const;
    static HmmSmoother fromJson(const nlohmann::json& j);

private:
    TransMatrix trans_;
    std::array<double, N_STATES> prior_;
};

}  // namespace ml
}  // namespace hms_cpap
