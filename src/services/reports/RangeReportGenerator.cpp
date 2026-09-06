#include "services/reports/RangeReportGenerator.h"
#include "utils/OximetryDevice.h"
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

    // SDD-024: the chart is titled by the index it plots. Any ungraded night in
    // the range makes the whole series ungraded -- a line that is an AHI for
    // half its length and an apnea index for the other half cannot be titled
    // either one, and the weaker claim is the true one.
    const bool graded = [&] {
        for (const auto& r : daily) if (!gradableIndex(r)) return false;
        return true;
    }();
    const std::string idx = graded ? "AHI" : "Apnea Index";

    GnuplotService::renderLineChart(makePoints("ahi"),
        out.slot_a,   idx + " (Events/Hour)",                    "Events/hr", "#ef4444", 0.0);
    GnuplotService::renderLineChart(usagePts,
        out.slot_b,   "Therapy Duration (Hours)",               "Hours",     "#3b82f6", 0.0, 10.0);
    GnuplotService::renderLineChart(makePoints("leak_95"),
        out.leak,     "Leak Rate 95th Percentile (L/min)",      "L/min",     "#f59e0b", 0.0);
    GnuplotService::renderLineChart(makePoints("mask_press_95"),
        out.pressure, "Pressure 95th Percentile (cmH2O)",       "cmH2O",     "#8b5cf6", 4.0);

    // O2Ring nightly SpO2 (fetched from DB)
    auto oxiNightly = db_->getOximetryNightlySpo2(kOximetryDeviceId,
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
        pdf.addChart(charts.slot_a,   "Event index trend — events per hour over the reporting period");
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
    // SDD-024: same range-level rule as the chart above.
    const bool graded = [&] {
        for (const auto& r : daily) if (!gradableIndex(r)) return false;
        return true;
    }();

    std::vector<std::string> headers = {
        "Date", "Hours", graded ? "AHI" : "Apnea Index",
        "OAI", "CAI", "HI", "Leak95", "Press95", "SpO2 50th"
    };
    std::vector<PdfRow> rows;
    for (const auto& r : daily) {
        std::string d = js(r, "record_date");
        if (d.size() >= 10) d = d.substr(0, 10);
        double hrs  = jd(r, "duration_minutes") / 60.0;
        double spo2 = jd(r, "spo2_50");
        // Per ROW, not per range: the type columns are zero on a night whose
        // machine did not classify, and a printed 0.0 in an OAI column is read
        // by a clinician as "no obstructive apneas". Nothing was measured, so
        // nothing is printed.
        //
        // "n/a" rather than an em dash: the PDF's base font has no glyph for
        // one, so the existing "—" in the SpO2 column below prints as an EMPTY
        // cell. Empty reads as a rendering gap; the reader cannot tell it from
        // a column that failed to populate. ASCII prints.
        const bool typed = gradableIndex(r);
        const double press95 = jd(r, "mask_press_95");
        rows.push_back({{
            d, fmtHM(hrs),
            fmt1(jd(r, "ahi")),
            typed ? fmt1(jd(r, "oai")) : "n/a",
            typed ? fmt1(jd(r, "cai")) : "n/a",
            typed ? fmt1(jd(r, "hi"))  : "n/a",
            fmt1(jd(r, "leak_95")),
            // A machine with no mask-pressure channel leaves this 0, and a
            // printed 0.0 cmH2O is a reading, not a blank (issue 15).
            press95 > 0 ? fmt1(press95) : "n/a",
            spo2 > 0 ? fmt1(spo2) + "%" : "n/a"
        }});
    }
    pdf.addDataTable(headers, rows);
}

} // namespace hms_cpap
