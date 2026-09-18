#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cpapdash::supervisor {

/**
 * SDD-041 §3.4: the service staged an update and stepped aside.
 *
 * The service downloads the release file, checks it against the release's
 * manifest, writes <data_dir>/update/pending.json, and exits with
 * kExitPendingUpdate. The supervisor then starts the helper that swaps the
 * whole install (D8, D9) and quits, because the swap replaces the supervisor
 * too.
 *
 * This is the part of that handoff that needs no Qt: what pending.json must say
 * before anything acts on it, and exactly how the helper is invoked. The Qt side
 * reads the file, hashes the download a second time, and launches what this
 * builds.
 *
 * Pure: no Qt, no filesystem, so run_tests pins it on every platform.
 */

/// Must match hms_cpap::kExitApplyUpdate in the service's UpdateService.h.
/// A different number here would turn every update into a "service failed".
constexpr int kExitPendingUpdate = 42;

struct PendingUpdate {
    std::string version;    ///< what is being installed
    std::string from;       ///< what is running now
    std::string file;       ///< absolute path of the verified download
    std::string name;
    std::string kind;       ///< dmg (macOS) or installer (Windows)
    std::string platform;   ///< macos-arm64 or windows-x64
    std::string sha256;     ///< lowercase hex, from the manifest
    long long size = 0;
};

/// Parse and validate pending.json. Every field has to be present and sane,
/// and the kind has to be the one this platform installs, because the helper
/// acts on it without asking again. On failure [why] names the problem.
std::optional<PendingUpdate> parsePendingUpdate(const std::string& json, std::string* why);

/// How the helper is started, as a program and its arguments. Built here so the
/// exact command line is tested rather than assembled in a slot.
struct HelperLaunch {
    std::string program;
    std::vector<std::string> args;
};

struct HelperParams {
    std::string platform;       ///< macos-arm64 or windows-x64
    std::string helper;         ///< the helper's COPY outside the install
    std::string pending;        ///< path of pending.json
    std::string install;        ///< the .app bundle on macOS, the install folder on Windows
    std::string data_dir;       ///< where result.json goes
    long long supervisor_pid = 0;
    int port = 8893;            ///< where /health will answer
};

/// Empty program when the platform has no helper.
HelperLaunch helperLaunch(const HelperParams& p);

}  // namespace cpapdash::supervisor
