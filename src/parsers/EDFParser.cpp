#include "parsers/EDFParser.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <regex>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <sstream>

namespace hms_cpap {

// ============================================================================
//  EDFFile — raw EDF reader tolerant of ResMed non-standard files
// ============================================================================

std::string EDFFile::trimField(const char* data, int len) {
    std::string s(data, len);
    // Trim trailing spaces and nulls
    auto end = s.find_last_not_of(" \0");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

bool EDFFile::open(const std::string& filepath) {
    filepath_ = filepath;

    std::ifstream f(filepath, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        std::cerr << "EDF: Cannot open " << filepath << std::endl;
        return false;
    }

    auto file_size = static_cast<long long>(f.tellg());
    f.seekg(0, std::ios::beg);

    // --- Main header: 256 bytes ---
    if (file_size < 256) {
        std::cerr << "EDF: File too small for header: " << filepath << std::endl;
        return false;
    }

    char hdr[256];
    f.read(hdr, 256);

    // version (8)  — should be "0       "
    // patient (80)
    patient = trimField(hdr + 8, 80);
    // recording (80)
    recording = trimField(hdr + 88, 80);
    // startdate dd.mm.yy (8)
    std::string date_str = trimField(hdr + 168, 8);
    // starttime hh.mm.ss (8)
    std::string time_str = trimField(hdr + 176, 8);
    // header_bytes (8)
    header_bytes = std::atoi(std::string(hdr + 184, 8).c_str());
    // reserved (44) — "EDF+C" or "EDF+D" for EDF+
    reserved = trimField(hdr + 192, 44);
    // number of data records (8)
    num_records_header = std::atoi(std::string(hdr + 236, 8).c_str());
    // data record duration (8)
    record_duration = std::atof(std::string(hdr + 244, 8).c_str());
    // number of signals (4)
    num_signals = std::atoi(std::string(hdr + 252, 4).c_str());

    // Parse date: dd.mm.yy
    if (date_str.size() >= 8) {
        start_day   = std::atoi(date_str.substr(0, 2).c_str());
        start_month = std::atoi(date_str.substr(3, 2).c_str());
        int yy      = std::atoi(date_str.substr(6, 2).c_str());
        start_year  = (yy >= 85) ? 1900 + yy : 2000 + yy;
    }
    // Parse time: hh.mm.ss
    if (time_str.size() >= 8) {
        start_hour   = std::atoi(time_str.substr(0, 2).c_str());
        start_minute = std::atoi(time_str.substr(3, 2).c_str());
        start_second = std::atoi(time_str.substr(6, 2).c_str());
    }

    if (num_signals <= 0 || num_signals > 512) {
        std::cerr << "EDF: Invalid signal count " << num_signals
                  << " in " << filepath << std::endl;
        return false;
    }

    // --- Signal headers: 256 bytes × num_signals ---
    int sig_hdr_size = 256 * num_signals;
    if (file_size < 256 + sig_hdr_size) {
        std::cerr << "EDF: File too small for signal headers: " << filepath << std::endl;
        return false;
    }

    std::vector<char> sig_hdr(sig_hdr_size);
    f.read(sig_hdr.data(), sig_hdr_size);

    signals.resize(num_signals);
    int pos = 0;

    // label (16 × ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].label = trimField(sig_hdr.data() + pos, 16);
        pos += 16;
    }
    // transducer (80 × ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].transducer = trimField(sig_hdr.data() + pos, 80);
        pos += 80;
    }
    // physical dimension (8 × ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].phys_dim = trimField(sig_hdr.data() + pos, 8);
        pos += 8;
    }
    // physical minimum (8 × ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].phys_min = std::atof(std::string(sig_hdr.data() + pos, 8).c_str());
        pos += 8;
    }
    // physical maximum (8 × ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].phys_max = std::atof(std::string(sig_hdr.data() + pos, 8).c_str());
        pos += 8;
    }
    // digital minimum (8 × ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].dig_min = std::atoi(std::string(sig_hdr.data() + pos, 8).c_str());
        pos += 8;
    }
    // digital maximum (8 × ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].dig_max = std::atoi(std::string(sig_hdr.data() + pos, 8).c_str());
        pos += 8;
    }
    // prefiltering (80 × ns) — skip
    pos += 80 * num_signals;
    // samples per data record (8 × ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].samples_per_record = std::atoi(std::string(sig_hdr.data() + pos, 8).c_str());
        pos += 8;
    }
    // reserved (32 × ns) — skip

    // Compute scaling for each signal
    // physical = (digital - dig_min) * scale + phys_min
    // where scale = (phys_max - phys_min) / (dig_max - dig_min)
    for (auto& sig : signals) {
        int dig_range = sig.dig_max - sig.dig_min;
        if (dig_range != 0) {
            sig.scale  = (sig.phys_max - sig.phys_min) / dig_range;
            sig.offset = sig.phys_min - sig.dig_min * sig.scale;
        }
    }

    // Compute record size in bytes (each sample = 2 bytes for EDF)
    record_size_bytes = 0;
    for (auto& sig : signals) {
        record_size_bytes += sig.samples_per_record * 2;
    }

    // How many complete data records are actually in the file?
    long long data_bytes = file_size - header_bytes;
    if (record_size_bytes > 0) {
        actual_records = static_cast<int>(data_bytes / record_size_bytes);
    }

    // Completeness check
    // ResMed writes num_records_header=1 at session start, then appends data
    // So we use actual_records from file size, NOT the stale header value
    if (num_records_header == -1) {
        // EDF+ unknown length (still recording)
        complete = false;
        growing = true;
        extra_records = actual_records;  // All records are "extra" when header is -1
    } else if (actual_records > num_records_header) {
        // File has MORE data than header declared = still recording
        // Keep actual_records as-is (read all data from file)
        complete = false;
        growing = true;
        extra_records = actual_records - num_records_header;
    } else if (actual_records == num_records_header) {
        // File matches header exactly - complete
        complete = true;
        growing = false;
        extra_records = 0;
    } else {
        // actual_records < num_records_header = truncated
        complete = false;
        growing = false;
        extra_records = 0;
    }

    return true;
}

