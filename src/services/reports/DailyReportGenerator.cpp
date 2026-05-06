#include "services/reports/DailyReportGenerator.h"
#include "services/PdfRenderer.h"
#include <filesystem>
#include <cstdio>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace hms_cpap {
namespace fs = std::filesystem;

std::string DailyReportGenerator::title() const {
    return "HMS-CPAP Daily Detail Report";
}
std::string DailyReportGenerator::subtitle(const std::string& period) const {
    return "Per-Minute Session Data — " + period;
}
std::string DailyReportGenerator::sectionSummaryHeading() const {
    return "Session Summary";
}

void DailyReportGenerator::buildCharts(const std::string& tmpDir,
                                        const std::string& start,
                                        const std::string& /*end*/,
                                        const Json::Value& /*daily*/,
                                        ChartPaths& out) {
    out.slot_a      = tmpDir + "/hr.png";
    out.leak        = tmpDir + "/leak.png";
    out.pressure    = tmpDir + "/pressure.png";
    out.spo2        = tmpDir + "/spo2.png";
    out.resp_rate   = tmpDir + "/resprate.png";
    out.tidal_vol   = tmpDir + "/tidalvol.png";
    out.minute_vent = tmpDir + "/minutevent.png";
    out.snore       = tmpDir + "/snore.png";

    auto signals = qs_->getSessionSignals(start);
    auto oxi     = qs_->getSessionOximetry(start, 60);

    // Bucket per-second/per-minute data into 30-min averages.
    // vmin: skip values <= vmin to exclude spurious zeros.
    auto extractCol30 = [](const Json::Value& root,
                            const char* tsKey, const char* valKey,
                            double vmin = -1e300) {
        std::vector<ChartPoint> pts;
        const auto& ts  = root[tsKey];
        const auto& col = root[valKey];

        std::string bucketLbl;
        double sum = 0; int cnt = 0;

        auto flush = [&]() {
            if (cnt > 0) {
                double avg = sum / cnt;
                if (std::isfinite(avg)) pts.push_back({bucketLbl, avg});
            }
            sum = 0; cnt = 0;
        };

        for (Json::ArrayIndex i = 0; i < ts.size() && i < col.size(); ++i) {
            if (col[i].isNull()) continue;
            double val;
            if (col[i].isDouble())      val = col[i].asDouble();
            else if (col[i].isInt())    val = (double)col[i].asInt();
            else if (col[i].isString()) {
                try { val = std::stod(col[i].asString()); }
                catch (...) { continue; }
            } else continue;
            if (!std::isfinite(val) || val <= vmin) continue;

            std::string t = ts[i].asString();
            if (t.size() < 16) continue;
            int hh = std::stoi(t.substr(11, 2));
            int mm = std::stoi(t.substr(14, 2));
            int bucket30 = (hh * 60 + mm) / 30;
            int bh = (bucket30 * 30) / 60;
            int bm = (bucket30 * 30) % 60;
            char lbl[8]; std::snprintf(lbl, sizeof(lbl), "%02d:%02d", bh, bm);

            if (bucketLbl.empty()) bucketLbl = lbl;
            if (std::string(lbl) != bucketLbl) { flush(); bucketLbl = lbl; }
            sum += val; ++cnt;
        }
        flush();
        return pts;
    };

    auto leakPts  = extractCol30(signals, "timestamps", "leak_rate");
    auto pressPts = extractCol30(signals, "timestamps", "mask_pressure", 0.0);
    auto rrPts    = extractCol30(signals, "timestamps", "respiratory_rate", 0.0);
    auto tvPts    = extractCol30(signals, "timestamps", "tidal_volume", 0.0);
    auto mvPts    = extractCol30(signals, "timestamps", "minute_ventilation", 0.0);
    auto snorePts = extractCol30(signals, "timestamps", "snore_index");
    auto spo2Pts  = extractCol30(oxi,     "timestamps", "spo2", 0.0);
    auto hrPts    = extractCol30(oxi,     "timestamps", "heart_rate", 0.0);

    // Store for addChartSection
    chartData_["leak"]      = leakPts;
    chartData_["pressure"]  = pressPts;
    chartData_["resp_rate"] = rrPts;
    chartData_["tidal_vol"] = tvPts;
    chartData_["minute_vent"] = mvPts;
    chartData_["snore"]     = snorePts;
    chartData_["spo2"]      = spo2Pts;
    chartData_["hr"]        = hrPts;

    if (!leakPts.empty())
        GnuplotService::renderLineChart(leakPts,  out.leak,
            "Leak Rate — 30-min avg (L/min)",           "L/min",    "#f59e0b", 0.0);
    if (!pressPts.empty())
        GnuplotService::renderLineChart(pressPts, out.pressure,
            "Mask Pressure — 30-min avg (cmH2O)",       "cmH2O",    "#8b5cf6", 0.0);
    if (!rrPts.empty())
        GnuplotService::renderLineChart(rrPts,    out.resp_rate,
            "Respiratory Rate — 30-min avg (br/min)",   "br/min",   "#81c784", 0.0);
    if (!tvPts.empty())
        GnuplotService::renderLineChart(tvPts,    out.tidal_vol,
            "Tidal Volume — 30-min avg (mL)",           "mL",       "#4dd0e1", 0.0);
    if (!mvPts.empty())
        GnuplotService::renderLineChart(mvPts,    out.minute_vent,
            "Minute Ventilation — 30-min avg (L/min)",  "L/min",    "#aed581", 0.0);
    if (!snorePts.empty())
        GnuplotService::renderLineChart(snorePts, out.snore,
            "Snore Index — 30-min avg",                 "Index",    "#ff8a65", 0.0);
    if (!spo2Pts.empty())
        GnuplotService::renderLineChart(spo2Pts,  out.spo2,
            "SpO2 — 30-min avg (%)",                    "SpO2 %",   "#e57373", 88.0, 100.0);
    if (!hrPts.empty())
        GnuplotService::renderLineChart(hrPts,    out.slot_a,
            "Heart Rate — 30-min avg (bpm)",            "bpm",      "#f06292", 30.0);
}

