#pragma once
#include "services/reports/BaseReportGenerator.h"
#include "services/GnuplotService.h"
#include <map>
#include <string>

namespace hms_cpap {

// Single-night report: per-minute charts from session signals + vitals.
class DailyReportGenerator : public BaseReportGenerator {
public:
    using BaseReportGenerator::BaseReportGenerator;

protected:
    void buildCharts   (const std::string& tmpDir,
                        const std::string& start, const std::string& end,
                        const Json::Value& daily, ChartPaths& out) override;
    void addChartSection(PdfRenderer& pdf, const ChartPaths& charts) override;
    void addDataSection (PdfRenderer& pdf,
                         const std::string& start, const std::string& end,
                         const Json::Value& daily) override;

    std::string title()                             const override;
    std::string subtitle(const std::string& period) const override;
    std::string sectionSummaryHeading()             const override;

private:
    // Chart data populated in buildCharts, consumed in addChartSection
    std::map<std::string, std::vector<ChartPoint>> chartData_;
};

} // namespace hms_cpap