std::chrono::system_clock::time_point EDFFile::getStartTime() const {
    std::tm t = {};
    t.tm_year = start_year - 1900;
    t.tm_mon  = start_month - 1;
    t.tm_mday = start_day;
    t.tm_hour = start_hour;
    t.tm_min  = start_minute;
    t.tm_sec  = start_second;
    t.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&t));
}

int EDFFile::findSignal(const std::string& partial_label) const {
    for (int i = 0; i < num_signals; ++i) {
        if (signals[i].label.find(partial_label) != std::string::npos) {
            return i;
        }
    }
    return -1;
}

int EDFFile::readSignal(int signal_idx, std::vector<double>& out) {
    if (signal_idx < 0 || signal_idx >= num_signals || actual_records <= 0) {
        return 0;
    }

    std::ifstream f(filepath_, std::ios::binary);
    if (!f.is_open()) return 0;

    const auto& sig = signals[signal_idx];
    int total_samples = actual_records * sig.samples_per_record;
    out.resize(total_samples);

    // Compute byte offset of this signal within each data record
    int sig_offset_in_record = 0;
    for (int i = 0; i < signal_idx; ++i) {
        sig_offset_in_record += signals[i].samples_per_record * 2;
    }

    int sample_out = 0;
    std::vector<int16_t> raw(sig.samples_per_record);

    for (int rec = 0; rec < actual_records; ++rec) {
        long long record_start = header_bytes + (long long)rec * record_size_bytes;
        f.seekg(record_start + sig_offset_in_record);
        f.read(reinterpret_cast<char*>(raw.data()), sig.samples_per_record * 2);

        if (f.gcount() < sig.samples_per_record * 2) {
            // Incomplete record — stop here
            out.resize(sample_out);
            return sample_out;
        }

        for (int s = 0; s < sig.samples_per_record; ++s) {
            out[sample_out++] = raw[s] * sig.scale + sig.offset;
        }
    }

    return sample_out;
}

std::vector<EDFAnnotation> EDFFile::readAnnotations() {
    std::vector<EDFAnnotation> annotations;

    // Find annotation signal(s) — label starts with "EDF Annotations"
    int annot_idx = -1;
    for (int i = 0; i < num_signals; ++i) {
        if (signals[i].label.find("EDF Annotations") != std::string::npos) {
            annot_idx = i;
            break;
        }
    }
    if (annot_idx < 0) return annotations;

    std::ifstream f(filepath_, std::ios::binary);
    if (!f.is_open()) return annotations;

    const auto& sig = signals[annot_idx];
    int annot_bytes_per_record = sig.samples_per_record * 2;

    // Compute byte offset of annotation signal in each record
    int sig_offset = 0;
    for (int i = 0; i < annot_idx; ++i) {
        sig_offset += signals[i].samples_per_record * 2;
    }

    std::vector<char> buf(annot_bytes_per_record);

    for (int rec = 0; rec < actual_records; ++rec) {
        long long record_start = header_bytes + (long long)rec * record_size_bytes;
        f.seekg(record_start + sig_offset);
        f.read(buf.data(), annot_bytes_per_record);

        if (f.gcount() < annot_bytes_per_record) break;

        // Parse TAL (Time-stamped Annotation List)
        // Format: +onset\x15duration\x14description\x14\x00
        //   or:   +onset\x14description\x14\x00  (no duration)
        int pos = 0;
        while (pos < annot_bytes_per_record) {
            // Skip null padding
            while (pos < annot_bytes_per_record && buf[pos] == '\0') pos++;
            if (pos >= annot_bytes_per_record) break;

            // Read onset: starts with '+' or '-', ends with \x14 or \x15
            int tal_start = pos;
            if (buf[pos] != '+' && buf[pos] != '-') break;

            // Find the end of this TAL entry (\x00 terminates)
            int tal_end = pos;
            while (tal_end < annot_bytes_per_record && buf[tal_end] != '\0') tal_end++;

            std::string tal(buf.data() + tal_start, tal_end - tal_start);
            pos = tal_end + 1;

            // Parse the TAL string
            // Split by \x14 (separator between onset+duration and descriptions)
            double onset = 0;
            double duration = -1;

            size_t first_sep = tal.find('\x14');
            if (first_sep == std::string::npos) continue;

            std::string onset_part = tal.substr(0, first_sep);

            // onset_part may contain \x15 separating onset from duration
            size_t dur_sep = onset_part.find('\x15');
            if (dur_sep != std::string::npos) {
                onset = std::atof(onset_part.substr(0, dur_sep).c_str());
                std::string dur_str = onset_part.substr(dur_sep + 1);
                if (!dur_str.empty()) duration = std::atof(dur_str.c_str());
            } else {
                onset = std::atof(onset_part.c_str());
            }

            // Everything after first \x14 are descriptions separated by \x14
            std::string rest = tal.substr(first_sep + 1);
            // Split by \x14
            size_t dpos = 0;
            while (dpos < rest.size()) {
                size_t next = rest.find('\x14', dpos);
                if (next == std::string::npos) next = rest.size();
                std::string desc = rest.substr(dpos, next - dpos);
                dpos = next + 1;

                if (desc.empty()) continue;
                // Skip "Recording starts" / "Recording ends" markers
                if (desc == "Recording starts" || desc == "Recording ends") continue;

                EDFAnnotation annot;
                annot.onset_sec = onset;
                annot.duration_sec = duration;
                annot.description = desc;
                annotations.push_back(annot);
            }
        }
    }

    return annotations;
}

