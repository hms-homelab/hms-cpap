#pragma once
//
// A firmware update as a job: one update of one unit, made of steps, one per
// image (a Mule & Miner "both" is two steps, mule then miner; a Sleep Bridge
// is one). The same object for every path and page:
//
//   {"job_id", "serial", "kind": "mule_miner"|"tapper", "path": "local",
//    "origin": "owner", "source": "release 1.0.2" | "file x.bin" | ...,
//    "confirmed_while_recording", "phase" (the current step's),
//    "step" (1-based), "steps", "steps_detail": [{"target", "phase",
//    "bytes", "total", "bytes_meaning", "version_before",
//    "version_target", "version_after"}], "hold_reason", "held_since",
//    "reason", "detail", "last_heard", "started_at", "updated_at",
//    "ended_at"}
//
// Phases of a step: queued, preparing (the image being made ready: read
// from the release, downloaded, checked), transferring (bytes of total),
// verifying (every byte went; the unit is checking), restarting (the unit
// took it and restarts into it), done, failed, cancelled, and skipped (a
// step the unit did not need). Nothing here judges by a clock: a unit that
// does not come back stays "restarting", with the unit's last word as a
// fact, until an event (its hello) resolves the step.
//
// `reason` is a fixed word the page translates: refused_image,
// connection_lost, back_on_old_version, image_unavailable, image_rejected
// (a downloaded image that is not the one asked for: nothing was sent),
// offline, interrupted; `detail` is the device's or server's own text.
//
// A job is kept as one JSON file per unit, written at every phase change, so
// a restart of this service reads it back: a job it finds before
// "restarting" was cut by the restart (failed, interrupted); one found
// "restarting" stays, and the unit's next hello resolves it.
//
// Header-only: the device services and their socket test share it.
//
#include <json/json.h>

#include <cstddef>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace hms_cpap::devices {

struct OtaStep {
    std::string target;                 // mule, miner, tapper
    std::string phase{"queued"};
    size_t      bytes{0};
    size_t      total{0};
    std::string bytes_meaning{"sent"};  // sent: handed to the connection or the unit's httpd
    std::string version_before;
    std::string version_target;
    std::string version_after;
};

struct OtaJob {
    std::string job_id;
    std::string serial;
    std::string kind;                   // mule_miner, tapper
    std::string path{"local"};
    std::string origin{"owner"};
    std::string source;
    bool        confirmed_while_recording{false};
    std::vector<OtaStep> steps;
    size_t      step{0};                // index of the current step
    std::string reason;
    std::string detail;
    std::time_t last_heard{0};
    std::time_t started_at{0};
    std::time_t updated_at{0};
    std::time_t ended_at{0};

    OtaStep* current() { return step < steps.size() ? &steps[step] : nullptr; }
    const OtaStep* current() const { return step < steps.size() ? &steps[step] : nullptr; }
    std::string phase() const { return current() ? current()->phase : std::string("done"); }
    bool ended() const {
        const std::string p = phase();
        return p == "done" || p == "failed" || p == "cancelled";
    }
};

inline bool otaRetryable(const std::string& reason) {
    return reason == "refused_image" || reason == "connection_lost" || reason == "back_on_old_version" ||
           reason == "image_unavailable" || reason == "interrupted";
}

inline Json::Value otaJson(const OtaJob& j) {
    Json::Value v(Json::objectValue);
    v["job_id"]                    = j.job_id;
    v["serial"]                    = j.serial;
    v["kind"]                      = j.kind;
    v["path"]                      = j.path;
    v["origin"]                    = j.origin;
    v["source"]                    = j.source;
    v["confirmed_while_recording"] = j.confirmed_while_recording;
    v["phase"]                     = j.phase();
    v["step"]                      = static_cast<Json::UInt64>(j.step + 1);
    v["steps"]                     = static_cast<Json::UInt64>(j.steps.size());
    Json::Value steps(Json::arrayValue);
    for (const auto& s : j.steps) {
        Json::Value d(Json::objectValue);
        d["target"]         = s.target;
        d["phase"]          = s.phase;
        d["bytes"]          = static_cast<Json::UInt64>(s.bytes);
        d["total"]          = static_cast<Json::UInt64>(s.total);
        d["bytes_meaning"]  = s.bytes_meaning;
        d["version_before"] = s.version_before;
        d["version_target"] = s.version_target;
        d["version_after"]  = s.version_after;
        steps.append(d);
    }
    v["steps_detail"] = steps;
    v["hold_reason"]  = Json::Value(Json::nullValue);   // nothing local is ever held
    v["held_since"]   = Json::Value(Json::nullValue);
    v["reason"]       = j.reason.empty() ? Json::Value(Json::nullValue) : Json::Value(j.reason);
    v["detail"]       = j.detail.empty() ? Json::Value(Json::nullValue) : Json::Value(j.detail);
    v["retryable"]    = j.phase() == "failed" && otaRetryable(j.reason);
    v["last_heard"]   = static_cast<Json::Int64>(j.last_heard);
    v["started_at"]   = static_cast<Json::Int64>(j.started_at);
    v["updated_at"]   = static_cast<Json::Int64>(j.updated_at);
    v["ended_at"]     = j.ended_at ? Json::Value(static_cast<Json::Int64>(j.ended_at)) : Json::Value(Json::nullValue);
    return v;
}

