#pragma once
//
// SDD-013: the local sleep model's file. Decided: downloaded on first enable
// from a pinned URL with a hash check, plus "load model from file" for an
// install with no network. Both end the same way: the file is hashed, and only
// a file whose SHA-256 is the pinned one is put in place.
//
// The file lands in the model directory under its version, the convention
// shhs-rf-v1 uses: <model_dir>/<version>/model.gguf.
//
// The pin is narrator-v1, a release asset of hms-homelab/hms-cpap (tag
// narrator-v1). A pin with an empty URL and hash means no model is published,
// and enabling refuses, saying so.
//
#include <functional>
#include <string>

namespace hms_cpap::model {

/// The pinned model's version string (the model_dir convention).
extern const char* const kNarratorModelVersion;
/// Where the pinned model is downloaded from (https).
extern const char* const kNarratorModelUrl;
/// The pinned model's SHA-256, lower-case hex.
extern const char* const kNarratorModelSha256;

/// What a model file is checked against.
struct Pin {
    std::string version;
    std::string url;
    std::string sha256;   ///< lower-case hex
};

/// The pin compiled into this build.
Pin pinnedModel();

/// "" when [pin] has a hash a file can be checked against; else why not,
/// said to the user.
std::string verifyRefusal(const Pin& pin);
/// "" when [pin] can be downloaded (an https URL and a hash); else why not.
std::string downloadRefusal(const Pin& pin);

/// [configured], or <data dir>/models when it is empty.
std::string modelDir(const std::string& configured);
/// <dir>/<version>/model.gguf
std::string modelPath(const std::string& dir, const Pin& pin);

/// True when the model file is in place and hashes to the pin.
bool installedAndVerified(const std::string& dir, const Pin& pin);

struct InstallResult {
    bool ok = false;
    std::string error;   ///< why not, said to the user
    std::string path;    ///< where the model is, when ok
};

/// Check the file at [src] against [pin] and, when it matches, copy it into
/// place. [src] is left where it is.
InstallResult installFromFile(const std::string& src, const std::string& dir, const Pin& pin);

/// One download: [url] to the file [path]. False on any failure.
using Download = std::function<bool(const std::string& url, const std::string& path)>;

/// Download the pinned model with [download] (libcurl when empty), check it
/// and put it in place. A file that does not match is deleted.
InstallResult downloadModel(const std::string& dir, const Pin& pin, const Download& download = {});

}  // namespace hms_cpap::model
