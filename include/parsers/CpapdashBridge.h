#pragma once

/**
 * CpapdashBridge.h — Type aliases bridging cpapdash::parser into hms_cpap namespace.
 *
 * hms-cpap used to have its own EDFParser + CPAPModels. These are now in the
 * hms-cpapdash-parser shared library (cpapdash::parser namespace). This bridge
 * lets existing hms-cpap code compile unchanged via type aliases.
 */

#include <cpapdash/parser/Models.h>
#include <cpapdash/parser/EDFFile.h>
#include <cpapdash/parser/EDFParser.h>
#include <cpapdash/parser/ISessionParser.h>

namespace hms_cpap {

// ── Core types ──────────────────────────────────────────────────────────────
using EventType        = cpapdash::parser::EventType;
using DeviceManufacturer = cpapdash::parser::DeviceManufacturer;
using DeviceSettings   = cpapdash::parser::DeviceSettings;
using CPAPEvent        = cpapdash::parser::SleepEvent;
using CPAPVitals       = cpapdash::parser::VitalSample;
using BreathingSummary = cpapdash::parser::BreathingSummary;
using SessionMetrics   = cpapdash::parser::SessionMetrics;
using CPAPSession      = cpapdash::parser::ParsedSession;
using STRDailyRecord   = cpapdash::parser::STRDailyRecord;

// ── Parser types ────────────────────────────────────────────────────────────
using EDFSignal        = cpapdash::parser::EDFSignal;
using EDFAnnotation    = cpapdash::parser::EDFAnnotation;
using EDFFile          = cpapdash::parser::EDFFile;
using EDFParser        = cpapdash::parser::EDFParser;
using ISessionParser   = cpapdash::parser::ISessionParser;

// ── Free functions ──────────────────────────────────────────────────────────
using cpapdash::parser::eventTypeToString;
using cpapdash::parser::createParser;

// ── hms-cpap-specific types (not in cpapdash-parser) ────────────────────────

/**
 * Summary period — controls which date range and LLM prompt to use.
 */
enum class SummaryPeriod { DAILY, WEEKLY, MONTHLY };

/**
 * SessionFileSet - Grouped EDF files for a single CPAP session.
 *
 * ONE session (identified by CSL/EVE timestamp) has:
 * - CSL: Summary file (written once when session starts)
 * - EVE: Events file (written hours later after session ends)
 * - Multiple BRP/PLD/SAD checkpoint files (written during session)
 */
struct SessionFileSet {
    std::string date_folder;
    std::string session_prefix;

    std::string csl_file;
    std::string eve_file;

    std::vector<std::string> brp_files;
    std::vector<std::string> pld_files;
    std::vector<std::string> sad_files;

    std::map<std::string, int> file_sizes_kb;

    int total_size_kb = 0;
    std::chrono::system_clock::time_point session_start;

    bool hasData() const {
        return !brp_files.empty() || !pld_files.empty() || !sad_files.empty();
    }

    bool isComplete() const {
        return !csl_file.empty() && !eve_file.empty() && hasData();
    }
};

} // namespace hms_cpap
