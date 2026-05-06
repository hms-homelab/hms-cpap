#pragma once
#include "services/reports/BaseReportGenerator.h"

namespace hms_cpap {

// Multi-night report: one data point per night, per-night data table.
class RangeReportGenerator : public BaseReportGenerator {
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
};

} // namespace hms_cpap
