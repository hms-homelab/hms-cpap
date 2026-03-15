#include "services/InsightsEngine.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace hms_cpap {

std::vector<Insight> InsightsEngine::analyze(const std::vector<STRDailyRecord>& records) {
    // Filter to therapy days only, sort by date
    std::vector<STRDailyRecord> sorted;
    for (const auto& r : records) {
        if (r.hasTherapy()) sorted.push_back(r);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.record_date < b.record_date;
    });

    if (sorted.size() < 7) {
        return {{
            "Not enough data",
            "Need at least 7 therapy days for insights.",
            "info", "", 0
        }};
    }

    std::vector<Insight> insights;
    auto append = [&](const std::vector<Insight>& v) {
        insights.insert(insights.end(), v.begin(), v.end());
    };

    append(ahiTrend(sorted));
    append(leakCorrelation(sorted));
    append(pressureTrend(sorted));
    append(therapyCompliance(sorted));
    append(bestWorstNights(sorted));
    append(recentSummary(sorted));

    return insights;
}

Json::Value InsightsEngine::toJson(const std::vector<Insight>& insights) {
    Json::Value arr(Json::arrayValue);
    for (const auto& i : insights) {
        Json::Value obj;
        obj["title"] = i.title;
        obj["body"] = i.body;
        obj["category"] = i.category;
        obj["metric"] = i.metric;
        obj["value"] = i.value;
        arr.append(obj);
    }
    return arr;
}

// --- Analysis methods ---

std::vector<Insight> InsightsEngine::ahiTrend(const std::vector<STRDailyRecord>& sorted) {
    std::vector<Insight> insights;
    auto last30 = lastN(sorted, 30);
    if (last30.size() < 7) return insights;

    std::vector<double> ahis;
    for (const auto& r : last30) ahis.push_back(r.ahi);
    double avgAhi = mean(ahis);

    // Compare to prior period
    if (sorted.size() > last30.size()) {
        std::vector<STRDailyRecord> prior(sorted.begin(),
            sorted.begin() + (sorted.size() - last30.size()));
        std::vector<double> priorAhis;
        for (const auto& r : prior) priorAhis.push_back(r.ahi);
        double priorAvg = mean(priorAhis);
        double change = avgAhi - priorAvg;
        double pctChange = priorAvg > 0 ? (change / priorAvg * 100) : 0;

        if (std::abs(change) > 0.5) {
            std::string direction = change < 0 ? "improved" : "worsened";
            std::string arrow = change < 0 ? "down" : "up";
            std::ostringstream body;
            body << std::fixed << std::setprecision(1);
            body << "Last 30 days average AHI is " << avgAhi
                 << " (" << arrow << " " << std::setprecision(0) << std::abs(pctChange)
                 << "% from " << std::setprecision(1) << priorAvg << "). "
                 << (change < 0 ? "Your therapy is trending in the right direction."
                                : "Consider checking mask fit and settings with your provider.");
            insights.push_back({
                "AHI trend: " + direction, body.str(),
                change < 0 ? "positive" : "warning", "AHI", avgAhi
            });
        } else {
            std::ostringstream body;
            body << std::fixed << std::setprecision(1);
            body << "Last 30 days average AHI is " << avgAhi
                 << " (was " << priorAvg << "). Consistent therapy.";
            insights.push_back({"AHI stable", body.str(), "positive", "AHI", avgAhi});
        }
    } else {
        std::ostringstream body;
        body << std::fixed << std::setprecision(1);
        body << "Average AHI is " << avgAhi << ". "
             << (avgAhi < 5 ? "Normal range (under 5)."
                : avgAhi <= 15 ? "Moderate range (5-15)."
                : "Elevated (over 15). Consult your provider.");
        insights.push_back({
            "Average AHI: " + std::to_string(avgAhi).substr(0, 4),
            body.str(),
            avgAhi < 5 ? "positive" : avgAhi <= 15 ? "warning" : "alert",
            "AHI", avgAhi
        });
    }

    return insights;
}

std::vector<Insight> InsightsEngine::leakCorrelation(const std::vector<STRDailyRecord>& sorted) {
    std::vector<Insight> insights;
    auto recent = lastN(sorted, 60);
    if (recent.size() < 14) return insights;

    std::vector<double> leaks;
    for (const auto& r : recent) leaks.push_back(r.leak_95);
    std::sort(leaks.begin(), leaks.end());
    double medianLeak = leaks[leaks.size() / 2];

    std::vector<double> highLeakAhis, lowLeakAhis;
    for (const auto& r : recent) {
        if (r.leak_95 >= medianLeak) highLeakAhis.push_back(r.ahi);
        else lowLeakAhis.push_back(r.ahi);
    }

    if (highLeakAhis.empty() || lowLeakAhis.empty()) return insights;

    double highAvg = mean(highLeakAhis);
    double lowAvg = mean(lowLeakAhis);
    double diff = highAvg - lowAvg;

    std::ostringstream body;
    body << std::fixed << std::setprecision(1);

    if (diff > 0.5) {
        body << "Nights with leak above " << std::setprecision(0) << medianLeak
             << " L/min average AHI " << std::setprecision(1) << highAvg
             << " vs " << lowAvg << " on low-leak nights. "
             << "Improving mask seal could lower your AHI by ~" << diff << " events/hr.";
        insights.push_back({
            "Mask leak is affecting your AHI", body.str(),
            "actionable", "Leak", medianLeak
        });
    } else {
        body << "High and low leak nights show similar AHI (" << highAvg
             << " vs " << lowAvg << "). Your mask seal is adequate.";
        insights.push_back({
            "Leak is not a major factor", body.str(),
            "positive", "Leak", medianLeak
        });
    }

    return insights;
}

