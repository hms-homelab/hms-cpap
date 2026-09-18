#pragma once
#include "database/IDatabase.h"
#include "database/SqlDialect.h"
#include <json/json.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * SDD-039 D1: a query that did not run is not an empty result.
 *
 * Thrown by QueryService::query(); the controllers already turn an exception
 * into a 500 carrying its message, which is the difference between a page that
 * says the database refused something and a page that draws an empty list over
 * data that is sitting right there (CpapDash support 129).
 */
class QueryFailed : public std::runtime_error {
public:
    explicit QueryFailed(const std::string& what) : std::runtime_error(what) {}
};

class QueryService {
public:
    explicit QueryService(std::shared_ptr<IDatabase> db, const std::string& device_id);

    Json::Value getDashboard();
    Json::Value getSessions(int limit, int offset);
    Json::Value getSessionDetail(const std::string& date);
    Json::Value getDailySummary(const std::string& start, const std::string& end);

    /// SDD-020: our night beside ResMed's, per component, for a date range.
    ///
    /// A LEFT JOIN from our side, so a night we have and myAir does not still
    /// appears; the reverse would hide exactly the case worth seeing. Nights
    /// where ResMed has no data are marked rather than reported as zeroes.
    Json::Value getMyAirComparison(const std::string& start, const std::string& end);
    Json::Value getTrend(const std::string& metric, int days);
    Json::Value getStatistics(const std::string& start, const std::string& end);
    Json::Value getSummaries(const std::string& period, int limit);
    Json::Value getInsights(int days = 90);
    Json::Value getSessionSignals(const std::string& date);
    Json::Value getSessionVitals(const std::string& date, int interval);
    Json::Value getSessionEvents(const std::string& date);
    /// SDD-009: cross-night event search. Empty strings / empty vector / 0
    /// mean "no filter" for their clause.
    Json::Value getEvents(const std::string& start, const std::string& end,
                          const std::vector<std::string>& types,
                          int min_duration, int limit, int offset);
    Json::Value getSessionBreaths(const std::string& date);
    Json::Value getSessionOximetry(const std::string& date, int interval);

    /// Access the underlying database (for ad-hoc queries by controller endpoints).
    std::shared_ptr<IDatabase> getDb() const { return db_; }

private:
    /**
     * SDD-039 D1: every read in this file goes through here.
     *
     * db_->executeQuery() answers an empty array both for "no rows" and for
     * "this statement did not prepare", and the sessions list is the one most
     * exposed to the second: it is a UNION ALL built for three dialects, and
     * its own comment warns that a column-order slip makes the whole list go
     * empty. Routing the reads through one place means such a slip reaches the
     * user as an error naming the column, and reaches the support log by
     * itself.
     */
    Json::Value query(const std::string& sql, const std::vector<std::string>& params = {});

    std::shared_ptr<IDatabase> db_;
    std::string device_id_;
    DbType dt_;  // cached db type for dialect helpers
};

} // namespace hms_cpap
