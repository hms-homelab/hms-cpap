#include "model/ModelFile.h"

#include "services/UpdateService.h"
#include "utils/AppConfig.h"

#include <cctype>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace hms_cpap::model {

// SDD-013: narrator-v1, the release asset of hms-homelab/hms-cpap tag
// narrator-v1 (47,175,872 bytes, Q4_K_M, llama.cpp 919794500475862f). The
// runtime's prompt contract (model/LocalModel.h) is this file's; a new model
// moves all three together.
const char* const kNarratorModelVersion = "narrator-v1";
const char* const kNarratorModelUrl =
    "https://github.com/hms-homelab/hms-cpap/releases/download/narrator-v1/narrator-v1-q4_k_m.gguf";
const char* const kNarratorModelSha256 = "a0d99c56137466735fa0371dd64e6bdb908b2e33cd250632670534734af48d00";

namespace {

bool hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Hash [file] against [pin]; "" when it matches, else why not.
std::string checkHash(const std::string& file, const Pin& pin) {
    const auto got = update::sha256File(file);
    if (got.empty()) return "The model file could not be read.";
    if (got != lower(pin.sha256))
        return "This is not the published model file: its SHA-256 is " + got + ", the published one's is " +
               lower(pin.sha256) + ".";
    return "";
}

// Move [tmp] (already verified) to the model's path.
InstallResult putInPlace(const std::string& tmp, const std::string& dest) {
    InstallResult r;
    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(tmp, ec);
        r.error = "The model file could not be put in place.";
        return r;
    }
    r.ok = true;
    r.path = dest;
    return r;
}

}  // namespace

Pin pinnedModel() { return {kNarratorModelVersion, kNarratorModelUrl, kNarratorModelSha256}; }

std::string verifyRefusal(const Pin& pin) {
    if (pin.sha256.empty())
        return "No local sleep model has been published yet, so there is no model file to check "
               "against. It can be turned on once one is.";
    if (!hex64(pin.sha256)) return "The published model's hash in this build is not a SHA-256.";
    return "";
}

std::string downloadRefusal(const Pin& pin) {
    if (pin.url.empty())
        return "No local sleep model has been published yet, so there is nothing to download. It can "
               "be turned on once one is.";
    if (pin.url.rfind("https://", 0) != 0) return "The published model's address in this build is not https.";
    return verifyRefusal(pin);
}

std::string modelDir(const std::string& configured) {
    if (!configured.empty()) return configured;
    return (fs::path(AppConfig::dataDir()) / "models").string();
}

std::string modelPath(const std::string& dir, const Pin& pin) {
    return (fs::path(dir) / pin.version / "model.gguf").string();
}

bool installedAndVerified(const std::string& dir, const Pin& pin) {
    if (!verifyRefusal(pin).empty()) return false;
    const auto path = modelPath(dir, pin);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    return checkHash(path, pin).empty();
}

InstallResult installFromFile(const std::string& src, const std::string& dir, const Pin& pin) {
    InstallResult r;
    r.error = verifyRefusal(pin);
    if (!r.error.empty()) return r;
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) {
        r.error = "No model file was given.";
        return r;
    }
    r.error = checkHash(src, pin);
    if (!r.error.empty()) return r;

    const auto dest = modelPath(dir, pin);
    fs::create_directories(fs::path(dest).parent_path(), ec);
    const auto tmp = dest + ".part";
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        r.error = "The model file could not be copied into " + fs::path(dest).parent_path().string() + ".";
        return r;
    }
    // Hashed again where it landed: a short copy is not the model.
    r.error = checkHash(tmp, pin);
    if (!r.error.empty()) {
        fs::remove(tmp, ec);
        return r;
    }
    return putInPlace(tmp, dest);
}

InstallResult downloadModel(const std::string& dir, const Pin& pin, const Download& download) {
    InstallResult r;
    r.error = downloadRefusal(pin);
    if (!r.error.empty()) return r;

    const auto dest = modelPath(dir, pin);
    std::error_code ec;
    fs::create_directories(fs::path(dest).parent_path(), ec);
    const auto tmp = dest + ".part";
    const bool got = download ? download(pin.url, tmp) : update::downloadFile(pin.url, tmp);
    if (!got) {
        fs::remove(tmp, ec);
        r.error = "The model could not be downloaded. Check the network, or load the model from a file.";
        return r;
    }
    r.error = checkHash(tmp, pin);
    if (!r.error.empty()) {
        fs::remove(tmp, ec);
        return r;
    }
    std::cout << "Local model: " << pin.version << " downloaded and verified" << std::endl;
    return putInPlace(tmp, dest);
}

}  // namespace hms_cpap::model
