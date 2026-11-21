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
// GgufModel is the backend for the GGUF artifact the SDD specifies, run by
// llama.cpp (cmake/LocalModel.cmake, pinned). A build made with
// BUILD_WITH_LOCAL_MODEL=OFF links no runtime: GgufModel then says so and
// generates nothing, so the nightly job writes no summary rather than a wrong
// one.
//
// Where the prompt contract lives: here, in the backend. The caller hands over
// the fact block and nothing else; how that block is framed for the model
// (narratorPrompt) is a property of the model file, fixed when it was trained
// and exported, and moves only with it. NarrationService checks the text
// against the same block it handed over.
//
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
    /// The model's text for the night whose fact block is [facts], or none
    /// when it could not run. The backend frames the block for its model.
    virtual std::optional<std::string> generate(const std::string& facts,
                                                const GenerateParams& params) = 0;
};

/// The narrator's prompt contract (SDD-013 Status, from the export in
/// training/narrator/train_colab.ipynb): " " + the fact block with each "\n"
/// as "<n>" + "<|summary|>". The tokenizer adds BOS and parses "<n>" and
/// "<|summary|>" as the model's own pieces. The one leading space stands in
/// for sentencepiece's dummy prefix, which the GGUF has off
/// (add_space_prefix=false), because llama.cpp would otherwise put one after
/// every "<n>".
std::string narratorPrompt(const std::string& facts);

/// The model's own text, made readable: a "<n>" it wrote is a line break (the
/// prompt contract read backwards), and the space the tokenizer leaves at the
/// start, and any at the end, are dropped.
std::string narratorText(std::string raw);

/// The context the narrator was trained with: prompt and summary fit in it.
constexpr int kNarratorContext = 1024;
/// The seed of a sampled (temperature > 0) generation, fixed so one night
/// gives one text.
constexpr std::uint32_t kNarratorSeed = 13;

/// The GGUF backend, on llama.cpp's C API. The file is loaded (mmap) on the
/// first generate() and kept until the object goes. One generation at a time:
/// generate() and tokenize() hold a lock, which is all the single worker of
/// LocalModelService needs.
class GgufModel : public ILocalModel {
public:
    /// [sha256], when given, is what the file must hash to before it is run;
    /// it is checked once, on the first ready().
    explicit GgufModel(std::string model_path, std::string sha256 = "");
    ~GgufModel() override;
    GgufModel(const GgufModel&) = delete;
    GgufModel& operator=(const GgufModel&) = delete;

    /// Whether this build links the GGUF runtime (BUILD_WITH_LOCAL_MODEL).
    static bool runtimeAvailable();

    bool ready() const override;
    std::string whyNotReady() const override;
    std::optional<std::string> generate(const std::string& facts,
                                        const GenerateParams& params) override;

    /// The token ids [text] becomes (BOS added, the model's pieces parsed),
    /// exactly what generate() gives the model for narratorPrompt(facts).
    /// Empty when the model cannot be loaded. The tokenizer parity check.
    std::vector<std::int32_t> tokenize(const std::string& text);

    const std::string& path() const { return path_; }

private:
    struct Runtime;
    bool loadLocked();   // with mu_ held
    std::vector<std::int32_t> tokenizeLocked(const std::string& text);

    std::string path_;
    std::string sha256_;
    mutable std::mutex mu_;
    mutable std::optional<bool> verified_;   // the hash check, once
    std::string load_error_;                 // why the file would not load
    std::unique_ptr<Runtime> rt_;
};

}  // namespace hms_cpap::model