// ============================================================================
//  EDFParser — orchestrates file-level parsing into CPAPSession
// ============================================================================

bool EDFParser::parseDeviceInfo(const std::string& filepath,
                                 std::string& serial_number,
                                 int& model_id,
                                 int& version_id) {
    EDFFile edf;
    if (!edf.open(filepath)) return false;

    // Recording field format: "Startdate DD-MMM-YYYY ... SRN=XXXXX MID=XX VID=XX"
    const std::string& rec = edf.recording;

    std::smatch m;
    if (std::regex_search(rec, m, std::regex("SRN=(\\d+)"))) {
        serial_number = m[1].str();
    }
    if (std::regex_search(rec, m, std::regex("MID=(\\d+)"))) {
        model_id = std::stoi(m[1].str());
    }
    if (std::regex_search(rec, m, std::regex("VID=(\\d+)"))) {
        version_id = std::stoi(m[1].str());
    }

    return true;
}

bool EDFParser::parseBRPFile(const std::string& filepath, CPAPSession& session) {
    std::cout << "Parser: Parsing BRP " << filepath << std::endl;

    EDFFile edf;
    if (!edf.open(filepath)) return false;

    // Get EDF header timestamp
    auto file_start = edf.getStartTime();

    // Set timestamps from EDF header only if not already set (from filename)
    if (!session.session_start.has_value()) {
        session.session_start = file_start;
        std::cout << "Parser: Using EDF header timestamp (no filename timestamp provided)" << std::endl;
    }

    session.duration_seconds = static_cast<int>(edf.actual_records * edf.record_duration);
    session.session_end = session.session_start.value() +
                          std::chrono::seconds(session.duration_seconds.value());
    session.data_records = edf.actual_records;
    session.file_complete = edf.complete;

    // Find Flow and Pressure signals
    int flow_idx = edf.findSignal("Flow");
    int press_idx = edf.findSignal("Press");

    if (flow_idx < 0) {
        std::cerr << "Parser: Flow signal not found in BRP" << std::endl;
        return false;
    }

    std::vector<double> flow_data, press_data;
    edf.readSignal(flow_idx, flow_data);
    if (press_idx >= 0) {
        edf.readSignal(press_idx, press_data);
    }

    if (flow_data.empty()) {
        std::cerr << "Parser: No flow data read from BRP" << std::endl;
        return false;
    }

    // ResMed stores flow in L/sec - convert to L/min (multiply by 60)
    // Reference: OSCAR resmed_loader.cpp line 3121
    for (double& val : flow_data) {
        val *= 60.0;
    }

    // Compute per-minute breathing summaries
    // Flow is 25 Hz → 1500 samples per minute
    double sample_rate = edf.signals[flow_idx].samples_per_record / edf.record_duration;
    int samples_per_minute = static_cast<int>(sample_rate * 60);
    int n_minutes = static_cast<int>(flow_data.size()) / samples_per_minute;

    bool have_pressure = !press_data.empty();

    for (int min = 0; min < n_minutes; ++min) {
        int start = min * samples_per_minute;
        int end   = start + samples_per_minute;

        auto flow_begin = flow_data.begin() + start;
        auto flow_end   = flow_data.begin() + end;

        BreathingSummary summary(session.session_start.value() + std::chrono::minutes(min));

        // Basic flow statistics
        double sum = std::accumulate(flow_begin, flow_end, 0.0);
        summary.avg_flow_rate = sum / samples_per_minute;
        summary.max_flow_rate = *std::max_element(flow_begin, flow_end);
        summary.min_flow_rate = *std::min_element(flow_begin, flow_end);

        // Basic pressure statistics
        if (have_pressure && end <= static_cast<int>(press_data.size())) {
            auto p_begin = press_data.begin() + start;
            auto p_end   = press_data.begin() + end;
            double psum = std::accumulate(p_begin, p_end, 0.0);
            summary.avg_pressure = psum / samples_per_minute;
            summary.max_pressure = *std::max_element(p_begin, p_end);
            summary.min_pressure = *std::min_element(p_begin, p_end);
        }

        // Calculate OSCAR-style respiratory metrics (RR, TV, MV, Ti/Te, I:E, FL, percentiles)
        calculateRespiratoryMetrics(flow_data, press_data, sample_rate, min, summary);

        session.breathing_summary.push_back(summary);
    }

    // Detect flow-based session boundaries (actual mask on/off times)
    std::optional<std::chrono::system_clock::time_point> actual_start, actual_end;
    bool session_active = false;
    detectFlowBasedSessionBoundaries(flow_data, sample_rate, file_start,
                                     actual_start, actual_end, session_active);

    // Override file-based timestamps with flow-based timestamps
    if (actual_start.has_value()) {
        session.session_start = actual_start;
        std::cout << "Parser: Actual session start detected from flow data" << std::endl;
    }
    if (actual_end.has_value()) {
        session.session_end = actual_end;
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(
            actual_end.value() - actual_start.value_or(file_start)
        );
        session.duration_seconds = static_cast<int>(duration.count());
        std::cout << "Parser: Actual session end detected from flow data" << std::endl;
    } else if (session_active) {
        // Determine session status from EDF file completeness
        // If actual_records > header (edf.complete=false), file is still being written = IN_PROGRESS
        if (!edf.complete) {
            // File is still being written - session is IN_PROGRESS
            session.session_end = std::nullopt;
            session.status = CPAPSession::Status::IN_PROGRESS;
            std::cout << "Parser: Session IN_PROGRESS (file growing: "
                      << edf.extra_records << " extra records beyond header "
                      << edf.num_records_header << ")" << std::endl;
        } else {
            // File is complete but flow detection didn't find end
            // Use staleness check as fallback
            auto last_data_time = file_start + std::chrono::seconds(static_cast<int>(flow_data.size() / sample_rate));
            auto now = std::chrono::system_clock::now();
            auto staleness = std::chrono::duration_cast<std::chrono::minutes>(now - last_data_time).count();

            if (staleness > 30) {
                // Data is stale - session ended when file stopped growing
                session.session_end = last_data_time;
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                    last_data_time - actual_start.value_or(file_start)
                );
                session.duration_seconds = static_cast<int>(duration.count());
                std::cout << "Parser: Session marked COMPLETED (data stale by " << staleness << " min)" << std::endl;
            } else {
                // Session is still ongoing - no end time yet
                session.session_end = std::nullopt;
                std::cout << "Parser: Session still active (ongoing, data is " << staleness << " min old)" << std::endl;
            }
        }
    }

    std::cout << "Parser: BRP parsed — " << n_minutes << " minutes, "
              << flow_data.size() << " flow samples"
              << (edf.complete ? "" : " (INCOMPLETE)") << std::endl;
    return true;
}

