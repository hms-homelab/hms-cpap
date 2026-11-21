#pragma once
//
// SDD-013 D-701-3: a night landed.
//
// Every path that stores a night's sessions says so here, once the sessions
// and the night's daily row are in the database: the burst's quiet close
// (SDD-046), the backfill worker (an uploaded zip, Reparse, POST /api/backfill,
// "Re-read my nights"), an uploaded Sefam, Löwenstein or BMC card, the live
// Prisma, Sefam, BMC and Philips card sources, the AirSense 11 and the
// DreamStation 2 over Bluetooth (and through a Mule), and the DreamStation 2
// card through OSCAR. Until 7.0.1 only the first told anyone, so an uploaded
// night got no local summary.
//
// Events, never timers: nothing here waits or repeats. A listener is told on
// the thread that stored the night and must only take the event (queue it)
// and return. The same night may be announced more than once (a re-read, a
// second session, a card read again after a restart); a listener that must
// not repeat work decides from the data (the local model compares the
// night's fact block, model/NarrationService.h).
//
// With no listener (a test, a one-shot command) it does nothing at all.
//
#include "parsers/CpapdashBridge.h"

#include <chrono>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hms_cpap::night_landed {

/// [date_folder]: the sleep day, YYYYMMDD (strDayForSessionStart's rule).
/// [str]: the night's STR row when the path has one.
using Listener = std::function<void(const std::string& date_folder, const std::optional<STRDailyRecord>& str)>;

/// Adds a listener; each is told in the order added.
void listen(Listener f);
/// Removes every listener (at shutdown, and between tests).
void clear();

/// The night [date_folder] (YYYYMMDD) landed.
void announce(const std::string& date_folder, const std::optional<STRDailyRecord>& str = std::nullopt);
/// The nights of [days], each YYYY-MM-DD or YYYYMMDD, oldest first.
void announceDays(const std::set<std::string>& days);
/// The nights the sessions starting at [starts] belong to, once each, oldest
/// first.
void announceStarts(const std::vector<std::chrono::system_clock::time_point>& starts);

}  // namespace hms_cpap::night_landed
