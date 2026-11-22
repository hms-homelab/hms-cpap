#include "services/GnuplotService.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <iostream>

namespace hms_cpap {

bool GnuplotService::renderLineChart(
    const std::vector<ChartPoint>& data,
    const std::string& out_png,
    const std::string& title,
    const std::string& ylabel,
    const std::string& color,
    double ymin,
    double ymax,
    double hline,
    const std::string& hline_label)
{
    if (data.empty()) return false;

    std::ostringstream script;
    script << "set terminal pngcairo size 900,300 enhanced font 'DejaVu Sans,11'\n";
    script << "set output '" << escapePath(out_png) << "'\n";
    script << "set title '" << sanitizeLabel(title) << "' font 'DejaVu Sans Bold,12'\n";
    script << "set xlabel 'Date'\n";
    script << "set ylabel '" << sanitizeLabel(ylabel) << "'\n";
    script << "set style data linespoints\n";
    script << "set grid ytics lt 0 lc rgb '#dddddd'\n";
    script << "set border 3\n";
    script << "set tics nomirror\n";
    // Rotated to end AT the tic, with room under the axis for them and the
    // xlabel: rotated the other way they ran off the bottom and the right edge.
    script << "set xtics rotate by 45 right font 'DejaVu Sans,9'\n";
    script << "set key off\n";
    script << "set margins 10, 3, 6, 2\n";
    if (ymax > ymin)
        script << "set yrange [" << ymin << ":" << ymax << "]\n";
    else
        script << "set yrange [" << ymin << ":*]\n";
    // SDD-082: the reference line spans the plot whatever the x range is
    // (graph coordinates across, data coordinates up).
    if (hline >= 0) {
        script << "set arrow from graph 0, first " << hline << " to graph 1, first " << hline
               << " nohead lw 1.5 dt 2 lc rgb '#dc2626'\n";
        if (!hline_label.empty())
            script << "set label '" << sanitizeLabel(hline_label) << "' at graph 1, first "
                   << hline << " right offset -0.5,0.7 tc rgb '#dc2626' font 'DejaVu Sans,9'\n";
    }

    // Inline data
    script << "plot '-' using 0:2:xtic(1) with linespoints "
           << "lc rgb '" << color << "' lw 2 pt 7 ps 0.6\n";

    // Subsample to max 60 points for readability
    size_t step = data.size() > 60 ? data.size() / 60 : 1;
    int written = 0;
    for (size_t i = 0; i < data.size(); i += step) {
        if (!std::isfinite(data[i].value)) continue;
        // Double quotes: gnuplot's data reader honours only those, so a single
        // quote was printed as part of every date label.
        script << "\"" << data[i].label << "\" " << data[i].value << "\n";
        ++written;
    }
    // Always include the last point, once: when the step already landed on it,
    // adding it again drew the last night twice.
    if (data.size() > 1 && (data.size() - 1) % step != 0 && std::isfinite(data.back().value)) {
        const auto& last = data.back();
        script << "\"" << last.label << "\" " << last.value << "\n";
        ++written;
    }
    if (written == 0) return false;
    script << "e\n";

    return run(script.str(), out_png);
}

// ---- SDD-082: one night on a clock axis --------------------------------------

namespace {

/// "set xtics (...)" for a night: a tick per hour (two when the night is
/// longer than ten hours), labelled on the clock. x is seconds since midnight
/// and may pass 86400 after midnight.
std::string clockTics(double x0, double span) {
    const double step = span > 10 * 3600.0 ? 7200.0 : 3600.0;
    double t = std::ceil(x0 / step) * step;
    std::ostringstream s;
    s << "set xtics (";
    bool first = true;
    for (; t <= x0 + span + 1; t += step) {
        const long clock = static_cast<long>(t) % 86400;
        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "%02ld:%02ld", clock / 3600, (clock % 3600) / 60);
        s << (first ? "" : ", ") << "'" << lbl << "' " << static_cast<long>(t);
        first = false;
    }
    s << ") font 'DejaVu Sans,9'\n";
    return s.str();
}

}  // namespace

std::string GnuplotService::safeColor(const std::string& c) {
    if (c.size() == 7 && c[0] == '#') {
        bool hex = true;
        for (std::size_t i = 1; i < 7; ++i) hex &= std::isxdigit(static_cast<unsigned char>(c[i])) != 0;
        if (hex) return c;
    }
    return "#888888";
}

