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
#include <cpapdash/parser/VLDParser.h>
#ifdef CPAPDASH_WITH_LOWENSTEIN
#include <cpapdash/parser/PrismaParser.h>
#endif

namespace hms_cpap {

// ── Core types ──────────────────────────────────────────────────────────────
using EventType        = cpapdash::parser::EventType;
using DeviceManufacturer = cpapdash::parser::DeviceManufacturer;
using DeviceSettings   = cpapdash::parser::DeviceSettings;
using CPAPEvent        = cpapdash::parser::SleepEvent;
using CPAPVitals       = cpapdash::parser::VitalSample;
using BreathingSummary = cpapdash::parser::BreathingSummary;
using Breath           = cpapdash::parser::Breath;
using DesatEvent       = cpapdash::parser::DesatEvent;
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

// ── VLD / Oximetry types ───────────────────────────────────────────────────
using VLDParser        = cpapdash::parser::VLDParser;
using OximetrySession  = cpapdash::parser::OximetrySession;
using OximetrySample   = cpapdash::parser::OximetrySample;
using OximetryMetrics  = cpapdash::parser::OximetryMetrics;

// ── hms-cpap-specific types (not in cpapdash-parser) ────────────────────────

/**
 * Summary period — controls which date range and LLM prompt to use.
 */
enum class SummaryPeriod { DAILY, WEEKLY, MONTHLY };

/**
 * SessionFileSet - Grouped EDF files for a single CPAP session.
 *
 * ONE session (identified by CSL/EVE timestamp) has:
 * - CSL: Summary files, one per mask-on block
 * - EVE: Events files, one per mask-on block (written hours later)
 * - Multiple BRP/PLD/SAD checkpoint files (written during session)
 *
 * EVE and CSL are vectors for the same reason the checkpoints are. A night is
 * several mask-on blocks and each writes its own pair. They used to be single
 * strings, and the matcher kept the FIRST one in prefix order, which is the
 * earliest block -- routinely a seconds-long mask-fit check whose EVE is the
 * empty 832-byte stub. Every real annotation of the night was dropped and the
 * session read AHI 0.0. See docs/SDD-014 and issue #22.
 */
/**
 * SessionFileRef - one file belonging to a session, in card-relative form.
 *
 * The `cpap_sessions.*_file_path` columns hold one path per kind and cannot
 * describe a night of several blocks. This is what `cpap_session_files` stores,
 * and it is the truth about which files make up a session; the columns are kept
 * as a denormalised convenience for readers that only ever wanted one.
 */
struct SessionFileRef {
    std::string kind;      // "brp" | "pld" | "sad" | "eve" | "csl"
    std::string rel_path;  // "DATALOG/20260812/20260812_233427_EVE.edf"
};

struct SessionFileSet {
    std::string date_folder;
    std::string session_prefix;

    std::vector<std::string> csl_files;
    std::vector<std::string> eve_files;

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
        return !csl_files.empty() && !eve_files.empty() && hasData();
    }
};

/**
 * Every file in a set, card-relative, ready for cpap_session_files.
 *
 * Three call sites build this (the burst cycle, backfill, and the reparse in
 * main), and they used to hand-roll the same five lines each. They are here so
 * a sixth kind of file, or a sixth call site, cannot quietly skip one of them.
 */
inline std::vector<SessionFileRef> sessionFileRefs(const SessionFileSet& s,
                                                   const std::string& date_folder) {
    const std::string base = "DATALOG/" + date_folder + "/";
    std::vector<SessionFileRef> out;
    auto add = [&](const char* kind, const std::vector<std::string>& names) {
        for (const auto& n : names) out.push_back({kind, base + n});
    };
    add("brp", s.brp_files);
    add("pld", s.pld_files);
    add("sad", s.sad_files);
    add("eve", s.eve_files);
    add("csl", s.csl_files);
    return out;
}

/**
 * Fill the singular cpap_sessions.*_file_path columns with the FIRST file of
 * each kind. They predate cpap_session_files and are kept for readers that only
 * ever wanted one path; the table is what describes the night.
 */
template <typename ParsedSessionT>
inline void applySessionFilePaths(ParsedSessionT& parsed, const SessionFileSet& s,
                                  const std::string& date_folder) {
    const std::string base = "DATALOG/" + date_folder + "/";
    if (!s.brp_files.empty()) parsed.brp_file_path = base + s.brp_files.front();
    if (!s.pld_files.empty()) parsed.pld_file_path = base + s.pld_files.front();
    if (!s.sad_files.empty()) parsed.sad_file_path = base + s.sad_files.front();
    if (!s.eve_files.empty()) parsed.eve_file_path = base + s.eve_files.front();
    if (!s.csl_files.empty()) parsed.csl_file_path = base + s.csl_files.front();
}

} // namespace hms_cpap