inline std::optional<OtaJob> otaFromJson(const Json::Value& v) {
    if (!v.isObject() || !v["job_id"].isString() || !v["steps_detail"].isArray()) return std::nullopt;
    OtaJob j;
    j.job_id                    = v["job_id"].asString();
    j.serial                    = v.get("serial", "").asString();
    j.kind                      = v.get("kind", "").asString();
    j.path                      = v.get("path", "local").asString();
    j.origin                    = v.get("origin", "owner").asString();
    j.source                    = v.get("source", "").asString();
    j.confirmed_while_recording = v.get("confirmed_while_recording", false).asBool();
    for (const auto& d : v["steps_detail"]) {
        if (!d.isObject()) return std::nullopt;
        OtaStep s;
        s.target         = d.get("target", "").asString();
        s.phase          = d.get("phase", "queued").asString();
        s.bytes          = static_cast<size_t>(d.get("bytes", 0).asUInt64());
        s.total          = static_cast<size_t>(d.get("total", 0).asUInt64());
        s.bytes_meaning  = d.get("bytes_meaning", "sent").asString();
        s.version_before = d.get("version_before", "").asString();
        s.version_target = d.get("version_target", "").asString();
        s.version_after  = d.get("version_after", "").asString();
        j.steps.push_back(s);
    }
    const auto step = v.get("step", 1).asUInt64();
    j.step       = step >= 1 ? static_cast<size_t>(step - 1) : 0;
    j.reason     = v["reason"].isString() ? v["reason"].asString() : "";
    j.detail     = v["detail"].isString() ? v["detail"].asString() : "";
    j.last_heard = static_cast<std::time_t>(v.get("last_heard", 0).asInt64());
    j.started_at = static_cast<std::time_t>(v.get("started_at", 0).asInt64());
    j.updated_at = static_cast<std::time_t>(v.get("updated_at", 0).asInt64());
    j.ended_at   = v["ended_at"].isNumeric() ? static_cast<std::time_t>(v["ended_at"].asInt64()) : 0;
    return j;
}

/// A job read back after this service restarted: one cut before the unit
/// took the image failed, interrupted; one restarting waits for the hello.
inline void otaAfterRestart(OtaJob& j, std::time_t now) {
    if (j.ended()) return;
    OtaStep* s = j.current();
    if (!s || s->phase == "restarting") return;
    s->phase   = "failed";
    j.reason   = "interrupted";
    j.detail   = "hms-cpap restarted during the update";
    j.updated_at = now;
    j.ended_at = now;
}

/// <dir>/<id>.json, written to a temporary file and renamed over the old
/// one. False (and nothing written) when the directory cannot be made.
inline bool otaSave(const std::string& dir, const std::string& id, const OtaJob& j) {
    if (dir.empty() || id.empty()) return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    const std::string text = Json::writeString(w, otaJson(j));
    const fs::path    dest = fs::path(dir) / (id + ".json");
    const fs::path    tmp  = fs::path(dir) / (id + ".json.part");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return false;
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

inline std::optional<OtaJob> otaLoad(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream ss(text);
    if (!Json::parseFromStream(rb, ss, &v, nullptr)) return std::nullopt;
    return otaFromJson(v);
}

/// Every <id>.json in `dir`, as (id, job).
inline std::vector<std::pair<std::string, OtaJob>> otaLoadAll(const std::string& dir) {
    std::vector<std::pair<std::string, OtaJob>> out;
    if (dir.empty()) return out;
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        if (auto j = otaLoad(e.path().string())) out.emplace_back(e.path().stem().string(), *j);
    }
    return out;
}

}  // namespace hms_cpap::devices