void EDFParser::detectFlowBasedSessionBoundaries(
    const std::vector<double>& flow_data,
    double sample_rate,
    std::chrono::system_clock::time_point file_start,
    std::optional<std::chrono::system_clock::time_point>& actual_start,
    std::optional<std::chrono::system_clock::time_point>& actual_end,
    bool& session_active
) {
    if (flow_data.empty()) return;

    const double FLOW_THRESHOLD = 0.1;  // L/min - below this is "no flow"
    const int ZERO_FLOW_DURATION = 5 * 60; // 5 minutes of zero flow = session ended
    const int samples_for_end = static_cast<int>(ZERO_FLOW_DURATION * sample_rate);

    // Find first non-zero flow (actual session start)
    for (size_t i = 0; i < flow_data.size(); ++i) {
        if (std::abs(flow_data[i]) > FLOW_THRESHOLD) {
            int seconds_offset = static_cast<int>(i / sample_rate);
            actual_start = file_start + std::chrono::seconds(seconds_offset);
            break;
        }
    }

    // Find last sustained non-zero flow (actual session end)
    // Work backwards from the end
    int last_flow_idx = -1;
    for (int i = flow_data.size() - 1; i >= 0; --i) {
        if (std::abs(flow_data[i]) > FLOW_THRESHOLD) {
            last_flow_idx = i;
            break;
        }
    }

    if (last_flow_idx < 0) {
        // No flow detected at all
        session_active = false;
        return;
    }

    // Check if there's sustained zero flow after last_flow_idx
    int zero_count = flow_data.size() - last_flow_idx - 1;
    if (zero_count >= samples_for_end) {
        // Session ended - sustained zero flow
        int seconds_offset = static_cast<int>(last_flow_idx / sample_rate);
        actual_end = file_start + std::chrono::seconds(seconds_offset);
        session_active = false;
    } else {
        // Session is still active - recent flow detected
        session_active = true;
    }
}

// ============================================================================
//  Respiratory Metrics Calculation (OSCAR-style)
// ============================================================================

double EDFParser::calculatePercentile(
    const std::vector<double>& data,
    double percentile
) {
    if (data.empty()) return 0.0;

    // Filter out invalid values (-1, NaN, Inf)
    std::vector<double> valid_data;
    valid_data.reserve(data.size());

    for (double val : data) {
        if (!std::isnan(val) && !std::isinf(val) && val != -1.0) {
            valid_data.push_back(val);
        }
    }

    if (valid_data.empty()) return 0.0;

    std::vector<double> sorted = valid_data;
    std::sort(sorted.begin(), sorted.end());

    double idx = (percentile / 100.0) * (sorted.size() - 1);
    int lower = static_cast<int>(std::floor(idx));
    int upper = static_cast<int>(std::ceil(idx));

    if (lower == upper) {
        return sorted[lower];
    }

    // Linear interpolation
    double weight = idx - lower;
    return sorted[lower] * (1 - weight) + sorted[upper] * weight;
}

