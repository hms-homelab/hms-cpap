#pragma once
#include <string>
#include <utility>
#include <vector>

namespace hms_cpap {

struct ChartPoint {
    std::string label;  // x-axis label (date string)
    double value;
};

class GnuplotService {
public:
    // Renders a line chart to a PNG file. Returns true on success.
    // title: chart title, ylabel: y-axis label, color: hex e.g. "#3b82f6"
    // SDD-082: `hline` >= 0 draws a dashed reference line across the chart at
    // that value (the compliance rule's share), labelled `hline_label`.
    static bool renderLineChart(
        const std::vector<ChartPoint>& data,
        const std::string& out_png,
        const std::string& title,
        const std::string& ylabel,
        const std::string& color,
        double ymin = 0.0,
        double ymax = -1.0,  // -1 = auto
        double hline = -1.0, // -1 = none
        const std::string& hline_label = ""
    );

    // SDD-082: one night on a clock axis. Each series is (seconds since the
    // night's first sample, value); `x0_clock` is that first sample's seconds
    // since midnight, so the ticks read HH:MM. A gap of more than 10 minutes
    // breaks the line (the machine was off). `bands` draws a min/max envelope
    // from series 0 and 1 instead of two lines. Returns false (and leaves no
    // file) when there is nothing to draw.
    struct TimeSeries {
        std::string name;
        std::string color;
        std::vector<std::pair<double, double>> points;
    };
    static bool renderTimeChart(const std::vector<TimeSeries>& series,
                                const std::string& out_png, const std::string& title,
                                const std::string& ylabel, double x0_clock, double x_span,
                                double ymin = 0.0, double ymax = -1.0, bool bands = false,
                                int height = 210);

    // The event-flag strip: one row per label (top first), a tick at each
    // event's offset (seconds, as above).
    struct FlagSeries {
        std::string label;
        std::string color;
        std::vector<double> offsets;
    };
    static bool renderEventStrip(const std::vector<FlagSeries>& rows, const std::string& out_png,
                                 const std::string& title, double x0_clock, double x_span);

    // Vertical bars over numeric x (time at pressure: cmH2O against minutes).
    static bool renderBars(const std::vector<std::pair<double, double>>& bars,
                           const std::string& out_png, const std::string& title,
                           const std::string& xlabel, const std::string& ylabel,
                           const std::string& color, double bar_width);

    // Strip characters that could break out of / inject into a single-quoted
    // gnuplot string (quotes, backslash, backtick, newlines, ';'). Applied to
    // caller-supplied chart title / axis labels.
    static std::string sanitizeLabel(const std::string& s);

    // The gnuplot script for renderTimeChart, without running gnuplot (tests).
    static std::string timeChartScript(const std::vector<TimeSeries>& series,
                                       const std::string& out_png, const std::string& title,
                                       const std::string& ylabel, double x0_clock, double x_span,
                                       double ymin, double ymax, bool bands, int height);
    // A colour as gnuplot may read it: "#rrggbb", or a safe grey.
    static std::string safeColor(const std::string& c);

private:
    static std::string escapePath(const std::string& p);
    static bool run(const std::string& script, const std::string& out_png);
};

} // namespace hms_cpap
