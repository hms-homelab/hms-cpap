#pragma once

#include "models/CPAPModels.h"
#include <string>
#include <memory>
#include <vector>
#include <chrono>

namespace hms_cpap {

/**
 * Raw EDF signal descriptor (parsed from header)
 */
struct EDFSignal {
    std::string label;
    std::string transducer;
    std::string phys_dim;
    double phys_min = 0;
    double phys_max = 0;
    int dig_min = 0;
    int dig_max = 0;
    int samples_per_record = 0;
    // Computed scaling: physical = digital * scale + offset
    double scale = 1.0;
    double offset = 0.0;
};

/**
 * EDF+ annotation entry
 */
struct EDFAnnotation {
    double onset_sec;       // Seconds from recording start
    double duration_sec;    // Duration in seconds (-1 if unused)
    std::string description;
};

/**
 * Raw EDF file reader — tolerant of ResMed non-standard files.
 *
 * edflib 1.27 rejects ResMed CPAP files (empty phys_dim on Crc16 signal,
 * EDF+D with 0-second datarecords). This reader handles those quirks and
 * also handles incomplete/growing files (partial last data record).
 */
class EDFFile {
public:
    // Header fields
    std::string patient;
    std::string recording;
    int start_year = 0, start_month = 0, start_day = 0;
    int start_hour = 0, start_minute = 0, start_second = 0;
    int header_bytes = 0;
    std::string reserved;       // "EDF+C", "EDF+D", or empty for plain EDF
    int num_records_header = 0; // From header (-1 means unknown/recording)
    double record_duration = 0; // Seconds per data record
    int num_signals = 0;
    std::vector<EDFSignal> signals;

    // Computed
    int record_size_bytes = 0;  // Bytes per data record (all signals)
    int actual_records = 0;     // Records actually present in file
    int extra_records = 0;      // Records beyond header declaration (actual - header, 0 if complete)
    bool complete = false;      // true if actual_records == num_records_header
    bool growing = false;       // true if actual_records > num_records_header (file still being written)

    bool open(const std::string& filepath);

    /** Read all physical samples for one signal. Returns sample count. */
    int readSignal(int signal_idx, std::vector<double>& out);

    /** Read EDF+ annotations from annotation signal(s). */
    std::vector<EDFAnnotation> readAnnotations();

    /** Get recording start as system_clock time_point. */
    std::chrono::system_clock::time_point getStartTime() const;

    /** Find signal index whose label contains partial_label (-1 if not found). */
    int findSignal(const std::string& partial_label) const;

    bool isEDFPlus() const { return reserved.find("EDF+") != std::string::npos; }

private:
    std::string filepath_;
    static std::string trimField(const char* data, int len);
};

/**
 * EDFParser - Parser for ResMed CPAP EDF files
 *
 * Uses raw EDF parsing (not edflib) for ResMed compatibility.
 * Handles incomplete/growing files gracefully.
 *
 * File types:
 *   BRP.edf  Breathing pattern (Flow @ 25 Hz, Pressure @ 25 Hz)
 *   EVE.edf  Events / annotations (Apnea, Hypopnea, Arousal)
 *   SAD.edf  SpO2 @ 1 Hz, Heart Rate @ 1 Hz
 *   PLD.edf  Pressure/Load (9 channels @ 0.5 Hz)
 *   CSL.edf  Clinical summary
 */
class EDFParser {
public:
    static std::unique_ptr<CPAPSession> parseSession(
        const std::string& session_dir,
        const std::string& device_id,
        const std::string& device_name,
        std::optional<std::chrono::system_clock::time_point> session_start_from_filename = std::nullopt
    );

private:
    static bool parseDeviceInfo(const std::string& filepath,
                                std::string& serial_number,
                                int& model_id,
                                int& version_id);

    static bool parseBRPFile(const std::string& filepath, CPAPSession& session);
    static bool parseEVEFile(const std::string& filepath, CPAPSession& session);
    static bool parseSADFile(const std::string& filepath, CPAPSession& session);

    // Flow-based session boundary detection
    static void detectFlowBasedSessionBoundaries(
        const std::vector<double>& flow_data,
        double sample_rate,
        std::chrono::system_clock::time_point file_start,
        std::optional<std::chrono::system_clock::time_point>& actual_start,
        std::optional<std::chrono::system_clock::time_point>& actual_end,
        bool& session_active
    );

    // Breath analysis and calculated metrics (OSCAR-style)
    struct BreathCycle {
        int start_idx;
        int end_idx;
        double tidal_volume;        // mL
        double inspiratory_time;    // seconds
        double expiratory_time;     // seconds
        double flow_limitation;     // 0-1 score
    };

    static std::vector<BreathCycle> detectBreaths(
        const std::vector<double>& flow_data,
        double sample_rate
    );

    static void calculateRespiratoryMetrics(
        const std::vector<double>& flow_data,
        const std::vector<double>& pressure_data,
        double sample_rate,
        int minute_idx,
        BreathingSummary& summary
    );

    static double calculatePercentile(
        const std::vector<double>& data,
        double percentile
    );
};

} // namespace hms_cpap