std::vector<EDFParser::BreathCycle> EDFParser::detectBreaths(
    const std::vector<double>& flow_data,
    double sample_rate
) {
    std::vector<BreathCycle> breaths;

    if (flow_data.empty()) return breaths;

    const double FLOW_THRESHOLD = 0.05;  // L/min - noise threshold
    const int MIN_BREATH_SAMPLES = static_cast<int>(sample_rate * 1.0);  // 1 second minimum
    const int MAX_BREATH_SAMPLES = static_cast<int>(sample_rate * 10.0); // 10 seconds maximum
    const double MAX_VALID_FLOW = 200.0;  // L/min - sanity check (normal max ~120 L/min)
    const double MIN_VALID_FLOW = -200.0; // L/min - sanity check

    // Validate and filter flow data first
    // Replace invalid values (-1, NaN, out-of-range) with 0.0
    std::vector<double> clean_flow;
    clean_flow.reserve(flow_data.size());

    int invalid_count = 0;
    for (double val : flow_data) {
        // Check for invalid values: -1 (sensor disconnected), NaN, Inf, out of range
        if (std::isnan(val) || std::isinf(val) ||
            val == -1.0 || val < MIN_VALID_FLOW || val > MAX_VALID_FLOW) {
            clean_flow.push_back(0.0);
            invalid_count++;
        } else {
            clean_flow.push_back(val);
        }
    }

    // DEBUG: Show flow data characteristics (first minute only)
    static int debug_count = 0;
    if (debug_count < 1) {
        debug_count++;
        std::cout << "\n=== BREATH DETECTION DEBUG ===" << std::endl;
        std::cout << "Flow data size: " << flow_data.size() << " samples" << std::endl;
        std::cout << "Sample rate: " << sample_rate << " Hz" << std::endl;
        std::cout << "Invalid values replaced: " << invalid_count << std::endl;

        // Show first 20 samples
        std::cout << "First 20 flow values (L/min): ";
        for (int i = 0; i < std::min(20, (int)clean_flow.size()); i++) {
            std::cout << clean_flow[i] << " ";
        }
        std::cout << std::endl;

        // Show flow range
        auto minmax = std::minmax_element(clean_flow.begin(), clean_flow.end());
        std::cout << "Flow range: [" << *minmax.first << ", " << *minmax.second << "] L/min" << std::endl;

        // Count non-zero values
        int non_zero = std::count_if(clean_flow.begin(), clean_flow.end(),
                                      [](double v) { return std::abs(v) > 0.001; });
        std::cout << "Non-zero samples: " << non_zero << " / " << clean_flow.size() << std::endl;
        std::cout << "FLOW_THRESHOLD: " << FLOW_THRESHOLD << " L/min" << std::endl;
    }

    // Detect zero-crossings (breath boundaries)
    // Positive flow = inspiration, negative flow = expiration
    std::vector<int> zero_crossings;
    zero_crossings.push_back(0);  // Start of data

    bool was_positive = (clean_flow[0] > 0);

    for (size_t i = 1; i < clean_flow.size(); ++i) {
        bool is_positive = (clean_flow[i] > FLOW_THRESHOLD);
        bool is_negative = (clean_flow[i] < -FLOW_THRESHOLD);

        // Detect crossing from negative to positive (start of inspiration)
        if (!was_positive && is_positive) {
            zero_crossings.push_back(i);
            was_positive = true;
        }
        // Detect crossing from positive to negative (start of expiration)
        else if (was_positive && is_negative) {
            zero_crossings.push_back(i);
            was_positive = false;
        }
    }

    zero_crossings.push_back(clean_flow.size() - 1);  // End of data

    // DEBUG: Show zero-crossing detection results
    if (debug_count == 1) {
        std::cout << "Zero-crossings detected: " << zero_crossings.size() << std::endl;
        std::cout << "Potential breath cycles: " << (zero_crossings.size() - 2) / 2 << std::endl;
        std::cout << "Breath duration range: [" << MIN_BREATH_SAMPLES << ", " << MAX_BREATH_SAMPLES << "] samples" << std::endl;
        std::cout << "(" << MIN_BREATH_SAMPLES / sample_rate << " - " << MAX_BREATH_SAMPLES / sample_rate << " seconds)" << std::endl;
    }

    // Process each breath cycle (inspiration + expiration)
    int filtered_duration = 0;
    int filtered_tidal_volume = 0;
    for (size_t i = 0; i < zero_crossings.size() - 2; i += 2) {
        int start = zero_crossings[i];
        int mid = zero_crossings[i + 1];  // Inspiration -> Expiration transition
        int end = zero_crossings[i + 2];

        int breath_duration = end - start;

        // Filter out breaths that are too short or too long
        if (breath_duration < MIN_BREATH_SAMPLES || breath_duration > MAX_BREATH_SAMPLES) {
            filtered_duration++;
            continue;
        }

        BreathCycle breath;
        breath.start_idx = start;
        breath.end_idx = end;

        // Calculate inspiratory time (start -> mid)
        breath.inspiratory_time = static_cast<double>(mid - start) / sample_rate;

        // Calculate expiratory time (mid -> end)
        breath.expiratory_time = static_cast<double>(end - mid) / sample_rate;

        // Calculate tidal volume (integrate flow over time)
        // Tidal volume = sum of inspiratory flow * (1/sample_rate) * 1000 mL/L * 60 sec/min
        // Flow is in L/min, we need L, so divide by 60
        double inspiratory_volume = 0.0;
        for (int j = start; j < mid; ++j) {
            if (clean_flow[j] > 0) {
                inspiratory_volume += clean_flow[j];
            }
        }
        // Convert: (L/min) * (samples) / (samples/sec) / (sec/min) = L
        // Then convert to mL
        breath.tidal_volume = (inspiratory_volume / sample_rate / 60.0) * 1000.0;

        // Validate tidal volume (sanity check)
        // Normal tidal volume: 300-800 mL, extreme range: 100-2000 mL
        if (breath.tidal_volume < 50.0 || breath.tidal_volume > 3000.0) {
            filtered_tidal_volume++;
            continue;  // Skip this invalid breath
        }

        // Calculate flow limitation score (0-1)
        // Flow limitation is detected by analyzing the shape of the inspiratory flow curve
        // A flattened curve (flow-limited) has lower peak-to-mean ratio
        double insp_max = 0.0;
        double insp_sum = 0.0;
        int insp_count = 0;

        for (int j = start; j < mid; ++j) {
            if (clean_flow[j] > 0) {
                insp_max = std::max(insp_max, clean_flow[j]);
                insp_sum += clean_flow[j];
                insp_count++;
            }
        }

        if (insp_count > 0 && insp_max > 0) {
            double insp_mean = insp_sum / insp_count;
            double peak_to_mean_ratio = insp_max / insp_mean;

            // Normal breath: peak ~2-3x mean (ratio 2.0-3.0)
            // Flow-limited: peak ~1.2-1.5x mean (ratio 1.2-1.5)
            // Score: 0 = no limitation, 1 = severe limitation
            // Invert and normalize: limitation = 1 - (ratio - 1.0) / 2.0
            breath.flow_limitation = std::max(0.0, std::min(1.0, 1.0 - (peak_to_mean_ratio - 1.0) / 2.0));
        } else {
            breath.flow_limitation = 0.0;
        }

        breaths.push_back(breath);
    }

    // DEBUG: Show final breath detection results
    if (debug_count == 1) {
        std::cout << "\nBreath detection results:" << std::endl;
        std::cout << "  Valid breaths: " << breaths.size() << std::endl;
        std::cout << "  Filtered (duration): " << filtered_duration << std::endl;
        std::cout << "  Filtered (tidal volume): " << filtered_tidal_volume << std::endl;
        std::cout << "=== END DEBUG ===\n" << std::endl;
    }

    return breaths;
}

