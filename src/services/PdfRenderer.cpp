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
    // SDD-082: libharu stores a loaded PNG's pixels raw unless told otherwise,
    // and the clinician report's two dozen charts make a file of megabytes.
    // Images only: text stays uncompressed, as it always was.
    HPDF_SetCompressionMode(pdf_, HPDF_COMP_IMAGE);
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

    // Title, kept clear of the logo (SDD-082: "Relatorio de adesao do
    // HMS-CPAP" at 26 pt runs under it). It shrinks to fit on one line, then
    // takes two, and the second line is cut short rather than run under it.
    {
        const float room = kPageW - kMarginL - kMarginR - 80.0f - 12.0f;
        const auto width = [&](const std::string& s, float size) {
            HPDF_Page_SetFontAndSize(page_, font_bold_, size);
            return HPDF_Page_TextWidth(page_, s.c_str());
        };
        std::vector<std::string> lines{title};
        float size = 26;
        while (size > 20 && width(title, size) > room) size -= 1;
        if (width(title, size) > room) {
            size = 18;
            std::string first, rest = title;
            std::size_t at = 0;
            while (true) {
                const auto sp = title.find(' ', at);
                const std::string cand = title.substr(0, sp);
                if (width(cand, size) > room) break;
                first = cand;
                rest  = sp == std::string::npos ? "" : title.substr(sp + 1);
                if (sp == std::string::npos) break;
                at = sp + 1;
            }
            if (first.empty()) { first = title; rest.clear(); }
            if (!rest.empty() && width(rest, size) > room) {
                // Whole words off the end until it fits with "...".
                while (!rest.empty() && width(rest + "...", size) > room) {
                    const auto sp = rest.rfind(' ');
                    rest = sp == std::string::npos ? "" : rest.substr(0, sp);
                }
                if (!rest.empty()) rest += "...";
            }
            lines = rest.empty() ? std::vector<std::string>{first} : std::vector<std::string>{first, rest};
        }
        HPDF_Page_SetFontAndSize(page_, font_bold_, size);
        HPDF_Page_SetRGBFill(page_, 1, 1, 1);
        float y = lines.size() == 1 ? kPageH - 65 : kPageH - 48;
        for (const auto& l : lines) {
            HPDF_Page_BeginText(page_);
            HPDF_Page_TextOut(page_, kMarginL, y, l.c_str());
            HPDF_Page_EndText(page_);
            y -= size + 4;
        }
    }

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

// ---- SDD-082: grids, notes, the verdict and the calendar ---------------------

void PdfRenderer::addGridTable(const std::vector<std::string>& headers,
                               const std::vector<PdfRow>& rows, float first_col,
                               float font_size) {
    if (headers.empty()) return;
    const std::size_t n = headers.size();
    const float firstW = n > 1 ? std::min(first_col, contentWidth() * 0.6f) : contentWidth();
    const float restW = n > 1 ? (contentWidth() - firstW) / static_cast<float>(n - 1) : 0.0f;
    std::vector<float> widths(n, restW);
    widths[0] = firstW;
    addGridTable(headers, rows, widths, font_size);
}

