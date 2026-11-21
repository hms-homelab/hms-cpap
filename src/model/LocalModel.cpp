#include "model/LocalModel.h"

#include "services/UpdateService.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <thread>

#ifdef HMS_WITH_LOCAL_MODEL
#include <llama.h>
#endif

namespace hms_cpap::model {

// -- The prompt contract --------------------------------------------------------

namespace {

constexpr const char* kNewline = "<n>";
constexpr const char* kSeparator = "<|summary|>";

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

}  // namespace

std::string narratorPrompt(const std::string& facts) {
    std::string block = facts;
    replaceAll(block, "\n", kNewline);
    return " " + block + kSeparator;
}

std::string narratorText(std::string raw) {
    replaceAll(raw, kNewline, "\n");
    const auto first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = raw.find_last_not_of(" \t\r\n");
    return raw.substr(first, last - first + 1);
}

// -- The runtime ----------------------------------------------------------------

#ifdef HMS_WITH_LOCAL_MODEL

namespace {

// llama.cpp talks a lot while it loads. Only its errors reach the log.
void quietLog(ggml_log_level level, const char* text, void*) {
    static thread_local bool last_was_error = false;
    if (level == GGML_LOG_LEVEL_CONT) {
        if (last_was_error) std::cerr << text;
        return;
    }
    last_was_error = level == GGML_LOG_LEVEL_ERROR;
    if (last_was_error) std::cerr << "Local model (llama.cpp): " << text;
}

void initBackendOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        llama_log_set(quietLog, nullptr);
        llama_backend_init();
    });
}

int threadCount() {
    const unsigned hw = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp(hw, 1u, 4u));
}

}  // namespace

struct GgufModel::Runtime {
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const llama_vocab* vocab = nullptr;

    ~Runtime() {
        if (ctx) llama_free(ctx);
        if (model) llama_model_free(model);
    }
};

#else

struct GgufModel::Runtime {};

#endif

GgufModel::GgufModel(std::string model_path, std::string sha256)
    : path_(std::move(model_path)), sha256_(std::move(sha256)) {}

GgufModel::~GgufModel() = default;

bool GgufModel::runtimeAvailable() {
#ifdef HMS_WITH_LOCAL_MODEL
    return true;
#else
    return false;
#endif
}

namespace {

std::string whyNot(const std::string& path, const std::string& sha256, std::optional<bool>& verified,
                   const std::string& load_error) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec)) return "No model file is installed.";
    if (!GgufModel::runtimeAvailable()) return "This build does not include the local model runtime.";
    if (!sha256.empty()) {
        if (!verified) verified = update::sha256File(path) == sha256;
        if (!*verified) return "The model file is not the published one: its SHA-256 does not match.";
    }
    return load_error;
}

}  // namespace

bool GgufModel::ready() const { return whyNotReady().empty(); }

std::string GgufModel::whyNotReady() const {
    std::lock_guard<std::mutex> lk(mu_);
    return whyNot(path_, sha256_, verified_, load_error_);
}

bool GgufModel::loadLocked() {
#ifdef HMS_WITH_LOCAL_MODEL
    if (rt_) return true;
    if (!load_error_.empty()) return false;
    initBackendOnce();

    auto rt = std::make_unique<Runtime>();
    auto mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    mp.load_mode = LLAMA_LOAD_MODE_MMAP;
    rt->model = llama_model_load_from_file(path_.c_str(), mp);
    if (!rt->model) {
        load_error_ = "The model file could not be loaded.";
        return false;
    }
    rt->vocab = llama_model_get_vocab(rt->model);

    auto cp = llama_context_default_params();
    cp.n_ctx = kNarratorContext;
    cp.n_batch = kNarratorContext;
    cp.n_seq_max = 1;
    cp.n_threads = threadCount();
    cp.n_threads_batch = cp.n_threads;
    cp.no_perf = true;
    rt->ctx = llama_init_from_model(rt->model, cp);
    if (!rt->ctx) {
        load_error_ = "The model could not be started (no memory for its context?).";
        return false;
    }
    rt_ = std::move(rt);
    return true;
#else
    load_error_ = "This build does not include the local model runtime.";
    return false;
#endif
}

std::vector<std::int32_t> GgufModel::tokenizeLocked(const std::string& text) {
#ifdef HMS_WITH_LOCAL_MODEL
    if (!loadLocked()) return {};
    std::vector<llama_token> ids(text.size() + 2);
    int n = llama_tokenize(rt_->vocab, text.data(), static_cast<int32_t>(text.size()), ids.data(),
                           static_cast<int32_t>(ids.size()), /*add_special=*/true, /*parse_special=*/true);
    if (n < 0) {
        ids.resize(static_cast<std::size_t>(-n));
        n = llama_tokenize(rt_->vocab, text.data(), static_cast<int32_t>(text.size()), ids.data(),
                           static_cast<int32_t>(ids.size()), true, true);
    }
    if (n < 0) return {};
    ids.resize(static_cast<std::size_t>(n));
    return {ids.begin(), ids.end()};
#else
    (void)text;
    return {};
#endif
}

