#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <map>

namespace hms_cpap {

/**
 * Summary period — controls which date range and LLM prompt to use.
 * Daily: single night (existing behavior).
 * Weekly: last 7 nights (auto-triggered on Sundays).
 * Monthly: last 30 nights (auto-triggered on 1st of month).
 */
enum class SummaryPeriod { DAILY, WEEKLY, MONTHLY };

/**
 * Event types for respiratory events
 */
enum class EventType {
    APNEA,
    HYPOPNEA,
    RERA,
    CSR,
    OBSTRUCTIVE,
    CENTRAL,
    CLEAR_AIRWAY
};

std::string eventTypeToString(EventType type);

/**
 * CPAPEvent - Respiratory event (apnea, hypopnea, etc.)
 */
struct CPAPEvent {
    EventType event_type;
    std::chrono::system_clock::time_point timestamp;
    double duration_seconds;
    std::optional<std::string> details;

    CPAPEvent() = default;
    CPAPEvent(EventType type, std::chrono::system_clock::time_point ts, double dur)
        : event_type(type), timestamp(ts), duration_seconds(dur) {}
};

/**
 * CPAPVitals - Vital signs (SpO2, Heart Rate) from SAD.edf
 */
struct CPAPVitals {
    std::chrono::system_clock::time_point timestamp;
    std::optional<double> spo2;         // Oxygen saturation %
    std::optional<int> heart_rate;      // bpm

    CPAPVitals() = default;
    CPAPVitals(std::chrono::system_clock::time_point ts)
        : timestamp(ts) {}
};

/**
 * BreathingSummary - Summary statistics for breathing waveforms (from BRP.edf)
 * Includes OSCAR-style calculated metrics
 */
struct BreathingSummary {
    std::chrono::system_clock::time_point timestamp;

    // Raw flow/pressure stats
    double avg_flow_rate;
    double max_flow_rate;
    double min_flow_rate;
    double avg_pressure;
    double max_pressure;
    double min_pressure;

    // Calculated respiratory metrics (OSCAR-style)
    std::optional<double> respiratory_rate;      // Breaths per minute
    std::optional<double> tidal_volume;          // mL per breath (avg)
    std::optional<double> minute_ventilation;    // L/min (RR × TV)
    std::optional<double> inspiratory_time;      // Ti (seconds, avg)
    std::optional<double> expiratory_time;       // Te (seconds, avg)
    std::optional<double> ie_ratio;              // I:E ratio
    std::optional<double> flow_limitation;       // Flow limitation score 0-1
    std::optional<double> leak_rate;             // Unintentional leak L/min

    // PLD-derived metrics (machine's own calculations)
    std::optional<double> mask_pressure;       // cmH2O (PLD MaskPress.2s)
    std::optional<double> epr_pressure;        // cmH2O (PLD EprPress.2s)
    std::optional<double> snore_index;         // 0-5 (PLD Snore.2s)
    std::optional<double> target_ventilation;  // L/min (PLD TgtVent.2s, ASV only)

    // Percentile statistics
    std::optional<double> flow_p95;              // 95th percentile flow
    std::optional<double> pressure_p95;          // 95th percentile pressure

    BreathingSummary() = default;
    BreathingSummary(std::chrono::system_clock::time_point ts)
        : timestamp(ts),
          avg_flow_rate(0), max_flow_rate(0), min_flow_rate(0),
          avg_pressure(0), max_pressure(0), min_pressure(0) {}
};

/**
 * SessionMetrics - Aggregated metrics for a CPAP session (OSCAR-compatible)
 */
struct SessionMetrics {
    // ===== EVENT METRICS =====
    int total_events = 0;
    double ahi = 0.0;  // Apnea-Hypopnea Index (events/hour)

    // Event breakdown
    int obstructive_apneas = 0;
    int central_apneas = 0;
    int hypopneas = 0;
    int reras = 0;
    int clear_airway_apneas = 0;

    // Event statistics
    std::optional<double> avg_event_duration;     // seconds
    std::optional<double> max_event_duration;     // seconds
    std::optional<double> time_in_apnea_percent;  // % of session

    // ===== PRESSURE METRICS =====
    std::optional<double> avg_pressure;           // cmH2O
    std::optional<double> min_pressure;           // cmH2O
    std::optional<double> max_pressure;           // cmH2O
    std::optional<double> pressure_p95;           // 95th percentile cmH2O
    std::optional<double> pressure_p50;           // Median cmH2O

    // ===== LEAK METRICS =====
    std::optional<double> avg_leak_rate;          // L/min
    std::optional<double> max_leak_rate;          // L/min
    std::optional<double> leak_p95;               // 95th percentile L/min
    std::optional<double> leak_p50;                // Median L/min

