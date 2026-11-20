#include "model/LocalModel.h"

#include <filesystem>
#include <iostream>

namespace hms_cpap::model {

GgufModel::GgufModel(std::string model_path) : path_(std::move(model_path)) {}

bool GgufModel::runtimeAvailable() {
    // SDD-013: the runtime (llama.cpp in third_party/) is not vendored yet.
    return false;
}

bool GgufModel::ready() const { return whyNotReady().empty(); }

std::string GgufModel::whyNotReady() const {
    std::error_code ec;
    if (path_.empty() || !std::filesystem::is_regular_file(path_, ec))
        return "No model file is installed.";
    if (!runtimeAvailable())
        return "This build cannot run the model yet: it has no GGUF runtime.";
    return "";
}

std::optional<std::string> GgufModel::generate(const std::string&, const GenerateParams&) {
    const auto why = whyNotReady();
    if (!why.empty()) {
        std::cerr << "Local model: not run: " << why << std::endl;
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace hms_cpap::model
