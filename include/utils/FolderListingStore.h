#pragma once
//
// SDD-054 D9: a folder's listing is stored, with done per file and per folder;
// a fully done folder is never listed again, and a done file never fetched
// again.
//
// ResMed writes only into the folder of the session it is running; an older
// folder never changes again (Albin, 2026-10-15). So a listing, once taken,
// stays true, and keeping it means:
//
//   - a card that drops at folder 251 of 300 keeps the 250 listings it gave,
//     because each is written the moment it succeeds;
//   - a folder that is not done is finished from its stored listing on a later
//     burst, without listing it again;
//   - a restart over a card with 300 nights lists a handful, not 300.
//
// Done, two passes (Albin): edf_done when every EDF is stored and the night is
// over (the pass that matters), fully_done when the rest of the folder's kept
// files (the per-night .crc and the like) are stored too.
//
// One row per (device, folder) in cpap_folder_listing, created at runtime like
// cpap_night_quiet. The database is the record; the collector keeps a copy of
// the rows it read and writes every change through. `files` is a JSON array of
// [name, size_kb, is_dir, year, month, day, hour, minute, second, done], the
// listing exactly as the card gave it plus each file's done mark, and an 11th
// element, the exact size, for a source that lists one (SDD-056 D2 b). An
// older build reads the first ten and ignores it.
//
#include "clients/EzShareClient.h"
#include "database/IDatabase.h"
#include "database/SqlDialect.h"

#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace hms_cpap {

struct StoredListing {
    std::vector<EzShareFileEntry> files;
    std::set<std::string> done_files;   ///< names of the files that are done
    long long listed_at{0};
    bool      edf_done{false};
    bool      fully_done{false};
};

namespace folder_listing {

inline void ensureTable(IDatabase& db) {
    db.executeQuery(
        "CREATE TABLE IF NOT EXISTS cpap_folder_listing ("
        " device_id VARCHAR(128) NOT NULL,"
        " date_folder VARCHAR(8) NOT NULL,"
        " files TEXT NOT NULL,"
        " listed_at BIGINT NOT NULL DEFAULT 0,"
        " edf_done INTEGER NOT NULL DEFAULT 0,"
        " fully_done INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY (device_id, date_folder))",
        {});
}

inline long long asLong(const Json::Value& v) {
    if (v.isNull()) return 0;
    if (v.isString()) return std::atoll(v.asCString());
    return v.asInt64();
}

inline std::string encode(const StoredListing& s) {
    Json::Value arr(Json::arrayValue);
    for (const auto& f : s.files) {
        Json::Value e(Json::arrayValue);
        e.append(f.name);
        e.append(f.size_kb);
        e.append(f.is_dir ? 1 : 0);
        e.append(f.year);
        e.append(f.month);
        e.append(f.day);
        e.append(f.hour);
        e.append(f.minute);
        e.append(f.second);
        e.append(s.done_files.count(f.name) ? 1 : 0);
        if (f.size_bytes > 0) e.append(Json::UInt64(f.size_bytes));
        arr.append(e);
    }
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return Json::writeString(wb, arr);
}

inline void decode(const std::string& text, StoredListing& s) {
    Json::Value arr;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream in(text);
    if (!Json::parseFromStream(rb, in, &arr, &errs) || !arr.isArray()) return;
    for (const auto& e : arr) {
        if (!e.isArray() || e.size() < 9) continue;
        EzShareFileEntry f;
        f.name    = e[0].asString();
        f.size_kb = e[1].asInt();
        f.is_dir  = e[2].asInt() != 0;
        f.year    = e[3].asInt();
        f.month   = e[4].asInt();
        f.day     = e[5].asInt();
        f.hour    = e[6].asInt();
        f.minute  = e[7].asInt();
        f.second  = e[8].asInt();
        if (e.size() > 9 && e[9].asInt() != 0) s.done_files.insert(f.name);
        if (e.size() > 10) f.size_bytes = static_cast<uint64_t>(asLong(e[10]));
        s.files.push_back(std::move(f));
    }
}

}  // namespace folder_listing

