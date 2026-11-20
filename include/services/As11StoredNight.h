#pragma once

#include <cpapdash/parser/AirSense11Pages.h>
#include <cpapdash/parser/AirSense11Summary.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * SDD-070: an AirSense 11 night the stream did not see, filled from the
 * pages the machine stores.
 *
 * After a night, the machine's stored pages are pulled (every night with
 * usage, D2) and kept as they came under
 *
 *   <root>/stored/<sleep day>/<spool>.<n>.page     n from 0, fsync'd
 *
 * and the minutes no streamed segment covers (the night's mask-on spans minus
 * the streamed ones, D2/D3) are written into the night's folder as card files
 * (D4): a BRP at 6.25 Hz for flow and mask pressure, a PLD with leak and
 * inspiratory pressure, and the stored events merged into the night's EVE.
 *
 * This file is the part with no radio and no EDF: which spools, the order a
 * night's pages are asked for in, where they are kept, which minutes are
 * gaps, and which samples fall in them.
 */

/// A span of time, UTC epoch ms, [start, end).
struct StoredSpan {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    int64_t length() const { return end_ms - start_ms; }
};

/// The spools pulled for a night: the events, then the four signals.
const std::vector<std::string>& storedSpools();
inline constexpr const char* kStoredEventsSpool = "TherapyEvents-RespiratoryEvents";

/// <root>/stored/<night>
std::string storedNightDir(const std::string& root, const std::string& night);

/// Keeps page n of a spool, atomically and fsync'd. True when it is on disk
/// (written now, or the same bytes were there already).
bool keepStoredPage(const std::string& root, const std::string& night, const std::string& spool,
                    int n, const std::vector<uint8_t>& bytes);

/// The pages kept for a spool, page 0 first, stopping at the first missing.
std::vector<std::vector<uint8_t>> keptStoredPages(const std::string& root, const std::string& night,
                                                  const std::string& spool);

/// <root>/stored/<night>/done: every spool of the night was pulled to its end.
bool storedNightDone(const std::string& root, const std::string& night);
bool markStoredNightDone(const std::string& root, const std::string& night);

/// A Summary day's mask-on spans.
std::vector<StoredSpan> maskOnSpans(const cpapdash::parser::AirSense11Day& day);

/// The mask-on spans minus the covered ones, keeping only stretches of at
/// least min_ms (a stream segment is cut at a 60 s pause, so a shorter hole
/// is a pause, not lost data).
std::vector<StoredSpan> gapsToFill(std::vector<StoredSpan> mask_on, std::vector<StoredSpan> covered,
                                   int64_t min_ms = 60 * 1000);

/// Samples of two signals that share a grid, cut into contiguous runs: a run
/// holds the instants where both have a sample, on a step of the interval,
/// inside one gap. Values are in the signals' units (L/s, cmH2O).
struct StoredRun {
    int64_t start_ms = 0;      // the first sample
    int64_t interval_ms = 0;
    std::vector<double> a, b;
};
std::vector<StoredRun> storedRuns(const std::vector<cpapdash::parser::AirSense11PageChunk>& a,
                                  const std::vector<cpapdash::parser::AirSense11PageChunk>& b,
                                  const std::vector<StoredSpan>& gaps);

/// Every chunk of one signal across a spool's pages, in time order, a chunk
/// held twice (two pages overlapping) kept once.
std::vector<cpapdash::parser::AirSense11PageChunk>
storedChunks(const std::vector<std::vector<uint8_t>>& pages, int* skipped = nullptr);

/// Every event across the events pages, in time order, each kept once.
std::vector<cpapdash::parser::AirSense11PageEvent>
storedEvents(const std::vector<std::vector<uint8_t>>& pages);

/// The live stream's name for a stored event type ("CentralApneaEnd",
/// "ObstructiveApneaEnd"), or "" for a type not yet seen.
std::string storedEventName(int type);

/// "2026-11-04T03:00:01.000Z" for a UTC instant: a spool's fromDateTime.
std::string storedFromDateTime(int64_t utc_ms);

/// One page of a spool from `from` (a fromDateTime), as the machine answers
/// it: true with the page in *page and, when more follows, the next page's
/// cursor in *next_from (empty at the spool's end, or when the machine has
/// nothing from there). False with *why when the page could not be read.
using StoredPagePull = std::function<bool(const std::string& spool, const std::string& from,
                                          std::vector<uint8_t>* page, std::string* next_from,
                                          std::string* why)>;

struct StoredPullResult {
    bool complete = false;   // every spool read to past the night (or to its end)
    int pages = 0;           // pages kept this time
    std::string why;         // the first failure, when not complete
};

/// D2: every spool of one night with usage, from its first mask-on, page by
/// page following the machine's cursor until it reaches past the night's
/// last mask-off or the spool ends, each page kept as it came
/// (keepStoredPage). The caller writes the night and then marks it done
/// (markStoredNightDone); a night that failed part way is asked again from
/// its start by the next pull (a page already kept is the same bytes and
/// stays as it is). No radio here: `pull` is it.
StoredPullResult pullStoredNight(const std::string& root, const std::string& night,
                                 const std::vector<StoredSpan>& mask_on, const StoredPagePull& pull);

}  // namespace hms_cpap
