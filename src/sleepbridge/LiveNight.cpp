#include "sleepbridge/LiveNight.h"

#include "parsers/CpapdashBridge.h"
#include "services/BmcIngestion.h"

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

/// The extension of a file at the card root, lower case; "" for one below it.
std::string rootExtension(const std::string& path) {
    if (path.find('/') != std::string::npos) return {};
    std::string ext = std::filesystem::path(path).extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

/// SDD-058 D1: a Luna's <serial>.000 (.001 ...), which grows while it treats.
bool isLunaData(const std::string& path) {
    const std::string ext = rootExtension(path);
    return ext.size() == 4 && std::isdigit(static_cast<unsigned char>(ext[1])) &&
           std::isdigit(static_cast<unsigned char>(ext[2])) &&
           std::isdigit(static_cast<unsigned char>(ext[3]));
}

bool isLunaEvents(const std::string& path) { return rootExtension(path) == ".evt"; }

#ifdef CPAPDASH_WITH_BMC
/// A Luna session's start as a ResMed names a session file: YYYYMMDD_HHMMSS,
/// the packets' own wall clock.
std::string lunaStem(cpapdash::parser::BmcCivilSeconds start) {
    const std::time_t t = static_cast<std::time_t>(start);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

/// The live machine label of a Luna event, as liveEventType reads it.
const char* lunaDescription(EventType t) {
    switch (t) {
        case EventType::OBSTRUCTIVE:  return "Obstructive apnea";
        case EventType::CLEAR_AIRWAY: return "Clear airway apnea";
        case EventType::CENTRAL:      return "Central apnea";
        case EventType::HYPOPNEA:     return "Hypopnea";
        default:                      return "Apnea";
    }
}
#endif

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

LiveNight::LiveNight(std::string card_root, LiveSink sink, std::chrono::seconds mask_quiet,
                     std::chrono::seconds stall_limit, bool computed_events)
    : root_(std::move(card_root)),
      sink_(std::move(sink)),
      mask_quiet_(mask_quiet),
      stall_limit_(std::max(stall_limit, mask_quiet)),
      computed_events_(computed_events) {}

void LiveNight::setMask(bool on) {
    if (mask_on_ == on) return;
    mask_on_ = on;
    if (sink_.mask) sink_.mask(on);
    if (!sink_.mask_change) return;
#ifdef CPAPDASH_WITH_BMC
    if (isLunaData(therapy_file_)) {
        // SDD-058 D1: the newest session on the card, from its own packets:
        // its first second for on, the second after its last for off.
        std::string session;
        Clock::time_point occurred = last_therapy_write_;
        if (const auto card = BmcIngestion::readCard(root_); card && !card->sessions.empty()) {
            const auto& span = card->sessions.back();
            session  = lunaStem(span.start);
            occurred = cpapdash::parser::bmcToTimePoint(on ? span.start : span.end + 1);
        }
        sink_.mask_change(on, session, occurred);
        return;
    }
#endif
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
    bool luna_data = false, luna_events = false;
    for (const auto& p : paths) {
        if (isLunaData(p)) luna_data = true;
        if (isLunaEvents(p)) {
            luna_events = true;
            luna_events_file_ = p;
        }
    }
    if (catch_up) {
        for (const auto& p : paths)
            if (isEventFile(p)) readEvents(p, /*report=*/false);
        if (luna_data || luna_events)
            readLunaEvents(true, computed_events_, /*report=*/false);
        return;
    }
    bool therapy = false, str = false;
    for (const auto& p : paths) {
        if (isLunaData(p)) {
            // SDD-058 D1: the Luna's one data file growing is the machine
            // treating; the session is found in its packets.
            therapy = true;
            therapy_file_ = p;
        } else if (isTherapyWrite(p)) {
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
    if (luna_events || (luna_data && computed_events_))
        readLunaEvents(luna_events, luna_data && computed_events_, /*report=*/true);
    if (total_ != before && sink_.counts) sink_.counts(counts_, total_);
    // The STR written after therapy is the machine closing the session.
    if (str && !therapy && mask_on_) setMask(false);
}

void LiveNight::onDelivered(Clock::time_point now) { last_delivery_ = now; }

void LiveNight::tick(Clock::time_point now) {
    if (!mask_on_) return;
    const auto quiet = now - last_therapy_write_;
    if (quiet < mask_quiet_) return;
    // The stream is alive and had no therapy to bring: a batch landed after
    // the last therapy write, long enough ago that its rebuild has run.
    const bool delivering = last_delivery_ > last_therapy_write_ &&
                            now - last_delivery_ >= kDeliverySettled;
    // Nothing at all since the last therapy write: a stall looks the same as
    // an end, so wait for the longer limit.
    if (delivering || quiet >= stall_limit_) setMask(false);
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
        this->report(std::move(e));
    }
    seen = annotations.size();
}

void LiveNight::report(LiveEvent e) {
    ++counts_[e.type];
    ++total_;
    if (sink_.event) sink_.event(e);
}

void LiveNight::readLunaEvents(bool machine, bool computed, bool report_new) {
#ifdef CPAPDASH_WITH_BMC
    namespace p = cpapdash::parser;
    const auto card = BmcIngestion::readCard(root_);
    if (!card) return;
    // Every session when only marking what is there; the newest when
    // reporting, the one the machine is writing or has just stopped.
    const size_t from = report_new && !card->sessions.empty() ? card->sessions.size() - 1 : 0;
    for (size_t i = from; i < card->sessions.size(); ++i) {
        const auto& span = card->sessions[i];
        const long long key = span.start;
        const auto emit = [&](const std::vector<CPAPEvent>& found, size_t& seen, bool is_computed,
                              const std::string& file) {
            // Fewer than reported: a card swapped under the same name.
            if (found.size() < seen) seen = 0;
            if (report_new) {
                for (size_t k = seen; k < found.size(); ++k) {
                    LiveEvent e;
                    e.description = lunaDescription(found[k].event_type);
                    if (is_computed) e.description += " (computed from flow)";
                    e.type       = liveEventType(e.description);
                    e.at         = found[k].timestamp;
                    e.duration_s = found[k].duration_seconds;
                    e.file       = file;
                    e.computed   = is_computed;
                    if (!e.type.empty()) report(std::move(e));
                }
            }
            seen = found.size();
        };
        if (machine) {
            const auto s = p::BmcParser::sessionFromCard(*card, span, {}, {});
            if (s) emit(s->events, luna_machine_seen_[key], false, luna_events_file_);
        }
        if (computed)
            emit(p::BmcParser::flowApneas(*card, span), luna_computed_seen_[key], true,
                 therapy_file_);
    }
#else
    (void)machine;
    (void)computed;
    (void)report_new;
#endif
}

}  // namespace hms_cpap::sleepbridge
