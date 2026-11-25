#pragma once

#include "IDatabase.h"
#include <string>
#include <sstream>

namespace hms_cpap { namespace sql {

// Generate parameterized placeholder: $1 for PG, ? for MySQL/SQLite
inline std::string param(int index, DbType type) {
    if (type == DbType::POSTGRESQL) return "$" + std::to_string(index);
    return "?";
}

// ROUND(expr, decimals) -- PG needs ::numeric cast
inline std::string round(const std::string& expr, int decimals, DbType type) {
    if (type == DbType::POSTGRESQL)
        return "ROUND((" + expr + ")::numeric, " + std::to_string(decimals) + ")";
    return "ROUND(" + expr + ", " + std::to_string(decimals) + ")";
}

// Date of (column - 12 hours) -- sleep day calculation
inline std::string sleepDay(const std::string& col, DbType type) {
    switch (type) {
        case DbType::POSTGRESQL: return "DATE(" + col + " - INTERVAL '12 hours')";
        case DbType::MYSQL:      return "DATE(DATE_SUB(" + col + ", INTERVAL 12 HOUR))";
        case DbType::SQLITE:     return "date(" + col + ", '-12 hours')";
    }
    return "";
}

// Issue #126: a DATALOG folder name ("20261030") as the night it holds
// ("2026-10-30"), or "" when it is not a date folder.
inline std::string nightOfFolder(const std::string& folder) {
    if (folder.size() != 8) return "";
    for (char c : folder)
        if (c < '0' || c > '9') return "";
    return folder.substr(0, 4) + "-" + folder.substr(4, 2) + "-" + folder.substr(6, 2);
}

// Issue #126: " AND <col>'s night = <param>", the folder's night, for the
// deletes that re-import a folder. Empty when [night] is (not a date folder):
// the path alone, as before.
inline std::string onNight(const std::string& col, const std::string& night, int paramIndex,
                           DbType type) {
    if (night.empty()) return "";
    std::string cast;
    switch (type) {
        case DbType::POSTGRESQL: cast = param(paramIndex, type) + "::date"; break;
        case DbType::MYSQL:      cast = "CAST(? AS DATE)"; break;
        case DbType::SQLITE:     cast = "date(?)"; break;
    }
    return " AND " + sleepDay(col, type) + " = " + cast;
}

// Cast to date: $1::date vs CAST(? AS DATE) vs date(?)
inline std::string castDate(int paramIndex, DbType type) {
    switch (type) {
        case DbType::POSTGRESQL: return param(paramIndex, type) + "::date";
        case DbType::MYSQL:      return "CAST(? AS DATE)";
        case DbType::SQLITE:     return "date(?)";
    }
    return "";
}

// Cast to timestamp
inline std::string castTimestamp(int paramIndex, DbType type) {
    switch (type) {
        case DbType::POSTGRESQL: return param(paramIndex, type) + "::timestamp";
        case DbType::MYSQL:      return "CAST(? AS DATETIME)";
        case DbType::SQLITE:     return "datetime(?)";
    }
    return "";
}

// CURRENT_DATE - N (for static values, not params)
inline std::string currentDateMinus(int days, DbType type) {
    std::string d = std::to_string(days);
    switch (type) {
        case DbType::POSTGRESQL: return "CURRENT_DATE - " + d;
        case DbType::MYSQL:      return "CURDATE() - INTERVAL " + d + " DAY";
        case DbType::SQLITE:     return "date('now', '-" + d + " days')";
    }
    return "";
}

// CURRENT_TIMESTAMP
// The server's own wall clock, the same reading on every engine.
//
// CURRENT_TIMESTAMP and NOW() are local to the server; SQLite's datetime('now')
// is UTC, which made the two timestamps on one report row disagree by the
// machine's offset (issue #35: created_at 22:11 local beside completed_at 05:11
// UTC, seven hours apart for the same instant). 'localtime' puts SQLite on the
// same clock as the other two.
inline std::string now(DbType type) {
    switch (type) {
        case DbType::POSTGRESQL: return "CURRENT_TIMESTAMP";
        case DbType::MYSQL:      return "NOW()";
        case DbType::SQLITE:     return "datetime('now','localtime')";
    }
    return "";
}

// STDDEV(expr) -- SQLite has no built-in stddev, return NULL
inline std::string stddev(const std::string& expr, DbType type) {
    switch (type) {
        case DbType::POSTGRESQL: return "STDDEV(" + expr + ")";
        case DbType::MYSQL:      return "STDDEV(" + expr + ")";
        case DbType::SQLITE:     return "NULL";
    }
    return "NULL";
}

// Render a timestamp column as text, e.g. "2026-08-12 07:24:26".
//
// PostgreSQL took `col::text`, which is why the report queries were readable on
// that backend and a syntax error on the other two — the reports table existed
// nowhere else and the SELECTs would not have run there anyway. SQLite already
// stores timestamps as text, so the column is returned as-is.
inline std::string tsText(const std::string& col, DbType type) {
    switch (type) {
        case DbType::POSTGRESQL: return col + "::text";
        case DbType::MYSQL:      return "DATE_FORMAT(" + col + ", '%Y-%m-%d %H:%i:%s')";
        case DbType::SQLITE:     return col;
    }
    return col;
}

// A timestamp column cut to its minute, as text: "2026-08-12 07:24:00".
// SDD-072 D4: the agent's per-minute vitals, on every engine (it was
// date_trunc, PostgreSQL only).
inline std::string minuteText(const std::string& col, DbType type) {
    switch (type) {
        case DbType::POSTGRESQL:
            return "to_char(date_trunc('minute', " + col + "), 'YYYY-MM-DD HH24:MI:SS')";
        case DbType::MYSQL:      return "DATE_FORMAT(" + col + ", '%Y-%m-%d %H:%i:00')";
        case DbType::SQLITE:     return "strftime('%Y-%m-%d %H:%M:00', " + col + ")";
    }
    return col;
}

// ── A night's respiratory indexes (SDD-047 D2) ──────────────────────────────
//
// ONE formula for every reader of a night's index: the night's typed event
// counts, summed over its sessions, divided by the night's summed hours. It is
// cpapdash::parser::computeIndexes (EventIndexes.h) written as SQL, with that
// header's numerators, so the daily summary, getNightlyMetrics and the range
// query cannot disagree about the same night on any engine.
//
// The daily summary used to take a duration-weighted mean of each session's
// own index instead, because the unclassified apnea (ResMed's bare "Apnea")
// was counted in that index and never stored, so the typed sums fell short of
// it. It is stored now (cpap_session_metrics.unclassified_apneas), and is in
// the numerators below.
//
// [m] is the query's cpap_session_metrics alias and [s] its cpap_sessions
// alias. Each returns NULL for a night with no hours; the caller decides what
// such a night reads.

/// SUM over the night of one cpap_session_metrics count, NULL read as 0.
inline std::string nightCount(const std::string& m, const std::string& col) {
    return "COALESCE(SUM(" + m + "." + col + "), 0)";
}

/// The AI numerator: every apnea, classified or not.
inline std::string nightApneas(const std::string& m) {
    return "(" + nightCount(m, "obstructive_apneas") + " + " + nightCount(m, "central_apneas") +
           " + " + nightCount(m, "clear_airway_apneas") + " + " +
           nightCount(m, "unclassified_apneas") + ")";
}

/// The AHI numerator: every apnea, plus hypopneas.
inline std::string nightApneasAndHypopneas(const std::string& m) {
    return "(" + nightApneas(m) + " + " + nightCount(m, "hypopneas") + ")";
}

/// [numerator] events per hour over the night's summed session spans.
inline std::string perNightHour(const std::string& numerator, const std::string& s) {
    return "(" + numerator + ") * 3600.0 / NULLIF(SUM(" + s + ".duration_seconds), 0)";
}

/// The night's AHI.
inline std::string nightAhi(const std::string& m, const std::string& s) {
    return perNightHour(nightApneasAndHypopneas(m), s);
}

// ── A night's SpO2 desaturations (SDD-001) ──────────────────────────────────
//
// Over the night's sessions that recorded SpO2 (avg_spo2 > 0) and no others: a
// session without SpO2 has no desaturations to count, and its hours are not
// hours the ODI was measured over. NULL for a night with no SpO2 at all, so a
// night without an oximeter publishes no ODI rather than an ODI of zero.

/// The night's desaturation count.
inline std::string nightSpo2Drops(const std::string& m) {
    return "SUM(CASE WHEN " + m + ".avg_spo2 > 0 THEN " + m + ".spo2_drops END)";
}

/// The night's ODI: its desaturations per hour of the sessions that recorded SpO2.
inline std::string nightOdi(const std::string& m, const std::string& s) {
    return "(" + nightSpo2Drops(m) + ") * 3600.0 / NULLIF(SUM(CASE WHEN " + m +
           ".avg_spo2 > 0 THEN " + s + ".duration_seconds END), 0)";
}

/// The daily summary's seven index columns, in its order ahi, hi, ai, oai,
/// cai, uai, rin, each rounded to the two places it stores. uai is the
/// unclassified apneas, as in computeIndexes and the STR's own UAI; a
/// clear-airway apnea (Philips) counts in ai and ahi only.
inline std::string nightIndexColumns(const std::string& m, const std::string& s, DbType type) {
    auto idx = [&](const std::string& numerator, const char* as) {
        return round(perNightHour(numerator, s), 2, type) + " AS " + as;
    };
    return idx(nightApneasAndHypopneas(m), "ahi") + ",\n" +
           idx(nightCount(m, "hypopneas"), "hi") + ",\n" +
           idx(nightApneas(m), "ai") + ",\n" +
           idx(nightCount(m, "obstructive_apneas"), "oai") + ",\n" +
           // SDD-083 F2: a DreamStation marks its central apneas "clear
           // airway"; AHI and AI count both columns, and so does the CAI.
           idx("(" + nightCount(m, "central_apneas") + " + " + nightCount(m, "clear_airway_apneas") + ")",
               "cai") + ",\n" +
           idx(nightCount(m, "unclassified_apneas"), "uai") + ",\n" +
           idx(nightCount(m, "reras"), "rin");
}

// ── The live O2 Ring session, and when a real ring file supersedes it ───────
//
// In bridge mode each poll appends one sample to a synthetic session named
// live_<YYYYMMDD>.vld. That stream stands in for the night until the ring's
// own file (a .vld, an O2Ring-S file, a CSV, a BLE download) lands for the same
// night; from then on the file is the night and the live stream is not. The
// writer removes the live session when the file is saved (saveOximetrySession).
// The readers also skip one that is still there: a live row written before that
// removal existed, or one written after the file (a second wear in the same
// sleep day).
//
// SUBSTR rather than LIKE 'live\_%': the three engines disagree on LIKE's
// escape character, and an unescaped '_' matches any character.

/// SDD-078 D3: rows stored before oximetry_sessions had a source (or by a
/// writer that does not set one, the live ring stream) get one: a CSV only
/// ever came in through the upload page, everything else is the ring's. Run
/// on every start; a row the new writers store already has its source. The
/// same statement on all three engines.
inline std::string oxiSourceBackfill() {
    return "UPDATE oximetry_sessions SET source = CASE WHEN LOWER(filename) LIKE '%.csv'"
           " THEN 'upload' ELSE 'o2ring' END WHERE source IS NULL";
}

/// True when [col] names a live session (live_<date>.vld).
inline std::string isLiveOxiFilename(const std::string& col) {
    return "(SUBSTR(" + col + ", 1, 5) = 'live_')";
}

/// Keeps the oximetry_sessions row [os] unless it is a live session whose
/// night (same device, same sleep day) already has a real ring file.
inline std::string oxiNotSuperseded(const std::string& os, DbType type) {
    return "NOT (" + isLiveOxiFilename(os + ".filename") +
           " AND EXISTS (SELECT 1 FROM oximetry_sessions oxr"
           " WHERE oxr.device_id = " + os + ".device_id"
           " AND NOT " + isLiveOxiFilename("oxr.filename") +
           " AND " + sleepDay("oxr.start_time", type) + " = " +
           sleepDay(os + ".start_time", type) + "))";
}

}} // namespace hms_cpap::sql
