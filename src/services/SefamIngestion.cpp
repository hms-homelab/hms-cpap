#include "services/SefamIngestion.h"

#include <algorithm>
#include <filesystem>
#include <iostream>

namespace hms_cpap {

namespace fs = std::filesystem;

namespace {

// A Sefam session sits two levels below the card root
// (<model><serial>/DATA_<n>/), and callers point at anything from the card root
// to the session folder itself. Four levels covers all of it without walking a
// whole disk if someone points at the wrong place.
constexpr int kMaxDepth = 4;

} // namespace

SefamIngestion::SefamIngestion(const std::string& data_dir)
    : data_dir_(data_dir) {}

void SefamIngestion::walk(const std::string& dir, int depth) {
    std::error_code ec;
    if (depth < 0 || !fs::is_directory(dir, ec)) return;

    for (const auto& stem : cpapdash::parser::SefamParser::listSessionStems(dir)) {
        SefamSessionFile s;
        s.dir = dir;
        s.stem = stem;

        // The INI is a couple of kilobytes of text and carries the start time,
        // so discovery never has to open a channel file. A card holding ten
        // months of nights is walked in a moment.
        const auto ini = cpapdash::parser::SefamParser::parseIni(
            (fs::path(dir) / (stem + ".INI")).string());

        // Case matters on Linux, and the two layouts disagree: DATA_<n>.INI is
        // upper, <HHMMSS>.ini is lower.
        const auto ini2 = ini.valid ? ini
            : cpapdash::parser::SefamParser::parseIni(
                  (fs::path(dir) / (stem + ".ini")).string());

        if (!ini2.valid || !ini2.start) continue;

        s.session_start = *ini2.start;
        if (serial_.empty()) serial_ = ini2.serial_number;
        if (model_.empty()) model_ = ini2.created_by;

        sessions_.push_back(std::move(s));
    }

    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec)) walk(e.path().string(), depth - 1);
}

bool SefamIngestion::initialize() {
    if (initialized_) return !sessions_.empty();

    std::error_code ec;
    if (!fs::is_directory(data_dir_, ec)) {
        std::cerr << "SefamIngestion: not a directory: " << data_dir_ << std::endl;
        return false;
    }

    sessions_.clear();
    walk(data_dir_, kMaxDepth);

    std::sort(sessions_.begin(), sessions_.end(),
              [](const SefamSessionFile& a, const SefamSessionFile& b) {
                  return a.session_start < b.session_start;
              });

    initialized_ = true;

    if (sessions_.empty()) {
        std::cerr << "SefamIngestion: no Sefam session manifests under " << data_dir_
                  << std::endl;
        return false;
    }

    std::cout << "SefamIngestion: " << sessions_.size() << " session(s) on "
              << (model_.empty() ? "a Sefam device" : model_)
              << " " << serial_ << std::endl;
    return true;
}

std::vector<SefamSessionFile> SefamIngestion::discoverSessions(
    std::optional<std::chrono::system_clock::time_point> last_session_start)
{
    if (!initialized_ && !initialize()) return {};

    std::vector<SefamSessionFile> out;
    for (const auto& s : sessions_) {
        if (last_session_start && s.session_start <= *last_session_start) continue;
        out.push_back(s);
    }
    return out;
}

} // namespace hms_cpap
