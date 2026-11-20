#include "services/ComputedEvents.h"

#include "database/SqlDialect.h"

#ifdef CPAPDASH_WITH_BMC
#include "cpapdash/parser/DesatDetector.h"
#include "cpapdash/parser/FlowEvents.h"
#endif

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace hms_cpap {

namespace {

/// The local wall clock, as every other session and event row is stored.
std::string localStamp(std::chrono::system_clock::time_point t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::ostringstream o;
    o << std::put_time(std::localtime(&tt), "%Y-%m-%d %H:%M:%S");
    return o.str();
}

std::string number(double v) {
    std::ostringstream o;
    o << v;
    return o.str();
}

}  // namespace

const char* indexEventsName(IndexEvents from) {
    return from == IndexEvents::Computed ? "computed" : "machine";
}

IndexEvents chooseIndexEvents(bool card_has_event_file, bool written_now) {
    return card_has_event_file && !written_now ? IndexEvents::Machine : IndexEvents::Computed;
}

void indexFromComputed(CPAPSession& session) {
    if (!session.metrics) return;
    const SessionMetrics kept = *session.metrics;
    std::swap(session.events, session.computed_events);
    session.calculateMetrics();
    std::swap(session.events, session.computed_events);
    SessionMetrics counted = *session.metrics;
    SessionMetrics out = kept;
    out.total_events          = counted.total_events;
    out.ahi                   = counted.ahi;
    out.obstructive_apneas    = counted.obstructive_apneas;
    out.central_apneas        = counted.central_apneas;
    out.hypopneas             = counted.hypopneas;
    out.reras                 = counted.reras;
    out.clear_airway_apneas   = counted.clear_airway_apneas;
    out.unclassified_apneas   = counted.unclassified_apneas;
    out.other_events          = counted.other_events;
    out.avg_event_duration    = counted.avg_event_duration;
    out.max_event_duration    = counted.max_event_duration;
    out.time_in_apnea_percent = counted.time_in_apnea_percent;
    // D5: apneas alone are not an AHI.
    out.index_kind = counted.hypopneas > 0 ? SessionMetrics::IndexKind::AHI
                                           : SessionMetrics::IndexKind::Ungraded;
    session.metrics = out;
}

bool storeComputedEvents(IDatabase& db, const std::string& device_id,
                         std::chrono::system_clock::time_point session_start,
                         const std::vector<CPAPEvent>& computed, IndexEvents index_from) {
    const DbType t = db.dbType();
    const auto p = [t](int i) { return sql::param(i, t); };

    const auto found = db.executeQueryChecked(
        "SELECT id FROM cpap_sessions WHERE device_id = " + p(1) + " AND session_start = " + p(2),
        {device_id, localStamp(session_start)});
    if (!found.ok || !found.rows.isArray() || found.rows.empty()) return false;
    const Json::Value& idv = found.rows[0]["id"];
    const std::string id = idv.isString() ? idv.asString() : std::to_string(idv.asInt64());

    // Replaced whole: a session read again as it grows finds its apneas again,
    // and one it found before may have moved or gone.
    bool ok = db.executeQueryChecked("DELETE FROM cpap_computed_events WHERE session_id = " + p(1),
                                     {id}).ok;
    for (const auto& e : computed) {
        if (!ok) break;
        ok = db.executeQueryChecked(
                   "INSERT INTO cpap_computed_events"
                   " (session_id, event_type, event_timestamp, duration_seconds, details)"
                   " VALUES (" + p(1) + ", " + p(2) + ", " + p(3) + ", " + p(4) + ", " + p(5) + ")",
                   {id, eventTypeToString(e.event_type), localStamp(e.timestamp),
                    number(e.duration_seconds), e.details.value_or("")})
                 .ok;
    }
    if (ok)
        ok = db.executeQueryChecked("UPDATE cpap_session_metrics SET index_events = " + p(1) +
                                        " WHERE session_id = " + p(2),
                                    {indexEventsName(index_from), id})
                 .ok;
    return ok;
}

#ifdef CPAPDASH_WITH_BMC
std::vector<CPAPEvent> ringHypopneas(const cpapdash::parser::BmcCard& card,
                                     const cpapdash::parser::BmcSessionSpan& span,
                                     const std::vector<CPAPVitals>& ring,
                                     std::chrono::seconds ring_to_machine) {
    namespace p = cpapdash::parser;
    if (ring.empty() || span.end < span.start) return {};

    // The ring's samples on the machine's clock, then its desaturations.
    std::vector<CPAPVitals> shifted = ring;
    for (auto& s : shifted) s.timestamp += ring_to_machine;
    std::sort(shifted.begin(), shifted.end(),
              [](const CPAPVitals& a, const CPAPVitals& b) { return a.timestamp < b.timestamp; });
    const auto desats = p::detectDesaturations(shifted);
    if (desats.empty()) return {};

    // The session's patient flow, as the parser reads it for its own apneas,
    // and the breaths in it that lie wholly on measured flow.
    const auto wave = p::BmcParser::waveform(card, span.start, span.end + 1);
    const auto noReading = [&wave](long a, long b) {
        a = std::max(0L, a);
        b = std::min(static_cast<long>(wave.flow.size()) - 1, b);
        for (long i = a; i <= b; ++i)
            if (std::isnan(wave.flow[static_cast<size_t>(i)])) return true;
        return false;
    };
    auto breaths = p::EDFParser::detectBreaths(wave.flow, wave.sample_rate);
    breaths.erase(std::remove_if(breaths.begin(), breaths.end(),
                                 [&](const p::EDFParser::BreathCycle& b) {
                                     return noReading(b.start_idx, b.end_idx);
                                 }),
                  breaths.end());

    // Word 98, the leak, per second: the large-leak exclusion.
    std::vector<double> leak(static_cast<size_t>(span.end - span.start + 1), std::nan(""));
    for (const auto& pk : card.packets) {
        if (pk.stamp < span.start || pk.stamp > span.end) continue;
        if (pk.words[p::bmc_word::kLeak] != p::kBmcInvalid)
            leak[static_cast<size_t>(pk.stamp - span.start)] = pk.words[p::bmc_word::kLeak] / 10.0;
    }

    const auto t0 = p::bmcToTimePoint(span.start);
    auto found = p::detectFlowHypopneas(breaths, wave.flow, wave.sample_rate, t0, desats, leak, 1.0);
    // A hypopnea, like a breath, lies wholly on measured flow.
    found.erase(std::remove_if(found.begin(), found.end(),
                               [&](const CPAPEvent& e) {
                                   const double from =
                                       std::chrono::duration<double>(e.timestamp - t0).count();
                                   const long a = static_cast<long>(std::floor(from * wave.sample_rate));
                                   const long b = static_cast<long>(
                                       std::ceil((from + e.duration_seconds) * wave.sample_rate));
                                   return noReading(a, b - 1);
                               }),
                found.end());
    return found;
}
#endif

}  // namespace hms_cpap