std::string GnuplotService::timeChartScript(const std::vector<TimeSeries>& series,
                                            const std::string& out_png,
                                            const std::string& title, const std::string& ylabel,
                                            double x0_clock, double x_span, double ymin,
                                            double ymax, bool bands, int height) {
    std::ostringstream s;
    s << "set terminal pngcairo size 900," << height << " enhanced font 'DejaVu Sans,10'\n";
    s << "set output '" << escapePath(out_png) << "'\n";
    s << "set title '" << sanitizeLabel(title) << "' font 'DejaVu Sans Bold,11'\n";
    s << "set ylabel '" << sanitizeLabel(ylabel) << "'\n";
    s << "set grid xtics ytics lt 0 lc rgb '#dddddd'\n";
    s << "set border 3\nset tics nomirror\nset margins 10, 2, 3, 2\n";
    s << "set xrange [" << static_cast<long>(x0_clock) << ":"
      << static_cast<long>(x0_clock + std::max(60.0, x_span)) << "]\n";
    s << clockTics(x0_clock, std::max(60.0, x_span));
    // -1 is "auto" whatever ymin is: a flow band's floor is negative, and
    // reading (-100, -1) as a range drew every breath above -1 off the chart.
    const bool autoMax = ymax == -1.0 || !(ymax > ymin);
    if (bands && series.size() >= 2 && autoMax) {
        // filledcurves autoscales on its first y column alone, so a band
        // whose top is the max would be cut off at the min's ceiling: the
        // range comes from both edges.
        double lo = 1e300, hi = -1e300;
        for (std::size_t k = 0; k < 2; ++k)
            for (const auto& p : series[k].points)
                if (std::isfinite(p.second)) { lo = std::min(lo, p.second); hi = std::max(hi, p.second); }
        if (lo <= hi) {
            const double pad = std::max(1.0, (hi - lo) * 0.05);
            s << "set yrange [" << lo - pad << ":" << hi + pad << "]\n";
        } else {
            s << "set yrange [*:*]\n";
        }
    } else if (!autoMax) {
        s << "set yrange [" << ymin << ":" << ymax << "]\n";
    } else {
        s << "set yrange [" << ymin << ":*]\n";
    }
    const bool keyed = !bands && series.size() > 1;
    s << (keyed ? "set key top right horizontal font 'DejaVu Sans,9'\n" : "set key off\n");

    // A blank line in inline data breaks a line in gnuplot: written wherever
    // the machine was off for more than 10 minutes.
    const auto block = [&](const std::vector<std::pair<double, double>>& pts) {
        double prev = -1e300;
        for (const auto& [x, y] : pts) {
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            if (x - prev > 600) s << "\n";
            s << static_cast<long>(x0_clock + x) << " " << y << "\n";
            prev = x;
        }
        s << "e\n";
    };

    if (bands && series.size() >= 2) {
        // One block of x, low, high for filledcurves.
        s << "plot '-' using 1:2:3 with filledcurves lc rgb '" << safeColor(series[0].color)
          << "' fs transparent solid 0.55 noborder notitle\n";
        const auto& lo = series[0].points;
        const auto& hi = series[1].points;
        double prev = -1e300;
        for (std::size_t i = 0; i < lo.size() && i < hi.size(); ++i) {
            const double x = lo[i].first;
            if (!std::isfinite(lo[i].second) || !std::isfinite(hi[i].second)) continue;
            if (x - prev > 600) s << "\n";
            s << static_cast<long>(x0_clock + x) << " " << lo[i].second << " " << hi[i].second
              << "\n";
            prev = x;
        }
        s << "e\n";
        return s.str();
    }

    s << "plot ";
    for (std::size_t i = 0; i < series.size(); ++i) {
        s << (i ? ", " : "") << "'-' using 1:2 with lines lw 1.5 lc rgb '"
          << safeColor(series[i].color) << "' title '" << sanitizeLabel(series[i].name) << "'";
    }
    s << "\n";
    for (const auto& ser : series) block(ser.points);
    return s.str();
}

bool GnuplotService::renderTimeChart(const std::vector<TimeSeries>& series,
                                     const std::string& out_png, const std::string& title,
                                     const std::string& ylabel, double x0_clock, double x_span,
                                     double ymin, double ymax, bool bands, int height) {
    bool any = false;
    for (const auto& ser : series)
        for (const auto& p : ser.points) any |= std::isfinite(p.second);
    if (!any) return false;
    return run(timeChartScript(series, out_png, title, ylabel, x0_clock, x_span, ymin, ymax,
                               bands, height),
               out_png);
}

