#pragma once
//
// SDD-058 D3 to D6: the events the parser computes from a machine's flow,
// kept apart from the events the machine scored.
//
//   D3  the parser returns them beside the machine's, never merged
//       (ParsedSession::computed_events, SleepEvent::source Computed)
//   D4  both are stored, each in its own table; the machine's events win
//       their night once its event file is there, the computed ones are what
//       a night has before that; a session says which set its index came from
//       (cpap_session_metrics.index_events), and an index is never a sum
//   D5  hypopneas need the O2 Ring's 3% desaturations, paired here; a night
//       without a ring has an apnea-only index (Ungraded)
//   D6  nothing computed reaches a user until it agrees with a reference on
//       real nights: kComputedEventsOpen is false, and no screen reads them
//
#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"

#ifdef CPAPDASH_WITH_BMC
#include "cpapdash/parser/BmcParser.h"
#endif

#include <chrono>
#include <string>
#include <vector>

namespace hms_cpap {

/// SDD-058 D6: closed. While it is, nothing is computed, stored or shown: a
/// Luna is parsed, stored and published exactly as before. It opens by
/// changing this line, once the proof of D6 passes on real nights.
inline constexpr bool kComputedEventsOpen = false;

/// Which set a session's index was counted from.
enum class IndexEvents { Machine, Computed };

/// "machine" / "computed", as stored.
const char* indexEventsName(IndexEvents from);

/// D4: the machine's events win once its event file is on the card and the
/// session is no longer being written (a Luna writes its .evt at the stop).
/// Before that, or on a card with no event file at all, the computed set is
/// what the night has.
IndexEvents chooseIndexEvents(bool card_has_event_file, bool written_now);

/// D4: the session's index counted from its computed events alone: the event
/// counts, the index and the event durations of its metrics are replaced,
/// every other figure is kept, and `events` (the machine's) is not touched.
/// An index with no hypopnea in it is apnea-only, so Ungraded (D5).
void indexFromComputed(CPAPSession& session);

/// D3, D4: replace the session's stored computed events with `computed` and
/// record which set its index came from. The session must be saved first.
/// False when the session is not stored or a statement failed.
bool storeComputedEvents(IDatabase& db, const std::string& device_id,
                         std::chrono::system_clock::time_point session_start,
                         const std::vector<CPAPEvent>& computed, IndexEvents index_from);

#ifdef CPAPDASH_WITH_BMC
/// D5: the hypopneas of one Luna session, from its flow and the O2 Ring's
/// desaturations. `ring` is the ring's samples on the RING's clock;
/// `ring_to_machine` is added to each to put it on the machine's (the two
/// clocks are set apart). The desaturations are found here (the 3% rule) and
/// the drop rule is the parser's (detectFlowHypopneas, its FlowEventParams).
/// No ring samples, or none that desaturate: nothing, never a flow-only
/// hypopnea.
std::vector<CPAPEvent> ringHypopneas(const cpapdash::parser::BmcCard& card,
                                     const cpapdash::parser::BmcSessionSpan& span,
                                     const std::vector<CPAPVitals>& ring,
                                     std::chrono::seconds ring_to_machine);
#endif

}  // namespace hms_cpap
