#pragma once

#include <iostream>
#include <memory>
#include <string>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "utils/AppConfig.h"
#include "utils/ConfigManager.h"
#ifdef WITH_POSTGRESQL
#include "database/DatabaseService.h"
#endif
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif

namespace hms_cpap {

/**
 * Build the IDatabase backend selected by env config (DB_TYPE + DB_* / SQLITE_PATH).
 *
 * Single source of truth for backend selection so the long-running service
 * (BurstCollectorService::initDatabase) and one-shot CLI tools (--backfill,
 * --reparse) can never disagree on which DB to use. Returns an UNCONNECTED
 * instance; the caller calls connect().
 *
 * Selection mirrors initDatabase(): sqlite (default) / postgresql / mysql, with
 * an SQLite fallback for an unknown DB_TYPE.
 */
inline std::shared_ptr<IDatabase> makeDatabaseFromConfig() {
    const std::string db_type = ConfigManager::get("DB_TYPE", "sqlite");

    auto makeSqlite = []() -> std::shared_ptr<IDatabase> {
        return std::make_shared<SQLiteDatabase>(
            ConfigManager::get("SQLITE_PATH",
                               hms_cpap::AppConfig::dataDir() + "/cpap.db"));
    };

    if (db_type == "sqlite") return makeSqlite();

#ifdef WITH_POSTGRESQL
    if (db_type == "postgresql") {
        std::string conn = "host=" + ConfigManager::get("DB_HOST", "localhost") +
                           " port=" + ConfigManager::get("DB_PORT", "5432") +
                           " dbname=" + ConfigManager::get("DB_NAME", "cpap_monitoring") +
                           " user=" + ConfigManager::get("DB_USER", "maestro") +
                           " password=" + ConfigManager::get("DB_PASSWORD", "");
        return std::make_shared<DatabaseService>(conn);
    }
#endif
#ifdef WITH_MYSQL
    if (db_type == "mysql") {
        return std::make_shared<MySQLDatabase>(
            ConfigManager::get("DB_HOST", "localhost"),
            ConfigManager::getInt("DB_PORT", 3306),
            ConfigManager::get("DB_USER", ""),
            ConfigManager::get("DB_PASSWORD", ""),
            ConfigManager::get("DB_NAME", "cpap_monitoring"));
    }
#endif

    std::cerr << "DB: Unknown DB_TYPE '" << db_type << "', falling back to SQLite" << std::endl;
    return makeSqlite();
}

}  // namespace hms_cpap
