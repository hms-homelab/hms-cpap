#include "services/As11NightImport.h"

#include "database/SqlDialect.h"
#include "services/RemovedNights.h"
#include "services/SessionDiscoveryService.h"
#include "utils/SessionEnd.h"
#include "utils/SessionFileRefs.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

namespace hms_cpap {

namespace fs = std::filesystem;

namespace {

constexpr const char* kTag = "AirSense11 night: ";

/// A timestamp as every backend stores session_start: local wall time.
std::string wallText(const std::chrono::system_clock::time_point& tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

std::optional<std::chrono::system_clock::time_point> wallFrom(const std::string& text) {
    if (text.size() < 19) return std::nullopt;
    std::tm tm{};
    std::istringstream in(text.substr(0, 19));
    in >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (in.fail()) return std::nullopt;
    tm.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

bool isNightName(const std::string& name) {
    return name.size() == 8 &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); });
}

/// The bytes a stretch's files hold now: what says it grew.
uint64_t bytesOf(const fs::path& dir, const SessionFileSet& s) {
    uint64_t total = 0;
    auto add = [&](const std::vector<std::string>& names) {
        for (const auto& n : names) {
            std::error_code ec;
            const auto sz = fs::file_size(dir / n, ec);
            if (!ec) total += sz;
        }
    };
    add(s.brp_files);
    add(s.pld_files);
    add(s.sad_files);
    add(s.eve_files);
    add(s.csl_files);
    return total;
}

/// The parser merges a directory into one session, so a folder of several
/// stretches is parsed one stretch at a time from a staged copy of its files
/// (the collector does the same for a card folder, SDD-040 D7).
std::string dirToParse(const fs::path& folder, const SessionFileSet& s, bool one_group) {
    if (one_group) return folder.string();
    const fs::path staged = fs::temp_directory_path() / "as11_group" / (s.date_folder + "_" + s.session_prefix);
    std::error_code ec;
    fs::create_directories(staged, ec);
    for (fs::directory_iterator it(staged, ec), end; !ec && it != end; it.increment(ec))
        fs::remove(it->path(), ec);
    auto stage = [&](const std::vector<std::string>& names) {
        for (const auto& n : names) {
            std::error_code fe;
            if (fs::exists(folder / n, fe))
                fs::copy_file(folder / n, staged / n, fs::copy_options::overwrite_existing, fe);
        }
    };
    stage(s.brp_files);
    stage(s.pld_files);
    stage(s.sad_files);
    stage(s.csl_files);
    stage(s.eve_files);
    return staged.string();
}

}  // namespace

As11NightImport::As11NightImport(std::string data_dir, std::shared_ptr<IDatabase> db,
                                 std::string device_id, std::string device_name)
    : data_dir_(std::move(data_dir)),
      db_(std::move(db)),
      device_id_(std::move(device_id)),
      device_name_(std::move(device_name)) {}

std::string As11NightImport::root() const { return (fs::path(data_dir_) / "as11").string(); }

// ── the decisions ────────────────────────────────────────────────────────

bool As11NightImport::overlaps(const std::chrono::system_clock::time_point& a_start,
                               const std::chrono::system_clock::time_point& a_end,
                               const std::chrono::system_clock::time_point& b_start,
                               const std::chrono::system_clock::time_point& b_end) {
    return a_start <= b_end && b_start <= a_end;
}

bool As11NightImport::cardHolds(const std::chrono::system_clock::time_point& start,
                                const std::chrono::system_clock::time_point& end,
                                const std::vector<StoredSpan>& stored) {
    return std::any_of(stored.begin(), stored.end(), [&](const StoredSpan& s) {
        return !s.bluetooth && overlaps(start, end, s.start, s.end);
    });
}

std::vector<std::chrono::system_clock::time_point> As11NightImport::bluetoothRowsReplacedBy(
    const std::chrono::system_clock::time_point& start,
    const std::chrono::system_clock::time_point& end,
    const std::vector<StoredSpan>& stored) {
    std::vector<std::chrono::system_clock::time_point> out;
    for (const auto& s : stored)
        if (s.bluetooth && overlaps(start, end, s.start, s.end)) out.push_back(s.start);
    return out;
}

