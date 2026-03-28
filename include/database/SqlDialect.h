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

// NOW() - N days
inline std::string daysAgo(int days, DbType type) {
    std::string d = std::to_string(days);
    switch (type) {
        case DbType::POSTGRESQL: return "NOW() - INTERVAL '" + d + " days'";
        case DbType::MYSQL:      return "DATE_SUB(NOW(), INTERVAL " + d + " DAY)";
        case DbType::SQLITE:     return "datetime('now', '-" + d + " days')";
    }
    return "";
}

// NOW() - N days using parameter (for parameterized queries)
// For PG: NOW() - make_interval(days => $N)
// For MySQL: DATE_SUB(NOW(), INTERVAL ? DAY) -- pass days as param
// For SQLite: datetime('now', '-' || ? || ' days')
inline std::string daysAgoParam(int paramIndex, DbType type) {
    switch (type) {
        case DbType::POSTGRESQL: return "NOW() - make_interval(days => " + param(paramIndex, type) + ")";
        case DbType::MYSQL:      return "DATE_SUB(NOW(), INTERVAL ? DAY)";
        case DbType::SQLITE:     return "datetime('now', '-' || ? || ' days')";
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

// Timestamp comparison with tolerance: col BETWEEN param-5s AND param+5s
inline std::string timestampTolerance(const std::string& col, int paramIndex, DbType type) {
    std::string p = param(paramIndex, type);
    switch (type) {
        case DbType::POSTGRESQL:
            return col + " BETWEEN " + p + "::timestamp - INTERVAL '5 seconds' AND " + p + "::timestamp + INTERVAL '5 seconds'";
        case DbType::MYSQL:
            return col + " BETWEEN DATE_SUB(CAST(? AS DATETIME), INTERVAL 5 SECOND) AND DATE_ADD(CAST(? AS DATETIME), INTERVAL 5 SECOND)";
        case DbType::SQLITE:
            return col + " BETWEEN datetime(?, '-5 seconds') AND datetime(?, '+5 seconds')";
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
inline std::string now(DbType type) {
    switch (type) {
        case DbType::POSTGRESQL: return "CURRENT_TIMESTAMP";
        case DbType::MYSQL:      return "NOW()";
        case DbType::SQLITE:     return "datetime('now')";
    }
    return "";
}

// Auto-increment primary key for CREATE TABLE
inline std::string autoId(DbType type) {
    switch (type) {
        case DbType::POSTGRESQL: return "SERIAL PRIMARY KEY";
        case DbType::MYSQL:      return "INT AUTO_INCREMENT PRIMARY KEY";
        case DbType::SQLITE:     return "INTEGER PRIMARY KEY AUTOINCREMENT";
    }
    return "";
}

}} // namespace hms_cpap::sql
