#include "services/ReportGeneratorService.h"
#include "services/reports/RangeReportGenerator.h"
#include "services/reports/DailyReportGenerator.h"
#include "services/reports/ClinicianReportGenerator.h"
#include "services/reports/ComplianceReportGenerator.h"
#include "database/SqlDialect.h"
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>

namespace hms_cpap {
namespace fs = std::filesystem;

ReportGeneratorService::ReportGeneratorService(
    std::shared_ptr<IDatabase>       db,
    std::shared_ptr<QueryService>    qs,
    std::shared_ptr<hms::MqttClient> mqtt,
    const std::string& device_id,
    const std::string& archive_dir,
    const std::string& logo_path)
    : db_(db), qs_(qs), mqtt_(mqtt), device_id_(device_id), logo_path_(logo_path),
      dt_(db->dbType())
{
    report_dir_ = archive_dir + "/reports";
    fs::create_directories(report_dir_);
}

// ---- Job management --------------------------------------------------------

int ReportGeneratorService::triggerReport(const std::string& start, const std::string& end,
                                          const ReportRequest& request) {
    const std::string kind = validReportKind(request.kind) ? request.kind : "standard";
    const std::string filename = reportFilename(kind, start, end);
    std::string filepath = report_dir_ + "/" + filename;
    // SDD-082: the rule a compliance report is judged against, on its row.
    const std::string rule = kind == "compliance" ? ruleJsonOf(request.rule_hours, request.rule_share)
                                                  : std::string();

    // No RETURNING here: MySQL has none and SQLite only gained it in 3.35, so
    // the backend hands the id back its own way.
    const int id = db_->insertReturningId(
        "INSERT INTO cpap_reports (device_id, range_start, range_end, filename, filepath, status, rule)"
        " VALUES (" + sql::param(1, dt_) + "," + sql::param(2, dt_) + "," + sql::param(3, dt_) +
        "," + sql::param(4, dt_) + "," + sql::param(5, dt_) + ",'pending'," +
        (rule.empty() ? std::string("NULL") : sql::param(6, dt_)) + ")",
        rule.empty() ? std::vector<std::string>{device_id_, start, end, filename, filepath}
                     : std::vector<std::string>{device_id_, start, end, filename, filepath, rule});

    if (id < 0) return -1;

    // SDD-082: a clinician or compliance file is stored under its id, so two
    // reports of one range (two rules, say) never overwrite each other's file.
    // The standard report keeps its old path.
    if (kind != "standard") {
        filepath = report_dir_ + "/" + std::to_string(id) + "_" + filename;
        db_->executeQuery("UPDATE cpap_reports SET filepath=" + sql::param(1, dt_) +
                              " WHERE id=" + sql::param(2, dt_),
                          {filepath, std::to_string(id)});
    }

    ReportRequest req = request;
    req.kind = kind;
    std::thread([this, id, start, end, filepath, req]() {
        try {
            generate(id, start, end, filepath, req);
        } catch (const std::exception& ex) {
            std::cerr << "[ReportGeneratorService] exception: " << ex.what() << "\n";
            db_->executeQuery(
                "UPDATE cpap_reports SET status='error', error_msg=" + sql::param(1, dt_) +
                " WHERE id=" + sql::param(2, dt_),
                {ex.what(), std::to_string(id)});
        }
    }).detach();

    return id;
}

ReportJob ReportGeneratorService::jobOf(const Json::Value& r) const {
    auto jd = [&](const char* k) -> double {
        auto v = r.get(k, Json::nullValue);
        if (v.isNull()) return 0;
        if (v.isDouble() || v.isInt()) return v.asDouble();
        try { return std::stod(v.asString()); } catch (...) { return 0; }
    };
    auto js = [&](const char* k) -> std::string {
        auto v = r.get(k, Json::nullValue);
        return v.isNull() ? "" : v.asString();
    };
    ReportJob j;
    j.id           = (int)jd("id");
    j.device_id    = js("device_id");
    j.range_start  = js("range_start");
    j.range_end    = js("range_end");
    j.nights_count = (int)jd("nights_count");
    j.filename     = js("filename");
    j.filepath     = js("filepath");
    j.status       = js("status");
    j.error_msg    = js("error_msg");
    j.created_at   = js("created_at");
    j.completed_at = js("completed_at");
    j.kind         = reportKindOfFilename(j.filename);
    j.rule         = js("rule");
    return j;
}

std::vector<ReportJob> ReportGeneratorService::listReports(int limit) {
    auto rows = db_->executeQuery(
        "SELECT id, device_id, range_start, range_end, nights_count, filename, filepath,"
        " status, error_msg, rule, " + sql::tsText("created_at", dt_) + " AS created_at, "
        + sql::tsText("completed_at", dt_) + " AS completed_at"
        " FROM cpap_reports WHERE device_id=" + sql::param(1, dt_) +
        " ORDER BY created_at DESC LIMIT " + std::to_string(limit),
        {device_id_});

    std::vector<ReportJob> jobs;
    for (const auto& r : rows) jobs.push_back(jobOf(r));
    return jobs;
}

std::optional<ReportJob> ReportGeneratorService::getReport(int id) {
    auto rows = db_->executeQuery(
        "SELECT id, device_id, range_start, range_end, nights_count, filename, filepath,"
        " status, error_msg, rule, " + sql::tsText("created_at", dt_) + " AS created_at, "
        + sql::tsText("completed_at", dt_) + " AS completed_at"
        " FROM cpap_reports WHERE id=" + sql::param(1, dt_),
        {std::to_string(id)});

    if (rows.empty()) return std::nullopt;
    ReportJob j = jobOf(rows[0]);
    j.id = id;
    return j;
}

// ---- Dispatch --------------------------------------------------------------

void ReportGeneratorService::generate(int report_id,
                                       const std::string& start,
                                       const std::string& end,
                                       const std::string& filepath,
                                       const ReportRequest& request) {
    std::unique_ptr<BaseReportGenerator> gen;
    if (request.kind == "clinician")
        gen = std::make_unique<ClinicianReportGenerator>(db_, qs_, mqtt_, device_id_, logo_path_);
    else if (request.kind == "compliance")
        gen = std::make_unique<ComplianceReportGenerator>(
            db_, qs_, mqtt_, device_id_, logo_path_,
            compliance::Rule{request.rule_hours, request.rule_share});
    else if (start == end)
        gen = std::make_unique<DailyReportGenerator>(db_, qs_, mqtt_, device_id_, logo_path_);
    else
        gen = std::make_unique<RangeReportGenerator>(db_, qs_, mqtt_, device_id_, logo_path_);

    gen->setLanguage(request.language);
    gen->generate(report_id, start, end, filepath, report_dir_);
}

} // namespace hms_cpap