bool As11NightImport::isBluetoothPath(const std::string& rel_path) {
    return rel_path.rfind(kRelPrefix, 0) == 0;
}

// ── the database side ────────────────────────────────────────────────────

std::vector<As11NightImport::StoredSpan> As11NightImport::storedSpansOverlapping(
    IDatabase& db, const std::string& device_id,
    const std::chrono::system_clock::time_point& start,
    const std::chrono::system_clock::time_point& end) {
    const DbType dt = db.dbType();
    // Sessions that started up to a day before the window can still reach
    // into it; the overlap itself is decided here from each row's span.
    const std::string sql =
        "SELECT " + sql::tsText("session_start", dt) + " AS start, " +
        sql::tsText("session_end", dt) + " AS finish, duration_seconds, brp_file_path"
        " FROM cpap_sessions"
        " WHERE device_id = " + sql::param(1, dt) +
        "   AND session_start >= " + sql::castTimestamp(2, dt) +
        "   AND session_start <= " + sql::castTimestamp(3, dt) +
        " ORDER BY session_start";
    std::vector<StoredSpan> out;
    for (const auto& row : db.executeQuery(sql, {device_id, wallText(start - std::chrono::hours(24)),
                                                 wallText(end)})) {
        const auto s = wallFrom(row["start"].asString());
        if (!s) continue;
        StoredSpan span;
        span.start = *s;
        const Json::Value& finish = row["finish"];
        const Json::Value& dur = row["duration_seconds"];
        const long long secs = dur.isNull() ? 0 : std::atoll(dur.asString().c_str());
        const auto by_span = span.start + std::chrono::seconds(secs);
        auto e = finish.isNull() ? std::nullopt : wallFrom(finish.asString());
        span.end = e ? std::max(*e, by_span) : by_span;
        const Json::Value& brp = row["brp_file_path"];
        span.bluetooth = !brp.isNull() && isBluetoothPath(brp.asString());
        if (overlaps(start, end, span.start, span.end)) out.push_back(span);
    }
    return out;
}

bool As11NightImport::deleteSessionAt(IDatabase& db, const std::string& device_id,
                                      const std::chrono::system_clock::time_point& start) {
    const DbType dt = db.dbType();
    const std::string where =
        " WHERE device_id = " + sql::param(1, dt) +
        "   AND session_start = " + sql::castTimestamp(2, dt);
    const std::vector<std::string> params = {device_id, wallText(start)};
    // The children first, as deleteSessionsByDateFolder does: the cascade is
    // declared on every engine but SQLite only honours it with the pragma on.
    const std::string children =
        "DELETE FROM cpap_session_files WHERE session_id IN (SELECT id FROM cpap_sessions" + where + ")";
    if (!db.executeQueryChecked(children, params).ok) return false;
    return db.executeQueryChecked("DELETE FROM cpap_sessions" + where, params).ok;
}

int As11NightImport::replaceBluetoothRows(IDatabase& db, const std::string& device_id,
                                          const std::chrono::system_clock::time_point& start,
                                          const std::chrono::system_clock::time_point& end) {
    int replaced = 0;
    for (const auto& at : bluetoothRowsReplacedBy(start, end, storedSpansOverlapping(db, device_id, start, end))) {
        if (deleteSessionAt(db, device_id, at)) ++replaced;
    }
    if (replaced > 0)
        std::cout << kTag << "the card's copy replaced " << replaced
                  << " Bluetooth session(s) of the same stretch" << std::endl;
    return replaced;
}

// ── the walk ─────────────────────────────────────────────────────────────