    // ===== FLOW METRICS =====
    std::optional<double> avg_flow_rate;          // L/min
    std::optional<double> max_flow_rate;          // L/min
    std::optional<double> flow_p95;               // 95th percentile L/min

    // ===== RESPIRATORY METRICS =====
    std::optional<double> avg_respiratory_rate;   // breaths/minute
    std::optional<double> avg_tidal_volume;       // mL per breath
    std::optional<double> avg_minute_ventilation; // L/min
    std::optional<double> avg_inspiratory_time;   // Ti seconds
    std::optional<double> avg_expiratory_time;    // Te seconds
    std::optional<double> avg_ie_ratio;           // I:E ratio
    std::optional<double> avg_flow_limitation;    // 0-1 score

    // ===== PLD-DERIVED METRICS =====
    std::optional<double> avg_mask_pressure;       // cmH2O
    std::optional<double> avg_epr_pressure;        // cmH2O
    std::optional<double> avg_snore;               // 0-5 average
    // ASV-specific (NULL for CPAP/APAP)
    std::optional<double> avg_target_ventilation;  // L/min
    std::optional<int> therapy_mode;               // 0=CPAP, 1=APAP, 7=ASV, 8=ASVAuto

    // ===== SPO2 METRICS =====
    std::optional<double> avg_spo2;               // %
    std::optional<double> min_spo2;               // %
    std::optional<double> max_spo2;               // %
    std::optional<double> spo2_p95;               // 95th percentile %
    std::optional<double> spo2_p50;               // Median %
    std::optional<int> spo2_drops;                // Count of desaturations

    // ===== HEART RATE METRICS =====
    std::optional<int> avg_heart_rate;            // bpm
    std::optional<int> min_heart_rate;            // bpm
    std::optional<int> max_heart_rate;            // bpm
    std::optional<int> hr_p95;                    // 95th percentile bpm
    std::optional<int> hr_p50;                    // Median bpm

    // ===== USAGE METRICS =====
    std::optional<double> usage_hours;            // Total hours
    std::optional<double> usage_percent;          // % of 8-hour target

    // ===== DATE LABEL (for range queries) =====
    std::string sleep_day;  // "YYYY-MM-DD", filled by getMetricsForDateRange()
};

/**
 * SessionFileSet - Grouped EDF files for a single CPAP session
 *
 * ONE session (identified by CSL/EVE timestamp) has:
 * - CSL: Summary file (written once when session starts)
 * - EVE: Events file (written hours later after session ends)
 * - Multiple BRP/PLD/SAD checkpoint files (written during session)
 *
 * Example: Session 20260206_021037
 *   CSL: 20260206_021037_CSL.edf
 *   EVE: 20260206_021037_EVE.edf
 *   BRP: 20260206_021043_BRP.edf (checkpoint 1)
 *        20260206_022600_BRP.edf (checkpoint 2)
 *        20260206_023241_BRP.edf (checkpoint 3)
 *        20260206_031937_BRP.edf (checkpoint 4 - final)
 */
struct SessionFileSet {
    std::string date_folder;      // e.g., "20260205"
    std::string session_prefix;   // e.g., "20260206_021037" (from CSL/EVE)

    // Session identifier files (ONE per session)
    std::string csl_file;         // Clinical summary
    std::string eve_file;         // Events

    // Checkpoint files (MULTIPLE per session - collect ALL)
    std::vector<std::string> brp_files;  // Breathing pattern checkpoints
    std::vector<std::string> pld_files;  // Pressure/load checkpoints
    std::vector<std::string> sad_files;  // SpO2/HR vitals checkpoints

    // Individual file sizes (for smart caching - filename -> size in KB)
    std::map<std::string, int> file_sizes_kb;

    // Metadata
    int total_size_kb = 0;
    std::chrono::system_clock::time_point session_start;

    /** Check if session has any data files */
    bool hasData() const {
        return !brp_files.empty() || !pld_files.empty() || !sad_files.empty();
    }

    /** Check if session is complete (has summary and events) */
    bool isComplete() const {
        return !csl_file.empty() && !eve_file.empty() && hasData();
    }
};

/**
 * CPAPSession - Complete CPAP session data
 */
struct CPAPSession {
    // Device info
    std::string device_id;
    std::string device_name;
    std::string serial_number;
    std::optional<int> model_id;
    std::optional<int> version_id;

    // Session metadata
    std::optional<std::chrono::system_clock::time_point> session_start;
    std::optional<std::chrono::system_clock::time_point> session_end;
    std::optional<int> duration_seconds;
    int data_records = 0;
    bool file_complete = false;  // false if EDF files still being written

    // EDF file growth tracking (ResMed writes header=1, then appends data)
    int extra_records = 0;       // Records beyond header declaration (actual - header, 0 if complete)
    bool growing = false;        // true if actual_records > num_records_header (file still being written)

