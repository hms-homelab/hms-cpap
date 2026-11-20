#pragma once
// SDD-080, SDD-081: the suite's own stand-ins for OSCAR.
//
// No OSCAR here and no OSCAR-made file: the database is built by this file's
// own SQL, with the tables and columns OSCAR 2.1.1's database was observed to
// hold, and OSCAR itself is a fake that "imports" by writing rows into it. The
// card files are made-up bytes: nothing here is a card, nothing is decrypted.

#include "services/OscarCard.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace oscar_fakes {

namespace fs = std::filesystem;
using hms_cpap::oscar::IProcessRunner;
using hms_cpap::oscar::ProcessResult;

constexpr long long kT0 = 1893456000000LL;   // 2030-01-01 00:00 UTC, in ms; made up
constexpr const char* kDevice = "oscar_dev";
constexpr const char* kSerial = "TEST00000080";

// ── a synthetic oscar.db ──────────────────────────────────────────────────

inline const char* kSchema = R"SQL(
CREATE TABLE schema_version (version INTEGER PRIMARY KEY, applied_at TEXT);
CREATE TABLE app_preferences (id INTEGER PRIMARY KEY AUTOINCREMENT, category TEXT NOT NULL DEFAULT 'general',
  key TEXT NOT NULL, value TEXT, UNIQUE(category, key));
CREATE TABLE profiles (id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT UNIQUE NOT NULL, data_folder TEXT NOT NULL);
CREATE TABLE machines (id INTEGER PRIMARY KEY AUTOINCREMENT, profile_id INTEGER NOT NULL, machine_id INTEGER NOT NULL,
  loader_name TEXT NOT NULL, machine_type INTEGER NOT NULL, brand TEXT, model TEXT, series TEXT,
  serial_number TEXT, model_number TEXT);
CREATE TABLE sessions (id INTEGER PRIMARY KEY AUTOINCREMENT, session_id INTEGER NOT NULL, machine_id INTEGER NOT NULL,
  start_time INTEGER NOT NULL, end_time INTEGER NOT NULL, duration INTEGER NOT NULL, enabled INTEGER DEFAULT 1,
  summary_only INTEGER DEFAULT 0);
CREATE TABLE session_summaries (id INTEGER PRIMARY KEY AUTOINCREMENT, session_id INTEGER NOT NULL UNIQUE,
  profile_id INTEGER NOT NULL, ahi REAL DEFAULT 0, obstructive_count INTEGER DEFAULT 0,
  unclassified_count INTEGER DEFAULT 0, hypopnea_count INTEGER DEFAULT 0, rera_count INTEGER DEFAULT 0,
  clear_airway_count INTEGER DEFAULT 0);
CREATE TABLE channels (id INTEGER PRIMARY KEY AUTOINCREMENT, profile_id INTEGER NOT NULL, channel_id INTEGER NOT NULL,
  channel_code TEXT NOT NULL, type INTEGER);
CREATE TABLE session_channels (id INTEGER PRIMARY KEY AUTOINCREMENT, session_id INTEGER NOT NULL,
  profile_id INTEGER NOT NULL, channel_id INTEGER NOT NULL, count INTEGER DEFAULT 0, wavg REAL DEFAULT 0,
  min REAL DEFAULT 0, max REAL DEFAULT 0, median REAL DEFAULT 0, p90 REAL DEFAULT 0, p95 REAL DEFAULT 0);
CREATE TABLE event_lists (id INTEGER PRIMARY KEY AUTOINCREMENT, session_id INTEGER NOT NULL, profile_id INTEGER NOT NULL,
  channel_id INTEGER NOT NULL, eventlist_index INTEGER NOT NULL DEFAULT 0, event_type INTEGER NOT NULL,
  first_time INTEGER NOT NULL, last_time INTEGER NOT NULL, count INTEGER NOT NULL, rate REAL NOT NULL DEFAULT 0,
  gain REAL NOT NULL DEFAULT 1.0, offset REAL NOT NULL DEFAULT 0.0);
CREATE TABLE event_data (id INTEGER PRIMARY KEY AUTOINCREMENT, eventlist_id INTEGER NOT NULL, data_blob BLOB,
  time_blob BLOB, compression_method INTEGER NOT NULL DEFAULT 0);
)SQL";