void EDFParser::calculateRespiratoryMetrics(
    const std::vector<double>& flow_data,
    const std::vector<double>& pressure_data,
    double sample_rate,
    int minute_idx,
    BreathingSummary& summary
) {
    int samples_per_minute = static_cast<int>(sample_rate * 60);
    int start = minute_idx * samples_per_minute;
    int end = start + samples_per_minute;

    if (end > static_cast<int>(flow_data.size())) {
        end = flow_data.size();
    }

    // Extract this minute's flow data
    std::vector<double> minute_flow(flow_data.begin() + start, flow_data.begin() + end);

    // Detect breaths in this minute
    std::vector<BreathCycle> breaths = detectBreaths(minute_flow, sample_rate);

    if (breaths.empty()) {
        // No breaths detected - patient might not be breathing or mask is off
        if (minute_idx == 0) {
            std::cout << "Parser: No breaths detected in first minute (mask off or no flow)" << std::endl;
        }
        return;
    }

    if (minute_idx == 0) {
        std::cout << "Parser: Detected " << breaths.size() << " breaths in first minute" << std::endl;
    }

    // Calculate respiratory rate (breaths per minute)
    summary.respiratory_rate = static_cast<double>(breaths.size());

    // Calculate average tidal volume
    double total_tv = 0.0;
    for (const auto& breath : breaths) {
        total_tv += breath.tidal_volume;
    }
    summary.tidal_volume = total_tv / breaths.size();

    // Calculate minute ventilation (L/min)
    // MV = RR * TV (breaths/min * mL/breath * L/1000mL)
    if (summary.respiratory_rate.has_value() && summary.tidal_volume.has_value()) {
        summary.minute_ventilation = (summary.respiratory_rate.value() *
                                     summary.tidal_volume.value()) / 1000.0;
    }

    // Calculate average inspiratory and expiratory times
    double total_ti = 0.0;
    double total_te = 0.0;
    for (const auto& breath : breaths) {
        total_ti += breath.inspiratory_time;
        total_te += breath.expiratory_time;
    }
    summary.inspiratory_time = total_ti / breaths.size();
    summary.expiratory_time = total_te / breaths.size();

    // Calculate I:E ratio
    if (summary.inspiratory_time.has_value() &&
        summary.expiratory_time.has_value() &&
        summary.expiratory_time.value() > 0) {
        summary.ie_ratio = summary.inspiratory_time.value() /
                          summary.expiratory_time.value();
    }

    // Calculate average flow limitation score
    double total_fl = 0.0;
    for (const auto& breath : breaths) {
        total_fl += breath.flow_limitation;
    }
    summary.flow_limitation = total_fl / breaths.size();

    // Calculate percentile statistics for flow
    summary.flow_p95 = calculatePercentile(minute_flow, 95.0);

    // Calculate percentile statistics for pressure
    if (!pressure_data.empty() && end <= static_cast<int>(pressure_data.size())) {
        std::vector<double> minute_pressure(pressure_data.begin() + start,
                                           pressure_data.begin() + end);
        summary.pressure_p95 = calculatePercentile(minute_pressure, 95.0);
    }

    // Calculate leak rate (unintentional leak = inspiratory volume - expiratory volume)
    if (!breaths.empty()) {
        double total_leak = 0.0;
        int valid_breaths = 0;

        for (const auto& breath : breaths) {
            // Calculate expiratory volume (integrate negative flow during expiration phase)
            double expiratory_volume = 0.0;
            int mid_idx = breath.start_idx + (breath.end_idx - breath.start_idx) / 2;

            for (int j = mid_idx; j < breath.end_idx; ++j) {
                // j is already an index into minute_flow (not flow_data), so use it directly
                if (j >= 0 && j < static_cast<int>(minute_flow.size())) {
                    if (minute_flow[j] < 0) {  // Negative = expiration
                        expiratory_volume += std::abs(minute_flow[j]);
                    }
                }
            }
            // Convert: (L/min) × (samples) / (samples/sec) / (sec/min) = L → mL
            expiratory_volume = (expiratory_volume / sample_rate / 60.0) * 1000.0;

            // Leak = inspiratory - expiratory (positive = air escaped from mask)
            double breath_leak = breath.tidal_volume - expiratory_volume;

            // Sanity check: reasonable leak range (0-100 L/min equivalent per breath)
            if (breath_leak >= 0 && breath_leak < 100000.0) {
                total_leak += breath_leak;
                valid_breaths++;
            }
        }

        if (valid_breaths > 0 && summary.respiratory_rate.has_value()) {
            // Convert to L/min: (mL/breath) * (breaths/min) / 1000
            double avg_leak_ml_per_breath = total_leak / valid_breaths;
            summary.leak_rate = (avg_leak_ml_per_breath * summary.respiratory_rate.value()) / 1000.0;

            // Sanity check: typical leak 0-40 L/min, mask leaks >24 L/min problematic per OSCAR
            if (summary.leak_rate.value() < 0 || summary.leak_rate.value() > 100.0) {
                summary.leak_rate = std::nullopt;  // Invalid, discard
            }
        } else {
            summary.leak_rate = std::nullopt;
        }
    } else {
        summary.leak_rate = std::nullopt;
    }
}

