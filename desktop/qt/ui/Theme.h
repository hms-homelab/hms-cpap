#pragma once

#include <QString>

namespace cpapdash::supervisor::theme {

/**
 * The CpapDash palette, sampled from the logo rather than invented, so the
 * desktop app and the web dashboard look like the same product.
 *
 *   #07172F  deepest navy   the card's outer edge
 *   #102E54  navy           the dominant fill
 *   #1E4874  mid navy       the inner gradient
 *   #72ACCB  sky            the moon and star
 */
inline constexpr const char* kNavyDeep = "#07172F";
inline constexpr const char* kNavy     = "#102E54";
inline constexpr const char* kNavyMid  = "#1E4874";
inline constexpr const char* kSky      = "#72ACCB";
inline constexpr const char* kSkyPale  = "#A8CFE4";

/// The banner across the top of the wizard and the settings window.
///
/// Painted rather than left to the system theme because it is the one piece of
/// the window that should look like CpapDash in both light and dark mode. Its
/// text colour is fixed for the same reason: it sits on a known dark gradient,
/// so it must not follow the system's idea of "foreground" and turn dark-on-dark.
inline QString headerStyle() {
    // Solid where the brand sits, then fading out down the panel so it dissolves
    // into the page instead of stopping at a hard edge. The fade runs on ALPHA
    // rather than toward a fixed colour: a gradient ending in a chosen "page
    // grey" looks right in one appearance and wrong in the other, whereas
    // fading to transparent lets whatever theme is underneath finish the blend.
    return QString(
        "QFrame#Header {"
        "  background: qlineargradient(x1:0, y1:0, x2:0.35, y2:1,"
        "      stop:0    %1,"
        "      stop:0.38 %2,"
        "      stop:0.72 rgba(30, 72, 116, 150),"
        "      stop:1    rgba(30, 72, 116, 0));"
        "  border: none;"
        "}"
        // The labels sit in the solid part, so their colours stay fixed. They
        // must NOT follow the system palette: the panel behind them is dark in
        // both appearances, and light-mode "foreground" would be dark on dark.
        "QFrame#Header QLabel     { color: #FFFFFF; background: transparent; }"
        "QFrame#Header QLabel#Sub { color: %3; background: transparent; }"
    ).arg(kNavyDeep, kNavy, kSkyPale);
}

/// Two real buttons, and nothing else.
///
/// No track and no handle -- a bar down the side of a short list is noise. What
/// remains has to look pressable, so the arrows are sized and bordered like
/// buttons rather than left as faint glyphs floating in the margin: at 9px with
/// no outline they read as decoration and nobody tries to click them.
///
/// Qt cannot draw scrollbar arrows from a stylesheet, so the glyphs are real
/// images; the button around each one is styled here.
inline QString scrollBarStyle() {
    return QString(
        "QScrollBar:vertical {"
        "  background: transparent; width: 24px; border: none;"
        "  margin: 26px 0 26px 0;"          // room for a button at each end
        "}"
        "QScrollBar::handle:vertical { background: transparent; border: none; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "  background: transparent;"
        "}"
        "QScrollBar::sub-line:vertical, QScrollBar::add-line:vertical {"
        "  height: 22px; width: 22px; margin: 2px 1px;"
        "  border: 1px solid %1; border-radius: 5px;"
        "  background: rgba(128, 128, 128, 0.14);"
        "}"
        "QScrollBar::sub-line:vertical { subcontrol-position: top;    subcontrol-origin: margin; }"
        "QScrollBar::add-line:vertical { subcontrol-position: bottom; subcontrol-origin: margin; }"
        "QScrollBar::sub-line:vertical:hover, QScrollBar::add-line:vertical:hover {"
        "  background: %2; border-color: %3;"
        "}"
        "QScrollBar::sub-line:vertical:pressed, QScrollBar::add-line:vertical:pressed {"
        "  background: %4;"
        "}"
        "QScrollBar::up-arrow:vertical {"
        "  image: url(:/icons/arrow-up.png); width: 12px; height: 12px;"
        "}"
        "QScrollBar::down-arrow:vertical {"
        "  image: url(:/icons/arrow-down.png); width: 12px; height: 12px;"
        "}"
    ).arg("rgba(150, 150, 150, 0.55)", kNavyMid, kSky, kNavyDeep);
}

/// Accent for primary buttons, progress and focus rings.
inline QString accentStyle() {
    return QString(
        "QPushButton[accent=\"true\"] {"
        "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        "      stop:0 %1, stop:1 %2);"
        "  color: #FFFFFF; border: none; border-radius: 5px;"
        "  padding: 6px 18px; font-weight: 600;"
        "}"
        "QPushButton[accent=\"true\"]:hover:!disabled { background: %3; }"
        "QPushButton[accent=\"true\"]:disabled {"
        "  background: palette(button); color: palette(mid);"
        "}"
    ).arg(kNavyMid, kNavy, kSky);
}

}  // namespace cpapdash::supervisor::theme
