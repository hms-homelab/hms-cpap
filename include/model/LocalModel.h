#pragma once
//
// SDD-013: the local sleep model's runner, behind an interface.
//
// The model's only job is turning a night's fact block into sentences; C++
// does the retrieval (model/NarrationService.h). It sits behind ILocalModel for
// the same reason the agent's LLM sits behind IAgentLLM: the suite must not
// need a model file, and the backend is something plugged into the back of a
// harness that stays ours.
//
// GgufModel is the backend for the GGUF artifact the SDD specifies. This
// build carries no GGUF runtime: the SDD's runtime is llama.cpp vendored into
// third_party/, which is a new dependency and is not added here. Until it is,
// GgufModel reports why it cannot run and generates nothing, so the nightly
// job writes no summary rather than a wrong one.
//
#include <optional>
#include <string>

namespace hms_cpap::model {

/// One generation's knobs. Greedy decode is temperature 0 (the parity check).
struct GenerateParams {
    double temperature = 0.7;
    int max_tokens = 400;
};

class ILocalModel {
public:
    virtual ~ILocalModel() = default;

    /// True when generate() can run: a verified model file and a runtime.
    virtual bool ready() const = 0;
    /// Why ready() is false, said to the user; "" when it is true.
    virtual std::string whyNotReady() const = 0;
    /// The model's text for [prompt], or none when it could not run.
    virtual std::optional<std::string> generate(const std::string& prompt,
                                                const GenerateParams& params) = 0;
};

/// The GGUF backend. Disabled unless the model file is present, and in this
/// build disabled in any case, because no GGUF runtime is linked.
class GgufModel : public ILocalModel {
public:
    explicit GgufModel(std::string model_path);

    /// Whether this build links a GGUF runtime (false until one is vendored).
    static bool runtimeAvailable();

    bool ready() const override;
    std::string whyNotReady() const override;
    std::optional<std::string> generate(const std::string& prompt,
                                        const GenerateParams& params) override;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

}  // namespace hms_cpap::model
