#include "services/ReportGeneratorService.h"
#include "services/reports/RangeReportGenerator.h"
#include "services/reports/DailyReportGenerator.h"
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
    : db_(db), qs_(qs), mqtt_(mqtt), device_id_(device_id), logo_path_(logo_path)
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

    auto rows = db_->executeQuery(
        "INSERT INTO cpap_reports (device_id, range_start, range_end, filename, filepath, status)"
        " VALUES ($1,$2,$3,$4,$5,'pending') RETURNING id",
        {device_id_, start, end, filename, filepath});

    if (rows.empty()) return -1;
    int id = std::stoi(rows[0]["id"].asString());

    std::thread([this, id, start, end, filepath]() {
        try {
            generate(id, start, end, filepath);
        } catch (const std::exception& ex) {
            std::cerr << "[ReportGeneratorService] exception: " << ex.what() << "\n";
            db_->executeQuery(
                "UPDATE cpap_reports SET status='error', error_msg=$1 WHERE id=$2",
                {ex.what(), std::to_string(id)});
        }
    }).detach();

    return id;
}

std::vector<ReportJob> ReportGeneratorService::listReports(int limit) {
    auto rows = db_->executeQuery(
        "SELECT id, device_id, range_start, range_end, nights_count, filename, filepath,"
        " status, error_msg, created_at::text, completed_at::text"
        " FROM cpap_reports WHERE device_id=$1"
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
        " status, error_msg, created_at::text, completed_at::text"
        " FROM cpap_reports WHERE id=$1",
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
