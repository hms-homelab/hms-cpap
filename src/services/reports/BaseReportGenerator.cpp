#include "services/reports/BaseReportGenerator.h"
#include "services/InsightsEngine.h"
#include "parsers/CpapdashBridge.h"
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <cmath>

namespace hms_cpap {
namespace fs = std::filesystem;

// ---- Static helpers --------------------------------------------------------

double BaseReportGenerator::jd(const Json::Value& o, const char* k) {
    auto v = o.get(k, Json::nullValue);
    if (v.isNull()) return 0.0;
    if (v.isDouble() || v.isInt()) return v.asDouble();
    if (v.isString()) { try { return std::stod(v.asString()); } catch (...) {} }
    return 0.0;
}

std::string BaseReportGenerator::js(const Json::Value& o, const char* k) {
    auto v = o.get(k, Json::nullValue);
    if (v.isNull()) return "";
    return v.asString();
}

std::string BaseReportGenerator::fmt1(double v) {
    std::ostringstream o; o << std::fixed << std::setprecision(1) << v; return o.str();
}

std::string BaseReportGenerator::fmt0(double v) {
    std::ostringstream o; o << std::fixed << std::setprecision(0) << v; return o.str();
}

std::string BaseReportGenerator::fmtHM(double hours) {
    int h = (int)hours;
    int m = (int)std::round((hours - h) * 60);
    std::ostringstream o; o << h << "h " << m << "m"; return o.str();
}

std::string BaseReportGenerator::nowStr() {
    std::time_t t = std::time(nullptr);
    std::tm tm = *std::localtime(&t);
    std::ostringstream o; o << std::put_time(&tm, "%Y-%m-%d %H:%M");
    return o.str();
}

std::string BaseReportGenerator::toOxiDate(const std::string& s) {
    return s.substr(0,4) + s.substr(5,2) + s.substr(8,2);
}

// ---- Constructor -----------------------------------------------------------

BaseReportGenerator::BaseReportGenerator(
    std::shared_ptr<IDatabase>       db,
    std::shared_ptr<QueryService>    qs,
    std::shared_ptr<hms::MqttClient> mqtt,
    const std::string& device_id,
    const std::string& logo_path)
    : db_(db), qs_(qs), mqtt_(mqtt), device_id_(device_id), logo_path_(logo_path)
{}

// ---- DB helpers ------------------------------------------------------------

void BaseReportGenerator::updateStatus(int id, const std::string& status,
                                        const std::string& err) {
    std::string ts = (status == "ready" || status == "error") ? "NOW()" : "NULL";
    db_->executeQuery(
        "UPDATE cpap_reports SET status=$1, error_msg=$2, completed_at=" + ts +
        " WHERE id=" + std::to_string(id),
        {status, err});
}

void BaseReportGenerator::updateNightsCount(int id, int n) {
    db_->executeQuery("UPDATE cpap_reports SET nights_count=$1 WHERE id=$2",
                      {std::to_string(n), std::to_string(id)});
}

void BaseReportGenerator::publishMqtt(int id, const std::string& filename, int nights) {
    if (!mqtt_ || !mqtt_->isConnected()) return;
    std::ostringstream payload;
    payload << R"({"report_id":)" << id
            << R"(,"filename":")" << filename << R"(")"
            << R"(,"nights_count":)" << nights
            << R"(,"status":"ready"})";
    mqtt_->publish("cpap/" + device_id_ + "/reports/ready", payload.str(), 0, false);
}

// ---- Shared PDF sections ---------------------------------------------------

void BaseReportGenerator::addSummarySection(PdfRenderer& pdf, const Json::Value& st,
                                             const std::string& start, const std::string& end,
                                             int nights) {
    pdf.addSectionHeading(sectionSummaryHeading());
    double avgHours   = jd(st, "avg_duration_min") / 60.0;
    double compliance = jd(st, "compliance_pct");
    double totalHours = jd(st, "total_therapy_minutes") / 60.0;
    pdf.addKeyValueTable({
        {"Reporting Period",           start + " to " + end},
        {"Total Nights",               std::to_string(nights)},
        {"Total Therapy Time",         fmtHM(totalHours)},
        {"Avg Usage / Night",          fmtHM(avgHours)},
        {"Compliance (>= 4 hrs)",      fmt1(compliance) + "%"},
        {"Average AHI",                fmt1(jd(st, "avg_ahi")) + " events/hr"},
        {"Best AHI",                   fmt1(jd(st, "min_ahi")) + " events/hr"},
        {"Worst AHI",                  fmt1(jd(st, "max_ahi")) + " events/hr"},
        {"AHI Std Dev",                fmt1(jd(st, "stddev_ahi"))},
        {"Avg Leak 95th (L/min)",      fmt1(jd(st, "avg_leak_95"))},
        {"Avg Pressure 95th (cmH2O)",  fmt1(jd(st, "avg_pressure_95"))},
        {"Avg SpO2",                   jd(st, "avg_spo2") > 0
                                           ? fmt1(jd(st, "avg_spo2")) + "%" : "N/A"},
    });
}

void BaseReportGenerator::addOximetrySection(PdfRenderer& pdf,
                                              const IDatabase::OxiRangeSummary& oxi) {
    if (!oxi.found || oxi.nights == 0) return;
    pdf.addSectionHeading("Oximetry Summary (O2 Ring)");
    pdf.addKeyValueTable({
        {"Nights with O2 Data",     std::to_string(oxi.nights)},
        {"Average SpO2",            fmt1(oxi.avg_spo2) + "%"},
        {"Minimum SpO2",            fmt1(oxi.min_spo2) + "%"},
        {"ODI 3% (events/hr)",      fmt1(oxi.avg_odi)},
        {"Avg Time Below 90% SpO2", fmt1(oxi.avg_below_90) + " min"},
        {"Average Heart Rate",      fmt0(oxi.avg_hr) + " bpm"},
    });
}

void BaseReportGenerator::addInsightsSection(PdfRenderer& pdf,
                                              const Json::Value& daily,
                                              const std::string& start,
                                              const std::string& end) {
    // Build STRDailyRecord vector
    std::vector<STRDailyRecord> records;
    for (const auto& r : daily) {
        STRDailyRecord rec;
        rec.device_id = device_id_;
        std::string ds = js(r, "record_date");
        if (ds.size() >= 10) {
            std::tm tm{};
            tm.tm_year = std::stoi(ds.substr(0,4)) - 1900;
            tm.tm_mon  = std::stoi(ds.substr(5,2)) - 1;
            tm.tm_mday = std::stoi(ds.substr(8,2));
            tm.tm_hour = 12;
            rec.record_date = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        }
        rec.duration_minutes = jd(r, "duration_minutes");
        rec.ahi       = jd(r, "ahi");
        rec.hi        = jd(r, "hi");
        rec.ai        = jd(r, "ai");
        rec.oai       = jd(r, "oai");
        rec.cai       = jd(r, "cai");
        rec.leak_95   = jd(r, "leak_95");
        rec.leak_50   = jd(r, "leak_50");
        rec.mask_press_50 = jd(r, "mask_press_50");
        rec.mask_press_95 = jd(r, "mask_press_95");
        rec.spo2_50   = jd(r, "spo2_50");
        records.push_back(rec);
    }

    auto insights = InsightsEngine::analyze(records);
    pdf.addSectionHeading("Therapy Insights");
    if (insights.empty() || (insights.size() == 1 && insights[0].category == "info")) {
        pdf.addParagraph("Not enough data to generate insights (minimum 7 therapy nights required).");
    } else {
        for (const auto& ins : insights)
            pdf.addInsightBlock(ins.title, ins.body, ins.category);
    }
}

// ---- Main orchestration ----------------------------------------------------

void BaseReportGenerator::generate(int report_id,
                                    const std::string& start,
                                    const std::string& end,
                                    const std::string& filepath,
                                    const std::string& report_dir) {
    (void)report_dir;
    updateStatus(report_id, "generating");

    // Fetch aggregate stats
    Json::Value stats = qs_->getStatistics(start, end);
    if (stats.empty()) {
        updateStatus(report_id, "error", "No data for date range");
        return;
    }
    const auto& st = stats[0];
    int nights = (int)jd(st, "total_nights");
    updateNightsCount(report_id, nights);

    if (nights == 0) {
        updateStatus(report_id, "error", "No therapy nights in range");
        return;
    }

    Json::Value daily = qs_->getDailySummary(start, end);

    auto oxiRange   = db_->getOximetryRangeSummary("o2ring", toOxiDate(start), toOxiDate(end));

    // Build charts
    std::string tmpDir = "/tmp/cpap_report_" + std::to_string(report_id);
    fs::create_directories(tmpDir);
    ChartPaths charts;
    buildCharts(tmpDir, start, end, daily, charts);

    // Build PDF
    std::string period = (start == end) ? start : (start + " to " + end);
    PdfRenderer pdf;
    pdf.addCoverPage(title(), subtitle(period), "ResMed AirSense 10", period, nowStr(), logo_path_);

    addSummarySection (pdf, st, start, end, nights);
    addOximetrySection(pdf, oxiRange);
    addInsightsSection(pdf, daily, start, end);

    pdf.addPageBreak();
    addChartSection(pdf, charts);

    addDataSection(pdf, start, end, daily);

    // Save
    if (!pdf.save(filepath)) {
        fs::remove_all(tmpDir);
        updateStatus(report_id, "error", "PDF save failed");
        return;
    }

    fs::remove_all(tmpDir);
    updateStatus(report_id, "ready");

    std::string filename = "cpap_report_" + start + "_to_" + end + ".pdf";
    publishMqtt(report_id, filename, nights);
    std::cout << "[ReportGenerator] report " << report_id << " ready: " << filepath << "\n";
}

} // namespace hms_cpap