/// A new listing of a folder, carrying over the done mark of every file whose
/// size and stamp did not change. A changed file is not done any more.
inline StoredListing relisted(const StoredListing& prev,
                              const std::vector<EzShareFileEntry>& files, long long listed_at) {
    StoredListing next;
    next.files = files;
    next.listed_at = listed_at;
    std::map<std::string, const EzShareFileEntry*> before;
    for (const auto& f : prev.files) before[f.name] = &f;
    bool all_same = files.size() == prev.files.size();
    for (const auto& f : files) {
        const auto it = before.find(f.name);
        const bool same = it != before.end() && it->second->size_kb == f.size_kb &&
                          it->second->size_bytes == f.size_bytes &&
                          it->second->year == f.year && it->second->month == f.month &&
                          it->second->day == f.day && it->second->hour == f.hour &&
                          it->second->minute == f.minute && it->second->second == f.second;
        if (!same) all_same = false;
        if (same && prev.done_files.count(f.name)) next.done_files.insert(f.name);
    }
    next.edf_done = all_same && prev.edf_done;
    next.fully_done = all_same && prev.fully_done;
    return next;
}

/// Every stored listing of [device_id], by folder.
inline std::map<std::string, StoredListing> loadFolderListings(IDatabase& db,
                                                               const std::string& device_id) {
    folder_listing::ensureTable(db);
    std::map<std::string, StoredListing> out;
    for (const auto& r : db.executeQuery(
             "SELECT date_folder, files, listed_at, edf_done, fully_done FROM cpap_folder_listing "
             "WHERE device_id = " + sql::param(1, db.dbType()),
             {device_id})) {
        StoredListing s;
        folder_listing::decode(r["files"].isNull() ? "" : r["files"].asString(), s);
        s.listed_at  = folder_listing::asLong(r["listed_at"]);
        s.edf_done   = folder_listing::asLong(r["edf_done"]) != 0;
        s.fully_done = folder_listing::asLong(r["fully_done"]) != 0;
        out[r["date_folder"].asString()] = std::move(s);
    }
    return out;
}

/// Write one folder's row: its listing, each file's done mark, both flags.
inline bool saveFolderListing(IDatabase& db, const std::string& device_id,
                              const std::string& date_folder, const StoredListing& s) {
    folder_listing::ensureTable(db);
    const DbType dt = db.dbType();
    const std::string ins =
        "INSERT INTO cpap_folder_listing (device_id, date_folder, files, listed_at, edf_done, "
        "fully_done) VALUES (" + sql::param(1, dt) + ", " + sql::param(2, dt) + ", " +
        sql::param(3, dt) + ", " + sql::param(4, dt) + ", " + sql::param(5, dt) + ", " +
        sql::param(6, dt) + ") ";
    const std::string upsert = dt == DbType::MYSQL
        ? "ON DUPLICATE KEY UPDATE files = VALUES(files), listed_at = VALUES(listed_at), "
          "edf_done = VALUES(edf_done), fully_done = VALUES(fully_done)"
        : "ON CONFLICT (device_id, date_folder) DO UPDATE SET files = excluded.files, "
          "listed_at = excluded.listed_at, edf_done = excluded.edf_done, "
          "fully_done = excluded.fully_done";
    return db.executeQueryChecked(ins + upsert,
                                  {device_id, date_folder, folder_listing::encode(s),
                                   std::to_string(s.listed_at), s.edf_done ? "1" : "0",
                                   s.fully_done ? "1" : "0"})
        .ok;
}

inline bool forgetFolderListing(IDatabase& db, const std::string& device_id,
                                const std::string& date_folder) {
    folder_listing::ensureTable(db);
    const DbType dt = db.dbType();
    return db.executeQueryChecked("DELETE FROM cpap_folder_listing WHERE device_id = " +
                                      sql::param(1, dt) + " AND date_folder = " + sql::param(2, dt),
                                  {device_id, date_folder})
        .ok;
}

}  // namespace hms_cpap
