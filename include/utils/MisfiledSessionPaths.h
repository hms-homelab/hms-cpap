#pragma once

// Issue #126: the stored paths a pre-7.3.1 collector got wrong, put right.
//
// The collector filed a session under the calendar date it started on, but the
// card's folders run noon to noon: the night of 10-30 that started at 00:41 on
// the 31st has its files in DATALOG/20261030 and was stored as
// DATALOG/20261031/... A path like that names no file, so a reader that opens
// it (the SleepHQ export) skips the night's files.
//
// A path is changed only when it names no file under [root] and the same file
// is in the folder next to it (the day before, or the day after for a machine
// that files a late evening under the next day). Paths only; no file is moved
// and no row is deleted but a duplicate file row. Run again, it finds nothing:
// every path it changed now names a file.

#include "database/IDatabase.h"
#include "database/SqlDialect.h"
#include "utils/TimeCompat.h"

#include <ctime>
#include <filesystem>
#include <set>
#include <string>
#include <utility>

namespace hms_cpap {
namespace misfiled_paths {

/// The date folder [days] from [folder] ("20261031", -1 -> "20261030"), or ""
/// when [folder] is not a date folder.
inline std::string shiftFolder(const std::string& folder, int days) {
    if (sql::nightOfFolder(folder).empty()) return "";
    std::tm tm{};
    tm.tm_year = std::stoi(folder.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(folder.substr(4, 2)) - 1;
    tm.tm_mday = std::stoi(folder.substr(6, 2)) + days;
    tm.tm_hour = 12;
    const std::time_t t = timegm_utc(&tm);
    std::tm out{};
    gmtime_r(&t, &out);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y%m%d", &out);
    return buf;
}

/// Where "DATALOG/<folder>/<name>" really is under [root]: [rel] itself when
/// the file is there or nowhere near, otherwise the neighbouring folder's path.
inline std::string correctedPath(const std::string& root, const std::string& rel) {
    namespace fs = std::filesystem;
    static const std::string kPrefix = "DATALOG/";
    if (rel.rfind(kPrefix, 0) != 0) return rel;
    const auto slash = rel.find('/', kPrefix.size());
    if (slash == std::string::npos) return rel;
    const std::string folder = rel.substr(kPrefix.size(), slash - kPrefix.size());
    const std::string name = rel.substr(slash + 1);
    if (name.empty() || name.find('/') != std::string::npos) return rel;
    std::error_code ec;
    if (fs::exists(fs::path(root) / rel, ec)) return rel;
    for (int days : {-1, 1}) {
        const std::string other = shiftFolder(folder, days);
        if (other.empty()) return rel;
        const std::string candidate = kPrefix + other + "/" + name;
        if (fs::exists(fs::path(root) / candidate, ec)) return candidate;
    }
    return rel;
}

inline std::string text(const Json::Value& v) {
    if (v.isNull()) return "";
    if (v.isString()) return v.asString();
    if (v.isIntegral()) return std::to_string(v.asLargestInt());
    return v.asString();
}

struct Result {
    int file_rows = 0;      ///< cpap_session_files rows whose path changed
    int session_paths = 0;  ///< cpap_sessions *_file_path values that changed
};

/// Put right every card path of [device_id] that names no file under [root]
/// while its file is in the folder next to it.
inline Result repair(IDatabase& db, const std::string& device_id, const std::string& root) {
    Result r;
    std::error_code ec;
    if (root.empty() || !std::filesystem::exists(std::filesystem::path(root) / "DATALOG", ec))
        return r;
    const DbType t = db.dbType();
    const std::string p1 = sql::param(1, t);
    const std::string p2 = sql::param(2, t);

    // The file rows. A session that already has the right path keeps that row
    // and loses the wrong one (UNIQUE (session_id, rel_path)).
    const auto files = db.executeQuery(
        "SELECT f.id AS id, f.session_id AS sid, f.rel_path AS p FROM cpap_session_files f"
        " JOIN cpap_sessions s ON s.id = f.session_id"
        " WHERE s.device_id = " + p1 +
        " AND f.rel_path LIKE 'DATALOG/%' AND COALESCE(f.source, 'card') = 'card'",
        {device_id});
    std::set<std::pair<std::string, std::string>> held;
    for (const auto& row : files) held.insert({text(row["sid"]), text(row["p"])});
    for (const auto& row : files) {
        const std::string id = text(row["id"]);
        const std::string sid = text(row["sid"]);
        const std::string rel = text(row["p"]);
        const std::string right = correctedPath(root, rel);
        if (right == rel) continue;
        if (held.count({sid, right})) {
            db.executeQuery("DELETE FROM cpap_session_files WHERE id = " + p1, {id});
        } else {
            db.executeQuery("UPDATE cpap_session_files SET rel_path = " + p1 + " WHERE id = " + p2,
                            {right, id});
            held.insert({sid, right});
        }
        ++r.file_rows;
    }

    // The one-per-kind columns.
    static const char* kColumns[] = {"brp_file_path", "pld_file_path", "sad_file_path",
                                     "eve_file_path", "csl_file_path"};
    const auto sessions = db.executeQuery(
        "SELECT id, brp_file_path, pld_file_path, sad_file_path, eve_file_path, csl_file_path"
        " FROM cpap_sessions WHERE device_id = " + p1,
        {device_id});
    for (const auto& row : sessions) {
        const std::string id = text(row["id"]);
        for (const char* col : kColumns) {
            const std::string rel = text(row[col]);
            if (rel.empty()) continue;
            const std::string right = correctedPath(root, rel);
            if (right == rel) continue;
            db.executeQuery(std::string("UPDATE cpap_sessions SET ") + col + " = " + p1 +
                                " WHERE id = " + p2,
                            {right, id});
            ++r.session_paths;
        }
    }
    return r;
}

}  // namespace misfiled_paths
}  // namespace hms_cpap