bool EDFParser::parseEVEFile(const std::string& filepath, CPAPSession& session) {
    std::cout << "Parser: Parsing EVE " << filepath << std::endl;

    EDFFile edf;
    if (!edf.open(filepath)) {
        std::cout << "Parser: EVE file could not be opened (may be incomplete)" << std::endl;
        return true;  // EVE is optional
    }

    if (!edf.isEDFPlus()) {
        std::cout << "Parser: EVE is not EDF+ format, skipping annotations" << std::endl;
        return true;
    }

    auto start_time = edf.getStartTime();
    auto annotations = edf.readAnnotations();

    for (const auto& annot : annotations) {
        EventType event_type = EventType::APNEA;  // Default
        std::string desc_lower = annot.description;
        std::transform(desc_lower.begin(), desc_lower.end(), desc_lower.begin(), ::tolower);

        if (desc_lower.find("hypopnea") != std::string::npos) {
            event_type = EventType::HYPOPNEA;
        } else if (desc_lower.find("obstructive") != std::string::npos &&
                   desc_lower.find("apnea") != std::string::npos) {
            event_type = EventType::OBSTRUCTIVE;
        } else if (desc_lower.find("central") != std::string::npos &&
                   desc_lower.find("apnea") != std::string::npos) {
            event_type = EventType::CENTRAL;
        } else if (desc_lower.find("clear") != std::string::npos &&
                   desc_lower.find("airway") != std::string::npos) {
            event_type = EventType::CLEAR_AIRWAY;
        } else if (desc_lower.find("apnea") != std::string::npos) {
            event_type = EventType::APNEA;
        } else if (desc_lower.find("rera") != std::string::npos) {
            event_type = EventType::RERA;
        } else if (desc_lower.find("csr") != std::string::npos) {
            event_type = EventType::CSR;
        } else {
            // Arousal, flow limitation, etc. — still record them
            event_type = EventType::RERA;
        }

        double dur = (annot.duration_sec > 0) ? annot.duration_sec : 0.0;

        CPAPEvent event(
            event_type,
            start_time + std::chrono::milliseconds(static_cast<long long>(annot.onset_sec * 1000)),
            dur
        );
        event.details = annot.description;
        session.events.push_back(event);
    }

    std::cout << "Parser: EVE parsed — " << session.events.size() << " events" << std::endl;
    return true;
}

bool EDFParser::parseSADFile(const std::string& filepath, CPAPSession& session) {
    std::cout << "Parser: Parsing SAD " << filepath << std::endl;

    EDFFile edf;
    if (!edf.open(filepath)) {
        std::cout << "Parser: SAD file could not be opened (may be incomplete)" << std::endl;
        return true;  // SAD is optional
    }

    auto start_time = edf.getStartTime();

    // Find SpO2 and Pulse/HR signals
    int spo2_idx  = edf.findSignal("SpO2");
    int pulse_idx = edf.findSignal("Pulse");
    if (pulse_idx < 0) pulse_idx = edf.findSignal("Heart");
    if (pulse_idx < 0) pulse_idx = edf.findSignal("HR");

    std::vector<double> spo2_data, pulse_data;
    if (spo2_idx >= 0)  edf.readSignal(spo2_idx, spo2_data);
    if (pulse_idx >= 0) edf.readSignal(pulse_idx, pulse_data);

    // Both at 1 Hz — one sample per second
    int n_samples = static_cast<int>(std::max(spo2_data.size(), pulse_data.size()));

    for (int i = 0; i < n_samples; ++i) {
        CPAPVitals vital(start_time + std::chrono::seconds(i));

        if (i < static_cast<int>(spo2_data.size())) {
            double val = spo2_data[i];
            // Filter invalid readings (0 = sensor disconnected)
            if (val > 0 && val <= 100) {
                vital.spo2 = val;
            }
        }
        if (i < static_cast<int>(pulse_data.size())) {
            int val = static_cast<int>(pulse_data[i]);
            // Filter invalid readings
            if (val > 0 && val < 300) {
                vital.heart_rate = val;
            }
        }

        session.vitals.push_back(vital);
    }

    std::cout << "Parser: SAD parsed — " << n_samples << " seconds of vitals"
              << (edf.complete ? "" : " (INCOMPLETE)") << std::endl;
    return true;
}

