#pragma once
// SDD-082: the compliance report. CpapDash's compliance PDF from the local
// database: one question on its cover, did the nights meet the rule over this
// range? The rule, at least H hours on at least P % of the calendar nights,
// is printed as it was asked, then the verdict in the largest type on the
// page, the figures, the calendar, the rolling 30-night line, the nights
// table, and the footer that says this is a record of usage and not a
// certification. The arithmetic is figures/ComplianceFigures.h.

#include "services/reports/BaseReportGenerator.h"
#include "services/reports/figures/ComplianceFigures.h"

#include <string>
#include <utility>
#include <vector>

namespace hms_cpap {

class ComplianceReportGenerator : public BaseReportGenerator {
public:
    ComplianceReportGenerator(std::shared_ptr<IDatabase> db, std::shared_ptr<QueryService> qs,
                              std::shared_ptr<hms::MqttClient> mqtt, const std::string& device_id,
                              const std::string& logo_path, compliance::Rule rule);

    void generate(int report_id, const std::string& start, const std::string& end,
                  const std::string& filepath, const std::string& report_dir) override;

    /// This report's own labels in the report's language. A label may hold
    /// {H}, {P}, {N}, {START} and {END}, filled by fill(). Public for tests.
    std::string ct(const std::string& en) const;
    static const std::vector<std::string>& labelsForTest();

    /// "At least 4.0 hours on at least 70 % of nights, 2026-09-01 to 2026-09-30".
    std::string ruleSentence(const std::string& start, const std::string& end) const;

    /// "6.5" for 390 minutes: hours to one decimal.
    static std::string hoursOf(double minutes);

    const compliance::Rule& rule() const { return rule_; }

protected:
    // The range report's hooks: unused, this generator draws its own document.
    void buildCharts(const std::string&, const std::string&, const std::string&,
                     const Json::Value&, ChartPaths&) override {}
    void addChartSection(PdfRenderer&, const ChartPaths&) override {}
    void addDataSection(PdfRenderer&, const std::string&, const std::string&,
                        const Json::Value&) override {}
    std::string title() const override;
    std::string subtitle(const std::string& period) const override;
    std::string sectionSummaryHeading() const override;

private:
    using Calendar = std::vector<std::pair<std::string, double>>;   // date, minutes

    /// A label with its rule and range placeholders filled.
    std::string fill(const std::string& label, const std::string& start = "",
                     const std::string& end = "", int n = 0) const;
    std::vector<std::string> weekdays() const;
    void addCalendarPages(PdfRenderer& pdf, const Calendar& calendar) const;
    void addRollingChart(PdfRenderer& pdf, const std::vector<clinician::Night>& nights,
                         const std::string& start, const std::string& end,
                         const std::string& tmpDir) const;
    void addNightsTable(PdfRenderer& pdf, const Calendar& calendar,
                        const std::vector<clinician::Night>& nights) const;

    compliance::Rule rule_;
    bool ungraded_ = false;
};

}  // namespace hms_cpap