static std::string fmtVal(double v, int decimals = 1) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(decimals) << v;
    return ss.str();
}

static void addChartWithTable(PdfRenderer& pdf,
                               const std::string& pngPath,
                               const std::string& caption,
                               const std::string& dataKey,
                               const std::string& unit,
                               int decimals,
                               const std::map<std::string, std::vector<ChartPoint>>& chartData) {
    if (!fs::exists(pngPath)) return;
    pdf.addChart(pngPath, caption);

    auto it = chartData.find(dataKey);
    if (it == chartData.end() || it->second.empty()) return;

    std::vector<PdfRow> rows;
    for (const auto& pt : it->second) {
        rows.push_back({{pt.label, fmtVal(pt.value, decimals) + " " + unit}});
    }
    pdf.addDataTable({"Time", "Value"}, rows);
}

void DailyReportGenerator::addChartSection(PdfRenderer& pdf, const ChartPaths& charts) {
    pdf.addSectionHeading("Session Charts — 30-min averages");

    addChartWithTable(pdf, charts.pressure,    "Mask pressure — 30-min averages (cmH2O)",        "pressure",   "cmH2O",  2, chartData_);
    addChartWithTable(pdf, charts.resp_rate,   "Respiratory rate — 30-min averages (br/min)",     "resp_rate",  "br/min", 1, chartData_);
    addChartWithTable(pdf, charts.tidal_vol,   "Tidal volume — 30-min averages (mL)",             "tidal_vol",  "mL",     0, chartData_);
    addChartWithTable(pdf, charts.minute_vent, "Minute ventilation — 30-min averages (L/min)",    "minute_vent","L/min",  2, chartData_);
    addChartWithTable(pdf, charts.leak,        "Leak rate — 30-min averages (L/min)",             "leak",       "L/min",  2, chartData_);
    addChartWithTable(pdf, charts.snore,       "Snore index — 30-min averages",                   "snore",      "",       2, chartData_);
    addChartWithTable(pdf, charts.spo2,        "SpO2 — 30-min averages (%) — O2Ring",            "spo2",       "%",      1, chartData_);
    addChartWithTable(pdf, charts.slot_a,      "Heart rate — 30-min averages (bpm) — O2Ring",    "hr",         "bpm",    0, chartData_);
}

void DailyReportGenerator::addDataSection(PdfRenderer& /*pdf*/,
                                           const std::string& /*start*/,
                                           const std::string& /*end*/,
                                           const Json::Value& /*daily*/) {
    // Daily detail reports have no per-night table
}

} // namespace hms_cpap
