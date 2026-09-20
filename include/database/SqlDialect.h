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


}} // namespace hms_cpap::sql