bool GnuplotService::renderEventStrip(const std::vector<FlagSeries>& rows,
                                      const std::string& out_png, const std::string& title,
                                      double x0_clock, double x_span) {
    if (rows.empty()) return false;
    const double span = std::max(60.0, x_span);
    std::ostringstream s;
    const int height = 60 + 18 * static_cast<int>(rows.size());
    s << "set terminal pngcairo size 900," << height << " enhanced font 'DejaVu Sans,10'\n";
    s << "set output '" << escapePath(out_png) << "'\n";
    s << "set title '" << sanitizeLabel(title) << "' font 'DejaVu Sans Bold,11'\n";
    s << "set border 3\nset tics nomirror\nset key off\nset margins 10, 2, 3, 2\n";
    s << "set xrange [" << static_cast<long>(x0_clock) << ":" << static_cast<long>(x0_clock + span)
      << "]\n";
    s << clockTics(x0_clock, span);
    s << "set yrange [0.3:" << rows.size() + 0.7 << "]\n";
    s << "set ytics (";
    for (std::size_t i = 0; i < rows.size(); ++i)
        s << (i ? ", " : "") << "'" << sanitizeLabel(rows[i].label) << "' " << rows.size() - i;
    s << ") font 'DejaVu Sans,9'\n";
    s << "set grid ytics lt 0 lc rgb '#eeeeee'\n";
    s << "plot ";
    bool first = true;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        s << (first ? "" : ", ") << "'-' using 1:2:(0):(0.7) with vectors nohead lw 1.5 lc rgb '"
          << safeColor(rows[i].color) << "'";
        first = false;
    }
    s << "\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const double y = static_cast<double>(rows.size() - i) - 0.35;
        // gnuplot refuses an empty inline block: a row with no events gets one
        // point outside the range.
        if (rows[i].offsets.empty()) s << static_cast<long>(x0_clock - 10 * span) << " " << y << "\n";
        for (const double off : rows[i].offsets)
            if (std::isfinite(off)) s << static_cast<long>(x0_clock + off) << " " << y << "\n";
        s << "e\n";
    }
    return run(s.str(), out_png);
}

bool GnuplotService::renderBars(const std::vector<std::pair<double, double>>& bars,
                                const std::string& out_png, const std::string& title,
                                const std::string& xlabel, const std::string& ylabel,
                                const std::string& color, double bar_width) {
    if (bars.empty()) return false;
    std::ostringstream s;
    s << "set terminal pngcairo size 900,230 enhanced font 'DejaVu Sans,10'\n";
    s << "set output '" << escapePath(out_png) << "'\n";
    s << "set title '" << sanitizeLabel(title) << "' font 'DejaVu Sans Bold,11'\n";
    s << "set xlabel '" << sanitizeLabel(xlabel) << "'\n";
    s << "set ylabel '" << sanitizeLabel(ylabel) << "'\n";
    s << "set border 3\nset tics nomirror\nset key off\nset margins 10, 2, 4, 2\n";
    s << "set grid ytics lt 0 lc rgb '#dddddd'\n";
    s << "set style fill solid 0.8 noborder\nset boxwidth " << bar_width << " absolute\n";
    s << "set yrange [0:*]\n";
    s << "plot '-' using 1:2 with boxes lc rgb '" << safeColor(color) << "'\n";
    for (const auto& [x, y] : bars)
        if (std::isfinite(x) && std::isfinite(y)) s << x + bar_width / 2.0 << " " << y << "\n";
    s << "e\n";
    return run(s.str(), out_png);
}

bool GnuplotService::run(const std::string& script, const std::string& out_png) {
    FILE* gp = popen("gnuplot 2>&1", "w");
    if (!gp) {
        std::cerr << "[GnuplotService] failed to open gnuplot\n";
        return false;
    }
    fputs(script.c_str(), gp);
    const int rc = pclose(gp);
    if (rc != 0) {
        std::cerr << "[GnuplotService] gnuplot exited with code " << rc << "\n";
        std::remove(out_png.c_str());   // a partial file would corrupt libharu
        return false;
    }
    return true;
}

std::string GnuplotService::escapePath(const std::string& p) {
    std::string out;
    for (char c : p) {
        if (c == '\'') out += "\\'";
        else out += c;
    }
    return out;
}

// Sanitize a caller-supplied label (chart title / axis label) before it is
// embedded inside a single-quoted gnuplot string that gets piped to gnuplot.
// gnuplot single-quoted strings do not honor backslash escapes, so a stray
// single quote would terminate the string early — and because gnuplot can run
// system() commands, an attacker-controlled quote is a script-injection vector.
// We drop quotes/backslashes/backticks and control characters (newlines etc.).
std::string GnuplotService::sanitizeLabel(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\'' || c == '"' || c == '`' || c == '\\' ||
            c == '\n' || c == '\r' || c == ';') {
            continue;
        }
        out += static_cast<char>(c);
    }
    return out;
}

} // namespace hms_cpap
