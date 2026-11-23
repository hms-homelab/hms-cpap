#pragma once
#include <string>
#include <vector>
#include <hpdf.h>

namespace hms_cpap {

struct PdfRow {
    std::vector<std::string> cells;
    // SDD-082: a section row in a grid table (its first cell spans the table,
    // bold, on grey): the "CPAP usage" / "Therapy efficacy" bands.
    bool section{false};
};

// SDD-082: one calendar night in the compliance calendar.
struct CalendarCell {
    std::string date;    // what the cell's corner reads ("09-17", or "17")
    std::string hours;   // what the cell's middle reads ("6.5")
    // 0 blank (a day outside the range), 1 no use, 2 under the rule's hours,
    // 3 at or over them: the three colours.
    int state{0};
};

// Stateful PDF builder. Call sections in order, then save().
class PdfRenderer {
public:
    PdfRenderer();
    ~PdfRenderer();

    // Disable copy
    PdfRenderer(const PdfRenderer&) = delete;
    PdfRenderer& operator=(const PdfRenderer&) = delete;

    // Section builders — call in sequence
    void addCoverPage(const std::string& title, const std::string& subtitle,
                      const std::string& device, const std::string& period,
                      const std::string& generated,
                      const std::string& logo_path = "");

    void addSectionHeading(const std::string& text);
    void addParagraph(const std::string& text);
    void addKeyValueTable(const std::vector<std::pair<std::string,std::string>>& rows);
    void addDataTable(const std::vector<std::string>& headers,
                      const std::vector<PdfRow>& rows);
    void addInsightBlock(const std::string& title, const std::string& body,
                         const std::string& category); // category: positive/warning/info/actionable
    void addChart(const std::string& png_path, const std::string& caption);
    // SDD-082 ------------------------------------------------------------------
    // A metric table: the first column `first_col` points wide and left
    // aligned, the rest share what is left and are right aligned. A header may
    // hold a second line after '\n' (a window's dates), drawn smaller.
    void addGridTable(const std::vector<std::string>& headers, const std::vector<PdfRow>& rows,
                      float first_col = 150.0f, float font_size = 8.0f);
    // The same with every column's width given, as proportions of the page.
    void addGridTable(const std::vector<std::string>& headers, const std::vector<PdfRow>& rows,
                      const std::vector<float>& widths, float font_size);
    // Vertical space, e.g. between a table and the next heading.
    void addSpacer(float pts);
    // A chart at a chosen height (an event strip is short, a waveform tall).
    void addChartSized(const std::string& png_path, const std::string& caption, float height);
    // Small grey text: a footnote under a table.
    void addNote(const std::string& text);
    // The disclaimer a report ends with: a rule, then small grey text, at the
    // bottom of the last page.
    void addDisclaimer(const std::string& text);
    // The verdict in the largest type on the page, centred on a band: green
    // when met, red otherwise. Shrinks to fit the width in any language.
    void addVerdict(const std::string& text, bool met);
    // One cell per calendar night, seven to a row under `weekdays` (seven
    // headers, Monday first), the first cell `first_column` cells in (the
    // first night's weekday), each coloured by its state. Drawn whole on one
    // page; a caption above it when given.
    void addCalendar(const std::string& caption, const std::vector<std::string>& weekdays,
                     int first_column, const std::vector<CalendarCell>& cells);
    void addPageBreak();

    bool save(const std::string& path);

private:
    HPDF_Doc  pdf_   = nullptr;
    HPDF_Page page_  = nullptr;
    HPDF_Font font_bold_   = nullptr;
    HPDF_Font font_normal_ = nullptr;

    float cursor_y_ = 0.0f;
    static constexpr float kMarginL = 50.0f;
    static constexpr float kMarginR = 50.0f;
    static constexpr float kMarginTop = 60.0f;
    static constexpr float kMarginBot = 60.0f;
    static constexpr float kPageW = 595.0f;  // A4
    static constexpr float kPageH = 842.0f;

    void ensurePage();
    void newPage();
    float contentWidth() const { return kPageW - kMarginL - kMarginR; }
    void moveDown(float pts);
    // Returns actual height consumed; wraps text to width
    float drawWrappedText(const std::string& text, HPDF_Font font, float size,
                          float x, float y, float maxWidth,
                          unsigned int r, unsigned int g, unsigned int b);
    void checkPageBreak(float needed);
    static void errorHandler(HPDF_STATUS ec, HPDF_STATUS dc, void*);
};

} // namespace hms_cpap
