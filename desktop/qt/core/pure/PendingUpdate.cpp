#include "PendingUpdate.h"

#include <cctype>

#include <nlohmann/json.hpp>

namespace cpapdash::supervisor {

namespace {

bool isHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (unsigned char c : s) {
        if (!std::isdigit(c) && !(c >= 'a' && c <= 'f')) return false;
    }
    return true;
}

std::string kindFor(const std::string& platform) {
    if (platform == "macos-arm64") return "dmg";
    if (platform == "windows-x64") return "installer";
    return "";
}

}  // namespace

std::optional<PendingUpdate> parsePendingUpdate(const std::string& text, std::string* why) {
    auto fail = [why](const std::string& w) -> std::optional<PendingUpdate> {
        if (why) *why = w;
        return std::nullopt;
    };

    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail("pending.json is not readable");

    auto str = [&j](const char* k) {
        return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
    };

    PendingUpdate p;
    p.version  = str("version");
    p.from     = str("from");
    p.file     = str("file");
    p.name     = str("name");
    p.kind     = str("kind");
    p.platform = str("platform");
    p.sha256   = str("sha256");
    p.size     = j.contains("size") && j["size"].is_number_integer() ? j["size"].get<long long>() : 0;

    if (p.version.empty()) return fail("pending.json names no version");
    if (p.file.empty())    return fail("pending.json names no file");
    if (!isHex64(p.sha256)) return fail("pending.json has no valid SHA-256");
    if (p.size <= 0)       return fail("pending.json has no size");
    const std::string expected = kindFor(p.platform);
    if (expected.empty())  return fail("pending.json is for " + p.platform + ", which has no helper");
    if (p.kind != expected) return fail("a " + p.kind + " cannot be installed on " + p.platform);
    return p;
}

HelperLaunch helperLaunch(const HelperParams& p) {
    HelperLaunch l;
    const std::string pid = std::to_string(p.supervisor_pid);
    const std::string port = std::to_string(p.port);
    if (p.platform == "macos-arm64") {
        l.program = "/bin/bash";
        l.args = {p.helper,
                  "--pending", p.pending,
                  "--app", p.install,
                  "--data-dir", p.data_dir,
                  "--pid", pid,
                  "--port", port};
    } else if (p.platform == "windows-x64") {
        l.program = "powershell.exe";
        l.args = {"-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden",
                  "-File", p.helper,
                  "-Pending", p.pending,
                  "-InstallDir", p.install,
                  "-DataDir", p.data_dir,
                  "-SupervisorPid", pid,
                  "-Port", port};
    }
    return l;
}

}  // namespace cpapdash::supervisor
