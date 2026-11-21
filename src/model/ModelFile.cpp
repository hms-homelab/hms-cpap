#include "model/ModelFile.h"

#include "services/SetupService.h"
#include "services/UpdateService.h"
#include "utils/AppConfig.h"

#include <cctype>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace hms_cpap::model {

// SDD-013: the pin is cmake/narrator-pin.cmake's (narrator-v1, the release
// asset of hms-homelab/hms-cpap tag narrator-v1), compiled in by
// cmake/NarratorModel.cmake, so the model a package bundles and the hash this
// checks it against are one pin (D-701-1).
#if !defined(HMS_NARRATOR_VERSION) || !defined(HMS_NARRATOR_URL) || !defined(HMS_NARRATOR_SHA256)
#error "the narrator pin comes from cmake/narrator-pin.cmake (cmake/NarratorModel.cmake)"
#endif
const char* const kNarratorModelVersion = HMS_NARRATOR_VERSION;
const char* const kNarratorModelUrl = HMS_NARRATOR_URL;
const char* const kNarratorModelSha256 = HMS_NARRATOR_SHA256;

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

bool modelFilePresent(const std::string& dir, const Pin& pin) {
    std::error_code ec;
    return fs::is_regular_file(modelPath(dir, pin), ec);
}

std::vector<std::string> bundledModelDirs() {
    const auto exe = SetupService::executableDir();
    if (exe.empty()) return {};
    const fs::path d(exe);
    return {(d / "models").string(),                                  // Windows, the zips, a build tree
            (d / ".." / "Resources" / "models").lexically_normal().string(),       // HmsCpap.app
            (d / ".." / "share" / "hms-cpap" / "models").lexically_normal().string()};  // Pi, Docker
}

std::string findBundledModel(const std::vector<std::string>& dirs, const Pin& pin) {
    for (const auto& dir : dirs)
        if (!dir.empty() && installedAndVerified(dir, pin)) return modelPath(dir, pin);
    return "";
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
