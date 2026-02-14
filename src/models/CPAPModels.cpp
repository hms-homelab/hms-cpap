#include "models/CPAPModels.h"
#include <numeric>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace hms_cpap {

std::string eventTypeToString(EventType type) {
    switch (type) {
        case EventType::APNEA: return "Apnea";
        case EventType::HYPOPNEA: return "Hypopnea";
        case EventType::RERA: return "RERA";
        case EventType::CSR: return "CSR";
        case EventType::OBSTRUCTIVE: return "Obstructive";
        case EventType::CENTRAL: return "Central";
        case EventType::CLEAR_AIRWAY: return "Clear Airway";
        default: return "Unknown";
    }
}

// Helper: Calculate percentile from sorted vector
template<typename T>
T calculatePercentile(std::vector<T> values, double percentile) {
    if (values.empty()) return T();

    std::sort(values.begin(), values.end());

    double index = (percentile / 100.0) * (values.size() - 1);
    size_t lower = static_cast<size_t>(std::floor(index));
    size_t upper = static_cast<size_t>(std::ceil(index));

    if (lower == upper) {
        return values[lower];
    }

    double weight = index - lower;
    return values[lower] * (1.0 - weight) + values[upper] * weight;
}

void CPAPSession::calculateMetrics() {
    metrics = SessionMetrics();

    // ===== USAGE METRICS =====
    if (duration_seconds.has_value() && duration_seconds.value() > 0) {
        metrics->usage_hours = duration_seconds.value() / 3600.0;
        metrics->usage_percent = (metrics->usage_hours.value() / 8.0) * 100.0;  // 8-hour target
    }

    // ===== EVENT METRICS =====
    metrics->total_events = static_cast<int>(events.size());

    // Event breakdown by type
    for (const auto& event : events) {
        switch (event.event_type) {
            case EventType::OBSTRUCTIVE:
                metrics->obstructive_apneas++;
                break;
            case EventType::CENTRAL:
                metrics->central_apneas++;
                break;
            case EventType::HYPOPNEA:
                metrics->hypopneas++;
                break;
            case EventType::RERA:
                metrics->reras++;
                break;
            case EventType::CLEAR_AIRWAY:
                metrics->clear_airway_apneas++;
                break;
            default:
                break;
        }
    }

    // Calculate AHI (events per hour)
    if (duration_seconds.has_value() && duration_seconds.value() > 0) {
        double hours = duration_seconds.value() / 3600.0;
        metrics->ahi = metrics->total_events / hours;
    }

    // Event duration statistics
    if (!events.empty()) {
        std::vector<double> durations;
        double total_duration = 0.0;

        for (const auto& event : events) {
            durations.push_back(event.duration_seconds);
            total_duration += event.duration_seconds;
        }

        metrics->avg_event_duration = total_duration / events.size();
        metrics->max_event_duration = *std::max_element(durations.begin(), durations.end());

        // Time in apnea (% of session)
        if (duration_seconds.has_value() && duration_seconds.value() > 0) {
            metrics->time_in_apnea_percent = (total_duration / duration_seconds.value()) * 100.0;
        }
    }

    // ===== BREATHING METRICS (from BreathingSummary) =====
    if (!breathing_summary.empty()) {
        std::vector<double> pressure_vals, leak_vals, flow_vals;
        std::vector<double> rr_vals, tv_vals, mv_vals, ti_vals, te_vals, ie_vals, fl_vals;

        for (const auto& breath : breathing_summary) {
            // Pressure
            pressure_vals.push_back(breath.avg_pressure);

            // Leak
            if (breath.leak_rate.has_value()) {
                leak_vals.push_back(breath.leak_rate.value());
            }

            // Flow
            flow_vals.push_back(breath.avg_flow_rate);

            // Respiratory metrics
            if (breath.respiratory_rate.has_value()) {
                rr_vals.push_back(breath.respiratory_rate.value());
            }
            if (breath.tidal_volume.has_value()) {
                tv_vals.push_back(breath.tidal_volume.value());
            }
            if (breath.minute_ventilation.has_value()) {
                mv_vals.push_back(breath.minute_ventilation.value());
            }
            if (breath.inspiratory_time.has_value()) {
                ti_vals.push_back(breath.inspiratory_time.value());
            }
            if (breath.expiratory_time.has_value()) {
                te_vals.push_back(breath.expiratory_time.value());
            }
            if (breath.ie_ratio.has_value()) {
                ie_vals.push_back(breath.ie_ratio.value());
            }
            if (breath.flow_limitation.has_value()) {
                fl_vals.push_back(breath.flow_limitation.value());
            }
        }

        // Pressure statistics
        if (!pressure_vals.empty()) {
            double sum = std::accumulate(pressure_vals.begin(), pressure_vals.end(), 0.0);
            metrics->avg_pressure = sum / pressure_vals.size();
            metrics->min_pressure = *std::min_element(pressure_vals.begin(), pressure_vals.end());
            metrics->max_pressure = *std::max_element(pressure_vals.begin(), pressure_vals.end());
            metrics->pressure_p95 = calculatePercentile(pressure_vals, 95.0);
            metrics->pressure_p50 = calculatePercentile(pressure_vals, 50.0);
        }

        // Leak statistics
        if (!leak_vals.empty()) {
            double sum = std::accumulate(leak_vals.begin(), leak_vals.end(), 0.0);
            metrics->avg_leak_rate = sum / leak_vals.size();
            metrics->max_leak_rate = *std::max_element(leak_vals.begin(), leak_vals.end());
            metrics->leak_p95 = calculatePercentile(leak_vals, 95.0);
        }

        // Flow statistics
        if (!flow_vals.empty()) {
            double sum = std::accumulate(flow_vals.begin(), flow_vals.end(), 0.0);
            metrics->avg_flow_rate = sum / flow_vals.size();
            metrics->max_flow_rate = *std::max_element(flow_vals.begin(), flow_vals.end());
            metrics->flow_p95 = calculatePercentile(flow_vals, 95.0);
        }

        // Respiratory rate
        if (!rr_vals.empty()) {
            double sum = std::accumulate(rr_vals.begin(), rr_vals.end(), 0.0);
            metrics->avg_respiratory_rate = sum / rr_vals.size();
        }

        // Tidal volume
        if (!tv_vals.empty()) {
            double sum = std::accumulate(tv_vals.begin(), tv_vals.end(), 0.0);
            metrics->avg_tidal_volume = sum / tv_vals.size();
        }

        // Minute ventilation
        if (!mv_vals.empty()) {
            double sum = std::accumulate(mv_vals.begin(), mv_vals.end(), 0.0);
            metrics->avg_minute_ventilation = sum / mv_vals.size();
        }

        // Inspiratory time
        if (!ti_vals.empty()) {
            double sum = std::accumulate(ti_vals.begin(), ti_vals.end(), 0.0);
            metrics->avg_inspiratory_time = sum / ti_vals.size();
        }

        // Expiratory time
        if (!te_vals.empty()) {
            double sum = std::accumulate(te_vals.begin(), te_vals.end(), 0.0);
            metrics->avg_expiratory_time = sum / te_vals.size();
        }

        // I:E ratio
        if (!ie_vals.empty()) {
            double sum = std::accumulate(ie_vals.begin(), ie_vals.end(), 0.0);
            metrics->avg_ie_ratio = sum / ie_vals.size();
        }

        // Flow limitation
        if (!fl_vals.empty()) {
            double sum = std::accumulate(fl_vals.begin(), fl_vals.end(), 0.0);
            metrics->avg_flow_limitation = sum / fl_vals.size();
        }
    }

    // ===== SPO2 METRICS =====
    if (!vitals.empty()) {
        std::vector<double> spo2_values;
        std::vector<int> hr_values;

        for (const auto& vital : vitals) {
            if (vital.spo2.has_value()) {
                spo2_values.push_back(vital.spo2.value());
            }
            if (vital.heart_rate.has_value()) {
                hr_values.push_back(vital.heart_rate.value());
            }
        }

        // SpO2 statistics
        if (!spo2_values.empty()) {
            double spo2_sum = std::accumulate(spo2_values.begin(), spo2_values.end(), 0.0);
            metrics->avg_spo2 = spo2_sum / spo2_values.size();
            metrics->min_spo2 = *std::min_element(spo2_values.begin(), spo2_values.end());
            metrics->max_spo2 = *std::max_element(spo2_values.begin(), spo2_values.end());
            metrics->spo2_p95 = calculatePercentile(spo2_values, 95.0);
            metrics->spo2_p50 = calculatePercentile(spo2_values, 50.0);

            // Count desaturations (drops >= 4% from baseline)
            int drops = 0;
            double baseline = metrics->avg_spo2.value();
            for (size_t i = 1; i < spo2_values.size(); ++i) {
                if (spo2_values[i-1] - spo2_values[i] >= 4.0) {
                    drops++;
                }
            }
            metrics->spo2_drops = drops;
        }

        // Heart rate statistics
        if (!hr_values.empty()) {
            int hr_sum = std::accumulate(hr_values.begin(), hr_values.end(), 0);
            metrics->avg_heart_rate = hr_sum / static_cast<int>(hr_values.size());
            metrics->max_heart_rate = *std::max_element(hr_values.begin(), hr_values.end());
            metrics->min_heart_rate = *std::min_element(hr_values.begin(), hr_values.end());

            // Convert to double vector for percentile calculation
            std::vector<double> hr_double(hr_values.begin(), hr_values.end());
            metrics->hr_p95 = static_cast<int>(calculatePercentile(hr_double, 95.0));
            metrics->hr_p50 = static_cast<int>(calculatePercentile(hr_double, 50.0));
        }
    }
}

