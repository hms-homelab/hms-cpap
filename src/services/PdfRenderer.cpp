#include "services/PdfRenderer.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <hpdf.h>

namespace hms_cpap {

void PdfRenderer::errorHandler(HPDF_STATUS ec, HPDF_STATUS dc, void*) {
    std::cerr << "[PdfRenderer] libharu error ec=" << ec << " dc=" << dc << "\n";
}

PdfRenderer::PdfRenderer() {
    pdf_ = HPDF_New(errorHandler, nullptr);
    if (!pdf_) {
        std::cerr << "[PdfRenderer] HPDF_New failed\n";
        return;
    }
    font_bold_   = HPDF_GetFont(pdf_, "Helvetica-Bold",   nullptr);
    font_normal_ = HPDF_GetFont(pdf_, "Helvetica",        nullptr);
}

PdfRenderer::~PdfRenderer() {
    if (pdf_) HPDF_Free(pdf_);
}

void PdfRenderer::newPage() {
    page_ = HPDF_AddPage(pdf_);
    HPDF_Page_SetSize(page_, HPDF_PAGE_SIZE_A4, HPDF_PAGE_PORTRAIT);
    cursor_y_ = kPageH - kMarginTop;
}

void PdfRenderer::ensurePage() {
    if (!page_) newPage();
}

void PdfRenderer::moveDown(float pts) {
    cursor_y_ -= pts;
}

void PdfRenderer::checkPageBreak(float needed) {
    if (cursor_y_ - needed < kMarginBot) newPage();
}

// Simple single-line text draw, returns line height
float PdfRenderer::drawWrappedText(const std::string& text, HPDF_Font font, float size,
                                    float x, float y, float maxWidth,
                                    unsigned int r, unsigned int g, unsigned int b) {
    HPDF_Page_SetFontAndSize(page_, font, size);
    HPDF_Page_SetRGBFill(page_, r/255.0f, g/255.0f, b/255.0f);

    float lineHeight = size * 1.4f;
    float spaceWidth = HPDF_Page_TextWidth(page_, " ");

    // Split into words and wrap
    std::istringstream ss(text);
    std::string word;
    std::string line;
    float lineY = y;
    float usedW = 0.0f;
    float totalH = 0.0f;

    auto flushLine = [&]() {
        if (line.empty()) return;
        HPDF_Page_BeginText(page_);
        HPDF_Page_TextOut(page_, x, lineY, line.c_str());
        HPDF_Page_EndText(page_);
        lineY -= lineHeight;
        totalH += lineHeight;
        line.clear();
        usedW = 0.0f;
    };

    while (ss >> word) {
        float ww = HPDF_Page_TextWidth(page_, word.c_str());
        if (!line.empty() && usedW + spaceWidth + ww > maxWidth) {
            flushLine();
        }
        if (!line.empty()) { line += ' '; usedW += spaceWidth; }
        line += word;
        usedW += ww;
    }
    flushLine();
    return totalH > 0 ? totalH : lineHeight;
}

void PdfRenderer::addCoverPage(const std::string& title, const std::string& subtitle,
                                const std::string& device, const std::string& period,
                                const std::string& generated,
                                const std::string& logo_path) {
    if (!pdf_) return;
    newPage();

    // Blue header band
    HPDF_Page_SetRGBFill(page_, 0.22f, 0.39f, 0.70f);
    HPDF_Page_Rectangle(page_, 0, kPageH - 120, kPageW, 120);
    HPDF_Page_Fill(page_);

    // Logo top-right in header band (if available)
    if (!logo_path.empty()) {
        HPDF_Image logo = HPDF_LoadPngImageFromFile(pdf_, logo_path.c_str());
        if (logo) {
            float logoSz = 80.0f;
            HPDF_Page_DrawImage(page_, logo,
                kPageW - kMarginR - logoSz,
                kPageH - 110,
                logoSz, logoSz);
        }
    }

    // Title
    HPDF_Page_SetFontAndSize(page_, font_bold_, 26);
    HPDF_Page_SetRGBFill(page_, 1, 1, 1);
    HPDF_Page_BeginText(page_);
    HPDF_Page_TextOut(page_, kMarginL, kPageH - 65, title.c_str());
    HPDF_Page_EndText(page_);

    // Subtitle
    HPDF_Page_SetFontAndSize(page_, font_normal_, 13);
    HPDF_Page_SetRGBFill(page_, 0.85f, 0.90f, 1.0f);
    HPDF_Page_BeginText(page_);
    HPDF_Page_TextOut(page_, kMarginL, kPageH - 90, subtitle.c_str());
    HPDF_Page_EndText(page_);

    cursor_y_ = kPageH - 150;

    // Device / period / generated lines
    HPDF_Page_SetFontAndSize(page_, font_bold_, 11);
    HPDF_Page_SetRGBFill(page_, 0.2f, 0.2f, 0.2f);
    HPDF_Page_BeginText(page_);
    HPDF_Page_TextOut(page_, kMarginL, cursor_y_, ("Device: " + device).c_str());
    HPDF_Page_EndText(page_);
    moveDown(18);

    HPDF_Page_BeginText(page_);
    HPDF_Page_TextOut(page_, kMarginL, cursor_y_, ("Period: " + period).c_str());
    HPDF_Page_EndText(page_);
    moveDown(18);

    HPDF_Page_BeginText(page_);
    HPDF_Page_TextOut(page_, kMarginL, cursor_y_, ("Generated: " + generated).c_str());
    HPDF_Page_EndText(page_);
    moveDown(30);

    // Separator line
    HPDF_Page_SetRGBStroke(page_, 0.22f, 0.39f, 0.70f);
    HPDF_Page_SetLineWidth(page_, 1.5f);
    HPDF_Page_MoveTo(page_, kMarginL, cursor_y_);
    HPDF_Page_LineTo(page_, kPageW - kMarginR, cursor_y_);
    HPDF_Page_Stroke(page_);
    moveDown(20);
}

void PdfRenderer::addSectionHeading(const std::string& text) {
    if (!pdf_) return;
    ensurePage();
    checkPageBreak(30);

    HPDF_Page_SetFontAndSize(page_, font_bold_, 13);
    HPDF_Page_SetRGBFill(page_, 0.22f, 0.39f, 0.70f);
    HPDF_Page_BeginText(page_);
    HPDF_Page_TextOut(page_, kMarginL, cursor_y_, text.c_str());
    HPDF_Page_EndText(page_);
    moveDown(6);

    // Underline
    HPDF_Page_SetRGBStroke(page_, 0.22f, 0.39f, 0.70f);
    HPDF_Page_SetLineWidth(page_, 0.8f);
    HPDF_Page_MoveTo(page_, kMarginL, cursor_y_);
    HPDF_Page_LineTo(page_, kPageW - kMarginR, cursor_y_);
    HPDF_Page_Stroke(page_);
    moveDown(14);
}

void PdfRenderer::addParagraph(const std::string& text) {
    if (!pdf_) return;
    ensurePage();
    checkPageBreak(20);
    float h = drawWrappedText(text, font_normal_, 10, kMarginL, cursor_y_,
                               contentWidth(), 60, 60, 60);
    moveDown(h + 6);
}

void PdfRenderer::addKeyValueTable(const std::vector<std::pair<std::string,std::string>>& rows) {
    if (!pdf_ || rows.empty()) return;
    ensurePage();

    float rowH = 18.0f;
    float colW = contentWidth() / 2.0f;
    bool shade = false;

    for (const auto& [k, v] : rows) {
        checkPageBreak(rowH);
        float rx = kMarginL;
        float ry = cursor_y_ - rowH + 4;

        // Alternating row background
        if (shade) {
            HPDF_Page_SetRGBFill(page_, 0.95f, 0.97f, 1.0f);
            HPDF_Page_Rectangle(page_, rx, ry, contentWidth(), rowH);
            HPDF_Page_Fill(page_);
        }

        HPDF_Page_SetFontAndSize(page_, font_bold_, 9);
        HPDF_Page_SetRGBFill(page_, 0.2f, 0.2f, 0.2f);
        HPDF_Page_BeginText(page_);
        HPDF_Page_TextOut(page_, rx + 4, cursor_y_ - 10, k.c_str());
        HPDF_Page_EndText(page_);

        HPDF_Page_SetFontAndSize(page_, font_normal_, 9);
        HPDF_Page_BeginText(page_);
        HPDF_Page_TextOut(page_, rx + colW + 4, cursor_y_ - 10, v.c_str());
        HPDF_Page_EndText(page_);

        shade = !shade;
        moveDown(rowH);
    }
    moveDown(10);
}

void PdfRenderer::addDataTable(const std::vector<std::string>& headers,
                                const std::vector<PdfRow>& rows) {
    if (!pdf_ || headers.empty()) return;
    ensurePage();

    float rowH = 16.0f;
    float cellW = contentWidth() / static_cast<float>(headers.size());

    // Header row
    checkPageBreak(rowH + 2);
    HPDF_Page_SetRGBFill(page_, 0.22f, 0.39f, 0.70f);
    HPDF_Page_Rectangle(page_, kMarginL, cursor_y_ - rowH + 4, contentWidth(), rowH);
    HPDF_Page_Fill(page_);

    HPDF_Page_SetFontAndSize(page_, font_bold_, 8);
    HPDF_Page_SetRGBFill(page_, 1, 1, 1);
    for (size_t i = 0; i < headers.size(); ++i) {
        HPDF_Page_BeginText(page_);
        HPDF_Page_TextOut(page_, kMarginL + i * cellW + 3, cursor_y_ - 11, headers[i].c_str());
        HPDF_Page_EndText(page_);
    }
    moveDown(rowH + 2);

    bool shade = false;
    for (const auto& row : rows) {
        checkPageBreak(rowH);
        float ry = cursor_y_ - rowH + 4;

        if (shade) {
            HPDF_Page_SetRGBFill(page_, 0.95f, 0.97f, 1.0f);
            HPDF_Page_Rectangle(page_, kMarginL, ry, contentWidth(), rowH);
            HPDF_Page_Fill(page_);
        }

        HPDF_Page_SetFontAndSize(page_, font_normal_, 8);
        HPDF_Page_SetRGBFill(page_, 0.15f, 0.15f, 0.15f);
        for (size_t i = 0; i < row.cells.size() && i < headers.size(); ++i) {
            HPDF_Page_BeginText(page_);
            HPDF_Page_TextOut(page_, kMarginL + i * cellW + 3, cursor_y_ - 11, row.cells[i].c_str());
            HPDF_Page_EndText(page_);
        }

        shade = !shade;
        moveDown(rowH);
    }
    moveDown(12);
}

void PdfRenderer::addInsightBlock(const std::string& title, const std::string& body,
                                   const std::string& category) {
    if (!pdf_) return;
    ensurePage();
    checkPageBreak(50);

    // Left accent bar color
    float cr = 0.22f, cg = 0.7f, cb = 0.39f;  // positive = green
    if (category == "warning")    { cr = 0.9f; cg = 0.6f; cb = 0.1f; }
    else if (category == "alert") { cr = 0.85f; cg = 0.2f; cb = 0.2f; }
    else if (category == "info")  { cr = 0.22f; cg = 0.39f; cb = 0.70f; }
    else if (category == "actionable") { cr = 0.5f; cg = 0.2f; cb = 0.8f; }

    // Estimate block height
    float blockW = contentWidth() - 12;
    float titleH = 14.0f;
    // Rough body line estimate: 60 chars per line at 9pt
    int bodyLines = std::max(1, (int)(body.size() / 70) + 1);
    float bodyH = bodyLines * 13.0f;
    float blockH = titleH + bodyH + 16;

    checkPageBreak(blockH);

    float bx = kMarginL;
    float by = cursor_y_ - blockH + 4;

    // Background
    HPDF_Page_SetRGBFill(page_, 0.97f, 0.98f, 0.99f);
    HPDF_Page_Rectangle(page_, bx + 6, by, blockW, blockH);
    HPDF_Page_Fill(page_);

    // Accent bar
    HPDF_Page_SetRGBFill(page_, cr, cg, cb);
    HPDF_Page_Rectangle(page_, bx, by, 5, blockH);
    HPDF_Page_Fill(page_);

    // Title
    float ty = cursor_y_ - 13;
    HPDF_Page_SetFontAndSize(page_, font_bold_, 9);
    HPDF_Page_SetRGBFill(page_, 0.1f, 0.1f, 0.1f);
    HPDF_Page_BeginText(page_);
    HPDF_Page_TextOut(page_, bx + 12, ty, title.c_str());
    HPDF_Page_EndText(page_);

    // Body
    float bh = drawWrappedText(body, font_normal_, 9, bx + 12, ty - 14, blockW - 16,
                                80, 80, 80);
    (void)bh;
    moveDown(blockH + 6);
}

void PdfRenderer::addChart(const std::string& png_path, const std::string& caption) {
    if (!pdf_) return;
    ensurePage();

    float imgH = 170.0f;
    float imgW = contentWidth();
    checkPageBreak(imgH + 20);

    HPDF_Image img = HPDF_LoadPngImageFromFile(pdf_, png_path.c_str());
    if (!img) {
        std::cerr << "[PdfRenderer] failed to load chart: " << png_path << "\n";
        return;
    }

    HPDF_Page_DrawImage(page_, img, kMarginL, cursor_y_ - imgH, imgW, imgH);
    moveDown(imgH + 4);

    // Caption
    HPDF_Page_SetFontAndSize(page_, font_normal_, 8);
    HPDF_Page_SetRGBFill(page_, 0.5f, 0.5f, 0.5f);
    HPDF_Page_BeginText(page_);
    HPDF_Page_TextOut(page_, kMarginL, cursor_y_, caption.c_str());
    HPDF_Page_EndText(page_);
    moveDown(16);
}

void PdfRenderer::addPageBreak() {
    if (!pdf_) return;
    newPage();
}

bool PdfRenderer::save(const std::string& path) {
    if (!pdf_) return false;
    HPDF_STATUS st = HPDF_SaveToFile(pdf_, path.c_str());
    return st == HPDF_OK;
}

} // namespace hms_cpap
