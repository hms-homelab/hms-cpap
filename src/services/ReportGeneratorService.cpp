#include "services/ReportGeneratorService.h"
#include "services/reports/RangeReportGenerator.h"
#include "services/reports/DailyReportGenerator.h"
#include "database/SqlDialect.h"
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

std::string ReportGeneratorService::buildFilename(const std::string& start,
                                                   const std::string& end) const {
    return "cpap_report_" + start + "_to_" + end + ".pdf";
}

// ---- Job management --------------------------------------------------------

int ReportGeneratorService::triggerReport(const std::string& start, const std::string& end) {
    std::string filename = buildFilename(start, end);
    std::string filepath = report_dir_ + "/" + filename;

    // No RETURNING here: MySQL has none and SQLite only gained it in 3.35, so
    // the backend hands the id back its own way.
    const int id = db_->insertReturningId(
        "INSERT INTO cpap_reports (device_id, range_start, range_end, filename, filepath, status)"
        " VALUES (" + sql::param(1, dt_) + "," + sql::param(2, dt_) + "," + sql::param(3, dt_) +
        "," + sql::param(4, dt_) + "," + sql::param(5, dt_) + ",'pending')",
        {device_id_, start, end, filename, filepath});

    if (id < 0) return -1;

    std::thread([this, id, start, end, filepath]() {
        try {
            generate(id, start, end, filepath);
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

std::vector<ReportJob> ReportGeneratorService::listReports(int limit) {
    auto rows = db_->executeQuery(
        "SELECT id, device_id, range_start, range_end, nights_count, filename, filepath,"
        " status, error_msg, " + sql::tsText("created_at", dt_) + " AS created_at, "
        + sql::tsText("completed_at", dt_) + " AS completed_at"
        " FROM cpap_reports WHERE device_id=" + sql::param(1, dt_) +
        " ORDER BY created_at DESC LIMIT " + std::to_string(limit),
        {device_id_});

    std::vector<ReportJob> jobs;
    for (const auto& r : rows) {
        ReportJob j;
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
        jobs.push_back(j);
    }
    return jobs;
}

std::optional<ReportJob> ReportGeneratorService::getReport(int id) {
    auto rows = db_->executeQuery(
        "SELECT id, device_id, range_start, range_end, nights_count, filename, filepath,"
        " status, error_msg, " + sql::tsText("created_at", dt_) + " AS created_at, "
        + sql::tsText("completed_at", dt_) + " AS completed_at"
        " FROM cpap_reports WHERE id=" + sql::param(1, dt_),
        {std::to_string(id)});

    if (rows.empty()) return std::nullopt;
    const auto& r = rows[0];
    auto js = [&](const char* k) -> std::string {
        auto v = r.get(k, Json::nullValue);
        return v.isNull() ? "" : v.asString();
    };
    auto jd = [&](const char* k) -> double {
        auto v = r.get(k, Json::nullValue);
        if (v.isNull()) return 0;
        if (v.isDouble() || v.isInt()) return v.asDouble();
        try { return std::stod(v.asString()); } catch (...) { return 0; }
    };
    ReportJob j;
    j.id           = id;
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
    return j;
}

// ---- Dispatch --------------------------------------------------------------

void ReportGeneratorService::generate(int report_id,
                                       const std::string& start,
                                       const std::string& end,
                                       const std::string& filepath) {
    std::unique_ptr<BaseReportGenerator> gen;
    if (start == end)
        gen = std::make_unique<DailyReportGenerator>(db_, qs_, mqtt_, device_id_, logo_path_);
    else
        gen = std::make_unique<RangeReportGenerator>(db_, qs_, mqtt_, device_id_, logo_path_);

    gen->generate(report_id, start, end, filepath, report_dir_);
}

} // namespace hms_cpap