std::string CPAPSession::toString() const {
    std::ostringstream oss;

    oss << "CPAP Session: " << device_name << " (SRN: " << serial_number << ")\n";

    if (session_start.has_value()) {
        std::time_t start_time = std::chrono::system_clock::to_time_t(session_start.value());
        oss << "Start: " << std::put_time(std::localtime(&start_time), "%Y-%m-%d %H:%M:%S") << "\n";
    }

    if (duration_seconds.has_value()) {
        int hours = duration_seconds.value() / 3600;
        int minutes = (duration_seconds.value() % 3600) / 60;
        oss << "Duration: " << hours << "h " << minutes << "m\n";
    }

    if (metrics.has_value()) {
        oss << "AHI: " << std::fixed << std::setprecision(1) << metrics->ahi << " events/hour\n";
        oss << "Events: " << metrics->total_events << " (OA=" << metrics->obstructive_apneas
            << ", CA=" << metrics->central_apneas << ", H=" << metrics->hypopneas << ")\n";

        if (metrics->avg_spo2.has_value()) {
            oss << "SpO2: avg=" << std::fixed << std::setprecision(1)
                << metrics->avg_spo2.value() << "%, min=" << metrics->min_spo2.value() << "%\n";
        }

        if (metrics->avg_heart_rate.has_value()) {
            oss << "HR: avg=" << metrics->avg_heart_rate.value()
                << " bpm, range=" << metrics->min_heart_rate.value()
                << "-" << metrics->max_heart_rate.value() << " bpm\n";
        }

        if (metrics->avg_pressure.has_value()) {
            oss << "Pressure: avg=" << std::fixed << std::setprecision(1)
                << metrics->avg_pressure.value() << " cmH2O\n";
        }

        if (metrics->avg_leak_rate.has_value()) {
            oss << "Leak: avg=" << std::fixed << std::setprecision(1)
                << metrics->avg_leak_rate.value() << " L/min\n";
        }
    }

    return oss.str();
}

} // namespace hms_cpap
