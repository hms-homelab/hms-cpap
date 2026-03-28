#pragma once
#include "database/IDatabase.h"
#include "database/SqlDialect.h"
#include <json/json.h>
#include <string>
#include <memory>

namespace hms_cpap {

class QueryService {
public:
    explicit QueryService(std::shared_ptr<IDatabase> db, const std::string& device_id);

    Json::Value getDashboard();
    Json::Value getSessions(int days, int limit);
    Json::Value getSessionDetail(const std::string& date);
    Json::Value getDailySummary(const std::string& start, const std::string& end);
    Json::Value getTrend(const std::string& metric, int days);
    Json::Value getStatistics(const std::string& start, const std::string& end);
    Json::Value getSummaries(const std::string& period, int limit);

private:
    std::shared_ptr<IDatabase> db_;
    std::string device_id_;
    DbType dt_;  // cached db type for dialect helpers
};

} // namespace hms_cpap
