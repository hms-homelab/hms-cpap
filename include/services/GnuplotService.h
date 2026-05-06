#pragma once
#include <string>
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
    static bool renderLineChart(
        const std::vector<ChartPoint>& data,
        const std::string& out_png,
        const std::string& title,
        const std::string& ylabel,
        const std::string& color,
        double ymin = 0.0,
        double ymax = -1.0  // -1 = auto
    );

private:
    static std::string escapePath(const std::string& p);
};

} // namespace hms_cpap