void PdfRenderer::addGridTable(const std::vector<std::string>& headers,
                               const std::vector<PdfRow>& rows,
                               const std::vector<float>& widths_in, float font_size) {
    if (!pdf_ || headers.empty()) return;
    ensurePage();

    const float rowH = font_size * 1.9f;
    const std::size_t n = headers.size();
    // Widths scaled to the content width, so a caller gives proportions.
    std::vector<float> widths(n, contentWidth() / static_cast<float>(n));
    if (widths_in.size() == n) {
        float sum = 0;
        for (const float w : widths_in) sum += w;
        if (sum > 0)
            for (std::size_t i = 0; i < n; ++i) widths[i] = widths_in[i] * contentWidth() / sum;
    }
    std::vector<float> xs(n, kMarginL);
    for (std::size_t i = 1; i < n; ++i) xs[i] = xs[i - 1] + widths[i - 1];
    // A band spans [top - h, top]; its text sits a third of the way up.
    const auto baseline = [&](float top, float h) { return top - h + (h - font_size) / 2.0f + 1.5f; };

    bool twoLines = false;
    for (const auto& h : headers) twoLines |= h.find('\n') != std::string::npos;
    const float headH = twoLines ? rowH * 1.8f : rowH * 1.1f;

    const auto drawRight = [&](const std::string& t, std::size_t col, float y, HPDF_Font f,
                               float size) {
        HPDF_Page_SetFontAndSize(page_, f, size);
        const float w = HPDF_Page_TextWidth(page_, t.c_str());
        const float x = col == 0 ? xs[col] + 3 : xs[col] + widths[col] - w - 3;
        HPDF_Page_BeginText(page_);
        HPDF_Page_TextOut(page_, x, y, t.c_str());
        HPDF_Page_EndText(page_);
    };

    const auto drawHeader = [&]() {
        checkPageBreak(headH + rowH);
        HPDF_Page_SetRGBFill(page_, 0.22f, 0.39f, 0.70f);
        HPDF_Page_Rectangle(page_, kMarginL, cursor_y_ - headH, contentWidth(), headH);
        HPDF_Page_Fill(page_);
        HPDF_Page_SetRGBFill(page_, 1, 1, 1);
        for (std::size_t i = 0; i < n; ++i) {
            const auto nl = headers[i].find('\n');
            const std::string top = headers[i].substr(0, nl);
            if (nl == std::string::npos) {
                drawRight(top, i, baseline(cursor_y_, headH), font_bold_, font_size);
            } else {
                drawRight(top, i, cursor_y_ - font_size - 3, font_bold_, font_size);
                drawRight(headers[i].substr(nl + 1), i, cursor_y_ - 2 * font_size - 5,
                          font_normal_, font_size - 1.5f);
            }
        }
        moveDown(headH);
    };
    drawHeader();

    bool shade = false;
    for (const auto& row : rows) {
        if (cursor_y_ - rowH < kMarginBot) {
            newPage();
            drawHeader();
            shade = false;
        }
        const float ry = cursor_y_ - rowH;
        if (row.section) {
            HPDF_Page_SetRGBFill(page_, 0.88f, 0.88f, 0.88f);
            HPDF_Page_Rectangle(page_, kMarginL, ry, contentWidth(), rowH);
            HPDF_Page_Fill(page_);
            HPDF_Page_SetRGBFill(page_, 0.1f, 0.1f, 0.1f);
            if (!row.cells.empty()) drawRight(row.cells[0], 0, baseline(cursor_y_, rowH), font_bold_, font_size);
            moveDown(rowH);
            shade = false;
            continue;
        }
        if (shade) {
            HPDF_Page_SetRGBFill(page_, 0.93f, 0.98f, 0.93f);
            HPDF_Page_Rectangle(page_, kMarginL, ry, contentWidth(), rowH);
            HPDF_Page_Fill(page_);
        }
        HPDF_Page_SetRGBFill(page_, 0.12f, 0.12f, 0.12f);
        for (std::size_t i = 0; i < row.cells.size() && i < n; ++i)
            drawRight(row.cells[i], i, baseline(cursor_y_, rowH), font_normal_, font_size);
        shade = !shade;
        moveDown(rowH);
    }
    moveDown(12);
}

void PdfRenderer::addSpacer(float pts) {
    if (!pdf_) return;
    ensurePage();
    moveDown(pts);
}

void PdfRenderer::addChartSized(const std::string& png_path, const std::string& caption,
                                float height) {
    if (!pdf_) return;
    ensurePage();
    checkPageBreak(height + (caption.empty() ? 6 : 20));
    HPDF_Image img = HPDF_LoadPngImageFromFile(pdf_, png_path.c_str());
    if (!img) {
        std::cerr << "[PdfRenderer] failed to load chart: " << png_path << "\n";
        return;
    }
    HPDF_Page_DrawImage(page_, img, kMarginL, cursor_y_ - height, contentWidth(), height);
    moveDown(height + 4);
    if (!caption.empty()) {
        HPDF_Page_SetFontAndSize(page_, font_normal_, 8);
        HPDF_Page_SetRGBFill(page_, 0.5f, 0.5f, 0.5f);
        HPDF_Page_BeginText(page_);
        HPDF_Page_TextOut(page_, kMarginL, cursor_y_, caption.c_str());
        HPDF_Page_EndText(page_);
        moveDown(14);
    } else {
        moveDown(4);
    }
}

void PdfRenderer::addNote(const std::string& text) {
    if (!pdf_ || text.empty()) return;
    ensurePage();
    checkPageBreak(14);
    const float h = drawWrappedText(text, font_normal_, 7.5f, kMarginL, cursor_y_,
                                    contentWidth(), 110, 110, 110);
    moveDown(h + 4);
}

void PdfRenderer::addDisclaimer(const std::string& text) {
    if (!pdf_ || text.empty()) return;
    ensurePage();
    // At the bottom of the last page: the room it needs above the bottom
    // margin, on a new page if the content already reaches it.
    constexpr float kRoom = 46.0f;
    checkPageBreak(kRoom + 14);
    if (cursor_y_ > kMarginBot + kRoom) cursor_y_ = kMarginBot + kRoom;
    HPDF_Page_SetRGBStroke(page_, 0.75f, 0.75f, 0.75f);
    HPDF_Page_SetLineWidth(page_, 0.5f);
    HPDF_Page_MoveTo(page_, kMarginL, cursor_y_);
    HPDF_Page_LineTo(page_, kPageW - kMarginR, cursor_y_);
    HPDF_Page_Stroke(page_);
    moveDown(12);
    const float h = drawWrappedText(text, font_normal_, 8, kMarginL, cursor_y_,
                                    contentWidth(), 110, 110, 110);
    moveDown(h + 4);
}

