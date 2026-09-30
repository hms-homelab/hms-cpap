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
           idx(nightCount(m, "central_apneas"), "cai") + ",\n" +
           idx(nightCount(m, "unclassified_apneas"), "uai") + ",\n" +
           idx(nightCount(m, "reras"), "rin");
}

}} // namespace hms_cpap::sql