std::vector<Insight> InsightsEngine::pressureTrend(const std::vector<STRDailyRecord>& sorted) {
    std::vector<Insight> insights;
    auto last30 = lastN(sorted, 30);
    if (sorted.size() <= last30.size()) return insights;

    std::vector<STRDailyRecord> prior(sorted.begin(),
        sorted.begin() + (sorted.size() - last30.size()));
    if (prior.size() < 7 || last30.size() < 7) return insights;

    std::vector<double> recentPress, priorPress;
    for (const auto& r : last30) recentPress.push_back(r.mask_press_50);
    for (const auto& r : prior) priorPress.push_back(r.mask_press_50);

    double recentAvg = mean(recentPress);
    double priorAvg = mean(priorPress);
    double change = recentAvg - priorAvg;

    if (std::abs(change) > 0.5) {
        std::string direction = change < 0 ? "decreasing" : "increasing";
        std::ostringstream body;
        body << std::fixed << std::setprecision(1);
        body << "Machine median pressure moved from " << priorAvg
             << " to " << recentAvg << " cmH2O. "
             << (change < 0 ? "The machine needs less pressure — a positive sign."
                            : "The machine is working harder. Check for weight changes or congestion.");
        insights.push_back({
            "Pressure is " + direction, body.str(),
            change < 0 ? "positive" : "info", "Pressure", recentAvg
        });
    }

    return insights;
}

std::vector<Insight> InsightsEngine::therapyCompliance(const std::vector<STRDailyRecord>& sorted) {
    std::vector<Insight> insights;
    auto last30 = lastN(sorted, 30);
    if (last30.size() < 7) return insights;

    std::vector<double> hours;
    int over4h = 0;
    for (const auto& r : last30) {
        double h = r.duration_minutes / 60.0;
        hours.push_back(h);
        if (h >= 4.0) over4h++;
    }

    double avgHours = mean(hours);
    int daysUsed = static_cast<int>(last30.size());
    double pct = static_cast<double>(over4h) / daysUsed * 100;

    std::ostringstream body;
    body << std::fixed << std::setprecision(1);
    body << "Averaging " << avgHours << " hours/night (last " << daysUsed << " nights). "
         << over4h << " of " << daysUsed << " nights (" << std::setprecision(0) << pct
         << "%) met the 4-hour threshold.";

    if (avgHours < 4) {
        insights.push_back({"Low therapy hours", body.str(), "warning", "Hours", avgHours});
    } else {
        insights.push_back({"Good therapy duration", body.str(), "positive", "Hours", avgHours});
    }

    return insights;
}

std::vector<Insight> InsightsEngine::bestWorstNights(const std::vector<STRDailyRecord>& sorted) {
    std::vector<Insight> insights;
    auto last30 = lastN(sorted, 30);
    if (last30.size() < 7) return insights;

    auto byAhi = last30;
    std::sort(byAhi.begin(), byAhi.end(), [](const auto& a, const auto& b) {
        return a.ahi < b.ahi;
    });

    const auto& best = byAhi.front();
    const auto& worst = byAhi.back();

    std::ostringstream body;
    body << std::fixed << std::setprecision(1);
    body << "Best: " << formatDate(best) << " — AHI " << best.ahi
         << ", " << best.duration_minutes / 60.0 << "h"
         << ", leak " << std::setprecision(0) << best.leak_95
         << " L/min, pressure " << std::setprecision(1) << best.mask_press_50 << " cmH2O.\n"
         << "Worst: " << formatDate(worst) << " — AHI " << worst.ahi
         << ", " << worst.duration_minutes / 60.0 << "h"
         << ", leak " << std::setprecision(0) << worst.leak_95
         << " L/min, pressure " << std::setprecision(1) << worst.mask_press_50 << " cmH2O.";

    insights.push_back({
        "Best vs worst night", body.str(), "info", "AHI", worst.ahi - best.ahi
    });

    return insights;
}

std::vector<Insight> InsightsEngine::recentSummary(const std::vector<STRDailyRecord>& sorted) {
    auto last7 = lastN(sorted, 7);
    if (last7.size() < 3) return {};

    std::vector<double> ahis, hrs, leaks, press;
    for (const auto& r : last7) {
        ahis.push_back(r.ahi);
        hrs.push_back(r.duration_minutes / 60.0);
        leaks.push_back(r.leak_95);
        press.push_back(r.mask_press_50);
    }

    std::ostringstream body;
    body << std::fixed << std::setprecision(1);
    body << "AHI " << mean(ahis) << " · " << mean(hrs) << " hrs · "
         << "Leak " << std::setprecision(0) << mean(leaks) << " L/min · "
         << "Pressure " << std::setprecision(1) << mean(press) << " cmH2O";

    return {{
        "Last " + std::to_string(last7.size()) + " nights",
        body.str(), "info", "Summary", mean(ahis)
    }};
}

// --- Helpers ---

double InsightsEngine::mean(const std::vector<double>& vals) {
    if (vals.empty()) return 0;
    double sum = 0;
    for (double v : vals) sum += v;
    return sum / vals.size();
}

std::vector<STRDailyRecord> InsightsEngine::lastN(
    const std::vector<STRDailyRecord>& sorted, size_t n) {
    if (sorted.size() <= n) return sorted;
    return std::vector<STRDailyRecord>(sorted.end() - n, sorted.end());
}

std::string InsightsEngine::formatDate(const STRDailyRecord& r) {
    auto t = std::chrono::system_clock::to_time_t(r.record_date);
    std::tm tm = *std::localtime(&t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%m/%d");
    return oss.str();
}

} // namespace hms_cpap
