#include "clients/As11Capture.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>

namespace hms_cpap::as11 {

namespace {

std::string utcStamp(std::time_t t, const char* fmt) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

std::string nowIso() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    char tail[8];
    std::snprintf(tail, sizeof(tail), ".%03dZ", static_cast<int>(ms));
    return utcStamp(system_clock::to_time_t(now), "%Y-%m-%dT%H:%M:%S") + tail;
}

}  // namespace

std::string As11Capture::pathFor(const std::string& data_dir, const std::string& serial,
                                 std::time_t when) {
    return (std::filesystem::path(data_dir) / "as11" / (serial.empty() ? "unknown" : serial) /
            "capture" / (utcStamp(when, "%Y%m%d-%H%M%S") + ".jsonl"))
        .string();
}

bool As11Capture::open(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_);
    if (out_.is_open()) out_.close();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) return false;
    path_ = path;
    bytes_ = 0;
    return true;
}

void As11Capture::line(const std::string& s) {
    if (!out_.is_open()) return;
    out_ << s << '\n';
    out_.flush();   // a night cut short keeps what it had
    bytes_ += static_cast<long long>(s.size() + 1);
}

void As11Capture::record(const char* dir, const std::string& text) {
    nlohmann::ordered_json j;
    j["t"] = nowIso();
    j["dir"] = dir;
    auto parsed = nlohmann::ordered_json::parse(text, nullptr, false);
    if (parsed.is_discarded()) j["msg"] = text;
    else j["msg"] = std::move(parsed);
    std::lock_guard<std::mutex> lk(m_);
    line(j.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));
}

void As11Capture::note(const std::string& what) {
    nlohmann::ordered_json j;
    j["t"] = nowIso();
    j["dir"] = "note";
    j["note"] = what;
    std::lock_guard<std::mutex> lk(m_);
    line(j.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));
}

void As11Capture::close(bool keep) {
    std::lock_guard<std::mutex> lk(m_);
    if (!out_.is_open()) return;
    out_.close();
    if (keep) {
        kept_path_ = path_;
        kept_bytes_ = bytes_.load();
    } else {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    path_.clear();
    bytes_ = 0;
}

bool As11Capture::isOpen() const {
    std::lock_guard<std::mutex> lk(m_);
    return out_.is_open();
}

std::string As11Capture::path() const {
    std::lock_guard<std::mutex> lk(m_);
    return out_.is_open() ? path_ : kept_path_;
}

long long As11Capture::bytes() const {
    std::lock_guard<std::mutex> lk(m_);
    return out_.is_open() ? bytes_.load() : kept_bytes_;
}

}  // namespace hms_cpap::as11