class FakeOscarDb {
public:
    explicit FakeOscarDb(const fs::path& dir, const std::string& version = "2.1.1+00000000", int schema = 19) {
        fs::create_directories(dir);
        EXPECT_EQ(sqlite3_open((dir / "oscar.db").string().c_str(), &db_), SQLITE_OK);
        exec("PRAGMA journal_mode = WAL");
        exec(kSchema);
        exec("INSERT INTO schema_version (version) VALUES (" + std::to_string(schema) + ")");
        exec("INSERT INTO app_preferences (key, value) VALUES ('VersionString', '" + version + "')");
        // The user's own profile first, hms-cpap's beside it.
        user_ = profile("someone");
        hms_ = profile(hms_cpap::oscar::kDefaultProfile);
    }
    ~FakeOscarDb() { sqlite3_close(db_); }

    void exec(const std::string& sql) {
        char* err = nullptr;
        ASSERT_EQ(sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "") << sql;
    }
    long long last() const { return sqlite3_last_insert_rowid(db_); }

    long long profile(const std::string& name) {
        exec("INSERT INTO profiles (username, data_folder) VALUES ('" + name + "', '/x')");
        const long long id = last();
        exec("INSERT INTO machines (profile_id, machine_id, loader_name, machine_type, brand, model) "
             "VALUES (" + std::to_string(id) + ", 1, 'Journal', 4, 'OSCAR', 'Journal')");
        int cid = 1;
        for (auto [code, type] : std::vector<std::pair<const char*, int>>{
                 {"Obstructive", 4}, {"ClearAirway", 4}, {"Hypopnea", 4}, {"RERA", 4}, {"PB", 16},
                 {"LeakSpan", 16}, {"FlowRate", 32}, {"Pressure", 32}, {"LeakTotal", 32}, {"PressurePulse", 8}})
            exec("INSERT INTO channels (profile_id, channel_id, channel_code, type) VALUES (" + std::to_string(id) +
                 ", " + std::to_string(cid++) + ", '" + code + "', " + std::to_string(type) + ")");
        machine_[id] = 0;
        return id;
    }

    long long machineOf(long long profile_id) {
        auto& m = machine_[profile_id];
        if (m) return m;
        exec("INSERT INTO machines (profile_id, machine_id, loader_name, machine_type, brand, model, serial_number, "
             "model_number) VALUES (" + std::to_string(profile_id) +
             ", 7, 'PRS1', 1, 'Philips Respironics', 'DreamStation 2 Auto CPAP Advanced', '" + kSerial +
             "', '520X110C')");
        m = last();
        return m;
    }