std::vector<std::int32_t> GgufModel::tokenize(const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!whyNot(path_, sha256_, verified_, load_error_).empty()) return {};
    return tokenizeLocked(text);
}

std::optional<std::string> GgufModel::generate(const std::string& facts, const GenerateParams& params) {
    std::lock_guard<std::mutex> lk(mu_);
    if (const auto why = whyNot(path_, sha256_, verified_, load_error_); !why.empty()) {
        std::cerr << "Local model: not run: " << why << std::endl;
        return std::nullopt;
    }
#ifdef HMS_WITH_LOCAL_MODEL
    if (!loadLocked()) {
        std::cerr << "Local model: not run: " << load_error_ << std::endl;
        return std::nullopt;
    }
    const std::vector<llama_token> prompt = tokenizeLocked(narratorPrompt(facts));
    const int n_ctx = static_cast<int>(llama_n_ctx(rt_->ctx));
    if (prompt.empty() || static_cast<int>(prompt.size()) >= n_ctx) {
        std::cerr << "Local model: not run: the facts take " << prompt.size() << " tokens, the model's context "
                  << n_ctx << std::endl;
        return std::nullopt;
    }

    // Each generation starts from an empty context: one night, one prompt.
    llama_memory_clear(llama_get_memory(rt_->ctx), true);

    auto sp = llama_sampler_chain_default_params();
    sp.no_perf = true;
    llama_sampler* sampler = llama_sampler_chain_init(sp);
    if (params.temperature <= 0.0) {
        llama_sampler_chain_add(sampler, llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(sampler, llama_sampler_init_temp(static_cast<float>(params.temperature)));
        llama_sampler_chain_add(sampler, llama_sampler_init_dist(kNarratorSeed));
    }
    llama_batch_ext* batch = llama_batch_ext_init(rt_->ctx);

    // [n] tokens at consecutive positions from [pos0]; logits for the last only.
    auto fill = [batch](const llama_token* tokens, int n, llama_pos pos0) {
        llama_batch_ext_clear(batch);
        for (int i = 0; i < n; ++i) {
            const int32_t idx = llama_batch_ext_add_token(batch, 0, tokens[i]);
            const llama_pos pos = pos0 + i;
            llama_batch_ext_set_pos(batch, idx, &pos);
        }
        llama_batch_ext_set_output_logits(batch, n - 1, true);
    };

    std::vector<llama_token> out;
    const int budget = std::min(params.max_tokens, n_ctx - static_cast<int>(prompt.size()));
    fill(prompt.data(), static_cast<int>(prompt.size()), 0);
    llama_pos pos = 0;
    int in_batch = static_cast<int>(prompt.size());
    bool failed = false;
    while (static_cast<int>(out.size()) < budget) {
        if (llama_process(rt_->ctx, LLAMA_PROCESS_TYPE_DECODE, batch) != 0) {
            failed = true;
            break;
        }
        pos += in_batch;
        const llama_token next = llama_sampler_sample(sampler, rt_->ctx, -1);
        if (llama_vocab_is_eog(rt_->vocab, next)) break;   // </s>
        out.push_back(next);
        fill(&next, 1, pos);
        in_batch = 1;
    }
    llama_batch_ext_free(batch);
    llama_sampler_free(sampler);
    if (failed) {
        std::cerr << "Local model: the model stopped with an error after " << out.size() << " tokens"
                  << std::endl;
        return std::nullopt;
    }

    // The text without special tokens (none should be there; </s> ended it).
    std::string text(out.size() * 16 + 16, '\0');
    int n = llama_detokenize(rt_->vocab, out.data(), static_cast<int32_t>(out.size()), text.data(),
                             static_cast<int32_t>(text.size()), /*remove_special=*/true,
                             /*unparse_special=*/false);
    if (n < 0) {
        text.resize(static_cast<std::size_t>(-n));
        n = llama_detokenize(rt_->vocab, out.data(), static_cast<int32_t>(out.size()), text.data(),
                             static_cast<int32_t>(text.size()), true, false);
    }
    if (n < 0) return std::nullopt;
    text.resize(static_cast<std::size_t>(n));
    return narratorText(std::move(text));
#else
    (void)facts;
    (void)params;
    return std::nullopt;
#endif
}

}  // namespace hms_cpap::model