As11NightImport::Result As11NightImport::run(const std::set<std::string>& removed) {
    Result r;
    if (!db_) return r;
    std::error_code ec;
    const fs::path root = this->root();
    if (!fs::is_directory(root, ec)) return r;

    std::vector<fs::path> machines;
    for (const auto& m : fs::directory_iterator(root, ec))
        if (m.is_directory()) machines.push_back(m.path());
    std::sort(machines.begin(), machines.end());

    for (const auto& machine : machines) {
        const std::string serial = machine.filename().string();
        const fs::path datalog = machine / "DATALOG";
        std::vector<std::string> nights;
        for (const auto& n : fs::directory_iterator(datalog, ec))
            if (n.is_directory() && isNightName(n.path().filename().string()))
                nights.push_back(n.path().filename().string());
        std::sort(nights.begin(), nights.end());

        // Every stretch of every night, oldest first; the last is the newest.
        struct Stretch { std::string night; SessionFileSet set; bool one_group; };
        std::vector<Stretch> stretches;
        for (const auto& night : nights) {
            auto groups = SessionDiscoveryService::groupLocalFolder((datalog / night).string(), night);
            for (auto& g : groups) stretches.push_back({night, std::move(g), groups.size() == 1});
        }

        for (std::size_t i = 0; i < stretches.size(); ++i) {
            const auto& st = stretches[i];
            const auto& set = st.set;
            const auto start = set.session_start;
            if (isRemovedNight(removed, start)) continue;   // SDD-029

            const fs::path folder = datalog / st.night;
            const std::string key = serial + "/" + st.night + "/" + set.session_prefix;
            const uint64_t bytes = bytesOf(folder, set);
            const bool newest = i + 1 == stretches.size();
            const bool stored = db_->sessionExists(device_id_, start);
            if (stored) {
                const auto seen = seen_.find(key);
                const bool grew = seen == seen_.end() ? newest : bytes > seen->second;
                if (!grew) {
                    seen_[key] = bytes;
                    continue;
                }
            } else if (seen_.count(key) && seen_[key] == bytes) {
                continue;   // left to the card last time, and unchanged since
            }

            auto parsed = EDFParser::parseSession(dirToParse(folder, set, st.one_group),
                                                  device_id_, device_name_, start);
            if (!parsed) {
                std::cerr << kTag << "could not parse " << serial << " " << st.night << " "
                          << set.session_prefix << std::endl;
                ++r.failed;
                seen_[key] = bytes;
                continue;
            }
            keepOwnEvents(*parsed, set);   // SDD-047: this stretch's events
            const auto end = dataEndOf(*parsed).value_or(start);

            // D5: the card wins the stretch it carries.
            if (cardHolds(start, end, storedSpansOverlapping(*db_, device_id_, start, end))) {
                if (stored) deleteSessionAt(*db_, device_id_, start);
                std::cout << kTag << st.night << " " << set.session_prefix
                          << " is on the card: the card's copy is the session" << std::endl;
                ++r.card_wins;
                seen_[key] = bytes;
                continue;
            }

            const std::string rel_base = std::string(kRelPrefix) + serial + "/DATALOG/" + st.night + "/";
            auto refs = recordSessionFiles(folder.string(), rel_base, *parsed);
            if (!db_->saveSession(*parsed)) {
                std::cerr << kTag << "could not save " << st.night << " " << set.session_prefix << std::endl;
                ++r.failed;
                continue;
            }
            db_->replaceSessionFiles(device_id_, start, refs);
            closeWithDataEnd(*db_, device_id_, start, *parsed);   // SDD-037 D2
            // SDD-026: the night's daily row follows its sessions.
            db_->aggregateDailySummaryFromSessions(device_id_);
            seen_[key] = bytes;
            if (stored) ++r.grown; else ++r.imported;
            r.saved.push_back(*parsed);
            std::cout << kTag << (stored ? "read again " : "imported ") << serial << " " << st.night
                      << " " << set.session_prefix << " (" << parsed->duration_seconds.value_or(0)
                      << " s)" << std::endl;
        }
    }
    return r;
}

}  // namespace hms_cpap