std::unique_ptr<CPAPSession> EDFParser::parseSession(
    const std::string& session_dir,
    const std::string& device_id,
    const std::string& device_name,
    std::optional<std::chrono::system_clock::time_point> session_start_from_filename
) {
    std::cout << "Parser: Parsing session from " << session_dir << std::endl;

    if (session_start_from_filename.has_value()) {
        auto ts = std::chrono::system_clock::to_time_t(session_start_from_filename.value());
        std::cout << "Parser: Using filename timestamp: "
                  << std::put_time(std::localtime(&ts), "%Y-%m-%d %H:%M:%S")
                  << std::endl;
    }

    if (!std::filesystem::exists(session_dir)) {
        std::cerr << "Parser: Directory not found: " << session_dir << std::endl;
        return nullptr;
    }

    // CRITICAL FIX: Find ALL checkpoint files (multiple BRP/PLD/SAD per session!)
    std::vector<std::string> brp_files, pld_files, sad_files;
    std::string eve_file, csl_file;

    for (const auto& entry : std::filesystem::directory_iterator(session_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        std::string lower = filename;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower.find("_brp.edf") != std::string::npos) {
            brp_files.push_back(entry.path().string());
        } else if (lower.find("_pld.edf") != std::string::npos) {
            pld_files.push_back(entry.path().string());
        } else if (lower.find("_sad.edf") != std::string::npos) {
            sad_files.push_back(entry.path().string());
        } else if (lower.find("_eve.edf") != std::string::npos) {
            eve_file = entry.path().string();
        } else if (lower.find("_csl.edf") != std::string::npos) {
            csl_file = entry.path().string();
        }
    }

    if (brp_files.empty()) {
        std::cerr << "Parser: No BRP.edf files found in " << session_dir << std::endl;
        return nullptr;
    }

    // Sort checkpoint files chronologically (by filename timestamp)
    auto sort_by_filename = [](const std::string& a, const std::string& b) {
        return std::filesystem::path(a).filename() < std::filesystem::path(b).filename();
    };
    std::sort(brp_files.begin(), brp_files.end(), sort_by_filename);
    std::sort(pld_files.begin(), pld_files.end(), sort_by_filename);
    std::sort(sad_files.begin(), sad_files.end(), sort_by_filename);

    std::cout << "Parser: Found " << brp_files.size() << " BRP checkpoint(s), "
              << pld_files.size() << " PLD, "
              << sad_files.size() << " SAD, "
              << (eve_file.empty() ? 0 : 1) << " EVE, "
              << (csl_file.empty() ? 0 : 1) << " CSL" << std::endl;

    // Create session
    auto session = std::make_unique<CPAPSession>();
    session->device_id = device_id;
    session->device_name = device_name;

    // Use filename timestamp as session identifier (DB lookup key)
    if (session_start_from_filename.has_value()) {
        session->session_start = session_start_from_filename;
        std::cout << "Parser: Session identifier from filename set" << std::endl;
    }

    // Parse device info from first BRP header
    int mid = 0, vid = 0;
    parseDeviceInfo(brp_files[0], session->serial_number, mid, vid);
    session->model_id = mid;
    session->version_id = vid;

    // Parse ALL BRP files and combine breathing data
    std::cout << "Parser: Processing " << brp_files.size() << " BRP checkpoint(s)..." << std::endl;
    for (size_t i = 0; i < brp_files.size(); ++i) {
        std::cout << "  BRP " << (i+1) << "/" << brp_files.size() << ": "
                  << std::filesystem::path(brp_files[i]).filename() << std::endl;

        if (!parseBRPFile(brp_files[i], *session)) {
            std::cerr << "  ⚠️  Failed to parse, skipping" << std::endl;
            continue;  // Skip failed files, continue with others
        }
    }

    // Parse ALL SAD files and combine vitals data
    if (!sad_files.empty()) {
        std::cout << "Parser: Processing " << sad_files.size() << " SAD checkpoint(s)..." << std::endl;
        for (size_t i = 0; i < sad_files.size(); ++i) {
            std::cout << "  SAD " << (i+1) << "/" << sad_files.size() << ": "
                      << std::filesystem::path(sad_files[i]).filename() << std::endl;

            if (!parseSADFile(sad_files[i], *session)) {
                std::cerr << "  ⚠️  Failed to parse, skipping" << std::endl;
                continue;
            }
        }
    }

    // Parse EVE (optional - written hours after session)
    if (!eve_file.empty()) {
        std::cout << "Parser: Processing EVE (events)..." << std::endl;
        parseEVEFile(eve_file, *session);
        session->has_events = true;
    } else {
        std::cout << "Parser: No EVE file (session in progress, events not yet available)" << std::endl;
        session->has_events = false;
    }

    // Parse CSL (optional - summary)
    if (!csl_file.empty()) {
        // TODO: Parse CSL file for session summary metrics
        session->has_summary = true;
    } else {
        session->has_summary = false;
    }

    // Set session status based on flow-based detection
    // If session_end is set, session is completed; if nullopt, still in progress
    if (session->session_end.has_value()) {
        session->status = CPAPSession::Status::COMPLETED;
        std::cout << "Parser: Session status = COMPLETED (session ended)" << std::endl;
    } else {
        session->status = CPAPSession::Status::IN_PROGRESS;
        std::cout << "Parser: Session status = IN_PROGRESS (session active)" << std::endl;
    }

    // Calculate aggregated metrics
    session->calculateMetrics();

    std::cout << "Parser: Session complete" << std::endl;
    std::cout << session->toString() << std::endl;

    return session;
}

} // namespace hms_cpap
