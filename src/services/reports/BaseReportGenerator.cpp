#include "services/reports/BaseReportGenerator.h"
#include "utils/MachineIdentity.h"
#include "utils/MachineZone.h"
#include "utils/OximetryDevice.h"
#include "utils/TherapyPressure.h"
#include "services/InsightsEngine.h"
#include "parsers/CpapdashBridge.h"
#include "database/SqlDialect.h"
#include "utils/ConfigManager.h"
#include "utils/AppConfig.h"
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

bool BaseReportGenerator::gradableIndex(const Json::Value& row) {
    return js(row, "index_kind") != "ungraded";
}

std::string BaseReportGenerator::indexLabel(const Json::Value& row) {
    return gradableIndex(row) ? "AHI" : "Apnea Index";
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
    // NOW() is PostgreSQL and MySQL; SQLite has no such function and would fail
    // the statement outright.
    const DbType dt = db_->dbType();
    std::string ts = (status == "ready" || status == "error") ? sql::now(dt) : "NULL";
    db_->executeQuery(
        "UPDATE cpap_reports SET status=" + sql::param(1, dt) +
        ", error_msg=" + sql::param(2, dt) + ", completed_at=" + ts +
        " WHERE id=" + std::to_string(id),
        {status, err});
}

void BaseReportGenerator::updateNightsCount(int id, int n) {
    const DbType dt = db_->dbType();
    db_->executeQuery("UPDATE cpap_reports SET nights_count=" + sql::param(1, dt) +
                      " WHERE id=" + sql::param(2, dt),
                      {std::to_string(n), std::to_string(id)});
}

void BaseReportGenerator::publishMqtt(int id, const std::string& filename, int nights,
                                      const Json::Value& extra) {
    if (!mqtt_ || !mqtt_->isConnected()) return;
    Json::Value payload(Json::objectValue);
    payload["report_id"]    = id;
    payload["filename"]     = filename;
    payload["nights_count"] = nights;
    payload["status"]       = "ready";
    // SDD-082: additive keys (kind, and a compliance report's verdict).
    if (extra.isObject())
        for (const auto& k : extra.getMemberNames()) payload[k] = extra[k];
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    mqtt_->publish("cpap/" + device_id_ + "/reports/ready", Json::writeString(w, payload), 0, false);
}

std::string BaseReportGenerator::coverMachineName() const {
    // The configured machine, not a hardcoded one; #94: and when the
    // configured name is one the service filled in itself, the machine's own
    // model, as the dashboard's title names it.
    const auto identity = installIdentity(
        ConfigManager::get("CPAP_SOURCE", "") == "local" ? ConfigManager::get("CPAP_LOCAL_DIR", "") : "",
        ConfigManager::get("CPAP_ARCHIVE_DIR", ""), AppConfig::dataDir());
    return machineName(ConfigManager::get("CPAP_DEVICE_NAME", AppConfig::kDefaultDeviceName), identity);
}

std::string BaseReportGenerator::downloadName(const std::string& filepath) {
    const std::string base = fs::path(filepath).filename().string();
    const auto us = base.find('_');
    if (us == std::string::npos || us == 0) return base;
    for (std::size_t i = 0; i < us; ++i)
        if (base[i] < '0' || base[i] > '9') return base;
    return base.substr(us + 1);
}

std::size_t BaseReportGenerator::languageIndex() const {
    static const char* kLangs[] = {"es", "pt", "hu", "fr"};
    for (std::size_t i = 0; i < 4; ++i)
        if (language_ == kLangs[i]) return i + 1;
    return 0;
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
        {"Average " + indexLabel(st),  fmt1(jd(st, "avg_ahi")) + " events/hr"},
        {"Best " + indexLabel(st),     fmt1(jd(st, "min_ahi")) + " events/hr"},
        {"Worst " + indexLabel(st),    fmt1(jd(st, "max_ahi")) + " events/hr"},
        {indexLabel(st) + " Std Dev",  fmt1(jd(st, "stddev_ahi"))},
        {"Avg Leak 95th (L/min)",      fmt1(jd(st, "avg_leak_95"))},
        // Same reason as the per-night table: a machine with no mask-pressure
        // channel averages to 0, and "0.0 cmH2O" is a reading rather than a
        // blank (issue 15).
        {"Avg Pressure 95th (cmH2O)",  jd(st, "avg_pressure_95") > 0
                                           ? fmt1(jd(st, "avg_pressure_95")) : "N/A"},
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
            rec.record_date = wall::point(wall::labelOf(tm));   // a label (SDD-074 D4)
        }
        rec.duration_minutes = jd(r, "duration_minutes");
        rec.ahi       = jd(r, "ahi");
        rec.hi        = jd(r, "hi");
        rec.ai        = jd(r, "ai");
        rec.oai       = jd(r, "oai");
        rec.cai       = jd(r, "cai");
        rec.leak_95   = jd(r, "leak_95");
        rec.leak_50   = jd(r, "leak_50");
        // Issue #56: the insights speak of the machine's pressure.
        rec.mask_press_50 = machinePressure(r, "50");
        rec.mask_press_95 = machinePressure(r, "95");
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

    auto oxiRange   = db_->getOximetryRangeSummary(kOximetryDeviceId, toOxiDate(start), toOxiDate(end));

    // Build charts
    std::string tmpDir = "/tmp/cpap_report_" + std::to_string(report_id);
    fs::create_directories(tmpDir);
    ChartPaths charts;
    buildCharts(tmpDir, start, end, daily, charts);

    // Build PDF
    std::string period = (start == end) ? start : (start + " to " + end);
    PdfRenderer pdf;
    // The configured machine, not a hardcoded one. This is the cover of a
    // document a user hands to a clinician, and it named every report "ResMed
    // AirSense 10" regardless of what produced the data -- so a Sefam S.Box
    // report was attributed to a machine the patient does not own. #94: and
    // when the configured name is one the service filled in itself, the
    // machine's own model, as the dashboard's title names it.
    const auto identity = installIdentity(
        ConfigManager::get("CPAP_SOURCE", "") == "local" ? ConfigManager::get("CPAP_LOCAL_DIR", "") : "",
        ConfigManager::get("CPAP_ARCHIVE_DIR", ""), AppConfig::dataDir());
    pdf.addCoverPage(title(), subtitle(period),
                     machineName(ConfigManager::get("CPAP_DEVICE_NAME", AppConfig::kDefaultDeviceName), identity),
                     period, nowStr(), logo_path_);

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
