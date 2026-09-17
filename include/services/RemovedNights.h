#pragma once
//
// SDD-029: the nights an operator removed, as every re-ingest path asks about
// them. One rule, one place: a night is strDayForSessionStart() of a start
// (YYYYMMDD, the start shifted back 12 h), and an STR day record (stamped at
// noon of its day) maps onto the same key through the same function.
//
#include "database/IDatabase.h"
#include "services/SyncFolderState.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <set>
#include <string>
#include <vector>

namespace hms_cpap {

/// The removed nights of [device_id], for one burst or one call. Empty when
/// nothing is removed or the backend does not support it.
inline std::set<std::string> removedNightSet(IDatabase& db, const std::string& device_id) {
    const auto v = db.removedNights(device_id);
    return {v.begin(), v.end()};
}

/// Does a session (or oximetry night) starting at [start] fall on a removed night?
inline bool isRemovedNight(const std::set<std::string>& removed,
                           const std::chrono::system_clock::time_point& start) {
    return !removed.empty() && removed.count(strDayForSessionStart(start)) > 0;
}

/// The night of an O2 ring recording. The ring's parsers read its printed wall
/// clock AS IF it were UTC (timegm) and the columns render it back with gmtime,
/// so its night is the start shifted back 12 h on the UTC clock. The local rule
/// above would put a start between noon and noon-plus-the-UTC-offset on the
/// night before.
inline std::string oximetryNightOf(const std::chrono::system_clock::time_point& start) {
    const std::time_t t = std::chrono::system_clock::to_time_t(start - std::chrono::hours(12));
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[9];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
    return std::string(buf);
}

/// Does an O2 ring recording starting at [start] fall on a removed night?
inline bool isRemovedOximetryNight(const std::set<std::string>& removed,
                                   const std::chrono::system_clock::time_point& start) {
    return !removed.empty() && removed.count(oximetryNightOf(start)) > 0;
}

/// STR day records minus the removed nights, so re-writing the whole STR
/// history every burst (the local branch does) cannot put a removed day's
/// summary row back. [R] is any record with a noon-stamped `record_date`.
template <class R>
std::vector<R> withoutRemovedNights(std::vector<R> records, const std::set<std::string>& removed) {
    if (removed.empty()) return records;
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [&](const R& r) { return isRemovedNight(removed, r.record_date); }),
                  records.end());
    return records;
}

/// "2026-08-23" or "20260823" -> "20260823"; "" when it is not a date.
inline std::string nightKeyOf(const std::string& date) {
    std::string digits;
    for (char c : date) if (c >= '0' && c <= '9') digits += c;
    return digits.size() == 8 ? digits : std::string();
}

/// DELETE /api/sessions/{date}: the date as the sessions page names it, removed.
/// Something that is not a date removes nothing (ok=false).
inline IDatabase::RemoveNightResult removeNightByDate(IDatabase& db, const std::string& device_id,
                                                      const std::string& date) {
    const auto night = nightKeyOf(date);
    if (night.empty()) return {};
    return db.removeNight(device_id, night);
}

/// D3: a Reparse of [date] asks for the night back. True when it had been removed.
inline bool restoreNightByDate(IDatabase& db, const std::string& device_id,
                               const std::string& date) {
    const auto night = nightKeyOf(date);
    if (night.empty() || !removedNightSet(db, device_id).count(night)) return false;
    return db.restoreNight(device_id, night);
}

/// SDD-036: GET /api/removed-nights, the removed nights as the sessions page
/// names them (YYYY-MM-DD), newest first, so they can be restored from there.
inline std::vector<std::string> removedNightDates(IDatabase& db, const std::string& device_id) {
    std::vector<std::string> dates;
    for (const auto& n : removedNightSet(db, device_id)) {
        if (nightKeyOf(n) != n) continue;
        dates.push_back(n.substr(0, 4) + "-" + n.substr(4, 2) + "-" + n.substr(6, 2));
    }
    std::reverse(dates.begin(), dates.end());
    return dates;
}

/// SDD-036 D2: an uploaded card is asking for its nights back, as the .vld
/// upload already was (SDD-029 section 7). Clears the record of each night in
/// [dates] (either form) that had been removed, and nothing else, so the import
/// that follows stores them. Returns the nights restored, YYYY-MM-DD.
template <class Dates>
std::vector<std::string> restoreUploadedNights(IDatabase& db, const std::string& device_id,
                                               const Dates& dates) {
    std::vector<std::string> restored;
    const auto removed = removedNightSet(db, device_id);
    if (removed.empty()) return restored;
    for (const auto& d : dates) {
        const auto night = nightKeyOf(d);
        if (night.empty() || !removed.count(night)) continue;
        if (db.restoreNight(device_id, night))
            restored.push_back(night.substr(0, 4) + "-" + night.substr(4, 2) + "-" +
                               night.substr(6, 2));
    }
    return restored;
}

}  // namespace hms_cpap