    // Session status tracking (for live updates)
    enum class Status {
        IN_PROGRESS,  // Has breathing data, no EVE file yet
        COMPLETED     // Has EVE file (events available)
    };
    Status status = Status::IN_PROGRESS;
    bool has_summary = false;   // CSL file present
    bool has_events = false;    // EVE file present

    // File path references (for original EDF files)
    std::optional<std::string> brp_file_path;
    std::optional<std::string> eve_file_path;
    std::optional<std::string> sad_file_path;
    std::optional<std::string> pld_file_path;
    std::optional<std::string> csl_file_path;

    // Session data
    std::vector<CPAPEvent> events;
    std::vector<CPAPVitals> vitals;
    std::vector<BreathingSummary> breathing_summary;

    // Aggregated metrics
    std::optional<SessionMetrics> metrics;

    /**
     * Calculate aggregated metrics from session data
     */
    void calculateMetrics();

    /**
     * Get human-readable session summary
     */
    std::string toString() const;
};

/**
 * STRDailyRecord - Daily therapy summary from STR.edf
 *
 * ResMed writes STR.edf at the SD root with 81 signals, 1 record per day
 * (86400s duration). Contains official ResMed values: AHI, mask timing,
 * pressure/leak percentiles, device settings, and cumulative hours.
 */
struct STRDailyRecord {
    std::string device_id;
    std::chrono::system_clock::time_point record_date;  // noon of this day

    // Mask timing (on/off pairs as timestamps)
    std::vector<std::pair<
        std::chrono::system_clock::time_point,
        std::chrono::system_clock::time_point>> mask_pairs;
    int mask_events = 0;           // raw count from STR (divide by 2 for pair count)

    // Duration
    double duration_minutes = 0;   // therapy time this day
    double patient_hours = 0;      // cumulative lifetime hours

    // Official ResMed indices (events/hour)
    double ahi = 0, hi = 0, ai = 0, oai = 0, cai = 0, uai = 0;
    double rin = 0;                // RERA index
    double csr = 0;                // Cheyne-Stokes minutes

    // Pressure (cmH2O)
    double blow_press_95 = 0, blow_press_5 = 0;
    double mask_press_50 = 0, mask_press_95 = 0, mask_press_max = 0;

    // Leak (L/min -- stored as L/s in EDF, multiply by 60)
    double leak_50 = 0, leak_95 = 0, leak_70 = 0, leak_max = 0;

    // SpO2 (%)
    double spo2_50 = 0, spo2_95 = 0, spo2_max = 0;

    // Respiratory
    double resp_rate_50 = 0, resp_rate_95 = 0, resp_rate_max = 0;
    double tid_vol_50 = 0, tid_vol_95 = 0, tid_vol_max = 0;
    double min_vent_50 = 0, min_vent_95 = 0, min_vent_max = 0;

    // Settings
    int mode = 0;
    double epr_level = 0, pressure_setting = 0;
    double max_pressure = 0, min_pressure = 0;

    // Faults
    int fault_device = 0, fault_alarm = 0;

    // ===== ASV SETTINGS (Mode=7: ASV Fixed EPAP, Mode=8: ASV Variable EPAP) =====
    std::optional<double> asv_start_press;   // S.AV.StartPress (cmH2O)
    std::optional<double> asv_epap;          // S.AV.EPAP (cmH2O)
    std::optional<double> asv_max_ps;        // S.AV.MaxPS (cmH2O, max pressure support)
    std::optional<double> asv_min_ps;        // S.AV.MinPS (cmH2O, min pressure support)

    // ASVAuto settings (Mode=8 only)
    std::optional<double> asvauto_min_epap;  // S.AA.MinEPAP (cmH2O)
    std::optional<double> asvauto_max_epap;  // S.AA.MaxEPAP (cmH2O)

    // ===== TARGET PERCENTILES (ASV daily targets) =====
    std::optional<double> tgt_ipap_50;       // TgtIPAP.50 (cmH2O)
    std::optional<double> tgt_ipap_95;       // TgtIPAP.95 (cmH2O)
    std::optional<double> tgt_ipap_max;      // TgtIPAP.Max (cmH2O)
    std::optional<double> tgt_epap_50;       // TgtEPAP.50 (cmH2O)
    std::optional<double> tgt_epap_95;       // TgtEPAP.95 (cmH2O)
    std::optional<double> tgt_epap_max;      // TgtEPAP.Max (cmH2O)
    std::optional<double> tgt_vent_50;       // TgtVent.50 (L/min)
    std::optional<double> tgt_vent_95;       // TgtVent.95 (L/min)
    std::optional<double> tgt_vent_max;      // TgtVent.Max (L/min)

    bool hasTherapy() const { return duration_minutes > 0; }
};

} // namespace hms_cpap