void PdfRenderer::addVerdict(const std::string& text, bool met) {
    if (!pdf_ || text.empty()) return;
    ensurePage();
    constexpr float kBand = 58.0f;
    checkPageBreak(kBand + 16);
    if (met) HPDF_Page_SetRGBFill(page_, 0.86f, 0.95f, 0.86f);
    else     HPDF_Page_SetRGBFill(page_, 0.98f, 0.87f, 0.87f);
    HPDF_Page_Rectangle(page_, kMarginL, cursor_y_ - kBand, contentWidth(), kBand);
    HPDF_Page_Fill(page_);
    // The largest type on the page, shrunk only as far as the band needs.
    float size = 30.0f;
    HPDF_Page_SetFontAndSize(page_, font_bold_, size);
    while (size > 14.0f && HPDF_Page_TextWidth(page_, text.c_str()) > contentWidth() - 20.0f) {
        size -= 1.0f;
        HPDF_Page_SetFontAndSize(page_, font_bold_, size);
    }
    const float w = HPDF_Page_TextWidth(page_, text.c_str());
    if (met) HPDF_Page_SetRGBFill(page_, 0.10f, 0.45f, 0.18f);
    else     HPDF_Page_SetRGBFill(page_, 0.72f, 0.11f, 0.11f);
    HPDF_Page_BeginText(page_);
    HPDF_Page_TextOut(page_, kMarginL + (contentWidth() - w) / 2.0f,
                      cursor_y_ - kBand / 2.0f - size * 0.35f, text.c_str());
    HPDF_Page_EndText(page_);
    moveDown(kBand + 14);
}

void PdfRenderer::addCalendar(const std::string& caption, const std::vector<std::string>& weekdays,
                              int first_column, const std::vector<CalendarCell>& cells) {
    if (!pdf_ || cells.empty() || weekdays.size() != 7) return;
    ensurePage();
    constexpr int   kCols  = 7;
    constexpr float kCellH = 34.0f;
    constexpr float kHeadH = 14.0f;
    const float cellW = contentWidth() / kCols;
    const int first = ((first_column % kCols) + kCols) % kCols;
    const int rows = (first + static_cast<int>(cells.size()) + kCols - 1) / kCols;
    const float captionH = caption.empty() ? 0.0f : 16.0f;
    // Whole on one page: a grid split by a page break reads as two ranges.
    checkPageBreak(captionH + kHeadH + rows * kCellH + 10.0f);

    if (!caption.empty()) {
        HPDF_Page_SetFontAndSize(page_, font_bold_, 10);
        HPDF_Page_SetRGBFill(page_, 0.22f, 0.39f, 0.70f);
        HPDF_Page_BeginText(page_);
        HPDF_Page_TextOut(page_, kMarginL, cursor_y_ - 10, caption.c_str());
        HPDF_Page_EndText(page_);
        moveDown(captionH);
    }
    HPDF_Page_SetFontAndSize(page_, font_bold_, 8);
    HPDF_Page_SetRGBFill(page_, 0.40f, 0.40f, 0.40f);
    for (int c = 0; c < kCols; ++c) {
        const float w = HPDF_Page_TextWidth(page_, weekdays[c].c_str());
        HPDF_Page_BeginText(page_);
        HPDF_Page_TextOut(page_, kMarginL + c * cellW + (cellW - w) / 2.0f, cursor_y_ - 10,
                          weekdays[c].c_str());
        HPDF_Page_EndText(page_);
    }
    moveDown(kHeadH);

    HPDF_Page_SetRGBStroke(page_, 0.75f, 0.75f, 0.75f);
    HPDF_Page_SetLineWidth(page_, 0.4f);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const int slot = first + static_cast<int>(i);
        const float x = kMarginL + (slot % kCols) * cellW;
        const float top = cursor_y_ - (slot / kCols) * kCellH;
        const CalendarCell& cell = cells[i];
        switch (cell.state) {
            case 1:  HPDF_Page_SetRGBFill(page_, 0.90f, 0.90f, 0.90f); break;
            case 2:  HPDF_Page_SetRGBFill(page_, 1.00f, 0.88f, 0.70f); break;
            case 3:  HPDF_Page_SetRGBFill(page_, 0.78f, 0.92f, 0.78f); break;
            default: HPDF_Page_SetRGBFill(page_, 1.0f, 1.0f, 1.0f);    break;
        }
        HPDF_Page_Rectangle(page_, x, top - kCellH, cellW, kCellH);
        HPDF_Page_FillStroke(page_);
        if (cell.state == 0) continue;
        HPDF_Page_SetRGBFill(page_, 0.35f, 0.35f, 0.35f);
        HPDF_Page_SetFontAndSize(page_, font_normal_, 6.5f);
        HPDF_Page_BeginText(page_);
        HPDF_Page_TextOut(page_, x + 3, top - 9, cell.date.c_str());
        HPDF_Page_EndText(page_);
        HPDF_Page_SetRGBFill(page_, 0.12f, 0.12f, 0.12f);
        HPDF_Page_SetFontAndSize(page_, font_bold_, 10);
        const float w = HPDF_Page_TextWidth(page_, cell.hours.c_str());
        HPDF_Page_BeginText(page_);
        HPDF_Page_TextOut(page_, x + (cellW - w) / 2.0f, top - kCellH + 9, cell.hours.c_str());
        HPDF_Page_EndText(page_);
    }
    moveDown(rows * kCellH + 10.0f);
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
