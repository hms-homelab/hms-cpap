#pragma once

#include "parsers/SleeplinkBridge.h"
#include <string>
#include <vector>
#include <json/json.h>

namespace hms_cpap {

struct Insight {
    std::string title;
    std::string body;
    std::string category;  // positive, warning, alert, actionable, info
    std::string metric;    // AHI, Leak, Pressure, Hours, Summary
    double value = 0;
};

class InsightsEngine {
public:
    static std::vector<Insight> analyze(const std::vector<STRDailyRecord>& records);
    static Json::Value toJson(const std::vector<Insight>& insights);

private:
    static std::vector<Insight> ahiTrend(const std::vector<STRDailyRecord>& sorted);
    static std::vector<Insight> leakCorrelation(const std::vector<STRDailyRecord>& sorted);
    static std::vector<Insight> pressureTrend(const std::vector<STRDailyRecord>& sorted);
    static std::vector<Insight> therapyCompliance(const std::vector<STRDailyRecord>& sorted);
    static std::vector<Insight> bestWorstNights(const std::vector<STRDailyRecord>& sorted);
    static std::vector<Insight> recentSummary(const std::vector<STRDailyRecord>& sorted);

    static double mean(const std::vector<double>& vals);
    static std::vector<STRDailyRecord> lastN(const std::vector<STRDailyRecord>& sorted, size_t n);
    static std::string formatDate(const STRDailyRecord& r);
};

} // namespace hms_cpap
