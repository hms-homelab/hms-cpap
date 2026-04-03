#pragma once

/**
 * SleeplinkBridge.h — Type aliases bridging sleeplink::parser into hms_cpap namespace.
 *
 * hms-cpap used to have its own EDFParser + CPAPModels. These are now in the
 * hms-sleeplink-parser shared library (sleeplink::parser namespace). This bridge
 * lets existing hms-cpap code compile unchanged via type aliases.
 */

#include <sleeplink/parser/Models.h>
#include <sleeplink/parser/EDFFile.h>
#include <sleeplink/parser/EDFParser.h>
#include <sleeplink/parser/ISessionParser.h>

namespace hms_cpap {

// ── Core types ──────────────────────────────────────────────────────────────
using EventType        = sleeplink::parser::EventType;
using DeviceManufacturer = sleeplink::parser::DeviceManufacturer;
using DeviceSettings   = sleeplink::parser::DeviceSettings;
using CPAPEvent        = sleeplink::parser::SleepEvent;
using CPAPVitals       = sleeplink::parser::VitalSample;
using BreathingSummary = sleeplink::parser::BreathingSummary;
using SessionMetrics   = sleeplink::parser::SessionMetrics;
using CPAPSession      = sleeplink::parser::ParsedSession;
using STRDailyRecord   = sleeplink::parser::STRDailyRecord;

// ── Parser types ────────────────────────────────────────────────────────────
using EDFSignal        = sleeplink::parser::EDFSignal;
using EDFAnnotation    = sleeplink::parser::EDFAnnotation;
using EDFFile          = sleeplink::parser::EDFFile;
using EDFParser        = sleeplink::parser::EDFParser;
using ISessionParser   = sleeplink::parser::ISessionParser;

// ── Free functions ──────────────────────────────────────────────────────────
using sleeplink::parser::eventTypeToString;
using sleeplink::parser::createParser;

// ── hms-cpap-specific types (not in sleeplink-parser) ───────────────────────

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
