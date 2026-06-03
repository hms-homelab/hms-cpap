#include "services/GnuplotService.h"
#include <sstream>
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
    double ymax)
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
    script << "set xtics rotate by -45 font 'DejaVu Sans,9'\n";
    script << "set key off\n";
    script << "set margins 10, 2, 4, 2\n";
    if (ymax > ymin)
        script << "set yrange [" << ymin << ":" << ymax << "]\n";
    else
        script << "set yrange [" << ymin << ":*]\n";

    // Inline data
    script << "plot '-' using 0:2:xtic(1) with linespoints "
           << "lc rgb '" << color << "' lw 2 pt 7 ps 0.6\n";

    // Subsample to max 60 points for readability
    size_t step = data.size() > 60 ? data.size() / 60 : 1;
    int written = 0;
    for (size_t i = 0; i < data.size(); i += step) {
        if (!std::isfinite(data[i].value)) continue;
        script << "'" << data[i].label << "' " << data[i].value << "\n";
        ++written;
    }
    // Always include last point
    if (data.size() > 1 && std::isfinite(data.back().value)) {
        const auto& last = data.back();
        script << "'" << last.label << "' " << last.value << "\n";
        ++written;
    }
    if (written == 0) return false;
    script << "e\n";

    FILE* gp = popen("gnuplot 2>&1", "w");
    if (!gp) {
        std::cerr << "[GnuplotService] failed to open gnuplot\n";
        return false;
    }
    fputs(script.str().c_str(), gp);
    int rc = pclose(gp);
    if (rc != 0) {
        std::cerr << "[GnuplotService] gnuplot exited with code " << rc << "\n";
        std::remove(out_png.c_str());  // partial file would corrupt libharu
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
