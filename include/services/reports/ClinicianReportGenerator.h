#pragma once
// SDD-082: the clinician report. CpapDash's clinician PDF from the local
// database: the statistics grid over fixed windows and the user's range, CMS
// adherence, the monthly page, settings periods, and a page for the last and
// the worst night. The arithmetic is figures/ClinicianFigures.h.

#include "services/reports/BaseReportGenerator.h"
#include "services/reports/figures/ClinicianFigures.h"

#include <string>
#include <vector>

namespace hms_cpap {

class ClinicianReportGenerator : public BaseReportGenerator {
public:
    using BaseReportGenerator::BaseReportGenerator;

    void generate(int report_id, const std::string& start, const std::string& end,
                  const std::string& filepath, const std::string& report_dir) override;

    /// This report's own labels in the report's language (accent-free for the
    /// core PDF font, keyed on the English). Public for tests.
    std::string ct(const std::string& en) const;
    static const std::vector<std::string>& labelsForTest();

    /// "-" for an absent figure, else the value with `decimals`.
    static std::string cell(const std::optional<double>& v, int decimals = 2,
                            const std::string& suffix = "");

    /// The disclaimer in the report's language (shared with the compliance report).
    static std::string disclaimerIn(std::size_t language_index);

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
    /// SDD-082 B7: the index's name when every night is ungraded.
    std::string ix(const std::string& en) const;
    std::vector<PdfRow> gridRows(const std::vector<clinician::WindowFigures>& figs) const;
    void addGrid(PdfRenderer& pdf, const std::vector<clinician::Window>& windows,
                 const std::vector<clinician::Night>& nights, bool header_dates) const;
    void addNightPage(PdfRenderer& pdf, const clinician::Night& night, const std::string& why,
                      const std::string& tmpDir, int index) const;

    bool ungraded_ = false;
};

}  // namespace hms_cpap