    int channelId(long long profile_id, const std::string& code) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_, "SELECT channel_id FROM channels WHERE profile_id = ?1 AND channel_code = ?2", -1, &s,
                           nullptr);
        sqlite3_bind_int64(s, 1, profile_id);
        sqlite3_bind_text(s, 2, code.c_str(), -1, SQLITE_TRANSIENT);
        int id = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : 0;
        sqlite3_finalize(s);
        return id;
    }

    /// One list: values as int16 (value = raw * gain + offset), times as
    /// uint32 ms after first (a timed list) or none (evenly sampled).
    void list(long long session_row, long long profile_id, const std::string& code, int event_type, long long first,
              double rate, double gain, const std::vector<int16_t>& raw, const std::vector<uint32_t>& times = {},
              int index = 0) {
        exec("INSERT INTO event_lists (session_id, profile_id, channel_id, eventlist_index, event_type, first_time, "
             "last_time, count, rate, gain, offset) VALUES (" +
             std::to_string(session_row) + ", " + std::to_string(profile_id) + ", " +
             std::to_string(channelId(profile_id, code)) + ", " + std::to_string(index) + ", " +
             std::to_string(event_type) + ", " + std::to_string(first) + ", " + std::to_string(first) + ", " +
             std::to_string(raw.size()) + ", " + std::to_string(rate) + ", " + std::to_string(gain) + ", 0)");
        const long long el = last();
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_, "INSERT INTO event_data (eventlist_id, data_blob, time_blob) VALUES (?1, ?2, ?3)", -1,
                           &s, nullptr);
        sqlite3_bind_int64(s, 1, el);
        std::string v, t;
        for (int16_t x : raw) {
            v.push_back(static_cast<char>(x & 0xff));
            v.push_back(static_cast<char>((x >> 8) & 0xff));
        }
        for (uint32_t x : times)
            for (int i = 0; i < 4; ++i) t.push_back(static_cast<char>((x >> (8 * i)) & 0xff));
        sqlite3_bind_blob(s, 2, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        if (event_type == 0) sqlite3_bind_null(s, 3);
        else sqlite3_bind_blob(s, 3, t.data(), static_cast<int>(t.size()), SQLITE_TRANSIENT);
        EXPECT_EQ(sqlite3_step(s), SQLITE_DONE);
        sqlite3_finalize(s);
    }

    /// A closed session OSCAR stored: `minutes` of 5 Hz flow (a 15 breaths a
    /// minute sine), `oa` obstructive apneas of 12 s, a pressure trend at 9.4,
    /// a large-leak span of 30 s, its summary counts.
    long long session(long long profile_id, long long number, long long start_ms, int minutes, int oa = 2) {
        const long long end = start_ms + minutes * 60000LL;
        exec("INSERT INTO sessions (session_id, machine_id, start_time, end_time, duration) VALUES (" +
             std::to_string(number) + ", " + std::to_string(machineOf(profile_id)) + ", " + std::to_string(start_ms) +
             ", " + std::to_string(end) + ", " + std::to_string(end - start_ms) + ")");
        const long long row = last();
        exec("INSERT INTO session_summaries (session_id, profile_id, obstructive_count) VALUES (" +
             std::to_string(row) + ", " + std::to_string(profile_id) + ", " + std::to_string(oa) + ")");
        exec("INSERT INTO session_channels (session_id, profile_id, channel_id, count, wavg, min, max, median, p95) "
             "VALUES (" + std::to_string(row) + ", " + std::to_string(profile_id) + ", " +
             std::to_string(channelId(profile_id, "Pressure")) + ", 10, 9.4, 8.0, 11.0, 9.3, 10.8)");
        // Flow: 5 Hz, gain 0.1 (value = raw / 10 L/min).
        std::vector<int16_t> flow;
        for (int i = 0; i < minutes * 60 * 5; ++i)
            flow.push_back(static_cast<int16_t>(std::lround(300 * std::sin(2 * M_PI * i / 20.0))));
        list(row, profile_id, "FlowRate", 0, start_ms, 200, 0.1, flow);
        // Obstructive apneas at their END, their value their length (12 s).
        std::vector<int16_t> lens;
        std::vector<uint32_t> at;
        for (int i = 0; i < oa; ++i) {
            lens.push_back(12);
            at.push_back(static_cast<uint32_t>(120000 + i * 60000));
        }
        if (oa > 0) list(row, profile_id, "Obstructive", 1, start_ms, 0, 1, lens, at);
        list(row, profile_id, "LeakSpan", 1, start_ms, 0, 1, {30}, {300000});
        list(row, profile_id, "Pressure", 1, start_ms, 0, 0.1, {94, 94, 94}, {0, 60000, 120000});
        return row;
    }

    long long user_{0}, hms_{0};

private:
    sqlite3* db_{nullptr};
    std::map<long long, long long> machine_;
};

/// The made-up card tree OSCAR is pointed at: P-SERIES/<machine>/PROP.BIN and
/// Pn/<8 hex>.B01|B02|B05. Bytes of nothing.
inline void cardFile(const fs::path& root, const std::string& rel, const std::string& bytes = "made-up bytes") {
    fs::create_directories((root / rel).parent_path());
    std::ofstream(root / rel, std::ios::binary) << bytes;
}

/// OSCAR, faked: records each command and does what it is told on a run.
class FakeOscar : public IProcessRunner {
public:
    std::function<ProcessResult(const std::vector<std::string>&)> on_run;
    std::vector<std::vector<std::string>> runs;
    std::vector<int> limits;
    ProcessResult run(const std::vector<std::string>& argv, int limit_s) override {
        runs.push_back(argv);
        limits.push_back(limit_s);
        if (on_run) return on_run(argv);
        ProcessResult r;
        r.started = true;
        r.exit_code = 0;
        return r;
    }
};

inline ProcessResult exited(int code, std::string out = "") {
    ProcessResult r;
    r.started = true;
    r.exit_code = code;
    r.output = std::move(out);
    return r;
}

}  // namespace oscar_fakes
