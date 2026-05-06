#pragma once
#include <string>
#include <vector>
#include <hpdf.h>

namespace hms_cpap {

struct PdfRow {
    std::vector<std::string> cells;
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
