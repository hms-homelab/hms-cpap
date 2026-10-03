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
#ifdef CPAPDASH_WITH_SEFAM
#include <cpapdash/parser/SefamParser.h>
#endif
#ifdef CPAPDASH_WITH_BMC
#include <cpapdash/parser/BmcParser.h>
#endif

#include <algorithm>
#include <chrono>
#include <optional>

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
 * ONE session is one mask-on stretch of a day folder (the checkpoints split
 * wherever the mask was off for SESSION_GAP_MINUTES). It has:
 * - Multiple BRP/PLD/SAD checkpoint files, written during the stretch
 * - EVE/CSL: every pair in the day folder, with the window of time whose
 *   events are this stretch's (events_from, events_until)
 *
 * An EVE/CSL pair belongs to the CARD session, not to a mask-on (SDD-047).
 * The machine opens a new pair at the first mask-on after the card is opened
 * or re-inserted, and every later mask-on appends to that same pair, however
 * long the break. So one EVE named at an afternoon stretch can carry every
 * event of the night that follows it, and a day on which the card was pulled
 * several times has several pairs. The file's NAME therefore says nothing
 * about which stretch an event belongs to; the event's own time does. Each
 * stretch lists the day's pairs, and keepOwnEvents() keeps the events that
 * fall in its window, so each event is counted once, on the stretch it
 * happened in.
 *
 * EVE and CSL are vectors because a day can hold several pairs. They used to
 * be single strings, and the matcher kept the FIRST one in prefix order, which
 * dropped every other pair's annotations and read AHI 0.0. See issue #22.
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
    /// SDD-033: the 11 series' trigger/cycle file, one per checkpoint prefix.
    /// Nothing here parses it, but it is the machine's and it grows all night,
    /// so it is fetched like a checkpoint (resumed, stamped) rather than
    /// re-downloaded whole every burst as a card leftover.
    std::vector<std::string> tcv_files;

    std::map<std::string, int> file_sizes_kb;

    /// The card's own last-modified stamp per file, as a time_t in a FIXED UTC
    /// frame, straight from the listing.
    ///
    /// The listing's SIZE is KB-rounded, so a file under a kilobyte cannot show
    /// that it grew -- and CSL and EVE are under a kilobyte and DO grow, being
    /// appended all night. That is why they were re-downloaded unconditionally
    /// every burst: size could not answer the question, so nobody asked it.
    ///
    /// UTC via timegm, deliberately, and NOT EzShareFileEntry::getModTime():
    /// that one builds its time_point with mktime(), which resolves a local
    /// wall-clock time. Twice a year an hour repeats, so two stamps genuinely an
    /// hour apart compare EQUAL -- and this value is compared for equality to
    /// decide whether to skip a download. A DST fold would silently skip a real
    /// append. The card's stamp is a label, not a local time, and is treated as
    /// one.
    ///
    /// 0 means absent or unparsable, which always means fetch.
    std::map<std::string, std::time_t> card_stamps;

    int total_size_kb = 0;
    std::chrono::system_clock::time_point session_start;

    /// SDD-047: the events that are this stretch's, by their onset:
    /// [events_from, events_until). events_from is this stretch's start and
    /// events_until the next stretch's start, so an event in the gap after a
    /// stretch stays with it (the stretch before it). The day's first stretch
    /// has no events_from, and takes anything before it; the day's last has
    /// no events_until. Together the windows cover the day once, so no event
    /// is counted on two stretches. Both unset means every event is this one's.
    std::optional<std::chrono::system_clock::time_point> events_from;
    std::optional<std::chrono::system_clock::time_point> events_until;

    bool holdsEventAt(std::chrono::system_clock::time_point t) const {
        return (!events_from || t >= *events_from) && (!events_until || t < *events_until);
    }

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

/**
 * SDD-047: keep only the events that happened in this stretch, and recount.
 *
 * The parser reads every EVE it is given and keeps all of their annotations,
 * which is right for a day of one stretch and wrong for a day of several: each
 * stretch is handed the day's EVE/CSL (an EVE belongs to the card session, not
 * to a mask-on; see SessionFileSet), so without this every stretch would carry
 * the whole day's events. An event is kept where the set's window holds its
 * onset (SessionFileSet::holdsEventAt), and the session's metrics are computed
 * again from what is left. Every parse of a discovered set calls this, on
 * every path (burst, reparse, backfill), so the three cannot disagree.
 */
template <typename ParsedSessionT>
inline void keepOwnEvents(ParsedSessionT& parsed, const SessionFileSet& s) {
    auto& ev = parsed.events;
    const auto before = ev.size();
    ev.erase(std::remove_if(ev.begin(), ev.end(),
                            [&](const auto& e) { return !s.holdsEventAt(e.timestamp); }),
             ev.end());
    if (ev.size() != before) parsed.calculateMetrics();
}

} // namespace hms_cpap
