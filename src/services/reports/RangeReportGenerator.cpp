#include "services/reports/RangeReportGenerator.h"
#include <filesystem>

namespace hms_cpap {
namespace fs = std::filesystem;

std::string RangeReportGenerator::title() const {
    return "HMS-CPAP Therapy Report";
}
std::string RangeReportGenerator::subtitle(const std::string& period) const {
    return "Sleep Therapy Summary — " + period;
}
std::string RangeReportGenerator::sectionSummaryHeading() const {
    return "Therapy Summary";
}

void RangeReportGenerator::buildCharts(const std::string& tmpDir,
                                        const std::string& /*start*/,
                                        const std::string& /*end*/,
                                        const Json::Value& daily,
                                        ChartPaths& out) {
    out.slot_a    = tmpDir + "/ahi.png";
    out.slot_b    = tmpDir + "/usage.png";
    out.leak      = tmpDir + "/leak.png";
    out.pressure  = tmpDir + "/pressure.png";
    out.spo2      = tmpDir + "/spo2.png";

    auto makePoints = [&](const char* col) {
        std::vector<ChartPoint> pts;
        for (const auto& r : daily) {
            std::string d = js(r, "record_date");
            if (d.size() >= 10) d = d.substr(5);
            pts.push_back({d, jd(r, col)});
        }
        return pts;
    };

    std::vector<ChartPoint> usagePts;
    for (const auto& r : daily) {
        std::string d = js(r, "record_date");
        if (d.size() >= 10) d = d.substr(5);
        usagePts.push_back({d, jd(r, "duration_minutes") / 60.0});
    }

    GnuplotService::renderLineChart(makePoints("ahi"),
        out.slot_a,   "AHI (Events/Hour)",                      "Events/hr", "#ef4444", 0.0);
    GnuplotService::renderLineChart(usagePts,
        out.slot_b,   "Therapy Duration (Hours)",               "Hours",     "#3b82f6", 0.0, 10.0);
    GnuplotService::renderLineChart(makePoints("leak_95"),
        out.leak,     "Leak Rate 95th Percentile (L/min)",      "L/min",     "#f59e0b", 0.0);
    GnuplotService::renderLineChart(makePoints("mask_press_95"),
        out.pressure, "Pressure 95th Percentile (cmH2O)",       "cmH2O",     "#8b5cf6", 4.0);

    // O2Ring nightly SpO2 (fetched from DB)
    auto oxiNightly = db_->getOximetryNightlySpo2("o2ring",
        toOxiDate(js(daily[0], "record_date")),
        toOxiDate(js(daily[static_cast<int>(daily.size())-1], "record_date")));

    std::vector<ChartPoint> spo2Pts;
    for (const auto& p : oxiNightly) {
        if (p.avg_spo2 <= 0) continue;
        std::string lbl = p.date.size() == 8
            ? p.date.substr(4,2) + "-" + p.date.substr(6,2) : p.date;
        spo2Pts.push_back({lbl, p.avg_spo2});
    }
    if (!spo2Pts.empty())
        GnuplotService::renderLineChart(spo2Pts, out.spo2,
            "O2 Ring SpO2 (Average per Night)", "SpO2 %", "#10b981", 88.0, 100.0);
}

void RangeReportGenerator::addChartSection(PdfRenderer& pdf, const ChartPaths& charts) {
    pdf.addSectionHeading("Trend Charts");
    if (fs::exists(charts.slot_a))
        pdf.addChart(charts.slot_a,   "AHI trend — events per hour over the reporting period");
    if (fs::exists(charts.slot_b))
        pdf.addChart(charts.slot_b,   "Therapy duration — hours per night");
    if (fs::exists(charts.leak))
        pdf.addChart(charts.leak,     "Mask leak 95th percentile — L/min per night");
    if (fs::exists(charts.pressure))
        pdf.addChart(charts.pressure, "Machine pressure 95th percentile — cmH2O per night");
    if (fs::exists(charts.spo2))
        pdf.addChart(charts.spo2,     "O2 Ring average SpO2 per night");
}

void RangeReportGenerator::addDataSection(PdfRenderer& pdf,
                                           const std::string& /*start*/,
                                           const std::string& /*end*/,
                                           const Json::Value& daily) {
    pdf.addPageBreak();
    pdf.addSectionHeading("Per-Night Data");
    std::vector<std::string> headers = {
        "Date", "Hours", "AHI", "OAI", "CAI", "HI", "Leak95", "Press95", "SpO2 50th"
    };
    std::vector<PdfRow> rows;
    for (const auto& r : daily) {
        std::string d = js(r, "record_date");
        if (d.size() >= 10) d = d.substr(0, 10);
        double hrs  = jd(r, "duration_minutes") / 60.0;
        double spo2 = jd(r, "spo2_50");
        rows.push_back({{
            d, fmtHM(hrs),
            fmt1(jd(r, "ahi")), fmt1(jd(r, "oai")), fmt1(jd(r, "cai")), fmt1(jd(r, "hi")),
            fmt1(jd(r, "leak_95")), fmt1(jd(r, "mask_press_95")),
            spo2 > 0 ? fmt1(spo2) + "%" : "—"
        }});
    }
    pdf.addDataTable(headers, rows);
}

} // namespace hms_cpap
