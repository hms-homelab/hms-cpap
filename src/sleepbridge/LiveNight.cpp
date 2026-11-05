#include "sleepbridge/LiveNight.h"

#include "parsers/CpapdashBridge.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <optional>

namespace hms_cpap::sleepbridge {

namespace {

bool endsWith(const std::string& s, const char* suffix) {
    const std::string suf(suffix);
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool underDatalog(const std::string& path) { return path.rfind("DATALOG/", 0) == 0; }

/// A file the machine writes while therapy runs.
bool isTherapyWrite(const std::string& path) {
    return underDatalog(path) &&
           (endsWith(path, "_BRP.edf") || endsWith(path, "_PLD.edf") || endsWith(path, "_SA2.edf") ||
            endsWith(path, "_SAD.edf") || endsWith(path, "_TCV.edf"));
}

bool isEventFile(const std::string& path) {
    return underDatalog(path) && (endsWith(path, "_EVE.edf") || endsWith(path, "_CSL.edf"));
}

/// The YYYYMMDD_HHMMSS a session file is named for, "" when it has none.
std::string stemOf(const std::string& path) {
    const std::string name = std::filesystem::path(path).filename().string();
    if (name.size() < 15 || name[8] != '_') return {};
    for (int i = 0; i < 15; ++i)
        if (i != 8 && !std::isdigit(static_cast<unsigned char>(name[i]))) return {};
    return name.substr(0, 15);
}

/// The stem as a moment, read in the host's zone (the machine writes its
/// names in its own, which at home is the host's).
std::optional<std::chrono::system_clock::time_point> stemTime(const std::string& stem) {
    if (stem.size() != 15) return std::nullopt;
    std::tm tm{};
    tm.tm_year  = std::stoi(stem.substr(0, 4)) - 1900;
    tm.tm_mon   = std::stoi(stem.substr(4, 2)) - 1;
    tm.tm_mday  = std::stoi(stem.substr(6, 2));
    tm.tm_hour  = std::stoi(stem.substr(9, 2));
    tm.tm_min   = std::stoi(stem.substr(11, 2));
    tm.tm_sec   = std::stoi(stem.substr(13, 2));
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1)) return std::nullopt;
    return std::chrono::system_clock::from_time_t(t);
}

}  // namespace

std::string liveEventType(const std::string& description) {
    std::string d;
    for (const char c : description) d.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (d.empty() || d.find("recording") != std::string::npos) return {};
    if (d.find("obstructive") != std::string::npos) return "OA";
    if (d.find("central") != std::string::npos || d.find("clear airway") != std::string::npos) return "CA";
    if (d.find("hypopnea") != std::string::npos) return "H";
    if (d.find("rera") != std::string::npos || d.find("arousal") != std::string::npos) return "RERA";
    if (d.find("cheyne") != std::string::npos || d.find("csr") != std::string::npos ||
        d.find("periodic") != std::string::npos)
        return "CSR";
    if (d.find("flow lim") != std::string::npos) return "FL";
    if (d.find("apnea") != std::string::npos) return "UA";
    return "OTHER";
}

LiveNight::LiveNight(std::string card_root, LiveSink sink, std::chrono::seconds mask_quiet)
    : root_(std::move(card_root)), sink_(std::move(sink)), mask_quiet_(mask_quiet) {}

void LiveNight::setMask(bool on) {
    if (mask_on_ == on) return;
    mask_on_ = on;
    if (sink_.mask) sink_.mask(on);
    if (!sink_.mask_change) return;
    const std::string stem  = stemOf(therapy_file_);
    const auto        start = stemTime(stem);
    // Unknown: the last write seen stands in.
    Clock::time_point occurred = last_therapy_write_;
    if (on && start) {
        occurred = *start;
    } else if (!on && start) {
        // The end of the session's last record, from its header.
        EDFFile edf;
        if (edf.open((std::filesystem::path(root_) / therapy_file_).string()) &&
            edf.record_duration > 0 && edf.actual_records > 0)
            occurred = *start + std::chrono::milliseconds(static_cast<int64_t>(
                                    edf.actual_records * edf.record_duration * 1000));
    }
    sink_.mask_change(on, stem, occurred);
}

void LiveNight::onWritten(const std::vector<std::string>& paths, Clock::time_point now,
                          bool catch_up) {
    if (catch_up) {
        for (const auto& p : paths)
            if (isEventFile(p)) readEvents(p, /*report=*/false);
        return;
    }
    bool therapy = false, str = false;
    for (const auto& p : paths) {
        if (isTherapyWrite(p)) {
            therapy = true;
            // The newest session's file, its BRP when it has one.
            const std::string s = stemOf(p), cur = stemOf(therapy_file_);
            if (therapy_file_.empty() || s > cur || (s == cur && endsWith(p, "_BRP.edf")))
                therapy_file_ = p;
        } else if (p == "STR.edf") {
            str = true;
        }
    }
    if (therapy) {
        last_therapy_write_ = now;
        setMask(true);
    }
    int before = total_;
    for (const auto& p : paths)
        if (isEventFile(p)) readEvents(p, /*report=*/true);
    if (total_ != before && sink_.counts) sink_.counts(counts_, total_);
    // The STR written after therapy is the machine closing the session.
    if (str && !therapy && mask_on_) setMask(false);
}

void LiveNight::tick(Clock::time_point now) {
    if (mask_on_ && now - last_therapy_write_ >= mask_quiet_) setMask(false);
}

void LiveNight::readEvents(const std::string& path, bool report) {
    EDFFile edf;
    if (!edf.open((std::filesystem::path(root_) / path).string())) return;
    const auto annotations = edf.readAnnotations();
    auto&      seen        = seen_[path];
    // A file with fewer annotations than reported is a new file under an old
    // name (a card swapped, a folder rebuilt): start over on it.
    if (annotations.size() < seen) seen = 0;
    if (!report) {
        seen = annotations.size();
        return;
    }
    const auto start = edf.getStartTime();
    for (size_t i = seen; i < annotations.size(); i++) {
        const auto& a    = annotations[i];
        const auto  type = liveEventType(a.description);
        if (type.empty()) continue;
        LiveEvent e;
        e.type        = type;
        e.description = a.description;
        e.at          = start + std::chrono::milliseconds(static_cast<int64_t>(a.onset_sec * 1000));
        e.duration_s  = a.duration_sec > 0 ? a.duration_sec : 0;
        e.file        = path;
        ++counts_[type];
        ++total_;
        if (sink_.event) sink_.event(e);
    }
    seen = annotations.size();
}

}  // namespace hms_cpap::sleepbridge
