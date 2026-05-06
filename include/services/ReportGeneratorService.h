#pragma once
#include "database/IDatabase.h"
#include "web/QueryService.h"
#include "services/reports/BaseReportGenerator.h"
#include "mqtt_client.h"
#include <string>
#include <memory>
#include <functional>

namespace hms_cpap {

struct ReportJob {
    int         id          = 0;
    std::string device_id;
    std::string range_start;  // YYYY-MM-DD
    std::string range_end;
    std::string filename;     // basename e.g. cpap_report_2026-01-01_to_2026-01-31.pdf
    std::string filepath;     // full absolute path
    std::string status;       // pending, generating, ready, error
    std::string error_msg;
    int         nights_count = 0;
    std::string created_at;
    std::string completed_at;
};

class ReportGeneratorService {
public:
    ReportGeneratorService(
        std::shared_ptr<IDatabase> db,
        std::shared_ptr<QueryService> qs,
        std::shared_ptr<hms::MqttClient> mqtt,
        const std::string& device_id,
        const std::string& archive_dir,
        const std::string& logo_path = "");

    // Create a report job and kick off async generation.
    // Returns the report id on success, -1 on error.
    int triggerReport(const std::string& start, const std::string& end);

    // List reports for device, most-recent first.
    std::vector<ReportJob> listReports(int limit = 20);

    // Get a single report by id.
    std::optional<ReportJob> getReport(int id);

private:
    std::shared_ptr<IDatabase>          db_;
    std::shared_ptr<QueryService>       qs_;
    std::shared_ptr<hms::MqttClient>    mqtt_;
    std::string                         device_id_;
    std::string                         report_dir_;
    std::string                         logo_path_;

    std::string buildFilename(const std::string& start, const std::string& end) const;
    // Picks DailyReportGenerator or RangeReportGenerator and calls generate().
    void generate(int report_id, const std::string& start, const std::string& end,
                  const std::string& filepath);
};

} // namespace hms_cpap
